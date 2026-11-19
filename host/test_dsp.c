/* Unit tests for audio_dsp. Signals are synthesised; no files, no hardware. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_failed;
static int g_checks;

#define CHECK(cond)                                                \
    do {                                                           \
        g_checks++;                                                \
        if (!(cond)) {                                             \
            g_failed++;                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);  \
        }                                                          \
    } while (0)

/* Like CHECK, but prints the measured value when it fails. */
#define CHECK_RANGE(value, lo, hi)                                                          \
    do {                                                                                    \
        double v_ = (value);                                                                \
        g_checks++;                                                                         \
        if (!(v_ >= (lo) && v_ <= (hi))) {                                                  \
            g_failed++;                                                                     \
            printf("FAIL %s:%d %s = %g, want %g..%g\n", __FILE__, __LINE__, #value, v_,     \
                   (double)(lo), (double)(hi));                                             \
        }                                                                                   \
    } while (0)

static void sine(int16_t *out, size_t n, double freq, double fs, double amp)
{
    for (size_t i = 0; i < n; i++) out[i] = (int16_t)lround(amp * sin(2 * M_PI * freq * (double)i / fs));
}

/* Amplitude of the component at freq. n must hold a whole number of cycles. */
static double tone_amp(const int16_t *x, size_t n, double freq, double fs)
{
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++) {
        double ph = 2 * M_PI * freq * (double)i / fs;
        re += x[i] * cos(ph);
        im += x[i] * sin(ph);
    }
    return 2 * sqrt(re * re + im * im) / (double)n;
}

static double rms(const int16_t *x, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
    return sqrt(s / (double)n);
}

static double db(double ratio) { return 20 * log10(ratio > 1e-12 ? ratio : 1e-12); }

#define AMP 20000.0

/* ---- resampler ---------------------------------------------------------- */

static void test_decimator_response(void)
{
    enum { N_IN = 9600, N_OUT = N_IN / 4, SKIP = 200, WIN = 1200 }; /* WIN = 0.1 s at 12 kHz */
    static int16_t in[N_IN], out[N_OUT];

    const double pass[] = {300, 1000, 2500, 4000};
    for (size_t i = 0; i < sizeof pass / sizeof pass[0]; i++) {
        dsp_decim4_t d;
        dsp_decim4_init(&d);
        sine(in, N_IN, pass[i], 48000, AMP);
        CHECK(dsp_decim4(&d, in, N_IN, out) == N_OUT);
        CHECK_RANGE(db(tone_amp(out + SKIP, WIN, pass[i], 12000) / AMP), -0.05, 0.05);
    }

    /* Anything from 6.5 kHz up would alias into the 12 kHz stream. */
    const double stop[] = {6500, 8000, 9000, 11000, 13000, 20000, 23000};
    for (size_t i = 0; i < sizeof stop / sizeof stop[0]; i++) {
        dsp_decim4_t d;
        dsp_decim4_init(&d);
        sine(in, N_IN, stop[i], 48000, AMP);
        dsp_decim4(&d, in, N_IN, out);
        CHECK_RANGE(db(rms(out + SKIP, WIN) / (AMP / sqrt(2))), -200, -60);
    }
}

static void test_decimator_chunking(void)
{
    enum { N = 5003 };
    static int16_t in[N], whole[N / 4 + 1], parts[N / 4 + 1];
    srand(1);
    for (int i = 0; i < N; i++) in[i] = (int16_t)(rand() % 65536 - 32768);

    dsp_decim4_t a, b;
    dsp_decim4_init(&a);
    size_t n_whole = dsp_decim4(&a, in, N, whole);
    CHECK(n_whole == N / 4);

    dsp_decim4_init(&b);
    const size_t sizes[] = {1, 7, 13, 160, 3, 48, 2};
    size_t at = 0, n_parts = 0, k = 0;
    while (at < N) {
        size_t n = sizes[k++ % (sizeof sizes / sizeof sizes[0])];
        if (n > N - at) n = N - at;
        n_parts += dsp_decim4(&b, in + at, n, parts + n_parts);
        at += n;
    }
    CHECK(n_parts == n_whole);
    CHECK(memcmp(whole, parts, n_whole * sizeof(int16_t)) == 0);
}

static void test_interpolator_response(void)
{
    enum { N_IN = 2400, N_OUT = N_IN * 4, SKIP = 800, WIN = 4800 }; /* WIN = 0.1 s at 48 kHz */
    static int16_t in[N_IN], out[N_OUT];

    const double pass[] = {300, 1000, 2500, 4000};
    for (size_t i = 0; i < sizeof pass / sizeof pass[0]; i++) {
        dsp_interp4_t u;
        dsp_interp4_init(&u);
        sine(in, N_IN, pass[i], 12000, AMP);
        dsp_interp4(&u, in, N_IN, out);
        CHECK_RANGE(db(tone_amp(out + SKIP, WIN, pass[i], 48000) / AMP), -0.05, 0.05);
        /* Images of the tone around 12, 24 kHz must be gone. */
        const double images[] = {12000 - pass[i], 12000 + pass[i], 24000 - pass[i]};
        for (int j = 0; j < 3; j++) {
            if (images[j] < 6500) continue;
            CHECK_RANGE(db(tone_amp(out + SKIP, WIN, images[j], 48000) / AMP), -200, -60);
        }
    }
}

