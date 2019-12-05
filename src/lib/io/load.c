/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Load generation algorithms
 * @file io/load.c
 *
 * @copyright 2019 Network RADIUS SARL <legal@networkradius.com>
 */
RCSID("$Id$")

#include <freeradius-devel/io/load.h>

/*
 *	We use *inverse* numbers to avoid numerical calculation issues.
 *
 *	i.e. The bad way is to take two small numbers divide them by
 *	alpha / beta and then add them.  That process can drop the
 *	lower digits.  Instead, we take two small numbers, add them,
 *	and then divide the result by alpha / beta.
 */
#define IBETA (4)
#define IALPHA (8)

#define DIFF(_rtt, _t) (_rtt < _t ? (_t - _rtt) : (_rtt - _t))
#define RTTVAR(_rtt, _rttvar, _t) ((((IBETA - 1) * _rttvar) + DIFF(_rtt, _t)) / IBETA)
#define RTT(_old, _new) ((_new + ((IALPHA - 1) * _old)) / IALPHA)

typedef enum {
	FR_LOAD_STATE_INIT = 0,
	FR_LOAD_STATE_SENDING,
	FR_LOAD_STATE_GATED,
	FR_LOAD_STATE_DRAINING,
} fr_load_state_t;

struct fr_load_s {
	fr_load_state_t		state;
	fr_event_list_t		*el;
	fr_load_config_t const *config;
	fr_load_callback_t	callback;
	void			*uctx;

	fr_load_stats_t		stats;			//!< sending statistics
	fr_time_t		step_start;		//!< when the current step started
	fr_time_t		step_end;		//!< when the current step will end
	int			step_received;

	uint32_t		pps;
	fr_time_delta_t		delta;			//!< between packets

	uint32_t		count;
	bool			header;			//!< for printing statistics

	fr_time_t		next;			//!< The next time we're supposed to send a packet
	fr_event_timer_t const	*ev;
};

fr_load_t *fr_load_generator_create(TALLOC_CTX *ctx, fr_event_list_t *el, fr_load_config_t *config,
				    fr_load_callback_t callback, void *uctx)
{
	fr_load_t *l;

	l = talloc_zero(ctx, fr_load_t);
	if (!l) return NULL;

	if (!config->start_pps) config->start_pps = 1;
	if (!config->milliseconds) config->milliseconds = 1000;
	if (!config->parallel) config->parallel = 1;

	l->el = el;
	l->config = config;
	l->callback = callback;
	l->uctx = uctx;

	return l;
}

static void load_timer(fr_event_list_t *el, fr_time_t now, void *uctx)
{
	fr_load_t *l = uctx;
	fr_time_t next;
	fr_time_t delta;
	int backlog;
	uint32_t i;

	/*
	 *	Send as many packets as necessary.
	 */
	l->stats.sent += l->count;

	/*
	 *	Keep track of the overall maximum backlog for the
	 *	duration of the entire test run.
	 */
	backlog = l->stats.sent - l->stats.received;
	if (backlog > l->stats.max_backlog) l->stats.max_backlog = backlog;

	/*
	 *	ema_n+1 = (sample - ema_n) * (2 / (n + 1)) + ema_n
	 *
	 *	Where we want the average over N samples.  For us,
	 *	this means "packets per second".
	 *
	 *	For numerical stability, we only divide *after* adding
	 *	everything together, not before.
	 */
	l->stats.backlog_ema = (((backlog - l->stats.backlog_ema) * 2) + ((l->pps + 1) * l->stats.backlog_ema)) / (l->pps + 1);
	l->stats.last_send = now;

	/*
	 *	We don't have "pps" packets in the backlog, go send
	 *	some more.  We scale the backlog by 1000 milliseconds
	 *	per second.  Then, multiple the PPS by the number of
	 *	milliseconds of backlog we want to keep.
	 *
	 *	If the backlog is smaller than packets/s *
	 *	milliseconds of backlog, then keep sending.
	 *	Otherwise, switch to a gated mode where we only send
	 *	new packets once a reply comes in.
	 */
	if (((uint32_t) l->stats.backlog_ema * 1000) < (l->pps * l->config->milliseconds)) {
		l->state = FR_LOAD_STATE_SENDING;
		l->stats.blocked = false;
		l->count = l->config->parallel;

		next = l->next + l->delta;
		if (next < now) {
			delta = 0;
		} else {
			delta = next - now;
		}
	} else {
		/*
		 *	We have too many packets in the backlog, we're
		 *	gated.  Don't send more packets until we have
		 *	a reply.
		 *
		 *	Note that we will send *these* packets.
		 */
		l->state = FR_LOAD_STATE_GATED;
		l->count = 1;
		next = now + l->delta;
		delta = l->delta; /* shut up compiler */
	}
	l->next = next;

	/*
	 *	If we're done this step, go to the next one.
	 */
	if (next >= l->step_end) {
		l->step_start = next;
		l->step_end = next + ((uint64_t) l->config->duration) * NSEC;
		l->step_received = l->stats.received;
		l->pps += l->config->step;
		l->stats.pps = l->pps;
		l->delta = (NSEC * l->config->parallel) / l->pps;

		/*
		 *	Stop at max PPS, if it's set.  Otherwise
		 *	continue without limit.
		 */
		if (l->config->max_pps && (l->pps > l->config->max_pps)) {
			l->state = FR_LOAD_STATE_DRAINING;
		}
	}

	/*
	 *	Set the timer for the next packet.
	 */
	if ((l->state == FR_LOAD_STATE_SENDING) &&
	    (fr_event_timer_in(l, el, &l->ev, delta, load_timer, l) < 0)) {
		l->state = FR_LOAD_STATE_DRAINING;
		return;
	}
	/*
	 *	Else we're gated, and we only send packets when we
	 *	receive a reply.
	 */

	/*
	 *	Run the callback AFTER we set the timer.  Which makes
	 *	it more likely that the next timer fires on time.
	 */
	for (i = 0; i < l->count; i++) {
		l->callback(now, l->uctx);
	}
}


