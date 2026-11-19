/*
 * Unit tests for mock_ctl: the mock's own controls on CI-V address F0,
 * through the real parser, the IC-705 personality and the audio engine.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine.h"
#include "cat_civ.h"
#include "mock_ctl.h"
#include "rig_ic705.h"

static uint8_t g_out[512];
static size_t g_out_len;
static int g_failed;
static int g_checks;

static civ_t g_civ;
static ae_t g_engine;
static mock_ctl_t g_ctl;

static void tx(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (g_out_len + n <= sizeof g_out) {
        memcpy(&g_out[g_out_len], b, n);
        g_out_len += n;
    }
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static size_t parse_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (*s) {
        int hi = hexval(s[0]);
        if (hi < 0) {
            s++;
            continue;
        }
        int lo = hexval(s[1]);
        if (lo < 0 || n >= cap) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

static void fail(const char *file, int line, const char *what)
{
    g_failed++;
    printf("FAIL %s:%d %s\n", file, line, what);
}

#define CHECK(cond)                                    \
    do {                                               \
        g_checks++;                                    \
        if (!(cond)) fail(__FILE__, __LINE__, #cond);  \
    } while (0)

static void send(const char *hex)
{
    uint8_t in[128];
    size_t n = parse_hex(hex, in, sizeof in);
    g_out_len = 0;
    civ_feed(&g_civ, in, n);
}

/* Send a frame, expect exactly this reply. */
static void expect_at(const char *hex, const char *want, const char *file, int line)
{
    uint8_t exp[128];
    size_t exp_len = parse_hex(want, exp, sizeof exp);
    send(hex);
    g_checks++;
    if (g_out_len != exp_len || memcmp(g_out, exp, exp_len) != 0) {
        fail(file, line, hex);
        printf("     want: %s\n     got: ", want);
        for (size_t i = 0; i < g_out_len; i++) printf(" %02x", g_out[i]);
        printf("\n");
    }
}
#define EXPECT(send, want) expect_at((send), (want), __FILE__, __LINE__)

#define OK "fe fe e0 f0 fb fd"
#define NG "fe fe e0 f0 fa fd"

static bool near(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

static ae_config_t config(void)
{
    ae_config_t cfg;
    ae_get_config(&g_engine, &cfg);
    return cfg;
}

/* Engine at its defaults, controls attached, power-up settings as given. */
static void fresh_with(const ae_config_t *power_up)
{
    ae_init(&g_engine, 1234u);
    ae_configure(&g_engine, power_up);
    mock_ctl_init(&g_ctl, &g_engine, power_up);
    civ_init(&g_civ, &rig_ic705, tx, NULL, NULL);
    civ_set_private(&g_civ, MOCK_CTL_ADDR, mock_ctl_command, &g_ctl);
}

static void fresh(void)
{
    ae_config_t cfg;
    ae_config_defaults(&cfg);
    fresh_with(&cfg);
}

static void tone(int16_t *out, size_t n, float amplitude)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = (int16_t)lrintf(amplitude * sinf(2.0f * 3.14159265f * 1500.0f * (float)i / 48000.0f));
    }
}

static void test_identity(void)
{
    fresh();
    EXPECT("fe fe f0 e0 00 fd", "fe fe e0 f0 00 01 30 85 fd"); /* version 1, Hamlib model 3085 */
    EXPECT("fe fe f0 e1 00 fd", "fe fe e1 f0 00 01 30 85 fd");
    EXPECT("fe fe f0 e0 00 01 fd", NG); /* read only */
}

static void test_read_defaults(void)
{
    fresh();
    EXPECT("fe fe f0 e0 10 fd", "fe fe e0 f0 10 00 fd");          /* monitor */
    EXPECT("fe fe f0 e0 11 fd", "fe fe e0 f0 11 00 fd");          /* noise off */
    EXPECT("fe fe f0 e0 12 fd", "fe fe e0 f0 12 01 00 00 fd");    /* +10.0 dB */
    EXPECT("fe fe f0 e0 13 fd", "fe fe e0 f0 13 00 25 00 fd");    /* 2500 Hz */
    EXPECT("fe fe f0 e0 14 fd", "fe fe e0 f0 14 04 00 fd");       /* -40.0 dBFS */
    EXPECT("fe fe f0 e0 15 fd", "fe fe e0 f0 15 00 00 fd");       /* 0.0 dB */
    EXPECT("fe fe f0 e0 16 fd", "fe fe e0 f0 16 00 30 fd");       /* 30 ms */
    EXPECT("fe fe f0 e0 20 fd", "fe fe e0 f0 20 00 99 99 00 00 00 fd"); /* nothing measured, unity gain */
}