static void test_round_trip(void)
{
    /* 48 kHz -> 12 kHz -> 48 kHz returns the signal 92 samples late:
     * 44.5 through the decimator as it is phased here, 47.5 through the
     * interpolator. */
    enum { N = 9600, DELAY = 92 };
    static int16_t in[N], mid[N / 4], out[N];
    sine(in, N, 1500, 48000, AMP);

    dsp_decim4_t d;
    dsp_interp4_t u;
    dsp_decim4_init(&d);
    dsp_interp4_init(&u);
    CHECK(dsp_decim4(&d, in, N, mid) == N / 4);
    dsp_interp4(&u, mid, N / 4, out);

    int worst = 0;
    for (int i = 1000; i < N; i++) {
        int e = abs(out[i] - in[i - DELAY]);
        if (e > worst) worst = e;
    }
    CHECK_RANGE(worst, 0, 30); /* out of an amplitude of 20000 */
}

/* ---- noise and channel -------------------------------------------------- */

static void test_gauss(void)
{
    enum { N = 200000 };
    dsp_rng_t r;
    dsp_rng_seed(&r, 12345);
    double s1 = 0, s2 = 0, s4 = 0, lag = 0, prev = 0;
    for (int i = 0; i < N; i++) {
        double g = dsp_gauss(&r) / 65536.0;
        s1 += g;
        s2 += g * g;
        s4 += g * g * g * g;
        lag += g * prev;
        prev = g;
    }
    double mean = s1 / N, var = s2 / N;
    CHECK_RANGE(mean, -0.01, 0.01);
    CHECK_RANGE(sqrt(var), 0.99, 1.01);
    CHECK_RANGE((s4 / N) / (var * var), 2.8, 3.0); /* 2.9 for twelve uniforms; 3.0 is exact Gaussian */
    CHECK_RANGE(lag / N / var, -0.01, 0.01);       /* white: no sample-to-sample correlation */

    dsp_rng_t a, b, c;
    dsp_rng_seed(&a, 7);
    dsp_rng_seed(&b, 7);
    dsp_rng_seed(&c, 8);
    int same = 0, differ = 0;
    for (int i = 0; i < 100; i++) {
        int32_t ga = dsp_gauss(&a);
        same += ga == dsp_gauss(&b);
        differ += ga != dsp_gauss(&c);
    }
    CHECK(same == 100);
    CHECK(differ > 90);

    dsp_rng_seed(&a, 0); /* zero would lock xorshift up */
    CHECK(dsp_gauss(&a) != dsp_gauss(&a));
}

static void test_channel(void)
{
    enum { N = 100000 };
    static int16_t in[N], out[N];
    dsp_chan_t c;

    /* Unity gain, no noise: bit exact, also in place. */
    dsp_chan_init(&c, 1);
    sine(in, N, 1000, 12000, AMP);
    dsp_chan_process(&c, in, out, N);
    CHECK(memcmp(in, out, sizeof in) == 0);
    memcpy(out, in, sizeof in);
    dsp_chan_process(&c, out, out, N);
    CHECK(memcmp(in, out, sizeof in) == 0);

    /* Attenuation. */
    dsp_chan_set_gain_db(&c, -6.0206f);
    dsp_chan_process(&c, in, out, N);
    CHECK_RANGE(tone_amp(out, 1200, 1000, 12000), AMP / 2 - 2, AMP / 2 + 2);
    dsp_chan_set_gain_db(&c, -200.0f);
    dsp_chan_process(&c, in, out, N);
    CHECK(rms(out, N) == 0);
    dsp_chan_set_gain_db(&c, +10.0f); /* never amplifies */
    CHECK(c.gain_q15 == 32768);

    /* Noise alone at a set level. */
    dsp_chan_set_noise_rms(&c, 1000.0f);
    dsp_chan_process(&c, NULL, out, N);
    CHECK_RANGE(rms(out, N), 980, 1020);

    /* Signal plus noise: the tone survives at its level, the rest is the noise. */
    dsp_chan_set_gain_db(&c, 0.0f);
    dsp_chan_process(&c, in, out, N);
    CHECK_RANGE(tone_amp(out, 96000, 1000, 12000), AMP * 0.99, AMP * 1.01);
    double total = rms(out, N), sig = AMP / sqrt(2);
    CHECK_RANGE(sqrt(total * total - sig * sig), 950, 1050);

    /* Loud noise saturates instead of wrapping. */
    dsp_chan_set_noise_rms(&c, 1e9f);
    CHECK(c.noise_rms_q8 == 32767u * 256u);
    dsp_chan_process(&c, in, out, N);
    int hit_rail = 0;
    for (int i = 0; i < N; i++) hit_rail += out[i] == 32767 || out[i] == -32768;
    CHECK(hit_rail > 1000);
    dsp_chan_set_noise_rms(&c, -5.0f);
    CHECK(c.noise_rms_q8 == 0);
}

