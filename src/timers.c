/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Timing routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "timers.h"
#include "memory.h"
#include "log.h"
// killed
#include "signals.h"
// set_blockingmode()
#include "setupVars.h"

struct timeval t0[NUMTIMERS];

void timer_start(const int i)
{
	if(i >= NUMTIMERS)
	{
		logg("Code error: Timer %i not defined in timer_start().", i);
		exit(EXIT_FAILURE);
	}
	gettimeofday(&t0[i], 0);
}

double timer_elapsed_msec(const int i)
{
	if(i >= NUMTIMERS)
	{
		logg("Code error: Timer %i not defined in timer_elapsed_msec().", i);
		exit(EXIT_FAILURE);
	}
	struct timeval t1;
	gettimeofday(&t1, 0);
	return (t1.tv_sec - t0[i].tv_sec) * 1000.0f + (t1.tv_usec - t0[i].tv_usec) / 1000.0f;
}

void sleepms(const int milliseconds)
{
	struct timeval tv;
	tv.tv_sec = milliseconds / 1000;
	tv.tv_usec = (milliseconds % 1000) * 1000;
	select(0, NULL, NULL, NULL, &tv);
}

static int timer_delay = -1;
static bool timer_targer_state;

void set_blockingmode_timer(int delay, bool blocked)
{
	timer_delay = delay;
	timer_targer_state = blocked;
}

void *timer(void *val)
{
	// Set thread name
	prctl(PR_SET_NAME,"int.timer",0,0,0);

	// Save timestamp as we do not want to store immediately
	// to the database
	//lastGCrun = time(NULL) - time(NULL)%GCinterval;
	while(!killed)
	{
		if(timer_delay > 0)
		{
			timer_delay--;
		}
		else if(timer_delay == 0)
		{
			set_blockingstatus(timer_targer_state);
			timer_delay = -1;
		}
		sleepms(1000);
	}

	return NULL;
}