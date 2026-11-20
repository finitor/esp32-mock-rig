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
    CHECK(c.gain_q24 == 1 << 24);

    /* The plain-factor setter does amplify, up to its limit, and saturates. */
    dsp_chan_set_gain(&c, 0.5f);
    CHECK(c.gain_q24 == 1 << 23);
    dsp_chan_set_gain(&c, 4.0f);
    dsp_chan_process(&c, in, out, N);
    int railed = 0;
    for (int i = 0; i < N; i++) railed += out[i] == 32767 || out[i] == -32768;
    CHECK(railed > 1000); /* AMP * 4 is past full scale */
    dsp_chan_set_gain(&c, 1e9f);
    CHECK(c.gain_q24 == (int32_t)DSP_CHAN_GAIN_MAX << 24);
    dsp_chan_set_gain(&c, -1.0f);
    CHECK(c.gain_q24 == 0);
    dsp_chan_set_gain(&c, 1.0f);

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

/* ---- fading ---------------------------------------------------------------- */

#define FADE_FS 12000.0

/* Nothing asked of it: the audio comes out as it went in, 63 samples late. */
static void test_fade_passthrough(void)
{
    enum { LEN = 4000 };
    static int16_t in[LEN], out[LEN];
    dsp_fade_t f;

    dsp_fade_init(&f, 1u);
    for (int i = 0; i < LEN; i++) in[i] = (int16_t)((i * 7919) % 60001 - 30000);
    dsp_fade_process(&f, in, out, LEN);
    int wrong = 0;
    for (int i = DSP_FADE_DELAY; i < LEN; i++) wrong += out[i] != in[i - DSP_FADE_DELAY];
    for (int i = 0; i < DSP_FADE_DELAY; i++) wrong += out[i] != 0;
    CHECK(wrong == 0);
    CHECK(dsp_fade_time(&f) == LEN);

    /* In place, and in pieces of any size. */
    dsp_fade_t g;
    static int16_t buf[LEN];
    dsp_fade_init(&g, 1u);
    memcpy(buf, in, sizeof buf);
    for (int at = 0, k = 1; at < LEN; at += k, k = k % 97 + 1) {
        int len = at + k <= LEN ? k : LEN - at;
        dsp_fade_process(&g, buf + at, buf + at, (size_t)len);
    }
    CHECK(memcmp(buf, out, sizeof buf) == 0);
}

/* A frequency offset moves a tone by that much, the right way, and leaves
 * almost nothing where the other sideband would be. */
static void test_fade_shift(void)
{
    enum { LEN = 24000, SKIP = 2000 };
    static int16_t in[LEN], out[LEN];
    static const double tones[] = {200.0, 300.0, 1000.0, 2700.0, 4500.0};
    dsp_fade_t f;

    for (size_t t = 0; t < sizeof tones / sizeof tones[0]; t++) {
        double tone = tones[t];
        sine(in, LEN, tone, FADE_FS, 10000.0);
        dsp_fade_init(&f, 1u);
        dsp_fade_set(&f, 0, 0, 1000); /* +100.0 Hz */
        dsp_fade_process(&f, in, out, LEN);
        CHECK_RANGE(db(tone_amp(out + SKIP, LEN - SKIP, tone + 100.0, FADE_FS) / 10000.0), -0.05, 0.05);
        CHECK(db(tone_amp(out + SKIP, LEN - SKIP, fabs(100.0 - tone), FADE_FS) / 10000.0) < -55.0);
        CHECK(db(tone_amp(out + SKIP, LEN - SKIP, tone, FADE_FS) / 10000.0) < -55.0);

        dsp_fade_init(&f, 1u);
        dsp_fade_set(&f, 0, 0, -505); /* -50.5 Hz */
        dsp_fade_process(&f, in, out, LEN);
        CHECK_RANGE(db(tone_amp(out + SKIP, LEN - SKIP, tone - 50.5, FADE_FS) / 10000.0), -0.05, 0.05);
    }

    /* Over a long run the level does not creep: the rotating phase stays a
     * unit vector. */
    dsp_fade_init(&f, 1u);
    dsp_fade_set(&f, 0, 0, 37);
    sine(in, LEN, 1500.0, FADE_FS, 10000.0);
    for (int i = 0; i < 100; i++) dsp_fade_process(&f, in, out, LEN); /* 200 s */
    CHECK_RANGE(db(rms(out, LEN) * sqrt(2.0) / 10000.0), -0.02, 0.02);
}

