#!/usr/bin/env python3
"""Generate the Hilbert transformer and the fading-gain filter for audio_dsp.

The fading stage needs the receive audio as a complex (analytic) signal: the
audio itself, delayed, as the real part and its Hilbert transform as the
imaginary part. The transformer is a Kaiser-windowed FIR at 12 kHz, odd
length and antisymmetric, so every second tap is zero and only the others
are stored. What matters is how well it suppresses the image: with a gain of
G where 1 is ideal, the unwanted sideband is |1 - G| / |1 + G| of the wanted.

The fading gains are complex noise shaped to a Gaussian Doppler spectrum, as
ITU-R F.1487 has it, with the spread equal to two standard deviations. They
are made at 32 samples per hertz of spread, so one table serves every spread.

    python3 host/tools/gen_hilbert_fir.py          # stats + tables
    python3 host/tools/gen_hilbert_fir.py --table  # tables only

Pure standard library: no numpy needed.
"""
import math
import sys

FS = 12000.0
TAPS = 127          # odd; the delay is (TAPS - 1) / 2 = 63 samples, 5.25 ms
BETA = 5.0
LOW_HZ = 300.0      # the image is judged from here ...
HIGH_HZ = FS / 2 - LOW_HZ   # ... to here

GAIN_RATE = 32      # gain samples per hertz of Doppler spread
GAIN_TAPS = 59      # +/- 4 standard deviations of the filter in time


def bessel_i0(x):
    s, term, k = 1.0, 1.0, 1
    while term > 1e-12 * s:
        term *= (x / (2 * k)) ** 2
        s += term
        k += 1
    return s


def hilbert():
    mid = (TAPS - 1) // 2
    q = []
    for n in range(TAPS):
        k = n - mid
        if k % 2 == 0:
            q.append(0)
            continue
        w = bessel_i0(BETA * math.sqrt(1 - (k / mid) ** 2)) / bessel_i0(BETA)
        q.append(int(round(2.0 / (math.pi * k) * w * 32768)))
    assert all(q[n] == -q[TAPS - 1 - n] for n in range(TAPS))
    return q


def hilbert_gain(q, f_hz):
    # Antisymmetric about the middle tap: the response is -j * G(f), G real.
    mid = (TAPS - 1) // 2
    w = 2 * math.pi * f_hz / FS
    return sum(2 * q[mid + k] * math.sin(w * k) for k in range(1, mid + 1)) / 32768


def gain_filter():
    # Spectrum exp(-f^2 / (2 s^2)) with s = spread / 2: the amplitude filter is
    # exp(-f^2 / (4 s^2)), whose impulse response is a Gaussian in time,
    # exp(-4 pi^2 s^2 t^2). At t = k / (GAIN_RATE * spread) the spread cancels.
    mid = (GAIN_TAPS - 1) // 2
    h = [math.exp(-(math.pi * (k - mid) / GAIN_RATE) ** 2) for k in range(GAIN_TAPS)]
    # Unit power for a complex gain: each of its two parts has power 1/2 when
    # fed noise of unit power.
    norm = math.sqrt(0.5 / sum(x * x for x in h))
    return [x * norm for x in h]


def main():
    q = hilbert()
    g = gain_filter()
    if "--table" not in sys.argv:
        worst, at = 0.0, 0.0
        f = LOW_HZ
        while f <= HIGH_HZ:
            gain = hilbert_gain(q, f)
            image = abs(1 - gain) / abs(1 + gain)
            if image > worst:
                worst, at = image, f
            f += 10.0
        nz = [c for c in q if c]
        print(f"// Hilbert: {TAPS} taps, {len(nz)} of them not zero, delay {(TAPS - 1) // 2} samples")
        print(f"// image {LOW_HZ:.0f}..{HIGH_HZ:.0f} Hz: {20 * math.log10(worst):.1f} dB at worst ({at:.0f} Hz)")
        for f in (100, 200, 250, 300, 500, 1500, 3000):
            gain = hilbert_gain(q, f)
            print(f"//   {f:5d} Hz: gain {gain:.5f}, image {20 * math.log10(abs(1 - gain) / abs(1 + gain) + 1e-12):.1f} dB")
        print(f"// sum|h| {sum(abs(c) for c in q) / 32768:.3f}")
        tail = g[0] / g[(GAIN_TAPS - 1) // 2]
        print(f"// gain filter: {GAIN_TAPS} taps at {GAIN_RATE} per Hz of spread, end tap {20 * math.log10(tail):.0f} dB below the middle")
        print(f"// sum of squares {sum(x * x for x in g):.6f}")
    mid = (TAPS - 1) // 2
    odd = [q[mid + k] for k in range(1, mid + 1, 2)]   # taps at +1, +3, ... ; those at -k are the negatives
    print(f"static const int16_t hilbert_coef[DSP_HILBERT_PAIRS] = {{")
    for i in range(0, len(odd), 8):
        print("    " + ", ".join(f"{c:6d}" for c in odd[i:i + 8]) + ",")
    print("};")
    print(f"static const float fade_gain_coef[DSP_FADE_GAIN_TAPS] = {{")
    for i in range(0, GAIN_TAPS, 4):
        print("    " + ", ".join(f"{x:.9e}f" for x in g[i:i + 4]) + ",")
    print("};")


if __name__ == "__main__":
    main()