static void test_switches(void)
{
    fresh();
    EXPECT("fe fe f0 e0 10 01 fd", OK);
    CHECK(config().route == AE_ROUTE_RADIO);
    EXPECT("fe fe f0 e0 10 fd", "fe fe e0 f0 10 01 fd");
    EXPECT("fe fe f0 e0 10 00 fd", OK);
    CHECK(config().route == AE_ROUTE_MONITOR);

    EXPECT("fe fe f0 e0 11 01 fd", OK);
    CHECK(config().noise_on);
    EXPECT("fe fe f0 e0 11 fd", "fe fe e0 f0 11 01 fd");
    CHECK(config().route == AE_ROUTE_MONITOR); /* one setting does not disturb another */

    EXPECT("fe fe f0 e0 10 02 fd", NG);
    EXPECT("fe fe f0 e0 11 00 00 fd", NG);
    CHECK(config().noise_on && config().route == AE_ROUTE_MONITOR);
}

static void test_snr(void)
{
    fresh();
    EXPECT("fe fe f0 e0 12 01 25 01 fd", OK); /* -12.5 dB */
    CHECK(near(config().snr_db, -12.5f, 0.001f));
    EXPECT("fe fe f0 e0 12 fd", "fe fe e0 f0 12 01 25 01 fd");
    EXPECT("fe fe f0 e0 12 00 03 00 fd", OK); /* +0.3 dB */
    CHECK(near(config().snr_db, 0.3f, 0.001f));
    EXPECT("fe fe f0 e0 12 00 00 01 fd", OK); /* minus zero is zero */
    EXPECT("fe fe f0 e0 12 fd", "fe fe e0 f0 12 00 00 00 fd");

    /* Both ends of the range are accepted and stored as sent; one step past is not. */
    EXPECT("fe fe f0 e0 12 06 00 01 fd", OK);
    EXPECT("fe fe f0 e0 12 fd", "fe fe e0 f0 12 06 00 01 fd");
    EXPECT("fe fe f0 e0 12 06 01 01 fd", NG);
    EXPECT("fe fe f0 e0 12 08 00 00 fd", OK);
    EXPECT("fe fe f0 e0 12 fd", "fe fe e0 f0 12 08 00 00 fd");
    EXPECT("fe fe f0 e0 12 08 01 00 fd", NG);
    CHECK(near(config().snr_db, 80.0f, 0.001f)); /* a refused value changes nothing */

    EXPECT("fe fe f0 e0 12 01 25 fd", NG);       /* sign byte missing */
    EXPECT("fe fe f0 e0 12 01 25 02 fd", NG);    /* not a sign */
    EXPECT("fe fe f0 e0 12 01 2a 00 fd", NG);    /* not BCD */
    EXPECT("fe fe f0 e0 12 01 25 00 00 fd", NG); /* too long */
    CHECK(near(config().snr_db, 80.0f, 0.001f));
}

