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
check 1 "route is monitor or radio"                  route=sideways
check 1 "no setting called 'volume'"                 volume=3
check 0 "snr         -12.5 dB in 3000 Hz"
check 0 "route       monitor"                        reset
check 0 "snr         +10.0 dB in 2500 Hz"

[ "$fail" -eq 0 ] && echo "PASS" || echo "FAILED"
exit "$fail"
