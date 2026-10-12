/*
 * waitprobe: which half of a timer wait fails, the timer's reply or the task's wake-up?
 *   1. reply:  SendIO() a 100 ms request (UNIT_MICROHZ, then UNIT_VBLANK), keep running (no Wait) and poll
 *              CheckIO() for up to 1 s of AROS uptime: does the timer reply at all, and when?
 *              timer.device's state is dumped right after SendIO() and after the poll: elapsed time, the request's
 *              deadline, flags and list links, which queue holds it, tick counters.
 *   2. signal: is the reply port's signal set once it has replied (Wait() would then return at once)?
 *   3. wait:   only if it replied: DoIO() a 100 ms request; prints before and after, so a hang shows as a missing "after".
 * A request that never replies is unlinked under Disable() (AbortIO() can spin on it) and the test moves on.
 * Build with -I <src>/rom/timer -I <src>/arch/all-unix/timer (timer_intern.h of the timer.device being tested).
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <devices/timer.h>

#include "timer_intern.h"

struct Device *TimerBase;

#define TB ((struct TimerBase *)TimerBase)

/* Strings as IPTR, numbers as ULONG: 64-bit RawDoFmt reads %lu as 32 bits (as ticklag does) */
#define say(...) do { Printf(__VA_ARGS__); Flush(Output()); } while (0)

static ULONG uptime_ms(void)
{
    struct timeval tv;

    GetUpTime(&tv);
    return tv.tv_secs * 1000 + tv.tv_micro / 1000;
}

static void dump(const char *name, const char *when, struct timerequest *tr)
{
    struct timeval el, cur, dl;
    struct Node *n;
    ULONG flags, type, ticks, count[NUM_LISTS], in = 99, i, tc, vt;

    Disable();
    el = TB->tb_Elapsed;
    cur = TB->tb_CurrentTime;
    dl = tr->tr_time;
    flags = tr->tr_node.io_Flags;
    type = tr->tr_node.io_Message.mn_Node.ln_Type;
    ticks = (ULONG)TB->tb_ticks_total;
    tc = TB->tb_Platform.tb_TimerCount;
    vt = TB->tb_Platform.tb_VBlankTicks;
    for (i = 0; i < NUM_LISTS; i++)
    {
        count[i] = 0;
        for (n = (struct Node *)TB->tb_Lists[i].mlh_Head; n->ln_Succ; n = n->ln_Succ)
        {
            count[i]++;
            if (n == (struct Node *)tr)
                in = i;
        }
    }
    Enable();

    say((CONST_STRPTR)"  %s %s: elapsed %lu.%06lu current %lu.", (IPTR)name, (IPTR)when, (ULONG)el.tv_secs,
        (ULONG)el.tv_micro, (ULONG)cur.tv_secs);
    say((CONST_STRPTR)"%06lu req time %lu.%06lu flags 0x%lx", (ULONG)cur.tv_micro, (ULONG)dl.tv_secs, (ULONG)dl.tv_micro,
        flags);
    say((CONST_STRPTR)" type %lu in list %lu (counts %lu/%lu/%lu)", type, in, count[0], count[1], count[2]);
    say((CONST_STRPTR)" ticks %lu timercount %lu/%lu\n", ticks, tc, vt);
}

static void reply_test(const char *name, ULONG unit)
{
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *tr = (struct timerequest *)CreateIORequest(port, sizeof(*tr));
    ULONG t0, t, polls = 0, sigs, done;

    if (!port || !tr || OpenDevice((CONST_STRPTR)"timer.device", unit, &tr->tr_node, 0))
    {
        say((CONST_STRPTR)"%s: setup failed\n", (IPTR)name);
        return;
    }
    SetSignal(0, 1UL << port->mp_SigBit);
    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = 0;
    tr->tr_time.tv_micro = 100000;
    t0 = uptime_ms();
    SendIO(&tr->tr_node);
    dump(name, "after SendIO", tr);
    do
    {
        polls++;
        t = uptime_ms();
    } while (!CheckIO(&tr->tr_node) && t - t0 < 1000);
    done = CheckIO(&tr->tr_node) != NULL;
    sigs = SetSignal(0, 0);
    dump(name, "after poll", tr);
    say((CONST_STRPTR)"%s reply: %s after %lu ms of uptime", (IPTR)name, (IPTR)(done ? "replied" : "NOT replied"),
        (ULONG)(t - t0));
    say((CONST_STRPTR)", %lu polls; port signal %s\n", (ULONG)polls,
        (IPTR)((sigs & (1UL << port->mp_SigBit)) ? "set" : "NOT set"));

    if (done)
    {
        WaitIO(&tr->tr_node);

        /* 3. the ordinary way */
        tr->tr_time.tv_secs = 0;
        tr->tr_time.tv_micro = 100000;
        say((CONST_STRPTR)"%s wait: before (uptime %lu ms)\n", (IPTR)name, uptime_ms());
        DoIO(&tr->tr_node);
        say((CONST_STRPTR)"%s wait: after (uptime %lu ms)\n", (IPTR)name, uptime_ms());
    }
    else
    {
        Disable();
        if (tr->tr_node.io_Message.mn_Node.ln_Type != NT_REPLYMSG)
            Remove(&tr->tr_node.io_Message.mn_Node);
        Enable();
        say((CONST_STRPTR)"%s: unlinked the stuck request\n", (IPTR)name);
    }

    CloseDevice(&tr->tr_node);
    DeleteIORequest(&tr->tr_node);
    DeleteMsgPort(port);
}

int main(void)
{
    struct timerequest tr;

    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ, &tr.tr_node, 0))
        return 20;
    TimerBase = tr.tr_node.io_Device;
    reply_test("microhz", UNIT_MICROHZ);
    reply_test("vblank", UNIT_VBLANK);
    CloseDevice(&tr.tr_node);
    return 0;
}
