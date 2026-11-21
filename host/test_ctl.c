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
#include "rig_g90.h"
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

    EXPECT("fe fe f0 e0 10 02 fd", NG); /* parrot, but there is no buffer */
    EXPECT("fe fe f0 e0 10 03 fd", NG); /* link, but there is none */
    EXPECT("fe fe f0 e0 10 04 fd", NG);
    EXPECT("fe fe f0 e0 11 02 fd", NG);
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

static int16_t g_store[4 * AE_PARROT_FS_HZ]; /* 4 s */

/* n samples of a tone through the whole radio, 10 ms at a time. */
static void radio(size_t n)
{
    static int16_t buf[480];
    for (size_t done = 0; done < n; done += 480) {
        tone(buf, 480, 8000.0f);
        ae_process(&g_engine, buf, buf, 480);
    }
}

static void test_parrot(void)
{
    fresh();
    EXPECT("fe fe f0 e0 34 fd", "fe fe e0 f0 34 00 00 00 00 00 00 fd"); /* idle, nothing, no room */
    ae_set_parrot_buffer(&g_engine, g_store, sizeof g_store / sizeof g_store[0]);
    EXPECT("fe fe f0 e0 34 fd", "fe fe e0 f0 34 00 00 00 00 00 40 fd"); /* room for 4.0 s */
    EXPECT("fe fe f0 e0 34 00 fd", NG);                                 /* read only */

    /* The route has a third value once there is a buffer. */
    EXPECT("fe fe f0 e0 10 02 fd", OK);
    CHECK(config().route == AE_ROUTE_PARROT);
    EXPECT("fe fe f0 e0 10 fd", "fe fe e0 f0 10 02 fd");
    EXPECT("fe fe f0 e0 10 03 fd", NG); /* still no link */

    /* Delay and period, in tenths of a second. */
    EXPECT("fe fe f0 e0 32 fd", "fe fe e0 f0 32 00 10 fd"); /* 1.0 s */
    EXPECT("fe fe f0 e0 33 fd", "fe fe e0 f0 33 00 00 fd"); /* off */
    EXPECT("fe fe f0 e0 32 00 25 fd", OK);
    CHECK(config().parrot_delay_ms == 2500);
    EXPECT("fe fe f0 e0 32 fd", "fe fe e0 f0 32 00 25 fd");
    EXPECT("fe fe f0 e0 32 06 00 fd", OK); /* 60.0 s, the most */
    EXPECT("fe fe f0 e0 32 06 01 fd", NG);
    EXPECT("fe fe f0 e0 32 00 fd", NG);
    CHECK(config().parrot_delay_ms == 60000);
    EXPECT("fe fe f0 e0 33 01 50 fd", OK); /* 15.0 s: FT8 */
    CHECK(config().parrot_period_ms == 15000);
    EXPECT("fe fe f0 e0 33 00 75 fd", OK); /* 7.5 s: FT4 */
    CHECK(config().parrot_period_ms == 7500);
    EXPECT("fe fe f0 e0 33 30 00 fd", OK); /* 300.0 s, the most */
    EXPECT("fe fe f0 e0 33 30 01 fd", NG);
    EXPECT("fe fe f0 e0 33 0a 00 fd", NG); /* not BCD */
    EXPECT("fe fe f0 e0 33 00 00 00 fd", NG);
    CHECK(config().parrot_period_ms == 300000);
    EXPECT("fe fe f0 e0 33 00 00 fd", OK);
    EXPECT("fe fe f0 e0 32 00 25 fd", OK);

    /* Replay needs a recording and a radio that is not transmitting. */
    EXPECT("fe fe f0 e0 30 fd", NG);
    ae_set_ptt(&g_engine, true);
    radio(48000);
    EXPECT("fe fe f0 e0 34 fd", "fe fe e0 f0 34 01 00 00 10 00 40 fd"); /* recording, 1.0 s so far */
    EXPECT("fe fe f0 e0 30 fd", NG);
    ae_set_ptt(&g_engine, false);
    radio(24000);
    EXPECT("fe fe f0 e0 34 fd", "fe fe e0 f0 34 02 00 00 11 00 40 fd"); /* waiting: 1.05 s with the tail */
    EXPECT("fe fe f0 e0 30 01 fd", NG);
    EXPECT("fe fe f0 e0 30 fd", OK); /* now, not after the 2.5 s */
    radio(480);
    EXPECT("fe fe f0 e0 34 fd", "fe fe e0 f0 34 03 00 00 11 00 40 fd"); /* playing */
    radio(96000);
    EXPECT("fe fe f0 e0 34 fd", "fe fe e0 f0 34 00 00 00 11 00 40 fd"); /* idle, recording kept */

    /* A transmission longer than the buffer is flagged. */
    ae_set_ptt(&g_engine, true);
    radio(5 * 48000);
    EXPECT("fe fe f0 e0 34 fd", "fe fe e0 f0 34 01 01 00 40 00 40 fd");
    ae_set_ptt(&g_engine, false);
    radio(480);

    /* Reset puts the parrot's settings back with the rest; in the monitor
     * route there is nothing to replay. */
    EXPECT("fe fe f0 e0 01 fd", OK);
    CHECK(config().route == AE_ROUTE_MONITOR);
    CHECK(config().parrot_delay_ms == 1000);
    radio(480);
    EXPECT("fe fe f0 e0 30 fd", NG);
    EXPECT("fe fe f0 e0 34 fd", "fe fe e0 f0 34 00 01 00 40 00 40 fd");
}

