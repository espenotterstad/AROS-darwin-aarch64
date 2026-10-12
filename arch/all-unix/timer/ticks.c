/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: EClock for the UNIX-hosted timer, read from the host's clock.

    SIGALRM only drives the timer interrupt. Time is the host clock's
    delta since the last update, so ticks the host merges or drops while
    AROS has interrupts disabled don't slow AROS's clocks.
*/

#include <aros/cpu.h>

#include "timer_intern.h"
#include "timer_macros.h"

/* Clocks that stop while the host sleeps, as the SIGALRM ticks do */
#ifdef HOST_OS_darwin
#define HOST_CLOCK 8    /* CLOCK_UPTIME_RAW */
#else
#define HOST_CLOCK 1    /* CLOCK_MONOTONIC */
#endif

/*
 * Called with interrupts disabled or from the timer interrupt, so no
 * HostLib_Lock(); clock_gettime() is async-signal-safe.
 */
static void host_time(struct TimerBase *TimerBase, struct timeval *tv)
{
    struct host_timespec ts;

    TimerBase->tb_Platform.clock_gettime(HOST_CLOCK, &ts);
    AROS_HOST_BARRIER

    tv->tv_secs  = ts.tv_sec;
    tv->tv_micro = ts.tv_nsec / 1000;
}

/*
 * Book the host time since the last update; tb_CurrentTime too if 'systime'.
 * No 64-bit division: 32-bit hosted targets may have no libgcc for it.
 */
static void book(struct TimerBase *TimerBase, BOOL systime)
{
    struct timeval now, delta;
    ULONG rate = TimerBase->tb_eclock_rate;

    host_time(TimerBase, &now);
    delta = now;
    SUBTIME(&delta, &TimerBase->tb_Platform.tb_HostLast);
    TimerBase->tb_Platform.tb_HostLast = now;

    if (systime)
    {
        ADDTIME(&TimerBase->tb_CurrentTime, &delta);
    }
    ADDTIME(&TimerBase->tb_Elapsed, &delta);

    /*
     * EClock from the start, so no remainder is carried between updates.
     * The part of a second, tv_micro * rate / 1000000, is split into
     * milliseconds and microseconds to stay exact in 32 bits (the tick
     * period is 1000000 / rate microseconds, so rate is at most 1000000).
     */
    SUBTIME(&now, &TimerBase->tb_Platform.tb_HostStart);
    TimerBase->tb_ticks_total = (UQUAD)now.tv_secs * rate +
                                ((now.tv_micro / 1000) * rate + (now.tv_micro % 1000) * rate / 1000) / 1000;
}

void EClockInit(struct TimerBase *TimerBase)
{
    host_time(TimerBase, &TimerBase->tb_Platform.tb_HostStart);
    TimerBase->tb_Platform.tb_HostLast = TimerBase->tb_Platform.tb_HostStart;
}

void EClockUpdate(struct TimerBase *TimerBase)
{
    /* Without a host clock TimerTick() counts a fixed period per tick */
    if (TimerBase->tb_Platform.clock_gettime)
        book(TimerBase, TRUE);
}

void EClockSet(struct TimerBase *TimerBase)
{
    /*
     * SetSysTime() has just overwritten tb_CurrentTime: book the time
     * since the last update to the other clocks only, or it would be
     * added to the new value too.
     */
    if (TimerBase->tb_Platform.clock_gettime)
        book(TimerBase, FALSE);
}
