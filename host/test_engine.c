/* Unit tests for audio_engine. Signals are synthesised; no files, no hardware. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS    48000
#define BLOCK 480 /* what the USB audio component fetches: 10 ms */

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

static void sine(int16_t *out, size_t n, double freq, double amp)
{
    for (size_t i = 0; i < n; i++) out[i] = (int16_t)lround(amp * sin(2 * M_PI * freq * (double)i / FS));
}

/* Amplitude of the component at freq. n must hold a whole number of cycles. */
static double tone_amp(const int16_t *x, size_t n, double freq)
{
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++) {
        double ph = 2 * M_PI * freq * (double)i / FS;
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

static double dbfs(double rms_value)
{
    return 20 * log10(rms_value / 32768.0);
}

static bool all_zero(const int16_t *x, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (x[i]) return false;
    return true;
}

/* Run n samples through the engine in blocks. in may be NULL. */
static void run(ae_t *e, const int16_t *in, int16_t *out, size_t n, size_t block)
{
    for (size_t i = 0; i < n; i += block) {
        size_t m = n - i < block ? n - i : block;
        ae_rx_process(e, in ? in + i : NULL, out + i, m);
    }
}

static void configure(ae_t *e, void (*edit)(ae_config_t *))
{
    ae_config_t cfg;
    ae_get_config(e, &cfg);
    edit(&cfg);
    ae_configure(e, &cfg);
}

enum { N = 4 * FS };       /* long enough to measure noise to a fraction of a dB */
enum { SETTLE = FS / 10 }; /* measurements skip the first 100 ms */
static int16_t g_in[N], g_out[N], g_out2[N];

/* What came out, measured after the settling time. */
typedef struct {
    double tone_rms, noise_rms, snr_db;
} measured_t;

static float g_snr, g_bw, g_atten;
static void edit_snr(ae_config_t *c)
{
    c->noise_on = true;
    c->snr_db = g_snr;
    c->ref_bw_hz = g_bw;
    c->atten_db = g_atten;
}

/* Run a tone of the given amplitude through an engine set to g_snr, g_bw and
 * g_atten, and measure signal and noise separately. A second engine with the
 * same seed and no source produces the identical noise, so subtracting its
 * output leaves the scaled signal alone and the measurement has no scatter. */
static measured_t run_snr_case(uint32_t seed, double freq, double amp)
{
    ae_t with, without;
    ae_init(&with, seed);
    ae_init(&without, seed);
    configure(&with, edit_snr);
    configure(&without, edit_snr);

    sine(g_in, N, freq, amp);
    run(&with, g_in, g_out, N, BLOCK);
    run(&without, NULL, g_out2, N, BLOCK);

    measured_t m;
    m.noise_rms = rms(g_out2 + SETTLE, N - SETTLE);
    for (size_t i = 0; i < N; i++) g_out2[i] = (int16_t)(g_out[i] - g_out2[i]);
    m.tone_rms = tone_amp(g_out2 + SETTLE, N - SETTLE, freq) / sqrt(2); /* a whole number of cycles */
    m.snr_db = 20 * log10(m.tone_rms / (m.noise_rms * sqrt(g_bw / (FS / 2.0))));
    return m;
}

static void test_defaults_pass_through(void)
{
    ae_t e;
    ae_status_t st;
    ae_init(&e, 1);
    sine(g_in, N, 1500, 12000);

    run(&e, g_in, g_out, N, BLOCK);
    CHECK(memcmp(g_in, g_out, sizeof g_in) == 0);

    memcpy(g_out, g_in, sizeof g_in); /* in place */
    run(&e, g_out, g_out, N, BLOCK);
    CHECK(memcmp(g_in, g_out, sizeof g_in) == 0);

    ae_set_ptt(&e, true); /* the monitor route ignores PTT */
    run(&e, g_in, g_out, N, BLOCK);
    CHECK(memcmp(g_in, g_out, sizeof g_in) == 0);

    ae_get_status(&e, &st);
    CHECK_RANGE(st.gain_db, -0.001, 0.001);
    CHECK(!st.gain_limited && !st.rx_muted);
    CHECK_RANGE(st.signal_dbfs, dbfs(12000 / sqrt(2)) - 0.05, dbfs(12000 / sqrt(2)) + 0.05);

    run(&e, NULL, g_out, BLOCK, BLOCK); /* no source, no noise: silence */
    CHECK(all_zero(g_out, BLOCK));
}

static void edit_atten_6(ae_config_t *c) { c->atten_db = 6.0206f; }

static void test_attenuation(void)
{
    ae_t e;
    ae_init(&e, 1);
    configure(&e, edit_atten_6);
    sine(g_in, N, 1500, 12000);
    run(&e, g_in, g_out, N, BLOCK);
    CHECK_RANGE(tone_amp(g_out, N, 1500), 6000 - 2, 6000 + 2);
    CHECK_RANGE(rms(g_out, N), 6000 / sqrt(2) - 2, 6000 / sqrt(2) + 2); /* nothing but the tone */
}

static void edit_noise_on(ae_config_t *c) { c->noise_on = true; }
static void edit_floor_30(ae_config_t *c) { c->noise_floor_dbfs = -30.0f; }

static void test_noise_floor(void)
{
    ae_t e;
    ae_init(&e, 7);
    configure(&e, edit_noise_on);
    run(&e, NULL, g_out, N, BLOCK);
    CHECK_RANGE(dbfs(rms(g_out, N)), -40.15, -39.85);

    configure(&e, edit_floor_30);
    run(&e, NULL, g_out, N, BLOCK);
    CHECK_RANGE(dbfs(rms(g_out, N)), -30.15, -29.85);
}

/* The SNR comes out as set whatever the drive level, with the floor fixed. */
static void test_snr(void)
{
    static const float snrs[] = {-30, -20, -10, 0, 10, 25};
    static const double amps[] = {20000, 1000};
    static const float bws[] = {2500, 3000};

    for (size_t b = 0; b < 2; b++) {
        for (size_t s = 0; s < 6; s++) {
            for (size_t a = 0; a < 2; a++) {
                g_snr = snrs[s];
                g_bw = bws[b];
                g_atten = 0;
                measured_t m = run_snr_case(100 + (uint32_t)(s * 2 + a), 1500, amps[a]);
                CHECK_RANGE(m.snr_db, snrs[s] - 0.1, snrs[s] + 0.1);
                CHECK_RANGE(dbfs(m.noise_rms), -40.1, -39.9);
            }
        }
    }

    /* A tone that does not fit the 1 ms measuring pieces is placed as well. */
    g_snr = -20;
    g_bw = 2500;
    measured_t m = run_snr_case(55, 1234, 15000);
    CHECK_RANGE(m.snr_db, -20.1, -19.9);
}

/* Above what the floor allows, the signal stops at -12 dBFS and the floor drops. */
static void test_snr_cap(void)
{
    g_snr = 45;
    g_bw = 2500;
    g_atten = 0;
    measured_t m = run_snr_case(3, 1500, 3000);
    CHECK_RANGE(dbfs(m.tone_rms), AE_SIGNAL_MAX_DBFS - 0.05, AE_SIGNAL_MAX_DBFS + 0.05);
    CHECK_RANGE(m.snr_db, 44.9, 45.1);
    CHECK_RANGE(dbfs(m.noise_rms), -47.3, -47.1); /* 7.2 dB below the -40 dBFS setting */
}

/* Attenuation comes on top: it lowers the signal, and so the SNR, by as much. */
static void test_atten_with_noise(void)
{
    g_snr = 10;
    g_bw = 2500;
    g_atten = 6;
    measured_t m = run_snr_case(4, 1500, 10000);
    CHECK_RANGE(m.snr_db, 3.9, 4.1);
    CHECK_RANGE(dbfs(m.noise_rms), -40.1, -39.9);
}

static void test_gain_limit(void)
{
    ae_t e;
    ae_status_t st;
    ae_init(&e, 5);
    g_snr = 30;
    g_bw = 2500;
    g_atten = 0;
    configure(&e, edit_snr);

    /* -53 dBFS in, -20 dBFS wanted: 33.5 dB is more than the engine gives. */
    sine(g_in, N, 1500, 100);
    run(&e, g_in, g_out, N, BLOCK);
    ae_get_status(&e, &st);
    CHECK(st.gain_limited);
    CHECK_RANGE(st.gain_db, AE_GAIN_MAX_DB - 0.05, AE_GAIN_MAX_DB + 0.05);
    CHECK_RANGE(tone_amp(g_out + SETTLE, N - SETTLE, 1500), 3162 * 0.97, 3162 * 1.03);

    /* A normal level is within reach. */
    ae_init(&e, 5);
    configure(&e, edit_snr);
    sine(g_in, N, 1500, 10000);
    run(&e, g_in, g_out, N, BLOCK);
    ae_get_status(&e, &st);
    CHECK(!st.gain_limited);
    CHECK_RANGE(st.gain_db, -19.82 - dbfs(10000 / sqrt(2)) - 0.1, -19.82 - dbfs(10000 / sqrt(2)) + 0.1);
}

static double level_now(ae_t *e)
{
    ae_status_t st;
    ae_get_status(e, &st);
    return st.signal_dbfs;
}

static void test_level_tracking(void)
{
    ae_t e;
    const double loud = dbfs(8000 / sqrt(2)), soft = dbfs(800 / sqrt(2)), half = dbfs(4000 / sqrt(2));

    /* Silence, then a tone that starts part-way through a block. */
    ae_init(&e, 1);
    memset(g_in, 0, sizeof g_in);
    size_t start = 10 * BLOCK + 250;
    sine(g_in + start, FS, 1234, 8000);
    run(&e, g_in, g_out, start - 250, BLOCK);
    CHECK(level_now(&e) == DSP_SILENCE_DBFS);
    run(&e, g_in + start - 250, g_out, BLOCK, BLOCK); /* the block the tone starts in */
    CHECK_RANGE(level_now(&e), loud - 1.0, loud + 1.0);
    run(&e, g_in + start - 250 + BLOCK, g_out, 30 * BLOCK, BLOCK);
    CHECK_RANGE(level_now(&e), loud - 0.2, loud + 0.2);

    /* After a gap, a quieter transmission is measured afresh. */
    memset(g_in, 0, sizeof g_in);
    run(&e, g_in, g_out, 30 * BLOCK, BLOCK);
    CHECK_RANGE(level_now(&e), loud - 0.2, loud + 0.2); /* held through the gap */
    sine(g_in, FS, 1234, 800);
    run(&e, g_in, g_out, 2 * BLOCK, BLOCK);
    CHECK_RANGE(level_now(&e), soft - 1.0, soft + 1.0);
    run(&e, g_in + 2 * BLOCK, g_out, 30 * BLOCK, BLOCK);
    CHECK_RANGE(level_now(&e), soft - 0.2, soft + 0.2);

    /* A level change with no gap is followed within a few seconds. */
    ae_init(&e, 1);
    sine(g_in, N, 1500, 8000);
    run(&e, g_in, g_out, 2 * FS, BLOCK);
    CHECK_RANGE(level_now(&e), loud - 0.05, loud + 0.05);
    sine(g_in, N, 1500, 4000);
    run(&e, g_in, g_out, N, BLOCK);
    run(&e, g_in, g_out, FS, BLOCK);
    CHECK_RANGE(level_now(&e), half - 0.2, half + 0.2);

    /* The estimate does not depend on how the audio is cut into blocks. */
    ae_t a, b;
    ae_init(&a, 1);
    ae_init(&b, 1);
    memset(g_in, 0, sizeof g_in);
    sine(g_in + 777, FS, 1234, 5000);
    run(&a, g_in, g_out, 2 * FS, BLOCK);
    run(&b, g_in, g_out, 2 * FS, 37);
    CHECK(level_now(&a) == level_now(&b));
}

static uint32_t g_turnaround;
static void edit_radio_noise(ae_config_t *c)
{
    c->route = AE_ROUTE_RADIO;
    c->noise_on = true;
    c->turnaround_ms = g_turnaround;
}
static void edit_radio_clean(ae_config_t *c)
{
    c->route = AE_ROUTE_RADIO;
    c->noise_on = false;
    c->turnaround_ms = g_turnaround;
}
static void edit_monitor(ae_config_t *c) { c->route = AE_ROUTE_MONITOR; }

static void test_radio_route(void)
{
    enum { ODD = 441, TURN = 2400 }; /* 50 ms */
    ae_t e;
    ae_status_t st;
    ae_init(&e, 9);
    g_turnaround = 50;
    configure(&e, edit_radio_noise);

    /* Not keyed: the noise floor. */
    run(&e, NULL, g_out, FS, ODD);
    CHECK_RANGE(dbfs(rms(g_out, FS)), -40.5, -39.5);

    /* Keyed: silence. */
    ae_set_ptt(&e, true);
    run(&e, NULL, g_out, FS, ODD);
    CHECK(all_zero(g_out, FS));
    ae_get_status(&e, &st);
    CHECK(st.rx_muted);

    /* Unkeyed: silence for the turnaround time to the sample, then noise. */
    ae_set_ptt(&e, false);
    run(&e, NULL, g_out, FS, ODD);
    CHECK(all_zero(g_out, TURN));
    CHECK_RANGE(dbfs(rms(g_out + TURN, ODD)), -42, -38);
    CHECK_RANGE(dbfs(rms(g_out + TURN, FS - TURN)), -40.5, -39.5);
    ae_get_status(&e, &st);
    CHECK(!st.rx_muted);

    /* Keying again during the turnaround starts it over. */
    ae_set_ptt(&e, true);
    run(&e, NULL, g_out, ODD, ODD);
    ae_set_ptt(&e, false);
    run(&e, NULL, g_out, 2 * ODD, ODD); /* 882 samples into the turnaround */
    ae_set_ptt(&e, true);
    run(&e, NULL, g_out, ODD, ODD);
    ae_set_ptt(&e, false);
    run(&e, NULL, g_out, FS, ODD);
    CHECK(all_zero(g_out, TURN));
    CHECK(!all_zero(g_out + TURN, ODD));

    /* With a source and no noise, the source comes through after the turnaround. */
    configure(&e, edit_radio_clean);
    sine(g_in, FS, 1500, 9000);
    ae_set_ptt(&e, true);
    run(&e, g_in, g_out, ODD, ODD);
    CHECK(all_zero(g_out, ODD));
    ae_set_ptt(&e, false);
    memcpy(g_out, g_in, FS * sizeof(int16_t)); /* in place */
    run(&e, g_out, g_out, FS, ODD);
    CHECK(all_zero(g_out, TURN));
    CHECK(memcmp(g_out + TURN, g_in + TURN, (FS - TURN) * sizeof(int16_t)) == 0);

    /* No turnaround time: receive opens with the first unkeyed block. */
    g_turnaround = 0;
    configure(&e, edit_radio_clean);
    ae_set_ptt(&e, true);
    run(&e, g_in, g_out, ODD, ODD);
    ae_set_ptt(&e, false);
    run(&e, g_in, g_out, ODD, ODD);
    CHECK(memcmp(g_out, g_in, ODD * sizeof(int16_t)) == 0);

    /* Leaving the radio route while keyed opens receive at once. */
    g_turnaround = 50;
    configure(&e, edit_radio_clean);
    ae_set_ptt(&e, true);
    run(&e, g_in, g_out, ODD, ODD);
    CHECK(all_zero(g_out, ODD));
    configure(&e, edit_monitor);
    run(&e, g_in, g_out, ODD, ODD);
    CHECK(memcmp(g_out, g_in, ODD * sizeof(int16_t)) == 0);
}

/* ---- the whole radio: ae_process() and the parrot --------------------------- */

enum { PN = 10 * FS };
static int16_t p_tx[PN], p_out[PN];
static int16_t p_store[12 * AE_PARROT_FS_HZ]; /* 12 s */

static uint32_t g_delay, g_period;
static void edit_parrot(ae_config_t *c)
{
    c->route = AE_ROUTE_PARROT;
    c->noise_on = false;
    c->atten_db = 0;
    c->turnaround_ms = 30;
    c->parrot_delay_ms = g_delay;
    c->parrot_period_ms = g_period;
}

static void edit_parrot_noise(ae_config_t *c)
{
    edit_parrot(c);
    c->noise_on = true;
    c->snr_db = 10;
    c->ref_bw_hz = 2500;
    c->noise_floor_dbfs = -40;
}

static void edit_radio(ae_config_t *c)
{
    c->route = AE_ROUTE_RADIO;
}

/* Samples from..to of p_tx through the radio into p_out, keyed from key_on
 * until key_off. PTT is set before each block, as a CAT command would. */
static void radio(ae_t *e, size_t from, size_t to, size_t block, size_t key_on, size_t key_off)
{
    for (size_t i = from; i < to; i += block) {
        size_t m = to - i < block ? to - i : block;
        ae_set_ptt(e, i >= key_on && i < key_off);
        ae_process(e, p_tx + i, p_out + i, m);
    }
}

/* A tone in p_tx from..to, silence everywhere else. */
static void tx_tone(size_t from, size_t to, double freq, double amp)
{
    memset(p_tx, 0, sizeof p_tx);
    sine(p_tx + from, to - from, freq, amp);
}

static int max_diff(const int16_t *a, const int16_t *b, size_t n)
{
    int worst = 0;
    for (size_t i = 0; i < n; i++) {
        int d = abs(a[i] - b[i]);
        if (d > worst) worst = d;
    }
    return worst;
}

static ae_parrot_status_t parrot_status(ae_t *e)
{
    ae_parrot_status_t st;
    ae_get_parrot_status(e, &st);
    return st;
}

static void parrot_engine(ae_t *e, uint32_t delay_ms, uint32_t period_ms)
{
    ae_init(e, 21);
    ae_set_parrot_buffer(e, p_store, sizeof p_store / sizeof p_store[0]);
    g_delay = delay_ms;
    g_period = period_ms;
    configure(e, edit_parrot);
}

/* How far a replay may differ from what was sent: the resampler's ripple
 * (0.01 dB) and rounding, a few counts on a tone of 8000. */
#define REPLAY_TOL 24

static void test_process_routes(void)
{
    ae_t e;
    ae_init(&e, 20);
    tx_tone(0, PN, 1500, 8000);

    /* Monitor: transmit audio comes straight back, keyed or not, in place too. */
    radio(&e, 0, FS, BLOCK, FS / 2, FS);
    CHECK(memcmp(p_out, p_tx, FS * sizeof(int16_t)) == 0);
    memcpy(p_out, p_tx, FS * sizeof(int16_t));
    for (size_t i = 0; i < FS; i += BLOCK) ae_process(&e, p_out + i, p_out + i, BLOCK);
    CHECK(memcmp(p_out, p_tx, FS * sizeof(int16_t)) == 0);

    /* Radio: the receiver hears nothing of the transmit audio. */
    configure(&e, edit_radio);
    radio(&e, 0, 2 * FS, BLOCK, FS / 2, FS);
    CHECK(all_zero(p_out, 2 * FS));

    /* Parrot without a buffer: the same, and it says it has no room. */
    g_delay = 100;
    g_period = 0;
    configure(&e, edit_parrot);
    radio(&e, 0, 2 * FS, BLOCK, FS / 2, FS);
    CHECK(all_zero(p_out, 2 * FS));
    CHECK(parrot_status(&e).capacity_ms == 0);
    CHECK(parrot_status(&e).state == AE_PARROT_IDLE);
    CHECK(!ae_parrot_replay(&e));
}

static void test_parrot_replay(void)
{
    enum { ON = FS / 2, OFF = ON + 2 * FS, PLAY = OFF + FS / 2 }; /* keyed 2 s, replay 500 ms after */
    ae_t e;
    parrot_engine(&e, 500, 0);
    CHECK(parrot_status(&e).capacity_ms == 12000);
    CHECK(parrot_status(&e).recorded_ms == 0);

    tx_tone(ON, OFF, 1500, 8000);
    radio(&e, 0, ON, BLOCK, ON, OFF);
    CHECK(parrot_status(&e).state == AE_PARROT_IDLE);
    radio(&e, ON, ON + FS, BLOCK, ON, OFF);
    CHECK(parrot_status(&e).state == AE_PARROT_RECORDING);
    CHECK(parrot_status(&e).recorded_ms == 1000);
    radio(&e, ON + FS, OFF + BLOCK, BLOCK, ON, OFF);
    CHECK(parrot_status(&e).state == AE_PARROT_RECORDING); /* the tail after unkeying */
    radio(&e, OFF + BLOCK, OFF + FS / 4, BLOCK, ON, OFF);
    CHECK(parrot_status(&e).state == AE_PARROT_WAITING);
    CHECK(parrot_status(&e).recorded_ms == 2000 + AE_PARROT_TAIL_MS);
    CHECK(!parrot_status(&e).truncated);
    radio(&e, OFF + FS / 4, PLAY + FS, BLOCK, ON, OFF);
    CHECK(parrot_status(&e).state == AE_PARROT_PLAYING);
    radio(&e, PLAY + FS, PN, BLOCK, ON, OFF);
    CHECK(parrot_status(&e).state == AE_PARROT_IDLE);
    CHECK(parrot_status(&e).recorded_ms == 2000 + AE_PARROT_TAIL_MS); /* still there */

    /* Nothing is heard until the replay, which is the transmission again,
     * sample for sample, and then nothing. */
    CHECK(all_zero(p_out, PLAY));
    CHECK_RANGE(max_diff(p_out + PLAY + 240, p_tx + ON + 240, 2 * FS - 480), 0, REPLAY_TOL);
    CHECK_RANGE(tone_amp(p_out + PLAY + FS / 2, FS, 1500), 7990, 8010);
    CHECK(all_zero(p_out + PLAY + 2 * FS + 240, PN - (PLAY + 2 * FS + 240)));

    /* One sample out of step would show plainly. */
    CHECK(max_diff(p_out + PLAY + 241, p_tx + ON + 240, 2 * FS - 480) > 1000);
}

/* Block sizes that divide nothing, in place, and no audio at all (NULL)
 * outside the transmission. */
static void test_parrot_odd_blocks(void)
{
    enum { ODD = 441, ON = 100 * ODD, OFF = 300 * ODD, PLAY = OFF + FS / 5 };
    ae_t e;
    parrot_engine(&e, 200, 0);
    tx_tone(ON + 1000, OFF - 1000, 1000, 8000);

    static int16_t ref[PN];
    memcpy(ref, p_tx, sizeof ref);
    memcpy(p_out, p_tx, sizeof p_out);
    for (size_t i = 0; i + ODD <= PN; i += ODD) {
        bool keyed = i >= ON && i < OFF;
        ae_set_ptt(&e, keyed);
        if (i >= ON && i < OFF + 10 * ODD) ae_process(&e, p_out + i, p_out + i, ODD);
        else ae_process(&e, NULL, p_out + i, ODD);
    }
    size_t end = PN - PN % ODD;
    CHECK(all_zero(p_out, PLAY));
    CHECK_RANGE(max_diff(p_out + PLAY + 1240, ref + ON + 1240, (OFF - ON) - 2480), 0, REPLAY_TOL);
    CHECK(all_zero(p_out + PLAY + (OFF - ON), end - (PLAY + (OFF - ON))));
}

/* With a period, the replay starts a whole number of periods after key-on,
 * wherever the unkeying fell. */
static void test_parrot_period(void)
{
    enum { ON = FS, OFF = ON + 77 * BLOCK, PERIOD = 3 * FS }; /* keyed 770 ms */
    enum { TONE = ON + FS / 5, TONE_LEN = (OFF - FS / 10) - TONE }; /* the tone starts and ends inside it */
    ae_t e;

    parrot_engine(&e, 500, 3000);
    tx_tone(TONE, TONE + TONE_LEN, 1500, 8000);
    radio(&e, 0, PN, BLOCK, ON, OFF);
    CHECK(all_zero(p_out, PERIOD + TONE - 240));
    CHECK_RANGE(max_diff(p_out + PERIOD + TONE + 240, p_tx + TONE + 240, TONE_LEN - 480), 0, REPLAY_TOL);

    /* A delay that runs past the first period puts it in the second. */
    parrot_engine(&e, 2500, 3000);
    radio(&e, 0, PN, BLOCK, ON, OFF);
    CHECK(all_zero(p_out, 2 * PERIOD + TONE - 240));
    CHECK_RANGE(max_diff(p_out + 2 * PERIOD + TONE + 240, p_tx + TONE + 240, TONE_LEN - 480), 0, REPLAY_TOL);

    /* So does a transmission longer than the period. */
    enum { LONG_OFF = ON + 4 * FS };
    parrot_engine(&e, 100, 3000);
    tx_tone(ON, LONG_OFF, 1500, 8000);
    radio(&e, 0, PN, BLOCK, ON, LONG_OFF);
    CHECK(all_zero(p_out, ON + 2 * PERIOD));
    CHECK_RANGE(max_diff(p_out + ON + 2 * PERIOD + 240, p_tx + ON + 240, PN - (ON + 2 * PERIOD) - 480), 0,
                REPLAY_TOL);
}

/* Keying again replaces the recording, whether the old one was waiting or playing. */
static void test_parrot_rekey(void)
{
    enum { ON1 = FS / 2, OFF1 = ON1 + FS, ON2 = OFF1 + FS, OFF2 = ON2 + FS, PLAY2 = OFF2 + FS / 2 };
    ae_t e;

    /* During the replay of the first (it starts 500 ms after OFF1). */
    parrot_engine(&e, 500, 0);
    tx_tone(ON1, OFF1, 1000, 8000);
    sine(p_tx + ON2, OFF2 - ON2, 2000, 4000);
    radio(&e, 0, ON2, BLOCK, ON1, OFF1);
    CHECK(parrot_status(&e).state == AE_PARROT_PLAYING);
    CHECK_RANGE(tone_amp(p_out + OFF1 + FS / 2 + 4800, FS / 4, 1000), 7900, 8100);
    radio(&e, ON2, PN, BLOCK, ON2, OFF2);
    CHECK(all_zero(p_out + ON2, PLAY2 - ON2));
    CHECK_RANGE(max_diff(p_out + PLAY2 + 240, p_tx + ON2 + 240, FS - 480), 0, REPLAY_TOL);
    CHECK_RANGE(tone_amp(p_out + PLAY2, FS, 1000), 0, 5); /* nothing of the first */
    CHECK(parrot_status(&e).recorded_ms == 1000 + AE_PARROT_TAIL_MS);

    /* While the first is still waiting (delay 3 s). */
    parrot_engine(&e, 3000, 0);
    radio(&e, 0, ON2, BLOCK, ON1, OFF1);
    CHECK(parrot_status(&e).state == AE_PARROT_WAITING);
    radio(&e, ON2, PN, BLOCK, ON2, OFF2);
    CHECK(all_zero(p_out, OFF2 + 3 * FS));
    CHECK_RANGE(max_diff(p_out + OFF2 + 3 * FS + 240, p_tx + ON2 + 240, FS - 480), 0, REPLAY_TOL);

    /* Keying again inside the tail starts over too. */
    enum { ON3 = OFF1 + BLOCK, OFF3 = ON3 + FS };
    parrot_engine(&e, 500, 0);
    tx_tone(ON1, OFF1, 1000, 8000);
    sine(p_tx + ON3, OFF3 - ON3, 2000, 4000);
    radio(&e, 0, ON3, BLOCK, ON1, OFF1);
    radio(&e, ON3, PN, BLOCK, ON3, OFF3);
    CHECK(all_zero(p_out, OFF3 + FS / 2));
    CHECK_RANGE(max_diff(p_out + OFF3 + FS / 2 + 240, p_tx + ON3 + 240, FS - 480), 0, REPLAY_TOL);
}

static void test_parrot_full_buffer(void)
{
    enum { ON = FS / 2, OFF = ON + 3 * FS, PLAY = OFF + FS / 2 };
    ae_t e;
    parrot_engine(&e, 500, 0);
    ae_set_parrot_buffer(&e, p_store, 2 * AE_PARROT_FS_HZ); /* room for 2 s */
    tx_tone(ON, OFF, 1500, 8000);
    radio(&e, 0, PN, BLOCK, ON, OFF);

    CHECK(parrot_status(&e).truncated);
    CHECK(parrot_status(&e).recorded_ms == 2000);
    CHECK(parrot_status(&e).capacity_ms == 2000);
    /* The first 2 s come back; the rest was never stored. */
    CHECK_RANGE(max_diff(p_out + PLAY + 240, p_tx + ON + 240, 2 * FS - 480), 0, REPLAY_TOL);
    CHECK(all_zero(p_out + PLAY + 2 * FS + 240, PN - (PLAY + 2 * FS + 240)));

    /* The next transmission fits and clears the flag. */
    tx_tone(ON, ON + FS, 1500, 8000);
    radio(&e, 0, PN, BLOCK, ON, ON + FS);
    CHECK(!parrot_status(&e).truncated);
    CHECK(parrot_status(&e).recorded_ms == 1000 + AE_PARROT_TAIL_MS);
}

static void test_parrot_replay_again(void)
{
    enum { ON = FS / 2, OFF = ON + FS, AGAIN = 5 * FS };
    ae_t e;
    parrot_engine(&e, 500, 0);
    CHECK(!ae_parrot_replay(&e)); /* nothing recorded */

    tx_tone(ON, OFF, 1500, 8000);
    radio(&e, 0, ON + FS / 2, BLOCK, ON, OFF);
    CHECK(!ae_parrot_replay(&e)); /* transmitting */
    radio(&e, ON + FS / 2, AGAIN, BLOCK, ON, OFF);
    CHECK(parrot_status(&e).state == AE_PARROT_IDLE);

    /* On request it plays once more, from the next block. */
    CHECK(ae_parrot_replay(&e));
    radio(&e, AGAIN, AGAIN + 3 * FS, BLOCK, ON, OFF);
    CHECK_RANGE(max_diff(p_out + AGAIN + 240, p_tx + ON + 240, FS - 480), 0, REPLAY_TOL);
    CHECK(all_zero(p_out + AGAIN + FS + 240, 2 * FS - 240));
    CHECK(parrot_status(&e).state == AE_PARROT_IDLE);

    /* Asked during a replay, it starts over. */
    CHECK(ae_parrot_replay(&e));
    radio(&e, 0, FS / 4, BLOCK, PN, PN);
    CHECK(parrot_status(&e).state == AE_PARROT_PLAYING);
    CHECK(ae_parrot_replay(&e));
    radio(&e, FS / 4, 2 * FS, BLOCK, PN, PN);
    CHECK_RANGE(max_diff(p_out + FS / 4 + 240, p_tx + ON + 240, FS - 480), 0, REPLAY_TOL);

    /* Leaving the route stops a replay and keeps the recording. */
    CHECK(ae_parrot_replay(&e));
    radio(&e, 0, FS / 4, BLOCK, PN, PN);
    configure(&e, edit_radio);
    radio(&e, FS / 4, FS, BLOCK, PN, PN);
    CHECK(all_zero(p_out + FS / 4, FS - FS / 4));
    CHECK(parrot_status(&e).state == AE_PARROT_IDLE);
    CHECK(!ae_parrot_replay(&e)); /* not the parrot route */
    configure(&e, edit_parrot);
    CHECK(ae_parrot_replay(&e));
    radio(&e, FS, 3 * FS, BLOCK, PN, PN);
    CHECK_RANGE(max_diff(p_out + FS + 240, p_tx + ON + 240, FS - 480), 0, REPLAY_TOL);
}

/* The replay is the receive source, so the channel applies to it: with
 * noise on it sits at the configured SNR whatever level it was sent at. */
static void test_parrot_through_channel(void)
{
    enum { ON = FS / 2, OFF = ON + 2 * FS, PLAY = OFF + FS / 2 };
    static const double amps[] = {300, 8000, 30000};

    for (size_t k = 0; k < sizeof amps / sizeof amps[0]; k++) {
        ae_t e;
        parrot_engine(&e, 500, 0);
        configure(&e, edit_parrot_noise);
        tx_tone(ON, OFF, 1500, amps[k]);
        radio(&e, 0, PN, BLOCK, ON, OFF);

        /* +10 dB in 2500 Hz over a -40 dBFS floor: the tone at -39.8 dBFS. */
        CHECK(all_zero(p_out + ON, OFF - ON));
        CHECK_RANGE(dbfs(tone_amp(p_out + PLAY + FS / 2, FS, 1500) / sqrt(2)), -40.3, -39.3);
        CHECK_RANGE(dbfs(rms(p_out + PLAY + 3 * FS, FS)), -40.5, -39.5); /* the floor alone, after */
    }
}

/* The delay is never shorter than the tail the recording needs. */
static void test_parrot_no_delay(void)
{
    enum { ON = FS / 2, OFF = ON + FS, TAIL = AE_PARROT_TAIL_MS * (FS / 1000) };
    ae_t e;
    parrot_engine(&e, 0, 0);
    tx_tone(ON, OFF, 1500, 8000);
    radio(&e, 0, PN, 100, ON, OFF);
    CHECK(all_zero(p_out, OFF + TAIL));
    CHECK_RANGE(max_diff(p_out + OFF + TAIL + 240, p_tx + ON + 240, FS - 480), 0, REPLAY_TOL);
}

/* ---- PTT sources: CAT, a control line, VOX ---------------------------------- */

static ae_ptt_t ptt_now(ae_t *e)
{
    ae_ptt_t p;
    ae_get_ptt(e, &p);
    return p;
}

static ae_line_t g_line;
static void edit_line(ae_config_t *c)
{
    c->route = AE_ROUTE_RADIO;
    c->turnaround_ms = 0;
    c->ptt_line = g_line;
}

/* Whether the radio route lets a block of the source through. */
static bool rx_open(ae_t *e)
{
    sine(g_in, BLOCK, 1500, 8000);
    ae_rx_process(e, g_in, g_out, BLOCK);
    return memcmp(g_in, g_out, BLOCK * sizeof(int16_t)) == 0;
}

static void test_line_ptt(void)
{
    ae_t e;
    ae_init(&e, 30);

    /* Off, as at power-up: a host that raises both lines keys nothing. */
    g_line = AE_LINE_OFF;
    configure(&e, edit_line);
    ae_set_lines(&e, true, true);
    CHECK(rx_open(&e));
    CHECK(!ptt_now(&e).line && !ptt_now(&e).cat && !ptt_now(&e).vox);

    /* RTS selected: RTS keys, DTR does not. */
    g_line = AE_LINE_RTS;
    configure(&e, edit_line);
    CHECK(ptt_now(&e).line);
    CHECK(!rx_open(&e));
    ae_set_lines(&e, true, false);
    CHECK(!ptt_now(&e).line);
    CHECK(rx_open(&e));
    ae_set_lines(&e, false, true);
    CHECK(!rx_open(&e));

    /* DTR selected: the other way round. */
    g_line = AE_LINE_DTR;
    configure(&e, edit_line);
    CHECK(!ptt_now(&e).line);
    CHECK(rx_open(&e));
    ae_set_lines(&e, true, false);
    CHECK(ptt_now(&e).line);
    CHECK(!rx_open(&e));

    /* CAT and the line each key on their own; it takes both off to unkey. */
    ae_set_ptt(&e, true);
    ae_set_lines(&e, false, false);
    CHECK(ptt_now(&e).cat && !ptt_now(&e).line);
    CHECK(!rx_open(&e));
    ae_set_ptt(&e, false);
    CHECK(rx_open(&e));
}

static float g_vox_dbfs;
static uint32_t g_vox_hang;
static void edit_vox(ae_config_t *c)
{
    c->vox_on = true;
    c->vox_threshold_dbfs = g_vox_dbfs;
    c->vox_hang_ms = g_vox_hang;
}
static void edit_vox_off(ae_config_t *c) { c->vox_on = false; }
static void edit_some_snr(ae_config_t *c) { c->snr_db = 3; }

/* Run p_tx from..to with no CAT keying; returns whether the VOX was open
 * after every block (all) or after none (none). */
static void vox_span(ae_t *e, size_t from, size_t to, bool *all, bool *none)
{
    *all = true;
    *none = true;
    for (size_t i = from; i < to; i += BLOCK) {
        ae_process(e, p_tx + i, p_out + i, BLOCK);
        if (ptt_now(e).vox) *none = false;
        else *all = false;
    }
}

static void test_vox(void)
{
    enum { ON = FS / 2, OFF = ON + FS, HANG = 100 * (FS / 1000) };
    ae_t e;
    bool all, none;

    /* Off, as at power-up: audio keys nothing. */
    parrot_engine(&e, 500, 0);
    tx_tone(ON, OFF, 1500, 8000);
    vox_span(&e, 0, 2 * FS, &all, &none);
    CHECK(none);
    CHECK(parrot_status(&e).recorded_ms == 0);

    /* On: open from the block the audio starts in until the hang time after
     * it stops, and the parrot records and replays just as with CAT. The
     * last loud sample is the one before OFF, so the VOX closes inside the
     * block that ends at OFF + HANG. */
    g_vox_dbfs = -40;
    g_vox_hang = 100;
    parrot_engine(&e, 500, 0);
    configure(&e, edit_vox);
    vox_span(&e, 0, ON, &all, &none);
    CHECK(none);
    vox_span(&e, ON, OFF + HANG - BLOCK, &all, &none);
    CHECK(all);
    CHECK(parrot_status(&e).state == AE_PARROT_RECORDING);
    vox_span(&e, OFF + HANG - BLOCK, PN, &all, &none);
    CHECK(none);
    size_t play = OFF + HANG - BLOCK + FS / 2;
    CHECK(all_zero(p_out, play));
    CHECK_RANGE(max_diff(p_out + play + 240, p_tx + ON + 240, FS - 480), 0, REPLAY_TOL);

    /* With no audio arriving at all (NULL) the hang time still runs out. */
    parrot_engine(&e, 500, 0);
    configure(&e, edit_vox);
    vox_span(&e, 0, OFF, &all, &none);
    CHECK(ptt_now(&e).vox);
    for (size_t i = 0; i < HANG - BLOCK; i += BLOCK) ae_process(&e, NULL, p_out, BLOCK);
    CHECK(ptt_now(&e).vox);
    ae_process(&e, NULL, p_out, BLOCK);
    CHECK(!ptt_now(&e).vox);

    /* A gap shorter than the hang time does not unkey: one transmission. */
    enum { GAP = 50 * (FS / 1000) };
    parrot_engine(&e, 500, 0);
    configure(&e, edit_vox);
    tx_tone(ON, OFF, 1500, 8000);
    memset(p_tx + ON + FS / 2, 0, GAP * sizeof(int16_t));
    vox_span(&e, ON, OFF, &all, &none);
    CHECK(all);
    vox_span(&e, OFF, PN, &all, &none);
    CHECK(parrot_status(&e).recorded_ms == 1000 + 100 - 10 + AE_PARROT_TAIL_MS);

    /* A gap longer than it does: the second burst is a new transmission. */
    parrot_engine(&e, 500, 0);
    configure(&e, edit_vox);
    tx_tone(ON, OFF, 1500, 8000);
    memset(p_tx + ON + FS / 4, 0, (FS / 2) * sizeof(int16_t)); /* 250 ms on, 500 off, 250 on */
    vox_span(&e, 0, PN, &all, &none);
    CHECK(parrot_status(&e).recorded_ms == 250 + 100 - 10 + AE_PARROT_TAIL_MS);

    /* The threshold: audio under it keys nothing until it is lowered. */
    parrot_engine(&e, 500, 0);
    configure(&e, edit_vox);
    tx_tone(0, PN, 1500, 100); /* peaks at -50 dBFS */
    vox_span(&e, 0, FS, &all, &none);
    CHECK(none);
    g_vox_dbfs = -60;
    configure(&e, edit_vox);
    vox_span(&e, FS, 2 * FS, &all, &none);
    CHECK(all);

    /* Changing some other setting mid-transmission does not drop it. */
    configure(&e, edit_some_snr);
    vox_span(&e, 2 * FS, 3 * FS, &all, &none);
    CHECK(all);
    CHECK(parrot_status(&e).recorded_ms == 2000);

    /* Turning the VOX off does, at once. */
    configure(&e, edit_vox_off);
    vox_span(&e, 3 * FS, 4 * FS, &all, &none);
    CHECK(none);

    /* In the monitor route the VOX still says the radio is transmitting,
     * and the audio comes back all the same. */
    ae_init(&e, 31);
    g_vox_dbfs = -40;
    configure(&e, edit_vox);
    tx_tone(0, PN, 1500, 8000);
    vox_span(&e, 0, FS, &all, &none);
    CHECK(all);
    CHECK(memcmp(p_out, p_tx, FS * sizeof(int16_t)) == 0);

    /* CAT and VOX together: either keeps the radio keyed. */
    parrot_engine(&e, 500, 0);
    configure(&e, edit_vox);
    tx_tone(ON, OFF, 1500, 8000);
    ae_set_ptt(&e, true);
    vox_span(&e, 0, OFF, &all, &none);
    ae_set_ptt(&e, false); /* CAT lets go while the VOX is still open */
    vox_span(&e, OFF, OFF + BLOCK, &all, &none);
    CHECK(all);
    CHECK(parrot_status(&e).state == AE_PARROT_RECORDING);
    vox_span(&e, OFF + BLOCK, PN, &all, &none);
    CHECK(parrot_status(&e).recorded_ms == 500 + 1000 + 100 - 10 + AE_PARROT_TAIL_MS); /* from CAT key-on */
}

static void test_config_clamping(void)
{
    ae_t e;
    ae_config_t c;
    ae_init(&e, 1);

    ae_get_config(&e, &c);
    CHECK(c.route == AE_ROUTE_MONITOR && !c.noise_on && c.atten_db == 0.0f);
    CHECK(c.snr_db == 10.0f && c.ref_bw_hz == 2500.0f && c.noise_floor_dbfs == -40.0f && c.turnaround_ms == 30);

    c.snr_db = 200.0f;
    c.ref_bw_hz = 0.0f;
    c.noise_floor_dbfs = 0.0f;
    c.atten_db = -5.0f;
    c.turnaround_ms = 99999;
    ae_configure(&e, &c);
    ae_get_config(&e, &c);
    CHECK(c.snr_db == 80.0f && c.ref_bw_hz == 100.0f && c.noise_floor_dbfs == AE_SIGNAL_MAX_DBFS);
    CHECK(c.atten_db == 0.0f && c.turnaround_ms == 5000);

    c.snr_db = nanf("");
    c.atten_db = nanf("");
    ae_configure(&e, &c);
    ae_get_config(&e, &c);
    CHECK(c.snr_db == -60.0f && c.atten_db == 0.0f);

    c.snr_db = -12.34f; /* kept to a hundredth of a dB */
    ae_configure(&e, &c);
    ae_get_config(&e, &c);
    CHECK_RANGE(c.snr_db, -12.341, -12.339);

    /* The parrot's and the PTT sources' settings. */
    CHECK(c.parrot_delay_ms == 1000 && c.parrot_period_ms == 0);
    CHECK(c.ptt_line == AE_LINE_OFF && !c.vox_on && c.vox_threshold_dbfs == -40.0f && c.vox_hang_ms == 100);
    c.route = (ae_route_t)7;
    c.parrot_delay_ms = 70000;
    c.parrot_period_ms = 400000;
    c.ptt_line = (ae_line_t)9;
    c.vox_threshold_dbfs = 0.0f;
    c.vox_hang_ms = 6000;
    ae_configure(&e, &c);
    ae_get_config(&e, &c);
    CHECK(c.route == AE_ROUTE_MONITOR && c.parrot_delay_ms == 60000 && c.parrot_period_ms == 300000);
    CHECK(c.ptt_line == AE_LINE_OFF && c.vox_threshold_dbfs == -6.0f && c.vox_hang_ms == 5000);
    c.vox_threshold_dbfs = -200.0f;
    ae_configure(&e, &c);
    ae_get_config(&e, &c);
    CHECK(c.vox_threshold_dbfs == -80.0f);
}

/* A change made between blocks applies from the next block. */
static void test_reconfigure_mid_stream(void)
{
    ae_t e;
    ae_init(&e, 1);
    sine(g_in, 4 * BLOCK, 1500, 12000);
    run(&e, g_in, g_out, 2 * BLOCK, BLOCK);
    configure(&e, edit_atten_6);
    run(&e, g_in + 2 * BLOCK, g_out + 2 * BLOCK, 2 * BLOCK, BLOCK);
    CHECK(memcmp(g_in, g_out, 2 * BLOCK * sizeof(int16_t)) == 0);
    CHECK_RANGE(tone_amp(g_out + 2 * BLOCK, 2 * BLOCK, 1500), 6000 - 2, 6000 + 2);
}

static void test_determinism(void)
{
    ae_t a, b, c;
    ae_init(&a, 42);
    ae_init(&b, 42);
    ae_init(&c, 43);
    g_snr = 0;
    g_bw = 2500;
    g_atten = 0;
    configure(&a, edit_snr);
    configure(&b, edit_snr);
    configure(&c, edit_snr);
    sine(g_in, FS, 1500, 5000);
    run(&a, g_in, g_out, FS, BLOCK);
    run(&b, g_in, g_out2, FS, BLOCK);
    CHECK(memcmp(g_out, g_out2, FS * sizeof(int16_t)) == 0);
    run(&c, g_in, g_out2, FS, BLOCK);
    CHECK(memcmp(g_out, g_out2, FS * sizeof(int16_t)) != 0);
}

int main(void)
{
    test_defaults_pass_through();
    test_attenuation();
    test_noise_floor();
    test_snr();
    test_snr_cap();
    test_atten_with_noise();
    test_gain_limit();
    test_level_tracking();
    test_radio_route();
    test_process_routes();
    test_parrot_replay();
    test_parrot_odd_blocks();
    test_parrot_period();
    test_parrot_rekey();
    test_parrot_full_buffer();
    test_parrot_replay_again();
    test_parrot_through_channel();
    test_parrot_no_delay();
    test_line_ptt();
    test_vox();
    test_config_clamping();
    test_reconfigure_mid_stream();
    test_determinism();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