static void test_snr_helper(void)
{
    /* At 12 kHz the noise fills 6 kHz; half of its power is inside 3 kHz. */
    CHECK_RANGE(dsp_noise_rms_for_snr(1000, 0, 12000, 3000), 1414.0, 1414.5);
    CHECK_RANGE(dsp_noise_rms_for_snr(1000, 10, 12000, 3000), 447.0, 447.4);
    CHECK_RANGE(dsp_noise_rms_for_snr(1000, -10, 12000, 3000), 4471, 4473);
    CHECK_RANGE(dsp_noise_rms_for_snr(1000, 0, 48000, 2500), 3098, 3099);
    CHECK(dsp_noise_rms_for_snr(0, 0, 12000, 3000) == 0);
    CHECK(dsp_noise_rms_for_snr(1000, 0, 12000, 0) == 0);
}

/* ---- meter and VOX ------------------------------------------------------ */

static void test_meter(void)
{
    enum { N = 12000 };
    static int16_t x[N];
    dsp_meter_t m;

    dsp_meter_reset(&m);
    CHECK(dsp_meter_rms_dbfs(&m) == DSP_SILENCE_DBFS);

    sine(x, N, 1000, 12000, 32767);
    dsp_meter_add(&m, x, N / 2);
    dsp_meter_add(&m, x + N / 2, N / 2);
    CHECK_RANGE(dsp_meter_rms_dbfs(&m), -3.03, -2.99);
    CHECK_RANGE(dsp_meter_peak_dbfs(&m), -0.01, 0.0);
    CHECK(m.clipped > 0 && m.count == N);

    dsp_meter_reset(&m);
    sine(x, N, 1000, 12000, 3276.7);
    dsp_meter_add(&m, x, N);
    CHECK_RANGE(dsp_meter_rms_dbfs(&m), -23.03, -22.99);
    CHECK(m.clipped == 0);

    dsp_meter_reset(&m);
    memset(x, 0, sizeof x);
    dsp_meter_add(&m, x, N);
    CHECK(dsp_meter_rms_dbfs(&m) == DSP_SILENCE_DBFS);

    int16_t lowest = -32768; /* must not overflow when squared or negated */
    dsp_meter_reset(&m);
    dsp_meter_add(&m, &lowest, 1);
    CHECK(m.peak == 32768 && m.clipped == 1);
}

static void test_vox(void)
{
    enum { HANG = 1200 }; /* 100 ms at 12 kHz */
    static int16_t quiet[HANG + 10], loud[100];
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; i++) quiet[i] = (i & 1) ? 100 : -100;
    sine(loud, 100, 1000, 12000, 5000);

    dsp_vox_t v;
    dsp_vox_init(&v, -30.0f, 100, 12000); /* threshold about 1036 */
    CHECK(v.hang_samples == HANG);
    CHECK_RANGE(v.threshold, 1030, 1040);

    CHECK(!dsp_vox_process(&v, quiet, 500));
    CHECK(dsp_vox_process(&v, loud, 100));
    /* The last loud sample of the burst is a few samples before its end. */
    CHECK(dsp_vox_process(&v, quiet, HANG - 10));
    CHECK(!dsp_vox_process(&v, quiet, 20));
    CHECK(!dsp_vox_process(&v, quiet, 500));

    dsp_vox_init(&v, -30.0f, 0, 12000); /* no hang: follows the audio */
    CHECK(dsp_vox_process(&v, loud + 1, 3));
    CHECK(!dsp_vox_process(&v, quiet, 1));
}

/* ---- FIFO ---------------------------------------------------------------- */

static void test_fifo_basic(void)
{
    static int16_t store[16];
    dsp_fifo_t f;
    CHECK(!dsp_fifo_init(&f, store, 12));
    CHECK(!dsp_fifo_init(&f, NULL, 16));
    CHECK(dsp_fifo_init(&f, store, 16));

    int16_t a[10], out[20];
    for (int i = 0; i < 10; i++) a[i] = (int16_t)(i + 1);

    CHECK(dsp_fifo_write(&f, a, 10) == 10 && dsp_fifo_level(&f) == 10);
    CHECK(dsp_fifo_write(&f, a, 10) == 6 && f.overruns == 4); /* full */
    CHECK(dsp_fifo_read(&f, out, 12) == 12);
    CHECK(out[0] == 1 && out[9] == 10 && out[10] == 1 && out[11] == 2);
    CHECK(dsp_fifo_write(&f, a, 10) == 10); /* wraps the end of the storage */
    CHECK(dsp_fifo_read(&f, out, 20) == 14 && f.underruns == 6);
    CHECK(out[0] == 3 && out[3] == 6 && out[4] == 1 && out[13] == 10 && out[14] == 0 && out[19] == 0);
    CHECK(dsp_fifo_level(&f) == 0);
}

