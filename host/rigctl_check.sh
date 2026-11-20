#!/bin/sh
# Acceptance check: real Hamlib (rigctl) against the CI-V parser over a pty.
# Needs no radio and no ESP32, only rigctl on PATH.
#
#   ./rigctl_check.sh            IC-705, Hamlib model 3085
#   MODEL=3088 ./rigctl_check.sh the G90 personality, Hamlib model 3088
#   MODEL=3073 ./rigctl_check.sh try another Hamlib model against the IC-705 personality
#
# Expected values were recorded with Hamlib 4.7.1. A different Hamlib version
# may format a value differently; the "rejected commands" check at the end
# does not depend on formatting.
set -eu
cd "$(dirname "$0")"

MODEL=${MODEL:-3085}
LINK="$PWD/build/civ"
LOG="build/rigctl_pty.log"

command -v rigctl >/dev/null || { echo "rigctl not found: install Hamlib"; exit 2; }
make -s build/civ_pty

# The parser takes the personality the model names, if there is one.
RIG=3085
[ "$MODEL" = 3088 ] && RIG=3088
./build/civ_pty -m "$RIG" "$LINK" 2>"$LOG" &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
i=0
while [ ! -e "$LINK" ] && [ $i -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done

fail=0
check() {
    want=$1
    shift
    got=$(rigctl -m "$MODEL" -r "$LINK" "$@" 2>/dev/null | tr '\n' ' ' | sed 's/ *$//')
    if [ "$got" = "$want" ]; then
        echo "ok    $*"
    else
        echo "FAIL  $*"
        echo "      want: $want"
        echo "      got:  $got"
        fail=1
    fi
}

echo "rigctl -m $MODEL ($(rigctl --version 2>/dev/null | cut -d' ' -f1-3))"
if [ "$RIG" = 3088 ]; then
    # Hamlib's G90 backend has no width for the data modes and reports the
    # split VFO as None.
    check "14074000 PKTUSB 0 0 None 0"        f m s t
    check "7074000"                           F 7074000 f
    check "PKTUSB 0"                          M PKTUSB 0 m
    check "PKTLSB 0"                          M PKTLSB 0 m
    check "LSB 2400"                          M LSB 2400 m
    check "USB 3000"                          M USB 3000 m
    check "CW 500"                            M CW 500 m
    check "CWR 500"                           M CWR 500 m
    check "FM 10000"                          M FM 10000 m
    check "1"                                 T 1 t
    check "0"                                 T 0 t
    check "1 VFOB"                            S 1 VFOB s
    check "7075000"                           I 7075000 i
    check "PKTUSB 0"                          X PKTUSB 0 x
    check "0 VFOA"                            S 0 VFOA s
    check "7075000"                           V VFOB f
    check "7074000"                           V VFOA f
    check "0.498039"                          L RFPOWER 0.5 l RFPOWER
    check "0.298039"                          L AF 0.3 l AF
    check "0.2"                               L SQL 0.2 l SQL
    check "700"                               L CWPITCH 700 l CWPITCH
    check "-27"                               l STRENGTH
    check "1"                                 l SWR
    check "1"                                 l MICGAIN
    check "2"                                 L AGC 2 l AGC
    check "10"                                L PREAMP 10 l PREAMP
    check "12"                                L ATT 12 l ATT
    check "1"                                 U NB 1 u NB
    check "1"                                 U COMP 1 u COMP
    check "1"                                 U LOCK 1 u LOCK
    check ""                                  G TUNE
    check ""                                  G XCHG
else
    check "14074000 PKTUSB 3000 0 currVFO 0" f m s t
    check "7074000"                           F 7074000 f
    check "PKTUSB 3000"                       M PKTUSB 0 m
    check "LSB 3000"                          M LSB 0 m
    check "CW 500"                            M CW 500 m
    check "1"                                 T 1 t
    check "0"                                 T 0 t
    check "1 VFOB"                            S 1 VFOB s
    check "7075000"                           I 7075000 i
    check "0 VFOA"                            S 0 VFOA s
    check "7075000"                           V VFOB f
    check "7074000"                           V VFOA f
    check "0.498039"                          L RFPOWER 0.5 l RFPOWER
    check "-47"                               l STRENGTH
    check "1"                                 U NB 1 u NB
    check "1"                                 get_powerstat
fi

frames=$(grep -c '^<-' "$LOG" || true)
ng=$(grep -c 'fa fd$' "$LOG" || true)
echo "$frames frames received, $ng rejected"
if [ "$ng" -ne 0 ]; then
    echo "Commands the parser rejected:"
    grep -B1 'fa fd$' "$LOG" | grep '^<-' | sort | uniq -c
    fail=1
fi
[ "$fail" -eq 0 ] && echo "PASS" || echo "FAILED"
exit "$fail"