/* The complex envelope of a faded tone, one value per 10 ms: what the two
 * gains did to it. tone must make whole cycles in 120 samples. */
#define ENV_BLOCK 120

static size_t fade_envelope(uint32_t seed, uint32_t spread_chz, uint32_t delay, double tone, size_t blocks,
                            double *env_re, double *env_im)
{
    enum { AMP_IN = 8000 };
    static int16_t in[ENV_BLOCK], out[ENV_BLOCK];
    static double ref_c[ENV_BLOCK], ref_s[ENV_BLOCK];
    dsp_fade_t f;

    sine(in, ENV_BLOCK, tone, FADE_FS, AMP_IN); /* whole cycles: the block repeats seamlessly */
    for (int i = 0; i < ENV_BLOCK; i++) {
        ref_c[i] = cos(2 * M_PI * tone * i / FADE_FS);
        ref_s[i] = sin(2 * M_PI * tone * i / FADE_FS);
    }
    dsp_fade_init(&f, seed);
    dsp_fade_set(&f, spread_chz, delay, 0);
    for (size_t b = 0; b < blocks; b++) {
        dsp_fade_process(&f, in, out, ENV_BLOCK);
        double re = 0, im = 0;
        for (int i = 0; i < ENV_BLOCK; i++) {
            re += out[i] * ref_s[i]; /* the tone is a sine: in phase with it is "real" */
            im += out[i] * ref_c[i];
        }
        env_re[b] = re * 2.0 / ENV_BLOCK / AMP_IN;
        env_im[b] = im * 2.0 / ENV_BLOCK / AMP_IN;
    }
    return blocks;
}

/* How alike two envelopes are: 1 the same but for a constant factor, 0 unrelated. */
static double env_correlation(const double *a_re, const double *a_im, const double *b_re, const double *b_im, size_t n,
                              size_t lag)
{
    double c_re = 0, c_im = 0, pa = 0, pb = 0;
    for (size_t i = 0; i + lag < n; i++) {
        size_t j = i + lag;
        c_re += a_re[i] * b_re[j] + a_im[i] * b_im[j];
        c_im += a_im[i] * b_re[j] - a_re[i] * b_im[j];
        pa += a_re[i] * a_re[i] + a_im[i] * a_im[i];
        pb += b_re[j] * b_re[j] + b_im[j] * b_im[j];
    }
    return sqrt(c_re * c_re + c_im * c_im) / sqrt(pa * pb);
}

enum { ENV_MAX = 40000 }; /* 400 s */
static double g_e1r[ENV_MAX], g_e1i[ENV_MAX], g_e2r[ENV_MAX], g_e2i[ENV_MAX];

/* Rayleigh fading with a Gaussian Doppler spectrum of the spread asked for,
 * and unit power on average. */