static void test_ptt_sources(void)
{
    fresh();
    /* Power-up: no control line, VOX off at -40.0 dBFS and 100 ms, receiving. */
    EXPECT("fe fe f0 e0 50 fd", "fe fe e0 f0 50 00 fd");
    EXPECT("fe fe f0 e0 51 fd", "fe fe e0 f0 51 00 fd");
    EXPECT("fe fe f0 e0 52 fd", "fe fe e0 f0 52 04 00 fd");
    EXPECT("fe fe f0 e0 53 fd", "fe fe e0 f0 53 01 00 fd");
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 00 fd");

    /* The control line. Hosts raise both on open; nothing keys until one is chosen. */
    ae_set_lines(&g_engine, true, true);
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 00 fd");
    EXPECT("fe fe f0 e0 50 01 fd", OK);
    CHECK(config().ptt_line == AE_LINE_RTS);
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 02 fd");
    ae_set_lines(&g_engine, true, false);
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 00 fd");
    EXPECT("fe fe f0 e0 50 02 fd", OK);
    CHECK(config().ptt_line == AE_LINE_DTR);
    EXPECT("fe fe f0 e0 50 fd", "fe fe e0 f0 50 02 fd");
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 02 fd");
    EXPECT("fe fe f0 e0 50 03 fd", NG);
    EXPECT("fe fe f0 e0 50 00 fd", OK);

    /* CAT. */
    ae_set_ptt(&g_engine, true);
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 01 fd");
    ae_set_ptt(&g_engine, false);

    /* VOX: its settings, then keyed by audio. */
    EXPECT("fe fe f0 e0 52 05 55 fd", OK);
    CHECK(near(config().vox_threshold_dbfs, -55.5f, 0.001f));
    EXPECT("fe fe f0 e0 52 00 60 fd", OK); /* -6.0, the highest */
    EXPECT("fe fe f0 e0 52 00 59 fd", NG);
    EXPECT("fe fe f0 e0 52 08 00 fd", OK); /* -80.0, the lowest */
    EXPECT("fe fe f0 e0 52 08 01 fd", NG);
    EXPECT("fe fe f0 e0 52 04 00 fd", OK);
    EXPECT("fe fe f0 e0 53 02 50 fd", OK);
    CHECK(config().vox_hang_ms == 250);
    EXPECT("fe fe f0 e0 53 50 00 fd", OK);
    EXPECT("fe fe f0 e0 53 50 01 fd", NG);
    EXPECT("fe fe f0 e0 53 00 20 fd", OK);
    CHECK(config().vox_hang_ms == 20);

    radio(4800);
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 00 fd"); /* VOX is off */
    EXPECT("fe fe f0 e0 51 01 fd", OK);
    CHECK(config().vox_on);
    EXPECT("fe fe f0 e0 51 fd", "fe fe e0 f0 51 01 fd");
    EXPECT("fe fe f0 e0 51 02 fd", NG);
    radio(4800);
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 04 fd");
    ae_set_ptt(&g_engine, true);
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 05 fd");
    ae_set_ptt(&g_engine, false);
    EXPECT("fe fe f0 e0 54 01 fd", NG); /* read only */

    /* The key on the front panel, with the VOX still open. */
    ae_set_panel_ptt(&g_engine, true);
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 0c fd");
    ae_set_panel_ptt(&g_engine, false);
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 04 fd");

    /* Reset turns the VOX off again. */
    EXPECT("fe fe f0 e0 01 fd", OK);
    CHECK(!config().vox_on && config().vox_hang_ms == 100 && config().ptt_line == AE_LINE_OFF);
    radio(480);
    EXPECT("fe fe f0 e0 54 fd", "fe fe e0 f0 54 00 fd");
}