/* A writer whose clock is fast or slow by one sample every `every` blocks. */
static void run_drift(int extra, int every, uint32_t *min_level, uint32_t *max_level, dsp_fifo_t *f)
{
    enum { BLOCK = 48, TARGET = 240, TOL = 24, TICKS = 200000 };
    static int16_t store[1024];
    static int16_t wbuf[BLOCK + 1], rbuf[BLOCK];
    dsp_fifo_init(f, store, 1024);
    memset(wbuf, 0, sizeof wbuf);
    /* Steering looks at the level when the reader calls, which is just after
     * a block has arrived. Prefill so that level starts at the target. */
    for (int filled = BLOCK; filled < TARGET; filled += BLOCK) dsp_fifo_write(f, wbuf, BLOCK);

    *min_level = UINT32_MAX;
    *max_level = 0;
    for (int t = 0; t < TICKS; t++) {
        size_t n = BLOCK;
        if (t % every == 0) n = (size_t)(BLOCK + extra);
        dsp_fifo_write(f, wbuf, n);
        uint32_t level = dsp_fifo_level(f); /* what the steering sees */
        dsp_fifo_read_steered(f, rbuf, BLOCK, TARGET, TOL);
        if (level < *min_level) *min_level = level;
        if (level > *max_level) *max_level = level;
    }
}

static void test_fifo_drift(void)
{
    dsp_fifo_t f;
    uint32_t lo, hi;

    /* Writer fast by 1 in 4800 (208 ppm, far worse than two crystals). */
    run_drift(+1, 100, &lo, &hi, &f);
    CHECK(f.overruns == 0 && f.underruns == 0);
    CHECK_RANGE(hi, 240 + 24, 240 + 24 + 2); /* held at the edge of the tolerance */
    CHECK_RANGE(lo, 240, 242);
    CHECK_RANGE(f.dropped, 1900, 2000); /* 2000 extra samples arrived */
    CHECK(f.repeated == 0);

    /* Writer slow by the same amount. */
    run_drift(-1, 100, &lo, &hi, &f);
    CHECK(f.overruns == 0 && f.underruns == 0);
    CHECK_RANGE(lo, 240 - 24 - 2, 240 - 24);
    CHECK_RANGE(hi, 238, 240);
    CHECK_RANGE(f.repeated, 1900, 2000);
    CHECK(f.dropped == 0);

    /* Matched clocks: steering never acts. */
    run_drift(0, 100, &lo, &hi, &f);
    CHECK(f.dropped == 0 && f.repeated == 0 && lo == 240 && hi == 240);
}

static void test_fifo_steer_content(void)
{
    static int16_t store[64];
    dsp_fifo_t f;
    dsp_fifo_init(&f, store, 64);
    int16_t in[40], out[8];
    for (int i = 0; i < 40; i++) in[i] = (int16_t)(i + 1);

    dsp_fifo_write(&f, in, 40);
    CHECK(dsp_fifo_read_steered(&f, out, 8, 10, 2) == 8); /* far too full: drop one */
    CHECK(out[0] == 2 && out[7] == 9 && f.dropped == 1);

    dsp_fifo_read(&f, out, 8);
    dsp_fifo_read(&f, out, 8);
    dsp_fifo_read(&f, out, 4); /* 11 left: 29..39, well under a target of 30 */
    CHECK(dsp_fifo_level(&f) == 11);
    CHECK(dsp_fifo_read_steered(&f, out, 8, 30, 2) == 7); /* too empty: repeat one */
    CHECK(out[0] == 30 && out[6] == 36 && out[7] == 36 && f.repeated == 1);
    CHECK(dsp_fifo_level(&f) == 4);

    /* Nearly empty: steering cannot help, plain read with silence. */
    CHECK(dsp_fifo_read_steered(&f, out, 8, 30, 2) == 4);
    CHECK(out[3] == 40 && out[4] == 0 && f.underruns == 4 && f.repeated == 1);
}

int main(void)
{
    test_decimator_response();
    test_decimator_chunking();
    test_interpolator_response();
    test_round_trip();
    test_gauss();
    test_channel();
    test_snr_helper();
    test_meter();
    test_vox();
    test_fifo_basic();
    test_fifo_drift();
    test_fifo_steer_content();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
