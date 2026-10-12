/*
 * ticklag: measure how far hosted AROS's clocks fall behind the host's under different loads.
 *
 * Each phase samples four clocks before and after: the host's CLOCK_MONOTONIC (clock_gettime through
 * hostlib.resource), ReadEClock (timer.device tb_ticks_total), GetUpTime (tb_Elapsed) and GetSysTime
 * (tb_CurrentTime), and prints each one's elapsed milliseconds and how much AROS lost against the host.
 * Hosted timer.device adds a fixed period per SIGALRM (arch/all-unix/timer/timer_init.c), so a signal
 * that coalesces with one still pending (masked by Disable(), or the thread not scheduled) loses its time.
 *
 * Usage: ticklag [phase ...]; phases: idle spin forbid dis3 dis8 dis13 dis50 cli3 gap3 one dos delay mark lat1 lat7 latd
 * (default: idle to dis50, dos, delay). "mark" prints one sample (host ms, uptime ms, systime s) to compare across separate runs.
 * lat1, lat7 and latd measure wake-up latency instead: the host time from sending a request (UNIT_MICROHZ 1 ms or 7 ms,
 * or Delay(1) = one 20 ms VBlank) to waking up, as min/median/90th percentile/max, plus how many woke before their time.
 *
 * Build (in bin/darwin-aarch64): as repro/hdtest.c, with repro/ticklag.c -o ticklag
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <proto/hostlib.h>
#include <proto/kernel.h>
#include <devices/timer.h>
#include <string.h>

struct Device *TimerBase;
APTR HostLibBase;
APTR KernelBase;

/* Host struct timespec on 64-bit darwin and Linux: two longs */
struct host_timespec { long tv_sec; long tv_nsec; };

static int (*host_clock_gettime)(int, struct host_timespec *);
static int host_monotonic;
static ULONG eclock_rate;
static volatile ULONG sink;
static UQUAD spins_per_ms;
static struct timerequest *latreq;

#define PHASE_MS 10000

struct sample
{
    UQUAD host_us;
    UQUAD eclock;
    UQUAD up_us;
    UQUAD sys_us;
};