static void test_other_numbers(void)
{
    fresh();
    /* reference bandwidth, Hz, three bytes */
    EXPECT("fe fe f0 e0 13 00 30 00 fd", OK);
    CHECK(near(config().ref_bw_hz, 3000.0f, 0.5f));
    EXPECT("fe fe f0 e0 13 00 01 00 fd", OK);
    EXPECT("fe fe f0 e0 13 00 00 99 fd", NG);
    EXPECT("fe fe f0 e0 13 02 40 00 fd", OK);
    EXPECT("fe fe f0 e0 13 fd", "fe fe e0 f0 13 02 40 00 fd");
    EXPECT("fe fe f0 e0 13 02 40 01 fd", NG);
    EXPECT("fe fe f0 e0 13 30 00 fd", NG); /* two bytes */

    /* noise floor, tenths of a dB below full scale */
    EXPECT("fe fe f0 e0 14 05 55 fd", OK);
    CHECK(near(config().noise_floor_dbfs, -55.5f, 0.001f));
    EXPECT("fe fe f0 e0 14 01 20 fd", OK);
    EXPECT("fe fe f0 e0 14 fd", "fe fe e0 f0 14 01 20 fd");
    EXPECT("fe fe f0 e0 14 01 19 fd", NG);
    EXPECT("fe fe f0 e0 14 09 00 fd", OK);
    EXPECT("fe fe f0 e0 14 fd", "fe fe e0 f0 14 09 00 fd");
    EXPECT("fe fe f0 e0 14 09 01 fd", NG);
    EXPECT("fe fe f0 e0 14 09 00 01 fd", NG); /* unsigned: no sign byte */

    /* attenuation, tenths of a dB */
    EXPECT("fe fe f0 e0 15 00 65 fd", OK);
    CHECK(near(config().atten_db, 6.5f, 0.001f));
    EXPECT("fe fe f0 e0 15 12 00 fd", OK);
    EXPECT("fe fe f0 e0 15 fd", "fe fe e0 f0 15 12 00 fd");
    EXPECT("fe fe f0 e0 15 12 01 fd", NG);

    /* turnaround, ms */
    EXPECT("fe fe f0 e0 16 01 50 fd", OK);
    CHECK(config().turnaround_ms == 150);
    EXPECT("fe fe f0 e0 16 50 00 fd", OK);
    EXPECT("fe fe f0 e0 16 fd", "fe fe e0 f0 16 50 00 fd");
    EXPECT("fe fe f0 e0 16 50 01 fd", NG);
    CHECK(config().turnaround_ms == 5000);
}

static void test_reset(void)
{
    ae_config_t up;
    ae_config_defaults(&up);
    up.route = AE_ROUTE_RADIO;
    up.noise_on = true;
    up.snr_db = 3.0f;
    up.turnaround_ms = 45;
    fresh_with(&up);

    EXPECT("fe fe f0 e0 12 fd", "fe fe e0 f0 12 00 30 00 fd");
    EXPECT("fe fe f0 e0 10 00 fd", OK);
    EXPECT("fe fe f0 e0 11 00 fd", OK);
    EXPECT("fe fe f0 e0 12 02 00 01 fd", OK);
    EXPECT("fe fe f0 e0 15 01 00 fd", OK);
    EXPECT("fe fe f0 e0 16 00 00 fd", OK);

    EXPECT("fe fe f0 e0 01 00 fd", NG); /* takes no data */
    CHECK(config().route == AE_ROUTE_MONITOR);
    EXPECT("fe fe f0 e0 01 fd", OK);
    ae_config_t now = config();
    CHECK(now.route == AE_ROUTE_RADIO && now.noise_on);
    CHECK(near(now.snr_db, 3.0f, 0.001f) && near(now.atten_db, 0.0f, 0.001f));
    CHECK(now.turnaround_ms == 45);
}

/* Decode a status reply: FE FE E0 F0 20 <flags> <level 2> <gain 2> <sign> FD */
static bool status(unsigned *flags, int *level, int *gain)
{
    uint16_t l, g;
    send("fe fe f0 e0 20 fd");
    if (g_out_len != 12 || g_out[4] != 0x20 || g_out[11] != 0xFD) return false;
    if (!civ_bcd_be_to_u16(&g_out[6], 2, &l) || !civ_bcd_be_to_u16(&g_out[8], 2, &g)) return false;
    *flags = g_out[5];
    *level = l;
    *gain = g_out[10] ? -(int)g : (int)g;
    return true;
}

