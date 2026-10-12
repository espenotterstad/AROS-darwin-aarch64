#!/bin/bash
# Boot hosted AROS once, headless, and run the timer regression suite:
# ticklag phases, upstream's misc/frequencies, timer/getsystime, crt/stdc/clock, crt/posix/usleep, exec/childwait,
# exec/childstatus, then upstream's CUnit startup (scripts/nightly/autotest/User-Startup.cunit, as CI stages it).
#   run-suite.sh <AROS dir> <aros src dir> <ticklag binary> <out dir> [timeout s (default 900)]
# Host: boot/linux or boot/darwin, whichever exists. On darwin the nowindow shim is used if present.
# Leaves the AROS dir as it found it (staged files removed, DEBUG mount and AROSBootstrap.conf restored).
# Exit status 3 on timeout (diag.txt then holds the AROS processes' signal state and CPU use).
# Env (Linux launch variants, for finding what a CI runner needs):
#   LAUNCH=setsid|plain (default setsid)  STDIN=null|pipe (default null; pipe = a FIFO held open, no EOF)
#   MEM=<MB>|none (default 512)  STARTUP=<file>: run this User-Startup instead of the suite + CUnit; the run
#   ends when it writes SYS:hosted-timer-results/suite-done.txt
#   STAGE="<host file> ...": also copy these into SYS: (by name; removed afterwards)
#   STRACE=1: run AROS under strace (setitimer calls and SIGALRM deliveries, both processes) into strace.txt
set -u
aros=$1 src=$2 tl=$3 out=$4 limit=${5:-900}
auto=$src/scripts/nightly/autotest
mkdir -p "$out" || exit 1
out=$(cd "$out" && pwd)
if [ -x "$aros/boot/linux/AROSBootstrap" ]; then host=linux; else host=darwin; fi
dbg=$aros/Devs/DOSDrivers/DEBUG
conf=$aros/boot/$host/AROSBootstrap.conf
res=$aros/hosted-timer-results

STAGE=${STAGE:-}
staged=
for f in $STAGE; do staged="$staged $(basename "$f")"; done
for f in S/User-Startup S/Test S/Try ticklag hosted-timer-results $staged; do
    [ -e "$aros/$f" ] && { echo "$aros/$f exists; not overwriting it" >&2; exit 1; }
done
grep -q '^Startup = 0$' "$dbg" || { echo "$dbg: expected 'Startup = 0'" >&2; exit 1; }
pgrep -x AROSBootstrap > /dev/null && { echo "an AROSBootstrap is already running" >&2; exit 1; }

