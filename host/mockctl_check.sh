#!/bin/sh
# Check host/tools/mockctl.py against the mock's controls over a pty.
# Needs no ESP32, only python3 on PATH.
set -eu
cd "$(dirname "$0")"

LINK="$PWD/build/civ_ctl"
make -s build/civ_pty

./build/civ_pty -q "$LINK" &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
i=0
while [ ! -e "$LINK" ] && [ $i -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done

fail=0
# check <expected exit status> <text the output must contain> <arguments...>
check() {
    want_status=$1
    want_text=$2
    shift 2
    status=0
    got=$(python3 tools/mockctl.py -p "$LINK" "$@" 2>&1) || status=$?
    if [ "$status" -eq "$want_status" ] && printf '%s\n' "$got" | grep -qF -- "$want_text"; then
        echo "ok    mockctl.py $*"
    else
        echo "FAIL  mockctl.py $*"
        echo "      want: exit $want_status, output containing: $want_text"
        echo "      got:  exit $status"
        printf '%s\n' "$got" | sed 's/^/        /'
        fail=1
    fi
}

check 0 "protocol 1, personality: Hamlib model 3085"
check 0 "snr         +10.0 dB in 2500 Hz"            show
check 0 "status      source nothing measured yet, gain +0.0 dB"
check 0 "snr         -12.5 dB in 3000 Hz"            noise=on snr=-12.5 bw=3000
check 0 "noise       on"
check 0 "route       radio"                          route=radio turnaround=150
check 0 "turnaround  150 ms"
check 0 "floor       -55.5 dBFS"                     floor=-55.5 atten=6
check 0 "atten       6.0 dB"
check 1 "snr=99: refused"                            snr=99
check 1 "floor=40 has the wrong sign"                floor=40
check 1 "route is monitor, radio or parrot"           route=sideways
check 1 "noise is off or on"                         noise=maybe
check 1 "no setting called 'volume'"                 volume=3
check 0 "snr         -12.5 dB in 3000 Hz"
check 0 "parrot      delay 1.0 s, period off; idle, 0.0 s recorded of 30.0 s"
check 0 "route       parrot"                         route=parrot delay=2.5 period=15
check 0 "parrot      delay 2.5 s, period 15.0 s; idle"
check 0 "period 7.5 s"                               period=7.5
check 1 "delay=61: refused by the mock (out of range?)" delay=61
check 1 "replay: refused by the mock (needs route=parrot, a recording and PTT off)" replay
check 0 "ptt         line off, vox off at -40.0 dBFS with 100 ms hang; receiving"
check 0 "ptt         line rts, vox on at -35.5 dBFS with 250 ms hang; receiving" line=rts vox=on voxlevel=-35.5 voxhang=250
check 1 "line is off, rts or dtr"                    line=cts
check 1 "voxlevel=-3: refused by the mock (out of range?)" voxlevel=-3
check 0 "route       monitor"                        reset
check 0 "parrot      delay 1.0 s, period off"
check 0 "ptt         line off, vox off at -40.0 dBFS with 100 ms hang; receiving"
check 0 "snr         +10.0 dB in 2500 Hz"

[ "$fail" -eq 0 ] && echo "PASS" || echo "FAILED"
exit "$fail"