static UQUAD host_now_us(void)
{
    struct host_timespec ts;

    HostLib_Lock();
    host_clock_gettime(host_monotonic, &ts);
    HostLib_Unlock();
    return (UQUAD)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void take(struct sample *s)
{
    struct EClockVal ev;
    struct timeval tv;

    s->host_us = host_now_us();
    ReadEClock(&ev);
    s->eclock = ((UQUAD)ev.ev_hi << 32) | ev.ev_lo;
    GetUpTime(&tv);
    s->up_us = (UQUAD)tv.tv_secs * 1000000 + tv.tv_micro;
    GetSysTime(&tv);
    s->sys_us = (UQUAD)tv.tv_secs * 1000000 + tv.tv_micro;
}

/* Spin for ms of host time. Only inside Disable()/KrnCli(): no other task can run there, so the host call needs no
 * HostLib_Lock() (a semaphore, which mustn't be taken with interrupts off) */
static void spin_host_masked(ULONG ms)
{
    struct host_timespec ts;
    UQUAD end;

    host_clock_gettime(host_monotonic, &ts);
    end = (UQUAD)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 + ms * 1000ULL;
    do
    {
        host_clock_gettime(host_monotonic, &ts);
        AROS_HOST_BARRIER
    } while ((UQUAD)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 < end);
}

static void spin(UQUAD n)
{
    UQUAD i;

    for (i = 0; i < n; i++)
        sink += (ULONG)i;
}

static void calibrate(void)
{
    UQUAD n = 20000000, t0, t1;

    t0 = host_now_us();
    spin(n);
    t1 = host_now_us();
    spins_per_ms = n * 1000 / (t1 - t0 ? t1 - t0 : 1);
}

/* Prints milliseconds as a signed decimal; RawDoFmt reads %ld as 32 bits, so pass LONGs only */
static void report(const char *name, const char *note, struct sample *a, struct sample *b, ULONG count)
{
    LONG host = (LONG)((b->host_us - a->host_us) / 1000);
    LONG ticks = (LONG)(((b->eclock - a->eclock) * 1000) / eclock_rate);
    LONG up = (LONG)((b->up_us - a->up_us) / 1000);
    LONG sys = (LONG)((b->sys_us - a->sys_us) / 1000);
    LONG pct = host ? (LONG)(((QUAD)up * 1000) / host) : 0;

    Printf((CONST_STRPTR)"%-6s host %6ld ms  eclock %6ld  uptime %6ld  systime %6ld  lost %6ld ms  aros/host %4ld.%ld%%  n %lu %s\n",
           (IPTR)name, host, ticks, up, sys, host - up, pct / 10, pct % 10, count, (IPTR)note);
}

static void phase_idle(void)
{
    struct sample a, b;

    take(&a);
    Delay(PHASE_MS / 20);
    take(&b);
    report("idle", "(one Delay of 10 s)", &a, &b, 1);
}

static void phase_spin(void)
{
    struct sample a, b;

    take(&a);
    spin(spins_per_ms * PHASE_MS);
    take(&b);
    report("spin", "(no AROS calls)", &a, &b, 1);
}

static void phase_forbid(void)
{
    struct sample a, b;
    ULONG n = 0;

    take(&a);
    while (host_now_us() - a.host_us < PHASE_MS * 1000ULL)
    {
        Forbid();
        spin(spins_per_ms * 50);
        Permit();
        n++;
    }
    take(&b);
    report("forbid", "(50 ms sections)", &a, &b, n);
}

static void phase_disable(const char *name, ULONG ms)
{
    struct sample a, b;
    ULONG n = 0;

    take(&a);
    while (host_now_us() - a.host_us < PHASE_MS * 1000ULL)
    {
        ULONG k;

        /* Several sections between host clock reads, so the read's own cost stays small */
        for (k = 0; k < 10; k++)
        {
            Disable();
            spin_host_masked(ms);
            Enable();
            n++;
        }
    }
    take(&b);
    report(name, "(back-to-back Disable sections)", &a, &b, n);
}

/* Variants that tell where a pending tick goes: KrnCli/KrnSti directly (no exec IDNestCnt), sections with an
 * unmasked gap after each, and one long section */
static void phase_cli(void)
{
    struct sample a, b;
    ULONG n = 0;

    take(&a);
    while (host_now_us() - a.host_us < PHASE_MS * 1000ULL)
    {
        ULONG k;

        for (k = 0; k < 10; k++)
        {
            KrnCli();
            spin_host_masked(3);
            KrnSti();
            n++;
        }
    }
    take(&b);
    report("cli3", "(KrnCli/KrnSti 3 ms sections)", &a, &b, n);
}

static void phase_gap(void)
{
    struct sample a, b;
    UQUAD per = spins_per_ms * 3;
    ULONG n = 0;

    take(&a);
    while (host_now_us() - a.host_us < PHASE_MS * 1000ULL)
    {
        ULONG k;

        for (k = 0; k < 10; k++)
        {
            Disable();
            spin_host_masked(3);
            Enable();
            spin(per);
            n++;
        }
    }
    take(&b);
    report("gap3", "(Disable 3 ms + 3 ms unmasked)", &a, &b, n);
}

static void phase_one(void)
{
    struct sample a, b;

    take(&a);
    Disable();
    spin_host_masked(1000);
    Enable();
    take(&b);
    report("one", "(one 1 s Disable section)", &a, &b, 1);
}

static void phase_dos(void)
{
    struct sample a, b;
    UBYTE buf[256];
    ULONG n = 0;

    take(&a);
    while (host_now_us() - a.host_us < PHASE_MS * 1000ULL)
    {
        ULONG k;

        for (k = 0; k < 100; k++)
        {
            BPTR l = Lock((CONST_STRPTR)"RAM:", SHARED_LOCK);

            if (l)
            {
                NameFromLock(l, buf, sizeof(buf));
                UnLock(l);
            }
            n++;
        }
    }
    take(&b);
    report("dos", "(Lock/NameFromLock/UnLock RAM:)", &a, &b, n);
}

static void phase_delay(void)
{
    struct sample a, b;
    ULONG i;

    take(&a);
    for (i = 0; i < 100; i++)
        Delay(5);
    take(&b);
    report("delay", "(100 x Delay(5) = 10 s)", &a, &b, 100);
}

#define LAT_MAX 200

/* Wake-up latency of n requests of 'us' microseconds each (0: Delay(1)), measured on the host clock */
static void phase_latency(const char *name, const char *note, ULONG us, ULONG n)
{
    static ULONG el[LAT_MAX];
    ULONG want = us ? us : 1000000 / 50, early = 0, i, j;
    UQUAD sum = 0;

    if (n > LAT_MAX)
        n = LAT_MAX;
    for (i = 0; i < n; i++)
    {
        UQUAD t0, t1, start;

        /* Start 0-4.9 ms after the last wake-up's tick, spread evenly over one 5 ms tick period, timed on the host clock */
        start = host_now_us() + ((i * 37) % 50) * 100;
        while ((t0 = host_now_us()) < start)
            ;
        if (us)
        {
            latreq->tr_node.io_Command = TR_ADDREQUEST;
            latreq->tr_time.tv_secs = us / 1000000;
            latreq->tr_time.tv_micro = us % 1000000;
            DoIO(&latreq->tr_node);
        }
        else
            Delay(1);
        t1 = host_now_us();
        el[i] = (ULONG)(t1 - t0);
        if (el[i] < want)
            early++;
        sum += el[i];
    }
    for (i = 1; i < n; i++)
    {
        ULONG v = el[i];

        for (j = i; j > 0 && el[j - 1] > v; j--)
            el[j] = el[j - 1];
        el[j] = v;
    }
    Printf((CONST_STRPTR)"%-6s want %6lu us  min %6lu  median %6lu  p90 %6lu  max %6lu  mean %6lu  early %lu of %lu %s\n",
           (IPTR)name, want, el[0], el[n / 2], el[n * 9 / 10], el[n - 1], (ULONG)(sum / n), early, n, (IPTR)note);
}

static void phase_mark(void)
{
    struct sample a;

    take(&a);
    Printf((CONST_STRPTR)"mark host-ms %lu uptime-ms %lu systime-s %lu\n", (ULONG)(a.host_us / 1000),
           (ULONG)(a.up_us / 1000), (ULONG)(a.sys_us / 1000000));
}

static const char *libc_names[] = { "libSystem.dylib", "libc.so.6", "libc.so", NULL };
static const char *all[] = { "idle", "spin", "forbid", "dis3", "dis8", "dis13", "dis50", "dos", "delay", NULL };

int main(int argc, char **argv)
{
    struct timerequest tr;
    struct MsgPort *port;
    const char **phases = all;
    void *libc = NULL;
    int i;

    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, &tr.tr_node, 0) != 0)
        return 20;
    TimerBase = tr.tr_node.io_Device;
    port = CreateMsgPort();
    if (!port)
        return 20;
    tr.tr_node.io_Message.mn_ReplyPort = port;
    tr.tr_node.io_Message.mn_Length = sizeof(tr);
    latreq = &tr;
    KernelBase = OpenResource((CONST_STRPTR)"kernel.resource");
    if (!KernelBase)
        return 20;

    HostLibBase = OpenResource((CONST_STRPTR)"hostlib.resource");
    if (!HostLibBase)
    {
        Printf((CONST_STRPTR)"no hostlib.resource\n");
        return 20;
    }
    for (i = 0; libc_names[i] && !libc; i++)
        libc = HostLib_Open(libc_names[i], NULL);
    if (!libc)
        return 20;
    host_monotonic = (i == 1) ? 6 : 1;      /* CLOCK_MONOTONIC: 6 on darwin, 1 on Linux */
    host_clock_gettime = (int (*)(int, struct host_timespec *))HostLib_GetPointer(libc, "clock_gettime", NULL);
    if (!host_clock_gettime)
        return 20;

    {
        struct EClockVal ev;
        eclock_rate = ReadEClock(&ev);
    }

    if (argc > 1)
        phases = (const char **)&argv[1];
    else
    {
        calibrate();
        Printf((CONST_STRPTR)"eclock %lu Hz, %lu spins/ms\n", eclock_rate, (ULONG)spins_per_ms);
    }

    for (i = 0; phases[i]; i++)
    {
        const char *p = phases[i];

        if (!spins_per_ms && strcmp(p, "mark") && strcmp(p, "idle") && strcmp(p, "delay") && strcmp(p, "dos") &&
            strncmp(p, "lat", 3))
            calibrate();
        if (!strcmp(p, "idle")) phase_idle();
        else if (!strcmp(p, "spin")) phase_spin();
        else if (!strcmp(p, "forbid")) phase_forbid();
        else if (!strcmp(p, "dis3")) phase_disable("dis3", 3);
        else if (!strcmp(p, "dis8")) phase_disable("dis8", 8);
        else if (!strcmp(p, "dis13")) phase_disable("dis13", 13);
        else if (!strcmp(p, "dis50")) phase_disable("dis50", 50);
        else if (!strcmp(p, "dos")) phase_dos();
        else if (!strcmp(p, "cli3")) phase_cli();
        else if (!strcmp(p, "gap3")) phase_gap();
        else if (!strcmp(p, "one")) phase_one();
        else if (!strcmp(p, "delay")) phase_delay();
        else if (!strcmp(p, "mark")) phase_mark();
        else if (!strcmp(p, "lat1")) phase_latency("lat1", "(UNIT_MICROHZ 1 ms)", 1000, 200);
        else if (!strcmp(p, "lat7")) phase_latency("lat7", "(UNIT_MICROHZ 7 ms)", 7000, 100);
        else if (!strcmp(p, "latd")) phase_latency("latd", "(Delay(1))", 0, 100);
        else
        {
            Printf((CONST_STRPTR)"unknown phase %s\n", (IPTR)p);
            HostLib_Close(libc, NULL);
            CloseDevice(&tr.tr_node);
            DeleteMsgPort(port);
            return 10;
        }
    }

    HostLib_Close(libc, NULL);
    CloseDevice(&tr.tr_node);
    DeleteMsgPort(port);
    return 0;
}