LAUNCH=${LAUNCH:-setsid} STDIN=${STDIN:-null} MEM=${MEM:-512} STARTUP=${STARTUP:-} STRACE=${STRACE:-}
pid= fifo=
# AROS runs in its own process group where setsid exists (Linux hosted forks the AROS process off the bootstrap)
stop() {
    kill -0 "$pid" 2>/dev/null || return
    kill -- -"$pid" 2>/dev/null || kill "$pid"
    for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$pid" 2>/dev/null || return; sleep 1; done
    kill -9 -- -"$pid" 2>/dev/null || kill -9 "$pid"
}
cleanup() {
    [ -n "$pid" ] && stop
    [ -n "$fifo" ] && { exec 3>&-; rm -f "$fifo"; }
    [ -d "$res" ] && cp -p "$res"/* "$out"/ 2>/dev/null
    rm -rf "$aros/S/User-Startup" "$aros/S/Test" "$aros/S/Try" "$aros/ticklag" "$res"
    for f in $staged; do rm -f "$aros/$f"; done
    [ -f "$dbg.suite-bak" ] && mv "$dbg.suite-bak" "$dbg"
    [ -f "$conf.suite-bak" ] && mv "$conf.suite-bak" "$conf"
    if [ -d "$aros/Devs/Monitors.suite-x11" ]; then
        mv "$aros/Devs/Monitors.suite-x11"/* "$aros/Devs/Monitors/" && rmdir "$aros/Devs/Monitors.suite-x11"
    fi
}
trap cleanup EXIT INT TERM

cp -p "$dbg" "$dbg.suite-bak" && sed 's/^Startup = 0$/Startup = 1/' "$dbg.suite-bak" > "$dbg" || exit 1
cp -p "$conf" "$conf.suite-bak" || exit 1
if [ $host = linux ]; then
    # Headless display as upstream's CI unit test step does (steps-unittests.yml)
    sed '/module Devs\/Drivers\/gfx.hidd/a\
module Storage/Drivers/headlessgfx.hidd' "$conf.suite-bak" > "$conf" || exit 1
    # and with the X11 monitors moved out of the way
    if ls "$aros"/Devs/Monitors/X1* > /dev/null 2>&1; then
        mkdir "$aros/Devs/Monitors.suite-x11" && mv "$aros"/Devs/Monitors/X1* "$aros/Devs/Monitors.suite-x11/" || exit 1
    fi
fi
cp "$auto/Test.cunit" "$aros/S/Test" && cp "$auto/Try" "$aros/S/Try" && cp "$tl" "$aros/ticklag" || exit 1
for f in $STAGE; do cp "$f" "$aros/" || exit 1; done
mkdir -p "$res" || exit 1

T=SYS:Developer/Debug/Tests
R=SYS:hosted-timer-results
if [ -n "$STARTUP" ]; then
    cp "$STARTUP" "$aros/S/User-Startup" || exit 1
else
{
    cat <<EOF
FailAt 21
Echo "## hosted-timer suite start" >DEBUG:
Date >$R/date.txt
Echo "## step ticklag" >DEBUG:
SYS:ticklag idle forbid dis3 dis8 dis13 dis50 cli3 gap3 one dos delay lat1 lat7 latd >$R/ticklag.txt
Echo "rc \$RC" >>$R/ticklag.txt
Echo "## step frequencies" >DEBUG:
$T/misc/frequencies >RAM:frequencies.txt
Search RAM:frequencies.txt "FAIL" >$R/frequencies.txt
Search RAM:frequencies.txt "EClock frequency" >>$R/frequencies.txt
Delete RAM:frequencies.txt QUIET
Echo "## step timer/getsystime" >DEBUG:
$T/timer/getsystime 10 >$R/getsystime.txt
Echo "rc \$RC" >>$R/getsystime.txt
Echo "## step crt/stdc/clock" >DEBUG:
$T/crt/stdc/clock >$R/clock.txt
Echo "rc \$RC" >>$R/clock.txt
Echo "## step crt/posix/usleep" >DEBUG:
$T/crt/posix/usleep >$R/usleep.txt
Echo "rc \$RC" >>$R/usleep.txt
Echo "## step exec/childwait" >DEBUG:
$T/exec/childwait >$R/childwait.txt
Echo "rc \$RC" >>$R/childwait.txt
Echo "## step exec/childstatus" >DEBUG:
$T/exec/childstatus >$R/childstatus.txt
Echo "rc \$RC" >>$R/childstatus.txt
Echo "done" >$R/suite-done.txt
Echo "## hosted-timer suite end" >DEBUG:
EOF
    cat "$auto/User-Startup.cunit"
} > "$aros/S/User-Startup" || exit 1
fi

cd "$aros" || exit 1
in=/dev/null
if [ "$STDIN" = pipe ]; then
    fifo=$out/stdin.fifo
    rm -f "$fifo" && mkfifo "$fifo" && exec 3<>"$fifo" || exit 1
    in=$fifo
fi
memarg="-m $MEM"; [ "$MEM" = none ] && memarg=
echo "launch=$LAUNCH stdin=$STDIN mem=$MEM startup=${STARTUP:-suite} strace=${STRACE:-no}" > "$out/launch.txt"
tracer=
[ -n "$STRACE" ] && tracer="strace -f -tt -e trace=setitimer,timer_settime,alarm -e signal=SIGALRM -o $out/strace.txt"
start=$(date +%s)
if [ $host = darwin ] && [ -f "$HOME/.cache/aros/nowindow.dylib" ]; then
    DYLD_INSERT_LIBRARIES=$HOME/.cache/aros/nowindow.dylib boot/darwin/AROSBootstrap $memarg > "$out/aros.log" 2>&1 < "$in" &
elif [ "$LAUNCH" = setsid ] && command -v setsid > /dev/null; then
    setsid $tracer "boot/$host/AROSBootstrap" $memarg > "$out/aros.log" 2>&1 < "$in" &
else
    $tracer "boot/$host/AROSBootstrap" $memarg > "$out/aros.log" 2>&1 < "$in" &
fi
pid=$!
# Shutdown ends the run where it exits AROS; otherwise the last CUnit test's summary does
while kill -0 "$pid" 2>/dev/null && [ $(( $(date +%s) - start )) -lt "$limit" ]; do
    if [ -n "$STARTUP" ] && [ -f "$res/suite-done.txt" ]; then
        sleep 2
        break
    fi
    if [ -f "$res/suite-done.txt" ] && grep -q 'TEST: cunit/crt/posixc/cunit-crt-wcwidth' "$out/aros.log" &&
       [ "$(grep -c 'Run Summary' "$out/aros.log")" -ge "$(grep -c '^TEST: ' "$out/aros.log")" ]; then
        sleep 2
        break
    fi
    sleep 1
done
timedout=0
if [ $(( $(date +%s) - start )) -ge "$limit" ]; then
    timedout=1
    echo "timeout after $limit s" >> "$out/aros.log"
    # What the stalled AROS processes are doing: signal masks and pending signals, and whether they burn CPU
    for p in $(pgrep -x AROSBootstrap); do
        echo "== pid $p ppid $(ps -o ppid= -p "$p") wchan $(cat /proc/$p/wchan 2>/dev/null) threads $(ls /proc/$p/task 2>/dev/null | wc -l)"
        grep -E '^(State|SigQ|SigPnd|ShdPnd|SigBlk|SigIgn|SigCgt)' /proc/$p/status 2>/dev/null
        echo "utime+stime ticks: $(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null)"
    done > "$out/diag.txt"
    sleep 5
    for p in $(pgrep -x AROSBootstrap); do
        echo "pid $p utime+stime ticks 5 s later: $(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null) (CLK_TCK $(getconf CLK_TCK))"
    done >> "$out/diag.txt"
fi
echo "elapsed $(( $(date +%s) - start )) s" > "$out/elapsed.txt"
stop
pid=
[ $timedout = 1 ] && exit 3
exit 0
