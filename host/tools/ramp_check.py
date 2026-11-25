#!/usr/bin/env python3
"""Find every lost, repeated or reordered sample in a recording of the ramp
that audio_loopcheck.swift plays (each sample one more than the last, modulo
16384). The first second after the ramp starts is skipped while the mock's
FIFO fills.

    python3 host/tools/ramp_check.py loop.wav

A step of +1 in the report is one sample dropped by drift steering, -1 one
repeated; a run of zeros with an equal jump is audio the host never got.
"""
import wave, struct, sys, collections
w = wave.open(sys.argv[1]); n = w.getnframes()
x = struct.unpack('<%dh' % n, w.readframes(n))
start = next(i for i in range(1,n) if x[i] != x[i-1])
settle = start + 48000  # ignore the first second while the FIFO primes
c = collections.Counter(); big = []
for i in range(settle, n):
    dd = (x[i]-x[i-1]) % 16384
    e = (dd if dd < 8192 else dd-16384) - 1
    if e: c[e] += 1
    if e not in (0, -1): big.append((round(i/48000, 2), e))
dur = (n - settle) / 48000
print(f"{dur:.1f} s after settling: events {dict(c)}; repeats {c[-1]/dur:.2f}/s, drops {c[1]/dur:.2f}/s")
print("other events (s, size):", big)
