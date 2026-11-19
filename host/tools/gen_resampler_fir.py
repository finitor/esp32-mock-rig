#!/usr/bin/env python3
"""Generate the 48 kHz <-> 12 kHz resampler low-pass for audio_dsp.

Kaiser-windowed sinc, quantised to Q15. Prints the C table and the response
of the quantised filter, so the numbers quoted in audio_dsp.h can be checked.

    python3 host/tools/gen_resampler_fir.py          # stats + table
    python3 host/tools/gen_resampler_fir.py --table  # table only

Pure standard library: no numpy needed.
"""
import cmath
import math
import sys

FS = 48000.0
TAPS = 96          # multiple of 4: 24 taps per interpolator phase
PASS_HZ = 4000.0   # flat to here
STOP_HZ = 6500.0   # rejected from here (aliases of >= 6.5 kHz land above 5.5 kHz)
ATTEN_DB = 80.0


def bessel_i0(x):
    s, term, k = 1.0, 1.0, 1
    while term > 1e-12 * s:
        term *= (x / (2 * k)) ** 2
        s += term
        k += 1
    return s


def design():
    fc = (PASS_HZ + STOP_HZ) / 2 / FS          # cycles per sample
    beta = 0.1102 * (ATTEN_DB - 8.7)
    mid = (TAPS - 1) / 2
    h = []
    for n in range(TAPS):
        t = n - mid
        sinc = 2 * fc if t == 0 else math.sin(2 * math.pi * fc * t) / (math.pi * t)
        w = bessel_i0(beta * math.sqrt(1 - (t / mid) ** 2)) / bessel_i0(beta)
        h.append(sinc * w)
    s = sum(h)
    return [x / s for x in h]                   # unity gain at DC


def quantise(h):
    q = [int(round(x * 32768)) for x in h]
    # Make the taps sum to exactly 1.0. A symmetric filter has an even sum, so
    # the rounding error is even and splits across the two centre taps.
    err = 32768 - sum(q)
    assert err % 2 == 0
    q[TAPS // 2 - 1] += err // 2
    q[TAPS // 2] += err // 2
    assert q == q[::-1]
    return q


def gain_db(q, f_hz):
    w = 2 * math.pi * f_hz / FS
    acc = sum(c * cmath.exp(-1j * w * n) for n, c in enumerate(q))
    return 20 * math.log10(max(abs(acc) / 32768, 1e-12))


def main():
    q = quantise(design())
    if "--table" not in sys.argv:
        pb = [gain_db(q, f) for f in range(0, int(PASS_HZ) + 1, 50)]
        sb = [gain_db(q, f) for f in range(int(STOP_HZ), 24001, 25)]
        print(f"// taps {TAPS}, sum {sum(q)}, sum|h| {sum(abs(c) for c in q) / 32768:.3f}")
        print(f"// passband 0..{PASS_HZ:.0f} Hz: {min(pb):+.3f} .. {max(pb):+.3f} dB")
        print(f"// stopband >= {STOP_HZ:.0f} Hz: {max(sb):.1f} dB or better")
        print(f"// at 5000 Hz {gain_db(q, 5000):.1f} dB, 6000 Hz {gain_db(q, 6000):.1f} dB")
        for p in range(4):
            ph = q[p::4]
            print(f"// phase {p}: sum {sum(ph)} (x4 = {4 * sum(ph) / 32768:.4f}), sum|h| x4 {4 * sum(abs(c) for c in ph) / 32768:.3f}")
    print(f"static const int16_t rs_coef[DSP_RS_TAPS] = {{")
    for i in range(0, TAPS, 8):
        print("    " + ", ".join(f"{c:6d}" for c in q[i:i + 8]) + ",")
    print("};")


if __name__ == "__main__":
    main()