static void test_fading_settings(void)
{
    fresh();
    /* Off at first; the custom numbers are a poor path's. */
    EXPECT("fe fe f0 e0 17 fd", "fe fe e0 f0 17 00 fd");
    EXPECT("fe fe f0 e0 18 fd", "fe fe e0 f0 18 01 00 fd"); /* 1.00 Hz */
    EXPECT("fe fe f0 e0 19 fd", "fe fe e0 f0 19 00 20 fd"); /* 2.0 ms */
    EXPECT("fe fe f0 e0 1a fd", "fe fe e0 f0 1a 00 00 00 fd");

    /* A named condition, and each of them. */
    EXPECT("fe fe f0 e0 17 02 fd", OK);
    CHECK(config().fade == AE_FADE_MPP);
    EXPECT("fe fe f0 e0 17 fd", "fe fe e0 f0 17 02 fd");
    EXPECT("fe fe f0 e0 17 08 fd", OK);
    CHECK(config().fade == AE_FADE_HILAT_D);
    EXPECT("fe fe f0 e0 17 09 fd", OK);
    CHECK(config().fade == AE_FADE_CUSTOM);
    EXPECT("fe fe f0 e0 17 10 fd", NG);
    EXPECT("fe fe f0 e0 17 0a fd", NG);
    EXPECT("fe fe f0 e0 17 00 fd", OK);

    /* Custom numbers: 0.15 Hz and 7.5 ms, and their limits. */
    EXPECT("fe fe f0 e0 18 00 15 fd", OK);
    CHECK(near(config().fade_doppler_hz, 0.15f, 0.0001f));
    EXPECT("fe fe f0 e0 18 fd", "fe fe e0 f0 18 00 15 fd");
    EXPECT("fe fe f0 e0 18 30 00 fd", OK);
    EXPECT("fe fe f0 e0 18 00 01 fd", OK);
    EXPECT("fe fe f0 e0 18 00 00 fd", NG);
    EXPECT("fe fe f0 e0 18 30 01 fd", NG);
    CHECK(near(config().fade_doppler_hz, 0.01f, 0.0001f));
    EXPECT("fe fe f0 e0 19 00 75 fd", OK);
    CHECK(near(config().fade_delay_ms, 7.5f, 0.001f));
    EXPECT("fe fe f0 e0 19 01 00 fd", OK);
    EXPECT("fe fe f0 e0 19 00 00 fd", OK);
    EXPECT("fe fe f0 e0 19 01 01 fd", NG);

    /* Frequency offset, either way. */
    EXPECT("fe fe f0 e0 1a 00 55 01 fd", OK);
    CHECK(near(config().freq_offset_hz, -5.5f, 0.001f));
    EXPECT("fe fe f0 e0 1a fd", "fe fe e0 f0 1a 00 55 01 fd");
    EXPECT("fe fe f0 e0 1a 20 00 00 fd", OK);
    EXPECT("fe fe f0 e0 1a 20 00 01 fd", OK);
    EXPECT("fe fe f0 e0 1a 20 01 00 fd", NG);
    EXPECT("fe fe f0 e0 1a 00 55 fd", NG);
    CHECK(near(config().freq_offset_hz, -200.0f, 0.001f));

    /* The shared channel is a setting of the link's. */
    EXPECT("fe fe f0 e0 42 fd", "fe fe e0 f0 42 01 fd");
    EXPECT("fe fe f0 e0 42 00 fd", OK);
    CHECK(!config().link_coord);
    EXPECT("fe fe f0 e0 42 02 fd", NG);

    /* Reset puts them all back. */
    EXPECT("fe fe f0 e0 17 03 fd", OK);
    EXPECT("fe fe f0 e0 01 fd", OK);
    CHECK(config().fade == AE_FADE_OFF && near(config().fade_doppler_hz, 1.0f, 0.0001f));
    CHECK(near(config().freq_offset_hz, 0.0f, 0.001f) && config().link_coord);
}

static int g_link_ended;

static void fake_link_tx(void *ctx, const int16_t *tx_audio, size_t n, bool keyed)
{
    (void)ctx;
    (void)tx_audio;
    (void)keyed;
    if (n == 0) g_link_ended++;
}

static bool fake_link_rx(void *ctx, int16_t *out, size_t n)
{
    (void)ctx;
    (void)out;
    (void)n;
    return false;
}