static void test_fade_statistics(void)
{
    /* 4 Hz of spread for 400 s: some thousands of fades. One path only would
     * do; with no delay the two add up to the same thing. */
    size_t n = fade_envelope(11u, 400, 0, 1500.0, ENV_MAX, g_e1r, g_e1i);
    double mean = 0;
    size_t below_10 = 0, below_20 = 0, above_3 = 0;
    for (size_t i = 0; i < n; i++) {
        double p = g_e1r[i] * g_e1r[i] + g_e1i[i] * g_e1i[i];
        mean += p;
        below_10 += p < 0.1;
        below_20 += p < 0.01;
        above_3 += p > 2.0;
    }
    mean /= (double)n;
    CHECK_RANGE(10 * log10(mean), -0.4, 0.4);                    /* unit power */
    CHECK_RANGE((double)below_10 / (double)n, 0.080, 0.110);     /* 1 - exp(-0.1) = 0.095: 10 dB down */
    CHECK_RANGE((double)below_20 / (double)n, 0.006, 0.014);     /* 1 - exp(-0.01) = 0.010: 20 dB down */
    CHECK_RANGE((double)above_3 / (double)n, 0.115, 0.155);      /* exp(-2) = 0.135: 3 dB up */

    /* A Gaussian spectrum with standard deviation s has the correlation
     * exp(-2 pi^2 s^2 tau^2). s is half the spread: 2 Hz. */
    CHECK_RANGE(env_correlation(g_e1r, g_e1i, g_e1r, g_e1i, n, 4), 0.85, 0.91);   /* 0.04 s: 0.881 */
    CHECK_RANGE(env_correlation(g_e1r, g_e1i, g_e1r, g_e1i, n, 8), 0.56, 0.65);   /* 0.08 s: 0.603 */
    CHECK_RANGE(env_correlation(g_e1r, g_e1i, g_e1r, g_e1i, n, 16), 0.08, 0.19);  /* 0.16 s: 0.133 */
    CHECK(env_correlation(g_e1r, g_e1i, g_e1r, g_e1i, n, 40) < 0.05);

    /* A tenth of the spread fades ten times as slowly. */
    n = fade_envelope(11u, 40, 0, 1500.0, ENV_MAX, g_e2r, g_e2i);
    CHECK_RANGE(env_correlation(g_e2r, g_e2i, g_e2r, g_e2i, n, 80), 0.50, 0.70);  /* 0.8 s: 0.603 */

    /* Another seed is another path. */
    n = fade_envelope(12u, 400, 0, 1500.0, ENV_MAX, g_e2r, g_e2i);
    CHECK(env_correlation(g_e1r, g_e1i, g_e2r, g_e2i, n, 0) < 0.05);
    /* The same seed is the same path, at another audio frequency too when
     * there is no delay between the paths: flat fading. */
    n = fade_envelope(11u, 400, 0, 2000.0, ENV_MAX, g_e2r, g_e2i);
    CHECK(env_correlation(g_e1r, g_e1i, g_e2r, g_e2i, n, 0) > 0.999);
}

/* A delayed second path makes the fading differ across the audio band: two
 * tones fade together or apart according to how many cycles of difference
 * the delay holds. */
static void test_fade_delay(void)
{
    enum { DELAY = 24 }; /* 2 ms */
    size_t n = fade_envelope(21u, 400, DELAY, 1000.0, ENV_MAX / 2, g_e1r, g_e1i);

    fade_envelope(21u, 400, DELAY, 1500.0, n, g_e2r, g_e2i); /* 500 Hz apart: one whole cycle in 2 ms */
    CHECK(env_correlation(g_e1r, g_e1i, g_e2r, g_e2i, n, 0) > 0.99);
    fade_envelope(21u, 400, DELAY, 1100.0, n, g_e2r, g_e2i); /* 100 Hz: cos(pi * 100 * 0.002) = 0.81 */
    CHECK_RANGE(env_correlation(g_e1r, g_e1i, g_e2r, g_e2i, n, 0), 0.76, 0.86);

    /* With 5 ms between the paths, 100 Hz is half a cycle: unrelated. And
     * 200 Hz a whole one again. (The tones are whole hundreds of hertz so
     * that the test's 10 ms blocks hold whole cycles.) */
    fade_envelope(21u, 400, 60, 1000.0, n, g_e1r, g_e1i);
    fade_envelope(21u, 400, 60, 1100.0, n, g_e2r, g_e2i);
    CHECK(env_correlation(g_e1r, g_e1i, g_e2r, g_e2i, n, 0) < 0.08);
    fade_envelope(21u, 400, 60, 1200.0, n, g_e2r, g_e2i);
    CHECK(env_correlation(g_e1r, g_e1i, g_e2r, g_e2i, n, 0) > 0.99);

    /* Still unit power. */
    double mean = 0;
    for (size_t i = 0; i < n; i++) mean += g_e1r[i] * g_e1r[i] + g_e1i[i] * g_e1i[i];
    CHECK_RANGE(10 * log10(mean / (double)n), -0.5, 0.5);
}