static void test_status(void)
{
    enum { N = 4800 };
    static int16_t in[N], out[N];
    unsigned flags;
    int level, gain;

    fresh();
    EXPECT("fe fe f0 e0 20 00 fd", NG); /* read only */

    /* A tone at -15.3 dBFS RMS through the default channel: measured, unity gain. */
    tone(in, N, 8000.0f);
    float in_dbfs = 20.0f * log10f(8000.0f / sqrtf(2.0f) / 32768.0f);
    ae_rx_process(&g_engine, in, out, N);
    CHECK(status(&flags, &level, &gain));
    CHECK(flags == 0);
    CHECK(abs(level - (int)lroundf(-in_dbfs * 10.0f)) <= 1);
    CHECK(gain == 0);

    /* Noise on at +10 dB in 2500 Hz over a -40 dBFS floor puts the signal at
     * -40 + 10 - 9.8 = -39.8 dBFS, so the gain is that minus the level. */
    EXPECT("fe fe f0 e0 11 01 fd", OK);
    ae_rx_process(&g_engine, in, out, N);
    float want_gain = -40.0f + 10.0f + 10.0f * log10f(2500.0f / 24000.0f) - in_dbfs;
    CHECK(status(&flags, &level, &gain));
    CHECK(flags == 0);
    CHECK(abs(gain - (int)lroundf(want_gain * 10.0f)) <= 2);

    /* The setting took effect in the audio, not only in the read-back. */
    EXPECT("fe fe f0 e0 12 03 00 01 fd", OK); /* -30 dB: the output is nearly all noise */
    ae_rx_process(&g_engine, in, out, N);
    double sum = 0.0;
    for (size_t i = 0; i < N; i++) sum += (double)out[i] * out[i];
    float out_dbfs = 10.0f * log10f((float)(sum / N) / (32768.0f * 32768.0f));
    CHECK(near(out_dbfs, -40.0f, 0.5f));

    /* A source too quiet to reach the SNR within the gain limit. */
    EXPECT("fe fe f0 e0 12 03 00 00 fd", OK); /* +30 dB */
    memset(in, 0, sizeof in);
    for (int i = 0; i < 3; i++) ae_rx_process(&g_engine, in, out, N); /* a gap, so the level is measured afresh */
    tone(in, N, 60.0f);                                               /* about -58 dBFS: needs about +38 dB */
    ae_rx_process(&g_engine, in, out, N);
    CHECK(status(&flags, &level, &gain));
    CHECK(flags == MOCK_CTL_FLAG_GAIN_LIMITED);
    CHECK(gain == 300);

    /* Radio route, keyed: receive is muted. */
    EXPECT("fe fe f0 e0 10 01 fd", OK);
    ae_set_ptt(&g_engine, true);
    ae_rx_process(&g_engine, NULL, out, N);
    CHECK(status(&flags, &level, &gain));
    CHECK((flags & MOCK_CTL_FLAG_RX_MUTED) != 0);
    ae_set_ptt(&g_engine, false);
}

static void test_not_the_radio(void)
{
    fresh();
    /* Reserved for parts that do not exist yet, and numbers never assigned. */
    EXPECT("fe fe f0 e0 30 fd", NG);
    EXPECT("fe fe f0 e0 31 01 fd", NG);
    EXPECT("fe fe f0 e0 40 fd", NG);
    EXPECT("fe fe f0 e0 50 fd", NG);
    EXPECT("fe fe f0 e0 60 fd", NG);
    EXPECT("fe fe f0 e0 17 fd", NG);
    EXPECT("fe fe f0 e0 7f fd", NG);

    /* Radio commands at the mock's address are not the radio's. */
    EXPECT("fe fe f0 e0 03 fd", NG);
    EXPECT("fe fe f0 e0 1c 00 01 fd", NG);
    CHECK(!civ_ptt(&g_civ));

    /* And the mock's commands at a radio address are not the mock's. */
    EXPECT("fe fe a4 e0 03 fd", "fe fe e0 a4 03 00 40 07 14 00 fd");
    EXPECT("fe fe a4 e0 12 01 25 01 fd", "fe fe e0 a4 fa fd");
    EXPECT("fe fe a4 e0 20 fd", "fe fe e0 a4 fa fd");
    CHECK(near(config().snr_db, 10.0f, 0.001f));

    /* 10 at a radio address is the radio's tuning step, not the route. */
    EXPECT("fe fe a4 e0 10 01 fd", "fe fe e0 a4 fb fd");
    CHECK(config().route == AE_ROUTE_MONITOR);
}

int main(void)
{
    test_identity();
    test_read_defaults();
    test_switches();
    test_snr();
    test_other_numbers();
    test_reset();
    test_status();
    test_not_the_radio();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