/** Start the load generator.
 *
 */
int fr_load_generator_start(fr_load_t *l)
{
	l->stats.start = fr_time();
	l->step_start = l->stats.start;
	l->step_end = l->step_start + ((uint64_t) l->config->duration) * NSEC;

	l->pps = l->config->start_pps;
	l->stats.pps = l->pps;
	l->delta = (NSEC * l->config->parallel) / l->pps;
	l->next = l->step_start + l->delta;
	l->count = l->config->parallel;

	load_timer(l->el, l->step_start, l);
	return 0;
}


/** Stop the load generation through the simple expedient of deleting
 * the timer associated with it.
 *
 */
int fr_load_generator_stop(fr_load_t *l)
{
	if (!l->ev) return 0;

	return fr_event_timer_delete(l->el, &l->ev);
}

/** Tell the load generator that we have a reply to a packet we sent.
 *
 */
fr_load_reply_t fr_load_generator_have_reply(fr_load_t *l, fr_time_t request_time)
{
	fr_time_t now;
	fr_time_delta_t t;

	now = fr_time();
	t = now - request_time;

	l->stats.rttvar = RTTVAR(l->stats.rtt, l->stats.rttvar, t);
	l->stats.rtt = RTT(l->stats.rtt, t);

	l->stats.received++;

	/*
	 *	Track packets/s.  Since times are in nanoseconds, we
	 *	have to scale the counters up by NSEC.  And since NSEC
	 *	is 1B, the calculations have to be done via 64-bit
	 *	numbers, and then converted to a final 32-bit counter.
	 */
	l->stats.pps_accepted = (((uint64_t) (l->stats.received - l->step_received)) * NSEC) / (now - l->step_start);

	/*
	 *	t is in nanoseconds.
	 */
	if (t < 1000) {
	       l->stats.times[0]++; /* microseconds */
	} else if (t < 10000) {
	       l->stats.times[1]++; /* tens of microseconds */
	} else if (t < 100000) {
	       l->stats.times[2]++; /* 100s of microseconds */
	} else if (t < 1000000) {
	       l->stats.times[3]++; /* milliseconds */
	} else if (t < 10000000) {
	       l->stats.times[4]++; /* 10s of milliseconds */
	} else if (t < 100000000) {
	       l->stats.times[5]++; /* 100s of milliseconds */
	} else if (t < NSEC) {
	       l->stats.times[6]++; /* seconds */
	} else {
	       l->stats.times[7]++; /* tens of seconds */
	}

	/*
	 *	Still sending packets.  Rely on the timer to send more
	 *	packets.
	 */
	if (l->state == FR_LOAD_STATE_SENDING) return FR_LOAD_CONTINUE;

	/*
	 *	The send code has decided that the backlog is too
	 *	high.  New requests are blocked until replies come in.
	 *	Since we have a reply, send another request.
	 */
	if (l->state == FR_LOAD_STATE_GATED) {
		l->stats.blocked = true;
		load_timer(l->el, now, l);
		return FR_LOAD_CONTINUE;
	}

	/*
	 *	We're still sending or gated, tell the caller to
	 *	continue.
	 */
	if (l->state != FR_LOAD_STATE_DRAINING) {
		return FR_LOAD_CONTINUE;
	}
	/*
	 *	Not yet received all replies.  Wait until we have all
	 *	replies.
	 */
	if (l->stats.received < l->stats.sent) return FR_LOAD_CONTINUE;

	l->stats.end = now;
	return FR_LOAD_DONE;
}

/** Print load generator statistics in CVS format.
 *
 */
size_t fr_load_generator_stats_sprint(fr_load_t *l, fr_time_t now, char *buffer, size_t buflen)
{
	double now_f, last_send_f;

	if (!l->header) {
		l->header = true;
		return snprintf(buffer, buflen, "\"time\",\"last_packet\",\"rtt\",\"rttvar\",\"pps\",\"pps_accepted\",\"sent\",\"received\",\"ema_backlog\",\"max_backlog\",\"usec\",\"10us\",\"100us\",\"ms\",\"10ms\",\"100ms\",\"s\",\"10s\"\n");
	}


	now_f = now - l->stats.start;
	now_f /= NSEC;

	last_send_f = l->stats.last_send - l->stats.start;
	last_send_f /= NSEC;

	return snprintf(buffer, buflen,
			"%f,%f,"
			"%" PRIu64 ",%" PRIu64 ","
			"%d,%d,"
			"%d,%d,"
			"%d,%d,"
			"%d,%d,%d,%d,%d,%d,%d,%d\n",
			now_f, last_send_f,
			l->stats.rtt, l->stats.rttvar,
			l->stats.pps, l->stats.pps_accepted,
			l->stats.sent, l->stats.received,
			l->stats.backlog_ema, l->stats.max_backlog,
			l->stats.times[0], l->stats.times[1], l->stats.times[2], l->stats.times[3],
			l->stats.times[4], l->stats.times[5], l->stats.times[6], l->stats.times[7]);
}

fr_load_stats_t const * fr_load_generator_stats(fr_load_t const *l)
{
	return &l->stats;
}