/* The fading is a function of seed and time: a stage put anywhere in it
 * agrees, sample for sample, with one that ran there. */
static void test_fade_seek(void)
{
    enum { LEN = 60000, WARM = 400 };
    static int16_t in[LEN], a[LEN], b[LEN];
    dsp_fade_t fa, fb;

    sine(in, LEN, 1500.0, FADE_FS, 9000.0);
    dsp_fade_init(&fa, 5u);
    dsp_fade_set(&fa, 1000, 48, 0); /* 10 Hz, 4 ms */
    dsp_fade_process(&fa, in, a, LEN);

    /* Joining late: the same audio from 50000 - WARM on, placed in time. */
    dsp_fade_init(&fb, 99u);
    dsp_fade_set(&fb, 1000, 48, 0);
    dsp_fade_seek(&fb, 5u, 50000 - WARM);
    dsp_fade_process(&fb, in + 50000 - WARM, b, WARM + 10000);
    CHECK(memcmp(a + 50000, b + WARM, 10000 * sizeof(int16_t)) == 0);
    CHECK(dsp_fade_time(&fb) == 60000);

    /* A second apart in time is another stretch of the fading. */
    dsp_fade_seek(&fb, 5u, 50000 - WARM + 12000);
    dsp_fade_process(&fb, in + 50000 - WARM, b, WARM + 10000);
    CHECK(memcmp(a + 50000, b + WARM, 10000 * sizeof(int16_t)) != 0);

    /* Stepping sample by sample and jumping arrive at the same place: at
     * every spread, far into the fading, and across 2^32 samples. */
    static const uint32_t spreads[] = {1, 10, 137, 400, 3000};
    static const uint64_t times[] = {0, 1, 12345, 0xFFFFFFF0ull, 0x100000010ull, 0xFFFFFFFFF0ull};
    int differ = 0;
    for (size_t sp = 0; sp < sizeof spreads / sizeof spreads[0]; sp++) {
        for (size_t ti = 0; ti < sizeof times / sizeof times[0]; ti++) {
            dsp_fade_init(&fa, 7u);
            dsp_fade_init(&fb, 7u);
            dsp_fade_set(&fa, spreads[sp], 0, 0);
            dsp_fade_set(&fb, spreads[sp], 0, 0);
            dsp_fade_seek(&fa, 7u, times[ti]);
            dsp_fade_process(&fa, in, a, 3000);
            dsp_fade_seek(&fb, 7u, times[ti] + 3000);
            differ += fa.k != fb.k || fa.frac != fb.frac || dsp_fade_time(&fa) != dsp_fade_time(&fb);
            differ += memcmp(fa.g0, fb.g0, sizeof fa.g0) != 0 || memcmp(fa.g1, fb.g1, sizeof fa.g1) != 0;
            differ += memcmp(fa.noise, fb.noise, sizeof fa.noise) != 0;
        }
    }
    CHECK(differ == 0);

    /* Out-of-range settings are clamped. */
    dsp_fade_set(&fa, 99999, 5000, 0);
    CHECK(fa.spread_chz == DSP_FADE_SPREAD_MAX_CHZ && fa.path_delay == DSP_FADE_PATH_MAX - 1);
    /* The fastest fading keeps its level too, and does not overflow on a
     * loud signal: it saturates. */
    sine(in, LEN, 1500.0, FADE_FS, 32000.0);
    dsp_fade_process(&fa, in, a, LEN);
    int peak = 0;
    for (int i = 0; i < LEN; i++) peak = abs(a[i]) > peak ? abs(a[i]) : peak;
    CHECK(peak == 32767 || peak == 32768);
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
    test_fade_passthrough();
    test_fade_shift();
    test_fade_statistics();
    test_fade_delay();
    test_fade_seek();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