static void test_link_settings(void)
{
    static const ae_link_t link = {.tx = fake_link_tx, .rx = fake_link_rx};
    ae_config_t cfg;
    int16_t audio[480] = {0};

    /* Without a link the route is refused, but its settings can be set. */
    fresh();
    EXPECT("fe fe f0 e0 10 03 fd", NG);
    EXPECT("fe fe f0 e0 40 fd", "fe fe e0 f0 40 00 30 fd"); /* 30 ms */
    EXPECT("fe fe f0 e0 41 fd", "fe fe e0 f0 41 01 fd");    /* matching on */

    ae_config_defaults(&cfg);
    ae_init(&g_engine, 1u);
    ae_set_link(&g_engine, &link);
    ae_configure(&g_engine, &cfg);
    mock_ctl_init(&g_ctl, &g_engine, &cfg);
    EXPECT("fe fe f0 e0 10 03 fd", OK);
    CHECK(config().route == AE_ROUTE_LINK);
    EXPECT("fe fe f0 e0 10 fd", "fe fe e0 f0 10 03 fd");
    EXPECT("fe fe f0 e0 10 04 fd", NG);

    EXPECT("fe fe f0 e0 40 01 00 fd", OK);
    CHECK(config().link_buffer_ms == 100);
    EXPECT("fe fe f0 e0 40 fd", "fe fe e0 f0 40 01 00 fd");
    EXPECT("fe fe f0 e0 40 00 10 fd", OK);
    EXPECT("fe fe f0 e0 40 02 00 fd", OK);
    EXPECT("fe fe f0 e0 40 00 09 fd", NG);
    EXPECT("fe fe f0 e0 40 02 01 fd", NG);
    EXPECT("fe fe f0 e0 40 30 fd", NG);
    CHECK(config().link_buffer_ms == 200);

    EXPECT("fe fe f0 e0 41 00 fd", OK);
    CHECK(!config().link_match);
    EXPECT("fe fe f0 e0 41 fd", "fe fe e0 f0 41 00 fd");
    EXPECT("fe fe f0 e0 41 02 fd", NG);
    EXPECT("fe fe f0 e0 41 01 fd", OK);
    CHECK(config().link_match);

    /* Reset leaves the link route, which ends a transmission under way. */
    ae_process(&g_engine, audio, audio, 480);
    g_link_ended = 0;
    EXPECT("fe fe f0 e0 01 fd", OK);
    CHECK(config().route == AE_ROUTE_MONITOR && config().link_buffer_ms == 30 && config().link_match);
    ae_process(&g_engine, audio, audio, 480);
    CHECK(g_link_ended == 1);
}

/* ---- power-up settings kept in flash, and clips: with stand-ins for the flash ---- */

static uint8_t g_blob[MOCK_CTL_BLOB_MAX];
static size_t g_blob_len;
static bool g_store_broken;

static bool store_save(void *ctx, const uint8_t *blob, size_t len)
{
    (void)ctx;
    if (g_store_broken || len > sizeof g_blob) return false;
    memcpy(g_blob, blob, len);
    g_blob_len = len;
    return true;
}

static bool store_forget(void *ctx)
{
    (void)ctx;
    if (g_store_broken) return false;
    g_blob_len = 0;
    return true;
}

static const mock_ctl_store_t g_flash_store = {.save = store_save, .forget = store_forget};

static void test_kept_settings(void)
{
    static int16_t parrot_store[4 * AE_PARROT_FS_HZ];
    ae_config_t cfg;

    /* With nowhere to keep them: refused, and it says the build's are in use. */
    fresh();
    EXPECT("fe fe f0 e0 04 fd", "fe fe e0 f0 04 00 fd");
    EXPECT("fe fe f0 e0 02 fd", NG);
    EXPECT("fe fe f0 e0 03 fd", NG);

    g_blob_len = 0;
    g_store_broken = false;
    mock_ctl_set_store(&g_ctl, &g_flash_store);
    EXPECT("fe fe f0 e0 51 01 fd", OK);       /* VOX on, as Data2G wants it */
    EXPECT("fe fe f0 e0 12 01 25 01 fd", OK); /* SNR -12.5 dB */
    EXPECT("fe fe f0 e0 41 00 fd", OK);       /* link frequency match off */
    EXPECT("fe fe f0 e0 02 fd", OK);
    CHECK(g_blob_len > 0 && g_blob_len <= MOCK_CTL_BLOB_MAX && mock_ctl_settings_kept(&g_ctl));
    EXPECT("fe fe f0 e0 04 fd", "fe fe e0 f0 04 01 fd");
    EXPECT("fe fe f0 e0 02 00 fd", NG); /* it takes no data */
    EXPECT("fe fe f0 e0 04 01 fd", NG); /* read only */

    /* Reset now returns to what was kept, not to the build's values. */
    EXPECT("fe fe f0 e0 51 00 fd", OK);
    EXPECT("fe fe f0 e0 12 00 30 00 fd", OK);
    EXPECT("fe fe f0 e0 01 fd", OK);
    CHECK(config().vox_on && near(config().snr_db, -12.5f, 0.001f) && !config().link_match);

    /* The next start: the build's settings, then what the flash holds. */
    uint8_t kept[MOCK_CTL_BLOB_MAX];
    size_t kept_len = g_blob_len;
    memcpy(kept, g_blob, kept_len);
    fresh();
    CHECK(!config().vox_on && !mock_ctl_settings_kept(&g_ctl));
    CHECK(mock_ctl_restore(&g_ctl, kept, kept_len));
    CHECK(config().vox_on && near(config().snr_db, -12.5f, 0.001f) && !config().link_match);
    CHECK(config().route == AE_ROUTE_MONITOR && config().turnaround_ms == 30); /* the rest as the build has it */
    EXPECT("fe fe f0 e0 04 fd", "fe fe e0 f0 04 01 fd");
    EXPECT("fe fe f0 e0 51 00 fd", OK);
    EXPECT("fe fe f0 e0 01 fd", OK);
    CHECK(config().vox_on);

    /* Forget: the power-up values are the build's again, the settings in
     * use stay until reset. */
    mock_ctl_set_store(&g_ctl, &g_flash_store);
    EXPECT("fe fe f0 e0 03 fd", OK);
    CHECK(g_blob_len == 0 && !mock_ctl_settings_kept(&g_ctl) && config().vox_on);
    EXPECT("fe fe f0 e0 04 fd", "fe fe e0 f0 04 00 fd");
    EXPECT("fe fe f0 e0 01 fd", OK);
    CHECK(!config().vox_on && near(config().snr_db, 10.0f, 0.001f) && config().link_match);

    /* Flash that fails: refused, and nothing is believed kept. */
    g_store_broken = true;
    EXPECT("fe fe f0 e0 51 01 fd", OK);
    EXPECT("fe fe f0 e0 02 fd", NG);
    CHECK(!mock_ctl_settings_kept(&g_ctl));
    EXPECT("fe fe f0 e0 01 fd", OK);
    CHECK(!config().vox_on);
    EXPECT("fe fe f0 e0 03 fd", NG);
    g_store_broken = false;

    /* Damaged, cut short, from another version, or not this at all: not
     * restored, and nothing changes. */
    fresh();
    for (size_t i = 0; i < kept_len; i++) {
        kept[i] ^= 0x04;
        g_checks++;
        if (mock_ctl_restore(&g_ctl, kept, kept_len) || config().vox_on) fail(__FILE__, __LINE__, "damaged settings restored");
        kept[i] ^= 0x04;
    }
    CHECK(!mock_ctl_restore(&g_ctl, kept, kept_len - 1));
    CHECK(!mock_ctl_restore(&g_ctl, kept, 0) && !mock_ctl_restore(&g_ctl, NULL, kept_len));
    CHECK(!mock_ctl_restore(&g_ctl, (const uint8_t *)"not settings", 12));
    CHECK(!mock_ctl_settings_kept(&g_ctl));

    /* Every setting survives the round trip. */
    ae_config_defaults(&cfg);
    cfg.route = AE_ROUTE_PARROT;
    cfg.noise_on = true;
    cfg.snr_db = -3.5f;
    cfg.ref_bw_hz = 3000;
    cfg.noise_floor_dbfs = -55.5f;
    cfg.atten_db = 6.0f;
    cfg.turnaround_ms = 150;
    cfg.parrot_delay_ms = 2500;
    cfg.parrot_period_ms = 15000;
    cfg.link_buffer_ms = 80;
    cfg.link_match = false;
    cfg.link_coord = false;
    cfg.fade = AE_FADE_CUSTOM;
    cfg.fade_doppler_hz = 0.15f;
    cfg.fade_delay_ms = 7.5f;
    cfg.freq_offset_hz = -12.5f;
    cfg.ptt_line = AE_LINE_RTS;
    cfg.vox_on = true;
    cfg.vox_threshold_dbfs = -35.5f;
    cfg.vox_hang_ms = 250;
    kept_len = mock_ctl_settings_pack(&cfg, kept, sizeof kept);
    CHECK(kept_len > 0 && kept_len <= MOCK_CTL_BLOB_MAX - 30); /* room for more settings */
    CHECK(mock_ctl_settings_pack(&cfg, kept, kept_len - 1) == 0);

    /* A board without a parrot buffer cannot start in the parrot route: that
     * one setting falls back, the others are taken. */
    fresh();
    CHECK(mock_ctl_restore(&g_ctl, kept, kept_len));
    ae_config_t got = config();
    CHECK(got.route == AE_ROUTE_MONITOR && got.noise_on && near(got.snr_db, -3.5f, 0.001f));
    /* With one, everything is back. */
    ae_config_defaults(&cfg);
    ae_init(&g_engine, 1u);
    ae_set_parrot_buffer(&g_engine, parrot_store, sizeof parrot_store / sizeof parrot_store[0]);
    ae_configure(&g_engine, &cfg);
    mock_ctl_init(&g_ctl, &g_engine, &cfg);
    CHECK(mock_ctl_restore(&g_ctl, kept, kept_len));
    got = config();
    CHECK(got.route == AE_ROUTE_PARROT && got.noise_on && near(got.snr_db, -3.5f, 0.001f));
    CHECK(near(got.ref_bw_hz, 3000.0f, 0.5f) && near(got.noise_floor_dbfs, -55.5f, 0.001f) && near(got.atten_db, 6.0f, 0.001f));
    CHECK(got.turnaround_ms == 150 && got.parrot_delay_ms == 2500 && got.parrot_period_ms == 15000);
    CHECK(got.link_buffer_ms == 80 && !got.link_match && got.ptt_line == AE_LINE_RTS && !got.link_coord);
    CHECK(got.fade == AE_FADE_CUSTOM && near(got.fade_doppler_hz, 0.15f, 0.0001f) && near(got.fade_delay_ms, 7.5f, 0.001f));
    CHECK(near(got.freq_offset_hz, -12.5f, 0.001f));
    CHECK(got.vox_on && near(got.vox_threshold_dbfs, -35.5f, 0.001f) && got.vox_hang_ms == 250);

    /* Kept by newer firmware: a setting this one does not know, and a value
     * out of today's range. Both are passed over. */
    uint8_t newer[4 + 3 * 5 + 2] = {'M', 'S', 1, 3, 0x7E, 1, 0, 0, 0, MOCK_CTL_VOX_HANG, 0xFF, 0xFF, 0, 0,
                                    MOCK_CTL_TURNAROUND, 77, 0, 0, 0};
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < sizeof newer - 2; i++) {
        crc ^= (uint16_t)(newer[i] << 8);
        for (int b = 0; b < 8; b++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    newer[sizeof newer - 2] = (uint8_t)(crc >> 8);
    newer[sizeof newer - 1] = (uint8_t)crc;
    fresh();
    CHECK(mock_ctl_restore(&g_ctl, newer, sizeof newer));
    CHECK(config().turnaround_ms == 77 && config().vox_hang_ms == 100);
}

static mock_ctl_clips_status_t g_clips;
static int g_clip_starts;
static mock_ctl_clip_op_t g_clip_op;
static unsigned g_clip_slot;
static bool g_clip_refuse;

static bool clips_start(void *ctx, mock_ctl_clip_op_t op, unsigned slot)
{
    (void)ctx;
    if (g_clip_refuse) return false;
    g_clip_starts++;
    g_clip_op = op;
    g_clip_slot = slot;
    return true;
}

static void clips_status(void *ctx, mock_ctl_clips_status_t *st)
{
    (void)ctx;
    *st = g_clips;
}

static void test_clips(void)
{
    static const mock_ctl_clips_t clips = {.start = clips_start, .status = clips_status};
    static int16_t parrot_store[4 * AE_PARROT_FS_HZ];
    static int16_t audio[4800];
    ae_config_t cfg;

    /* A mock without clips says it has none and refuses them all. */
    fresh();
    EXPECT("fe fe f0 e0 37 fd", "fe fe e0 f0 37 00 00 fd");
    EXPECT("fe fe f0 e0 35 01 fd", NG);
    EXPECT("fe fe f0 e0 31 01 fd", NG);
    EXPECT("fe fe f0 e0 36 01 fd", NG);

    ae_config_defaults(&cfg);
    ae_init(&g_engine, 1u);
    ae_set_parrot_buffer(&g_engine, parrot_store, sizeof parrot_store / sizeof parrot_store[0]);
    cfg.route = AE_ROUTE_PARROT;
    cfg.parrot_delay_ms = 60000;
    ae_configure(&g_engine, &cfg);
    mock_ctl_init(&g_ctl, &g_engine, &cfg);
    memset(&g_clips, 0, sizeof g_clips);
    g_clips.slots = 3;
    g_clips.length_ms[1] = 15040; /* clip 2: an FT8 transmission */
    g_clip_starts = 0;
    g_clip_refuse = false;
    mock_ctl_set_clips(&g_ctl, &clips);
    EXPECT("fe fe f0 e0 37 fd", "fe fe e0 f0 37 00 03 00 00 01 50 00 00 fd");

    /* Nothing recorded yet: nothing to keep. */
    EXPECT("fe fe f0 e0 35 01 fd", NG);
    CHECK(mock_ctl_clip(&g_ctl, MOCK_CTL_CLIP_SAVE, 1) == MOCK_CTL_CLIP_NOTHING);

    /* While it records: not yet. Afterwards: begun. */
    tone(audio, 4800, 8000.0f);
    ae_set_ptt(&g_engine, true);
    for (int i = 0; i < 5; i++) ae_process(&g_engine, audio, audio, 4800), tone(audio, 4800, 8000.0f);
    CHECK(mock_ctl_clip(&g_ctl, MOCK_CTL_CLIP_SAVE, 1) == MOCK_CTL_CLIP_STATE);
    EXPECT("fe fe f0 e0 31 02 fd", NG); /* nor play one over a transmission */
    CHECK(mock_ctl_clip(&g_ctl, MOCK_CTL_CLIP_PLAY, 2) == MOCK_CTL_CLIP_STATE);
    ae_set_ptt(&g_engine, false);
    for (int i = 0; i < 2; i++) ae_process(&g_engine, NULL, audio, 4800);
    EXPECT("fe fe f0 e0 35 01 fd", OK);
    CHECK(g_clip_starts == 1 && g_clip_op == MOCK_CTL_CLIP_SAVE && g_clip_slot == 1);
    EXPECT("fe fe f0 e0 35 03 fd", OK);
    CHECK(g_clip_starts == 2 && g_clip_slot == 3);

    /* Play: clip 2 is there, clip 1 is not. */
    EXPECT("fe fe f0 e0 31 02 fd", OK);
    CHECK(g_clip_starts == 3 && g_clip_op == MOCK_CTL_CLIP_PLAY && g_clip_slot == 2);
    EXPECT("fe fe f0 e0 31 01 fd", NG);
    CHECK(mock_ctl_clip(&g_ctl, MOCK_CTL_CLIP_PLAY, 1) == MOCK_CTL_CLIP_NOTHING);
    /* Only in the parrot route. */
    EXPECT("fe fe f0 e0 10 01 fd", OK);
    CHECK(mock_ctl_clip(&g_ctl, MOCK_CTL_CLIP_PLAY, 2) == MOCK_CTL_CLIP_STATE);
    EXPECT("fe fe f0 e0 35 02 fd", OK); /* keeping the recording needs no route */
    EXPECT("fe fe f0 e0 10 02 fd", OK);

    /* Erase, whatever is there. */
    EXPECT("fe fe f0 e0 36 02 fd", OK);
    CHECK(g_clip_op == MOCK_CTL_CLIP_ERASE && g_clip_slot == 2);
    EXPECT("fe fe f0 e0 36 01 fd", OK);

    /* Clips that are not there, and malformed requests. */
    int starts = g_clip_starts;
    EXPECT("fe fe f0 e0 35 00 fd", NG);
    EXPECT("fe fe f0 e0 35 04 fd", NG);
    EXPECT("fe fe f0 e0 31 0a fd", NG);
    EXPECT("fe fe f0 e0 36 fd", NG);
    EXPECT("fe fe f0 e0 35 01 01 fd", NG);
    EXPECT("fe fe f0 e0 37 01 fd", NG);
    CHECK(mock_ctl_clip(&g_ctl, MOCK_CTL_CLIP_ERASE, 4) == MOCK_CTL_CLIP_NONE);
    CHECK(mock_ctl_clip(&g_ctl, (mock_ctl_clip_op_t)9, 1) == MOCK_CTL_CLIP_NONE);

    /* One at a time: while one is under way, or if the task will not take
     * another, it is refused. The status shows it, and a failure. */
    g_clips.busy = true;
    EXPECT("fe fe f0 e0 36 01 fd", NG);
    CHECK(mock_ctl_clip(&g_ctl, MOCK_CTL_CLIP_SAVE, 1) == MOCK_CTL_CLIP_BUSY);
    EXPECT("fe fe f0 e0 37 fd", "fe fe e0 f0 37 01 03 00 00 01 50 00 00 fd");
    g_clips.busy = false;
    g_clips.failed = true;
    g_clips.length_ms[0] = 120000;
    g_clips.length_ms[2] = 49; /* rounds to nothing */
    EXPECT("fe fe f0 e0 37 fd", "fe fe e0 f0 37 02 03 12 00 01 50 00 00 fd");
    g_clip_refuse = true;
    CHECK(mock_ctl_clip(&g_ctl, MOCK_CTL_CLIP_ERASE, 1) == MOCK_CTL_CLIP_BUSY);
    CHECK(g_clip_starts == starts);

    /* All eight, the most there can be; more than that is not believed. */
    g_clips.slots = 12;
    g_clips.failed = false;
    mock_ctl_clips_status_t st;
    CHECK(mock_ctl_clips_status(&g_ctl, &st) && st.slots == MOCK_CTL_CLIPS_MAX);
    send("fe fe f0 e0 37 fd");
    CHECK(g_out_len == 5 + 2 + 16 + 1 && g_out[6] == 0x08);
}

static int g_rig_changes;
static const civ_rig_t *g_rig_now;

static void rig_changed(void *ctx, const civ_rig_t *rig)
{
    (void)ctx;
    g_rig_changes++;
    g_rig_now = rig;
}

static void test_personality(void)
{
    static const civ_rig_t *const rigs[] = {&rig_ic705, &rig_g90};

    /* Without a list of personalities it can be read and not set. */
    fresh();
    EXPECT("fe fe f0 e0 60 fd", "fe fe e0 f0 60 30 85 fd");
    EXPECT("fe fe f0 e0 60 30 88 fd", NG);
    CHECK(mock_ctl_rig(&g_ctl) == NULL && !mock_ctl_select_rig(&g_ctl, 3088));

    g_rig_changes = 0;
    mock_ctl_set_rigs(&g_ctl, &g_civ, rigs, 2, rig_changed, NULL);
    CHECK(mock_ctl_rig(&g_ctl) == &rig_ic705);
    EXPECT("fe fe a4 e0 05 00 00 20 44 01 fd", "fe fe e0 a4 fb fd"); /* 144.2 MHz, an IC-705 frequency */
    EXPECT("fe fe f0 e0 12 01 25 01 fd", OK);                        /* and a setting of the mock's */

    /* To the G90: a different radio answers, from its power-up state. */
    EXPECT("fe fe f0 e0 60 30 88 fd", OK);
    CHECK(g_rig_changes == 1 && g_rig_now == &rig_g90 && mock_ctl_rig(&g_ctl) == &rig_g90);
    EXPECT("fe fe f0 e0 60 fd", "fe fe e0 f0 60 30 88 fd");
    EXPECT("fe fe f0 e0 00 fd", "fe fe e0 f0 00 01 30 88 fd");
    EXPECT("fe fe a4 e0 19 00 fd", "fe fe e0 a4 19 00 70 fd");
    EXPECT("fe fe a4 e0 03 fd", "fe fe e0 a4 03 00 40 07 14 00 fd");
    EXPECT("fe fe a4 e0 05 00 00 20 44 01 fd", "fe fe e0 a4 fa fd");
    /* The mock's own settings are not the radio's: they stay. */
    CHECK(near(config().snr_db, -12.5f, 0.001f));

    /* The same again changes nothing and resets nothing. */
    EXPECT("fe fe a4 e0 05 00 40 07 07 00 fd", "fe fe e0 a4 fb fd");
    EXPECT("fe fe f0 e0 60 30 88 fd", OK);
    CHECK(g_rig_changes == 1);
    EXPECT("fe fe a4 e0 03 fd", "fe fe e0 a4 03 00 40 07 07 00 fd");

    /* Models the mock does not have, and malformed requests. */
    EXPECT("fe fe f0 e0 60 30 73 fd", NG);
    EXPECT("fe fe f0 e0 60 00 00 fd", NG);
    EXPECT("fe fe f0 e0 60 30 fd", NG);
    EXPECT("fe fe f0 e0 60 30 88 00 fd", NG);
    EXPECT("fe fe f0 e0 60 3a 88 fd", NG);
    CHECK(g_rig_changes == 1 && mock_ctl_rig(&g_ctl) == &rig_g90);

    /* Keyed over CAT when the radio changes: the new one is receiving. */
    EXPECT("fe fe a4 e0 1c 00 01 fd", "fe fe e0 a4 fb fd");
    CHECK(civ_ptt(&g_civ));
    EXPECT("fe fe f0 e0 60 30 85 fd", OK);
    CHECK(!civ_ptt(&g_civ) && g_rig_changes == 2 && g_rig_now == &rig_ic705);
    EXPECT("fe fe a4 e0 19 00 fd", "fe fe e0 a4 19 00 a4 fd");

    /* Reset is for the settings; the radio stays what it is. */
    CHECK(mock_ctl_select_rig(&g_ctl, 3088) && g_rig_changes == 3);
    EXPECT("fe fe f0 e0 01 fd", OK);
    CHECK(mock_ctl_rig(&g_ctl) == &rig_g90 && near(config().snr_db, 10.0f, 0.001f));
    CHECK(!mock_ctl_select_rig(&g_ctl, 3073) && mock_ctl_rig(&g_ctl) == &rig_g90);

    /* The new radio starts in receive: its front panel's key is released.
     * Choosing the radio it already is changes nothing. */
    ae_ptt_t ptt;
    ae_set_panel_ptt(&g_engine, true);
    CHECK(mock_ctl_select_rig(&g_ctl, 3088));
    ae_get_ptt(&g_engine, &ptt);
    CHECK(ptt.panel);
    CHECK(mock_ctl_select_rig(&g_ctl, 3085));
    ae_get_ptt(&g_engine, &ptt);
    CHECK(!ptt.panel);
}

static void test_not_the_radio(void)
{
    fresh();
    /* Reserved for parts that do not exist yet, and numbers never assigned. */
    EXPECT("fe fe f0 e0 31 fd", NG);
    EXPECT("fe fe f0 e0 31 01 fd", NG);
    EXPECT("fe fe f0 e0 43 fd", NG);
    EXPECT("fe fe f0 e0 55 fd", NG);
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
    test_parrot();
    test_ptt_sources();
    test_fading_settings();
    test_link_settings();
    test_kept_settings();
    test_clips();
    test_personality();
    test_not_the_radio();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
