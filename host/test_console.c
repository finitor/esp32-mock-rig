/*
 * Unit tests for console_api: the web console's settings and telemetry as
 * JSON, over the real engine and the mock's own controls.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine.h"
#include "cat_civ.h"
#include "console_api.h"
#include "mock_ctl.h"
#include "rig_g90.h"
#include "rig_ic705.h"

static int g_failed;
static int g_checks;

static ae_t g_engine;
static mock_ctl_t g_ctl;
static civ_t g_civ;
static int16_t g_parrot[2 * AE_PARROT_FS_HZ];
static char g_buf[CONSOLE_JSON_MAX];
static char g_err[CONSOLE_ERR_MAX];
static uint8_t g_out[128];
static size_t g_out_len;

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

static void check_str_at(const char *got, const char *want, const char *file, int line)
{
    g_checks++;
    if (strcmp(got, want) != 0) {
        fail(file, line, "text differs");
        printf("     want: %s\n     got:  %s\n", want, got);
    }
}
#define CHECK_STR(got, want) check_str_at((got), (want), __FILE__, __LINE__)

static void check_has_at(const char *got, const char *part, const char *file, int line)
{
    g_checks++;
    if (!strstr(got, part)) {
        fail(file, line, "text is missing a part");
        printf("     part: %s\n     in:   %s\n", part, got);
    }
}
#define CHECK_HAS(got, part) check_has_at((got), (part), __FILE__, __LINE__)

/* ---- a strict JSON checker, so the browser is not the first to parse it ---- */

static const char *jv_value(const char *p);

static const char *jv_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *jv_string(const char *p)
{
    if (*p != '"') return NULL;
    for (p++; *p != '"'; p++) {
        if ((unsigned char)*p < 0x20) return NULL;
        if (*p == '\\') {
            p++;
            if (*p == 'u') {
                for (int i = 1; i <= 4; i++) {
                    if (!strchr("0123456789abcdefABCDEF", p[i]) || !p[i]) return NULL;
                }
                p += 4;
            } else if (!*p || !strchr("\"\\/bfnrt", *p)) {
                return NULL;
            }
        }
    }
    return p + 1;
}

static const char *jv_number(const char *p)
{
    const char *start = p;
    if (*p == '-') p++;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (*p >= '0' && *p <= '9') p++;
    } else {
        return NULL;
    }
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return NULL;
        while (*p >= '0' && *p <= '9') p++;
    }
    return p > start ? p : NULL;
}

static const char *jv_value(const char *p)
{
    p = jv_ws(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        bool object = *p == '{';
        p = jv_ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (object) {
                p = jv_string(jv_ws(p));
                if (!p) return NULL;
                p = jv_ws(p);
                if (*p != ':') return NULL;
                p++;
            }
            p = jv_value(p);
            if (!p) return NULL;
            p = jv_ws(p);
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    }
    if (*p == '"') return jv_string(p);
    if (strncmp(p, "true", 4) == 0) return p + 4;
    if (strncmp(p, "false", 5) == 0) return p + 5;
    if (strncmp(p, "null", 4) == 0) return p + 4;
    return jv_number(p);
}

static bool json_valid(const char *text)
{
    const char *end = jv_value(text);
    return end && *jv_ws(end) == '\0';
}

/* ---- fixtures ---------------------------------------------------------------- */

static void civ_tx(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (n <= sizeof g_out) {
        memcpy(g_out, b, n);
        g_out_len = n;
    }
}

static void fresh_with(bool parrot_buffer)
{
    ae_config_t cfg;
    ae_init(&g_engine, 99u);
    if (parrot_buffer) ae_set_parrot_buffer(&g_engine, g_parrot, sizeof g_parrot / sizeof g_parrot[0]);
    ae_config_defaults(&cfg);
    ae_configure(&g_engine, &cfg);
    mock_ctl_init(&g_ctl, &g_engine, &cfg);
    civ_init(&g_civ, &rig_ic705, civ_tx, NULL, NULL);
    civ_set_private(&g_civ, MOCK_CTL_ADDR, mock_ctl_command, &g_ctl);
}

static void fresh(void)
{
    fresh_with(true);
}

static ae_config_t config(void)
{
    ae_config_t cfg;
    ae_get_config(&g_engine, &cfg);
    return cfg;
}

static console_result_t apply(const char *json)
{
    return console_settings_apply(&g_ctl, json, g_err, sizeof g_err);
}

static const char *settings(void)
{
    size_t n = console_settings_json(&g_ctl, g_buf, sizeof g_buf);
    CHECK(n == strlen(g_buf) && n > 0);
    return g_buf;
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 0.001f;
}

static void tone(int16_t *out, size_t n, float amplitude)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = (int16_t)lrintf(amplitude * sinf(2.0f * 3.14159265f * 1500.0f * (float)i / 48000.0f));
    }
}

#define DEFAULTS                                                                                                  \
    "{\"route\":\"monitor\",\"noise\":\"off\",\"snr\":10.0,\"bw\":2500,\"floor\":-40.0,\"atten\":0.0,"           \
    "\"turnaround\":30,\"fade\":\"off\",\"doppler\":1.00,\"fadedelay\":2.0,\"offset\":0.0,\"delay\":1.0,"      \
    "\"period\":0.0,\"linkbuffer\":30,\"match\":\"on\",\"share\":\"on\",\"line\":\"off\",\"vox\":\"off\","   \
    "\"voxlevel\":-40.0,\"voxhang\":100}"

/* ---- tests ------------------------------------------------------------------- */

static void test_settings_read(void)
{
    fresh();
    CHECK_STR(settings(), DEFAULTS);
    CHECK(json_valid(g_buf));

    /* Too small a buffer gives nothing, not half an object. */
    char small[40] = "x";
    CHECK(console_settings_json(&g_ctl, small, sizeof small) == 0);
    CHECK(small[0] == '\0');
    CHECK(console_settings_json(&g_ctl, small, 0) == 0);
}

static void test_settings_apply(void)
{
    fresh();
    uint32_t gen = ae_config_gen(&g_engine);
    CHECK(apply("{\"noise\":\"on\",\"snr\":-12.5}") == CONSOLE_OK);
    CHECK(g_err[0] == '\0');
    CHECK(config().noise_on && near(config().snr_db, -12.5f));
    CHECK(ae_config_gen(&g_engine) != gen);
    CHECK_HAS(settings(), "\"noise\":\"on\",\"snr\":-12.5,");

    /* Every setting, with white space, in another order. */
    CHECK(apply(" {\n \"voxhang\" : 250 , \"voxlevel\":-30.5,\"vox\":\"on\",\"line\":\"dtr\",\"period\":7.5,"
                "\"delay\":0,\"turnaround\":0,\"atten\":120,\"floor\":-90,\"bw\":3000,\"snr\":80,"
                "\"match\":\"off\",\"linkbuffer\":200,\"noise\":\"off\",\"route\":\"parrot\","
                "\"share\":\"off\",\"offset\":-12.5,\"fadedelay\":7.5,\"doppler\":0.15,\"fade\":\"custom\" }\r\n") ==
          CONSOLE_OK);
    CHECK_STR(settings(),
              "{\"route\":\"parrot\",\"noise\":\"off\",\"snr\":80.0,\"bw\":3000,\"floor\":-90.0,\"atten\":120.0,"
              "\"turnaround\":0,\"fade\":\"custom\",\"doppler\":0.15,\"fadedelay\":7.5,\"offset\":-12.5,\"delay\":0.0,"
              "\"period\":7.5,\"linkbuffer\":200,\"match\":\"off\",\"share\":\"off\",\"line\":\"dtr\",\"vox\":\"on\","
              "\"voxlevel\":-30.5,\"voxhang\":250}");
    ae_config_t cfg = config();
    CHECK(cfg.route == AE_ROUTE_PARROT && cfg.ptt_line == AE_LINE_DTR && cfg.vox_on);
    CHECK(cfg.parrot_period_ms == 7500 && cfg.parrot_delay_ms == 0 && cfg.vox_hang_ms == 250);
    CHECK(cfg.link_buffer_ms == 200 && !cfg.link_match && !cfg.link_coord);
    CHECK(cfg.fade == AE_FADE_CUSTOM && near(cfg.fade_doppler_hz, 0.15f) && near(cfg.fade_delay_ms, 7.5f));
    CHECK(near(cfg.freq_offset_hz, -12.5f));
    CHECK(near(cfg.vox_threshold_dbfs, -30.5f) && near(cfg.noise_floor_dbfs, -90.0f) && near(cfg.ref_bw_hz, 3000.0f));

    /* The other end of each range. */
    CHECK(apply("{\"snr\":-60,\"bw\":100,\"floor\":-12,\"atten\":0,\"turnaround\":5000,\"delay\":60,"
                "\"period\":300,\"voxlevel\":-6,\"voxhang\":5000,\"bw\":24000,\"linkbuffer\":10}") == CONSOLE_OK);
    CHECK_HAS(settings(), "\"snr\":-60.0,\"bw\":24000,\"floor\":-12.0,");
    CHECK_HAS(settings(), "\"delay\":60.0,\"period\":300.0,\"linkbuffer\":10,");
    CHECK_HAS(settings(), "\"voxlevel\":-6.0,\"voxhang\":5000}");

    /* Values are kept in the steps the CI-V controls use: tenths. */
    CHECK(apply("{\"snr\":3.14159,\"delay\":2.26}") == CONSOLE_OK);
    CHECK_HAS(settings(), "\"snr\":3.1,");
    CHECK_HAS(settings(), "\"delay\":2.3,");
    CHECK(apply("{\"snr\":1e1}") == CONSOLE_OK);
    CHECK_HAS(settings(), "\"snr\":10.0,");

    /* An empty object is a change of nothing. */
    gen = ae_config_gen(&g_engine);
    CHECK(apply("{}") == CONSOLE_OK);
    CHECK(apply("  { }  ") == CONSOLE_OK);
    CHECK(ae_config_gen(&g_engine) == gen);
}

static void test_settings_refused(void)
{
    fresh();
    /* Out of range: refused with the range, and nothing else in the request
     * is applied either. */
    CHECK(apply("{\"noise\":\"on\",\"snr\":80.1}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "snr: must be between -60.0 and 80.0 dB");
    CHECK_STR(settings(), DEFAULTS);
    CHECK(apply("{\"snr\":-60.1}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"floor\":-11.9}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "floor: must be between -90.0 and -12.0 dBFS");
    CHECK(apply("{\"floor\":-90.1}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"floor\":40}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"bw\":99}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "bw: must be between 100 and 24000 Hz");
    CHECK(apply("{\"bw\":24001}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"atten\":-0.1}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"atten\":120.1}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"turnaround\":5001}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"delay\":60.1}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "delay: must be between 0.0 and 60.0 s");
    CHECK(apply("{\"period\":300.1}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"voxlevel\":-5.9}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "voxlevel: must be between -80.0 and -6.0 dBFS");
    CHECK(apply("{\"voxlevel\":-80.1}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"voxhang\":5001}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"linkbuffer\":9}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "linkbuffer: must be between 10 and 200 ms");
    CHECK(apply("{\"linkbuffer\":201}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"doppler\":30.01}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "doppler: must be between 0.01 and 30.00 Hz");
    CHECK(apply("{\"doppler\":0}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"fadedelay\":10.1}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"offset\":-200.1}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "offset: must be between -200.0 and 200.0 Hz");
    CHECK(apply("{\"fade\":\"storm\"}") == CONSOLE_BAD_REQUEST);
    CHECK_HAS(g_err, "fade: must be one of \"off\", \"mpg\", \"mpp\",");
    CHECK(apply("{\"match\":\"yes\"}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"snr\":1e30}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"snr\":-1e300}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(settings(), DEFAULTS);

    /* Choices. */
    CHECK(apply("{\"route\":\"loop\"}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "route: must be one of \"monitor\", \"radio\", \"parrot\", \"link\"");
    CHECK(apply("{\"route\":1}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"noise\":true}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "noise: must be one of \"off\", \"on\"");
    CHECK(apply("{\"noise\":\"ON\"}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"line\":\"cts\"}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"vox\":null}") == CONSOLE_BAD_REQUEST);

    /* Wrong type, unknown names. */
    CHECK(apply("{\"snr\":\"10\"}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "snr: must be a number");
    CHECK(apply("{\"snr\":null}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"colour\":1}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "colour: no such setting");
    CHECK(apply("{\"<script>\":1}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(g_err, "no such setting");
    CHECK(apply("{\"\":1}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"SNR\":1}") == CONSOLE_BAD_REQUEST);

    /* Not a flat JSON object. */
    static const char *const bad[] = {
        "",
        "   ",
        "snr=10",
        "[]",
        "\"snr\"",
        "{",
        "{\"snr\"",
        "{\"snr\":",
        "{\"snr\":10",
        "{\"snr\":10,",
        "{\"snr\":10,}",
        "{,\"snr\":10}",
        "{\"snr\" 10}",
        "{snr:10}",
        "{'snr':10}",
        "{\"snr\":10}x",
        "{\"snr\":10}{}",
        "{\"snr\":10 \"noise\":\"on\"}",
        "{\"snr\":+10}",
        "{\"snr\":.5}",
        "{\"snr\":0x10}",
        "{\"snr\":10dB}",
        "{\"snr\":1.2.3}",
        "{\"snr\":-}",
        "{\"snr\":nan}",
        "{\"snr\":inf}",
        "{\"snr\":truex}",
        "{\"snr\":{\"db\":10}}",
        "{\"snr\":[10]}",
        "{\"route\":\"radio}",
        "{\"route\":\"ra\\u0064io\"}",
        "{\"route\":\"radio\\\"}",
        "{\"route\":\"ra\ndio\"}",
        "{\"snr\":11111111111111111111111111111111111111}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        g_checks++;
        if (apply(bad[i]) != CONSOLE_BAD_REQUEST || g_err[0] == '\0') {
            fail(__FILE__, __LINE__, "malformed request accepted");
            printf("     request: %s\n", bad[i]);
        }
    }
    CHECK_HAS(g_err, "expected a JSON object of settings");
    CHECK_STR(settings(), DEFAULTS);

    /* A valid setting after a refused one, and before it: still nothing. */
    CHECK(apply("{\"atten\":6,\"route\":\"radio\",\"voxhang\":9999}") == CONSOLE_BAD_REQUEST);
    CHECK(apply("{\"voxhang\":9999,\"atten\":6}") == CONSOLE_BAD_REQUEST);
    CHECK_STR(settings(), DEFAULTS);

    /* The same name twice: the last one counts. */
    CHECK(apply("{\"atten\":6,\"atten\":3}") == CONSOLE_OK);
    CHECK(near(config().atten_db, 3.0f));

    /* The error text fits a short buffer, and may be left out. */
    char tiny[8];
    CHECK(console_settings_apply(&g_ctl, "{\"snr\":99}", tiny, sizeof tiny) == CONSOLE_BAD_REQUEST);
    CHECK(strlen(tiny) < sizeof tiny);
    CHECK(console_settings_apply(&g_ctl, "{\"snr\":99}", NULL, 0) == CONSOLE_BAD_REQUEST);
    CHECK(console_settings_apply(&g_ctl, "{\"nope\":1}", NULL, 0) == CONSOLE_BAD_REQUEST);
    CHECK(console_settings_apply(&g_ctl, "nope", NULL, 0) == CONSOLE_BAD_REQUEST);
}

static void no_link_tx(void *ctx, const int16_t *tx, size_t n, bool keyed)
{
    (void)ctx;
    (void)tx;
    (void)n;
    (void)keyed;
}

static bool no_link_rx(void *ctx, int16_t *out, size_t n)
{
    (void)ctx;
    (void)out;
    (void)n;
    return false;
}

static void test_parrot_needs_buffer(void)
{
    fresh_with(false);
    CHECK(apply("{\"atten\":3,\"route\":\"parrot\"}") == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "route: \"parrot\" is not available, this board has no parrot buffer");
    CHECK_STR(settings(), DEFAULTS);
    CHECK(apply("{\"route\":\"radio\"}") == CONSOLE_OK);

    fresh();
    CHECK(apply("{\"route\":\"parrot\"}") == CONSOLE_OK);
    CHECK(config().route == AE_ROUTE_PARROT);

    /* The link route needs a link in the same way. */
    CHECK(apply("{\"route\":\"link\"}") == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "route: \"link\" is not available, this board was built without the link");
    CHECK(config().route == AE_ROUTE_PARROT);
    static const ae_link_t link = {.tx = no_link_tx, .rx = no_link_rx};
    ae_set_link(&g_engine, &link);
    CHECK(apply("{\"route\":\"link\",\"linkbuffer\":50}") == CONSOLE_OK);
    CHECK(config().route == AE_ROUTE_LINK && config().link_buffer_ms == 50);
    CHECK_HAS(settings(), "{\"route\":\"link\",");
}

/* The console and the CI-V controls are two views of the same settings. */
static void test_same_settings_as_civ(void)
{
    static const uint8_t set_snr[] = {0xFE, 0xFE, 0xF0, 0xE0, 0x12, 0x01, 0x25, 0x01, 0xFD}; /* -12.5 dB */
    static const uint8_t read_atten[] = {0xFE, 0xFE, 0xF0, 0xE0, 0x15, 0xFD};
    static const uint8_t atten_is_7_5[] = {0xFE, 0xFE, 0xE0, 0xF0, 0x15, 0x00, 0x75, 0xFD};
    static const uint8_t reset[] = {0xFE, 0xFE, 0xF0, 0xE0, 0x01, 0xFD};

    fresh();
    civ_feed(&g_civ, set_snr, sizeof set_snr);
    CHECK_HAS(settings(), "\"snr\":-12.5,");

    CHECK(apply("{\"atten\":7.5}") == CONSOLE_OK);
    g_out_len = 0;
    civ_feed(&g_civ, read_atten, sizeof read_atten);
    CHECK(g_out_len == sizeof atten_is_7_5 && memcmp(g_out, atten_is_7_5, g_out_len) == 0);

    /* Either path puts the power-up values back for both. */
    civ_feed(&g_civ, reset, sizeof reset);
    CHECK_STR(settings(), DEFAULTS);
    CHECK(apply("{\"atten\":7.5,\"vox\":\"on\"}") == CONSOLE_OK);
    mock_ctl_reset(&g_ctl);
    CHECK_STR(settings(), DEFAULTS);

    /* The ranges in the list of settings are mock_ctl's own. */
    int32_t lo, hi;
    CHECK(mock_ctl_setting_range(MOCK_CTL_SNR, &lo, &hi) && lo == -600 && hi == 800);
    CHECK(mock_ctl_setting_range(MOCK_CTL_ROUTE, &lo, &hi) && lo == 0 && hi == 3);
    CHECK(!mock_ctl_setting_range(MOCK_CTL_STATUS, &lo, &hi));
    ae_config_t cfg = config();
    CHECK(mock_ctl_setting_put(&g_ctl, &cfg, MOCK_CTL_STATUS, 0) == MOCK_CTL_SET_UNKNOWN);
    CHECK(mock_ctl_setting_put(&g_ctl, &cfg, MOCK_CTL_SNR, 801) == MOCK_CTL_SET_RANGE);
    CHECK(mock_ctl_setting_put(&g_ctl, &cfg, MOCK_CTL_SNR, 800) == MOCK_CTL_SET_OK);
    CHECK(mock_ctl_setting_get(&cfg, MOCK_CTL_SNR) == 800);
    CHECK(near(config().snr_db, 10.0f)); /* only the copy changed */
}

static void test_info(void)
{
    const console_info_t info = {.name = "mockradio-1a2b", .rig = "IC-705", .hamlib_model = 3085};
    fresh();
    size_t n = console_info_json(&g_ctl, &info, g_buf, sizeof g_buf);
    CHECK(n > 0 && n == strlen(g_buf));
    CHECK(json_valid(g_buf));
    CHECK_HAS(g_buf, "{\"api\":1,\"name\":\"mockradio-1a2b\",\"rig\":\"IC-705\",\"hamlib\":3085,\"rigs\":[],"
                     "\"parrot_capacity\":2.0,");
    CHECK_HAS(g_buf, "{\"name\":\"route\",\"label\":\"Receive route\",\"group\":\"Receive\",");
    CHECK_HAS(g_buf, "\"choices\":[\"monitor\",\"radio\",\"parrot\",\"link\"]}");
    CHECK_HAS(g_buf, "{\"name\":\"linkbuffer\",\"label\":\"Buffer\",\"group\":\"Link\",");
    CHECK_HAS(g_buf, "\"unit\":\"ms\",\"min\":10,\"max\":200,\"step\":1}");
    CHECK_HAS(g_buf, "{\"name\":\"snr\",\"label\":\"SNR\",\"group\":\"Receive\",\"unit\":\"dB\",\"min\":-60.0,"
                     "\"max\":80.0,\"step\":0.1}");
    CHECK_HAS(g_buf, "\"unit\":\"Hz\",\"min\":100,\"max\":24000,\"step\":1}");
    CHECK_HAS(g_buf, "\"unit\":\"dBFS\",\"min\":-90.0,\"max\":-12.0,\"step\":0.1}");
    CHECK_HAS(g_buf, "\"unit\":\"s\",\"min\":0.0,\"max\":300.0,\"step\":0.1}");
    CHECK_HAS(g_buf, "\"unit\":\"dBFS\",\"min\":-80.0,\"max\":-6.0,\"step\":0.1}");
    CHECK_HAS(g_buf, "\"choices\":[\"off\",\"rts\",\"dtr\"]}");
    CHECK_HAS(g_buf, "{\"name\":\"voxhang\",\"label\":\"VOX hang\",\"group\":\"PTT\",\"unit\":\"ms\",\"min\":0,"
                     "\"max\":5000,\"step\":1}]}");
    /* Every setting the settings object has is in the list, and no other. */
    static const char *const names[] = {"route",     "noise",  "snr",   "bw",     "floor",      "atten", "turnaround",
                                        "fade",      "doppler", "fadedelay", "offset", "delay", "period",
                                        "linkbuffer", "match",  "share", "line",   "vox",        "voxlevel", "voxhang"};
    CHECK_HAS(g_buf, "{\"name\":\"doppler\",\"label\":\"Doppler spread\",\"group\":\"Path\",");
    CHECK_HAS(g_buf, "\"unit\":\"Hz\",\"min\":0.01,\"max\":30.00,\"step\":0.01}");
    CHECK_HAS(g_buf, "\"choices\":[\"off\",\"mpg\",\"mpp\",\"mpd\",\"mps\",\"nvis-d\",\"lowlat-d\",\"hilat-m\",\"hilat-d\","
                     "\"custom\"],\"labels\":[\"Off\",\"Good: mpg (0.1 Hz, 0.5 ms)\",");
    CHECK_HAS(g_buf, "\"ITU high latitude disturbed: hilat-d (30 Hz, 7 ms)\",\"Custom: the two numbers below\"]}");
    /* A label for every choice of a setting that has labels, and none for the others. */
    size_t labelled = 0;
    for (const char *q = g_buf; (q = strstr(q, "\"labels\":[")) != NULL; q++) labelled++;
    CHECK(labelled == 1);
    size_t listed = 0;
    for (const char *p = g_buf; (p = strstr(p, "{\"name\":\"")) != NULL; p++) listed++;
    CHECK(listed == sizeof names / sizeof names[0]);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char key[40];
        snprintf(key, sizeof key, "{\"name\":\"%s\",", names[i]);
        CHECK_HAS(g_buf, key);
    }
    CHECK(n < CONSOLE_JSON_MAX - 1024); /* room to grow */

    /* A name is escaped; no buffer, no parrot capacity. */
    const console_info_t odd = {.name = "a\"b\\c\n", .rig = NULL, .hamlib_model = 0};
    fresh_with(false);
    CHECK(console_info_json(&g_ctl, &odd, g_buf, sizeof g_buf) > 0);
    CHECK(json_valid(g_buf));
    CHECK_HAS(g_buf, "\"name\":\"a\\\"b\\\\c\\u000a\",\"rig\":\"\",\"hamlib\":0,\"rigs\":[],\"parrot_capacity\":0.0,");
    CHECK(console_info_json(&g_ctl, &odd, g_buf, 100) == 0);
}

/* The radio imitated: listed, shown, and changed. */
static void test_personality(void)
{
    static const civ_rig_t *const rigs[] = {&rig_ic705, &rig_g90};
    const console_info_t info = {.name = "mockradio-1a2b", .rig = "ignored", .hamlib_model = 1};
    console_sample_t s;
    memset(&s, 0, sizeof s);

    fresh();
    /* With no list of personalities there is nothing to change to. */
    CHECK(console_rig_apply(&g_ctl, "{\"hamlib\":3088}", g_err, sizeof g_err) == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "the mock has no personality for Hamlib model 3088");

    mock_ctl_set_rigs(&g_ctl, &g_civ, rigs, 2, NULL, NULL);
    CHECK(console_info_json(&g_ctl, &info, g_buf, sizeof g_buf) > 0);
    CHECK(json_valid(g_buf));
    CHECK_HAS(g_buf, "\"rig\":\"Icom IC-705\",\"hamlib\":3085,\"rigs\":[{\"name\":\"Icom IC-705\",\"hamlib\":3085},"
                     "{\"name\":\"Xiegu G90\",\"hamlib\":3088}],\"parrot_capacity\":");
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK_HAS(g_buf, ",\"hamlib\":3085,\"kept\":false,\"host\":");

    CHECK(apply("{\"atten\":6}") == CONSOLE_OK);
    CHECK(console_rig_apply(&g_ctl, " { \"hamlib\" : 3088 } ", g_err, sizeof g_err) == CONSOLE_OK);
    CHECK(g_err[0] == '\0' && g_civ.rig == &rig_g90);
    CHECK(console_info_json(&g_ctl, &info, g_buf, sizeof g_buf) > 0);
    CHECK_HAS(g_buf, "\"rig\":\"Xiegu G90\",\"hamlib\":3088,\"rigs\":[");
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK_HAS(g_buf, ",\"hamlib\":3088,\"kept\":false,\"host\":");
    CHECK(near(config().atten_db, 6.0f)); /* the mock's settings are not the radio's */

    /* What it refuses, leaving the radio as it is. */
    CHECK(console_rig_apply(&g_ctl, "{\"hamlib\":3073}", g_err, sizeof g_err) == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "the mock has no personality for Hamlib model 3073");
    static const char *const bad[] = {
        "",          "{}",           "3088",       "{\"hamlib\":\"3088\"}", "{\"hamlib\":3088.5}", "{\"hamlib\":-3088}",
        "{\"hamlib\":0}", "{\"hamlib\":1e9}", "{\"model\":3088}", "{\"hamlib\":3088,\"x\":1}", "{\"hamlib\":3088",   "{\"hamlib\":null}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        g_checks++;
        if (console_rig_apply(&g_ctl, bad[i], g_err, sizeof g_err) != CONSOLE_BAD_REQUEST || g_err[0] == '\0') {
            fail(__FILE__, __LINE__, "malformed request accepted");
            printf("     request: %s\n", bad[i]);
        }
    }
    CHECK(g_civ.rig == &rig_g90);
    CHECK(console_rig_apply(&g_ctl, "{\"hamlib\":3085}", NULL, 0) == CONSOLE_OK);
    CHECK(g_civ.rig == &rig_ic705);
}

/* Kept settings and clips, as the page sees and asks for them. */
static mock_ctl_clips_status_t g_clips;
static int g_clip_op = -1;
static unsigned g_clip_slot;

static bool clips_start(void *ctx, mock_ctl_clip_op_t op, unsigned slot)
{
    (void)ctx;
    g_clip_op = (int)op;
    g_clip_slot = slot;
    return true;
}

static void clips_status(void *ctx, mock_ctl_clips_status_t *st)
{
    (void)ctx;
    *st = g_clips;
}

static bool kept_save(void *ctx, const uint8_t *blob, size_t len)
{
    (void)ctx;
    (void)blob;
    return len > 0;
}

static bool kept_forget(void *ctx)
{
    (void)ctx;
    return true;
}

static void test_kept_and_clips(void)
{
    static const mock_ctl_clips_t clips = {.start = clips_start, .status = clips_status};
    static const mock_ctl_store_t store = {.save = kept_save, .forget = kept_forget};
    static int16_t audio[4800];
    console_sample_t s;
    memset(&s, 0, sizeof s);

    fresh();
    CHECK(console_clip_apply(&g_ctl, "{\"op\":\"erase\",\"clip\":1}", g_err, sizeof g_err) == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "there is no clip 1 on this board");

    /* Kept settings show in the telemetry. */
    mock_ctl_set_store(&g_ctl, &store);
    CHECK(mock_ctl_keep_settings(&g_ctl));
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK_HAS(g_buf, ",\"kept\":true,\"host\":");
    CHECK(mock_ctl_forget_settings(&g_ctl));
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK_HAS(g_buf, ",\"kept\":false,\"host\":");

    /* Clips: three places, one filled, one operation under way. */
    memset(&g_clips, 0, sizeof g_clips);
    g_clips.slots = 3;
    g_clips.length_ms[1] = 15040;
    g_clips.busy = true;
    mock_ctl_set_clips(&g_ctl, &clips);
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK(json_valid(g_buf));
    CHECK_HAS(g_buf, "\"clips\":{\"busy\":true,\"failed\":false,\"seconds\":[0.0,15.0,0.0]},\"link\":");
    CHECK(console_clip_apply(&g_ctl, "{\"op\":\"erase\",\"clip\":2}", g_err, sizeof g_err) == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "another clip operation is still under way");
    g_clips.busy = false;
    g_clips.failed = true;
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK_HAS(g_buf, "\"clips\":{\"busy\":false,\"failed\":true,");

    /* Each operation, and why it may not be begun. */
    CHECK(console_clip_apply(&g_ctl, "{\"op\":\"save\",\"clip\":1}", g_err, sizeof g_err) == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "there is no recording to keep: transmit in the parrot route first");
    CHECK(console_clip_apply(&g_ctl, "{\"op\":\"play\",\"clip\":1}", g_err, sizeof g_err) == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "that clip is empty");
    CHECK(console_clip_apply(&g_ctl, "{\"op\":\"play\",\"clip\":2}", g_err, sizeof g_err) == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "a clip plays only in the parrot route, with PTT off");
    CHECK(console_clip_apply(&g_ctl, "{\"op\":\"play\",\"clip\":4}", g_err, sizeof g_err) == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "there is no clip 4 on this board");
    CHECK(g_clip_op == -1);
    CHECK(apply("{\"route\":\"parrot\"}") == CONSOLE_OK);
    CHECK(console_clip_apply(&g_ctl, " {\"clip\":2, \"op\":\"play\"} ", g_err, sizeof g_err) == CONSOLE_OK);
    CHECK(g_err[0] == '\0' && g_clip_op == MOCK_CTL_CLIP_PLAY && g_clip_slot == 2);
    CHECK(console_clip_apply(&g_ctl, "{\"op\":\"erase\",\"clip\":3}", NULL, 0) == CONSOLE_OK);
    CHECK(g_clip_op == MOCK_CTL_CLIP_ERASE && g_clip_slot == 3);

    /* With a recording: keeping it is begun, but not while it is being made. */
    ae_set_ptt(&g_engine, true);
    tone(audio, 4800, 8000.0f);
    for (int i = 0; i < 3; i++) ae_process(&g_engine, audio, audio, 4800), tone(audio, 4800, 8000.0f);
    CHECK(console_clip_apply(&g_ctl, "{\"op\":\"save\",\"clip\":1}", g_err, sizeof g_err) == CONSOLE_UNAVAILABLE);
    CHECK_STR(g_err, "the parrot is still recording");
    ae_set_ptt(&g_engine, false);
    for (int i = 0; i < 2; i++) ae_process(&g_engine, NULL, audio, 4800);
    CHECK(console_clip_apply(&g_ctl, "{\"op\":\"save\",\"clip\":1}", g_err, sizeof g_err) == CONSOLE_OK);
    CHECK(g_clip_op == MOCK_CTL_CLIP_SAVE && g_clip_slot == 1);

    /* Malformed. */
    static const char *const bad[] = {
        "",
        "{}",
        "{\"op\":\"save\"}",
        "{\"clip\":1}",
        "{\"op\":\"keep\",\"clip\":1}",
        "{\"op\":1,\"clip\":1}",
        "{\"op\":\"save\",\"clip\":\"1\"}",
        "{\"op\":\"save\",\"clip\":0}",
        "{\"op\":\"save\",\"clip\":1.5}",
        "{\"op\":\"save\",\"clip\":-1}",
        "{\"op\":\"save\",\"clip\":1,\"x\":2}",
        "{\"op\":\"save\",\"clip\":1",
    };
    g_clip_op = -1;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        g_checks++;
        if (console_clip_apply(&g_ctl, bad[i], g_err, sizeof g_err) != CONSOLE_BAD_REQUEST || g_err[0] == '\0') {
            fail(__FILE__, __LINE__, "malformed request accepted");
            printf("     request: %s\n", bad[i]);
        }
    }
    CHECK(g_clip_op == -1);

    /* Eight clips at the longest still fit the snapshot's room. */
    g_clips.slots = 8;
    for (int i = 0; i < 8; i++) g_clips.length_ms[i] = 120000;
    s.has_link = true;
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) < 2048 - 300);
    CHECK(json_valid(g_buf));
}

static void test_json_string(void)
{
    char out[16];
    const char *form = "{\"ssid\":\"Shack 2.4\",\"password\":\"a\\\"b\\\\c\\/d\",\"n\":3}";
    CHECK(console_json_string(form, "ssid", out, sizeof out));
    CHECK_STR(out, "Shack 2.4");
    CHECK(console_json_string(form, "password", out, sizeof out));
    CHECK_STR(out, "a\"b\\c/d");
    CHECK(!console_json_string(form, "n", out, sizeof out));    /* not a string */
    CHECK(!console_json_string(form, "name", out, sizeof out)); /* not there */
    CHECK(!console_json_string(form, "ssi", out, sizeof out));
    CHECK(console_json_string("{\"ssid\":\"\"}", "ssid", out, sizeof out));
    CHECK_STR(out, "");
    CHECK(console_json_string("{\"ssid\":\"123456789012345\"}", "ssid", out, sizeof out)); /* fits exactly */
    CHECK(!console_json_string("{\"ssid\":\"1234567890123456\"}", "ssid", out, sizeof out));
    CHECK(!console_json_string("{\"ssid\":\"x\"", "ssid", out, sizeof out)); /* malformed after the member */
    CHECK(!console_json_string("{\"ssid\":\"x\"} trailing", "ssid", out, sizeof out));
    CHECK(!console_json_string("", "ssid", out, sizeof out));
    CHECK(console_json_string("{\"p\":\"tab\\there\\n\"}", "p", out, sizeof out));
    CHECK_STR(out, "tab\there\n");
    /* UTF-8 passes through as it is. */
    CHECK(console_json_string("{\"ssid\":\"caf\xc3\xa9\"}", "ssid", out, sizeof out));
    CHECK_STR(out, "caf\xc3\xa9");

    CHECK(console_error_json("snr: must be \"x\"", g_buf, sizeof g_buf) > 0);
    CHECK_STR(g_buf, "{\"error\":\"snr: must be \\\"x\\\"\"}");
    CHECK(json_valid(g_buf));
    CHECK(console_error_json(NULL, g_buf, sizeof g_buf) > 0);
    CHECK_STR(g_buf, "{\"error\":\"\"}");

    static const char *const keys[] = {"name", "ssid", "ip"};
    static const char *const values[] = {"mockradio-1a2b", "The \"shack\"", NULL};
    CHECK(console_strings_json(keys, values, 3, g_buf, sizeof g_buf) > 0);
    CHECK_STR(g_buf, "{\"name\":\"mockradio-1a2b\",\"ssid\":\"The \\\"shack\\\"\",\"ip\":\"\"}");
    CHECK(json_valid(g_buf));
    CHECK(console_strings_json(keys, values, 0, g_buf, sizeof g_buf) == 2);
    CHECK(console_strings_json(keys, values, 3, g_buf, 20) == 0);
}

static void test_meter(void)
{
    static int16_t audio[CONSOLE_METER_WINDOW];
    console_meter_t m;
    console_level_t lvl;
    uint32_t seen = 0;

    console_meter_init(&m);
    console_meter_read(&m, &seen, &lvl);
    CHECK(!lvl.active);

    /* Half-scale sine: peak -6.02 dBFS, RMS 3.01 dB below that. */
    tone(audio, CONSOLE_METER_WINDOW, 16384.0f);
    console_meter_add(&m, audio, CONSOLE_METER_WINDOW - 1);
    console_meter_read(&m, &seen, &lvl);
    CHECK(!lvl.active); /* the window is not full yet */
    console_meter_add(&m, audio, 1);
    console_meter_read(&m, &seen, &lvl);
    CHECK(lvl.active);
    CHECK(abs(lvl.rms_cdb - -903) <= 2);
    CHECK(abs(lvl.peak_cdb - -602) <= 2);

    /* Nothing added since: not active, though the last level is still there. */
    console_meter_read(&m, &seen, &lvl);
    CHECK(!lvl.active);

    /* In 1 ms pieces, as USB playback delivers it, then silence. */
    for (size_t i = 0; i < CONSOLE_METER_WINDOW; i += 48) console_meter_add(&m, &audio[i], 48);
    console_meter_read(&m, &seen, &lvl);
    CHECK(lvl.active && abs(lvl.rms_cdb - -903) <= 2);
    console_meter_add(&m, NULL, CONSOLE_METER_WINDOW);
    console_meter_read(&m, &seen, &lvl);
    CHECK(lvl.active && lvl.rms_cdb == -12000 && lvl.peak_cdb == -12000);

    /* A block that spans windows: the level is the newest whole window. */
    console_meter_add(&m, NULL, CONSOLE_METER_WINDOW / 2);
    console_meter_add(&m, audio, CONSOLE_METER_WINDOW);
    console_meter_add(&m, audio, CONSOLE_METER_WINDOW);
    console_meter_read(&m, &seen, &lvl);
    CHECK(lvl.active && abs(lvl.rms_cdb - -903) <= 2);
    console_meter_add(&m, audio, CONSOLE_METER_WINDOW - 1);
    console_meter_add(&m, NULL, 5 * CONSOLE_METER_WINDOW + 1);
    console_meter_read(&m, &seen, &lvl);
    CHECK(lvl.active && lvl.rms_cdb == -12000);
}

static void test_mode_names(void)
{
    char name[16];
    console_mode_name(0x01, false, name, sizeof name);
    CHECK_STR(name, "USB");
    console_mode_name(0x01, true, name, sizeof name);
    CHECK_STR(name, "USB-D");
    console_mode_name(0x00, true, name, sizeof name);
    CHECK_STR(name, "LSB-D");
    console_mode_name(0x08, false, name, sizeof name);
    CHECK_STR(name, "RTTY-R");
    console_mode_name(0x17, false, name, sizeof name);
    CHECK_STR(name, "DV");
    console_mode_name(0x3c, false, name, sizeof name);
    CHECK_STR(name, "3C");
    char tiny[3];
    console_mode_name(0x08, true, tiny, sizeof tiny);
    CHECK(strlen(tiny) == 2);
}

static void test_telemetry(void)
{
    static int16_t audio[4800];
    console_sample_t s;
    memset(&s, 0, sizeof s);
    s.uptime_ms = 1234;
    s.freq_hz = 7074000;
    s.tx_freq_hz = 7074000;
    s.mode = 0x01;
    s.data = true;
    s.fifo_target = 960;

    fresh();
    uint32_t gen = ae_config_gen(&g_engine);
    size_t n = console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf);
    CHECK(n > 0 && n == strlen(g_buf));
    CHECK(json_valid(g_buf));
    char want[1024];
    snprintf(want, sizeof want,
             "{\"t\":1234,\"gen\":%lu,\"hamlib\":0,\"kept\":false,\"host\":false,"
             "\"ptt\":{\"on\":false,\"cat\":false,\"line\":false,\"vox\":false},"
             "\"cat\":{\"freq\":7074000,\"txfreq\":7074000,\"mode\":\"USB-D\",\"split\":false,\"ok\":0,\"ng\":0,"
             "\"dropped\":0},"
             "\"audio\":{\"tx\":null,\"rx\":null,"
             "\"fifo\":{\"level\":0,\"target\":960,\"over\":0,\"under\":0,\"dropped\":0,\"repeated\":0}},"
             "\"channel\":{\"muted\":false,\"level\":null,\"gain\":0.0,\"limited\":false},"
             "\"parrot\":{\"state\":\"idle\",\"recorded\":0.0,\"capacity\":2.0,\"truncated\":false},"
             "\"clips\":null,\"link\":null,\"rssi\":null}",
             (unsigned long)gen);
    CHECK_STR(g_buf, want);

    /* A radio at work: keyed by CAT in the parrot route with noise on, a
     * tone going out, everything the sample can carry filled in. */
    CHECK(apply("{\"route\":\"parrot\",\"noise\":\"on\",\"snr\":0}") == CONSOLE_OK);
    ae_set_ptt(&g_engine, true);
    tone(audio, 4800, 8000.0f);
    for (int i = 0; i < 5; i++) ae_process(&g_engine, audio, audio, 4800), tone(audio, 4800, 8000.0f);

    s.host = true;
    s.freq_hz = 14074000;
    s.tx_freq_hz = 14075000;
    s.split = true;
    s.data = false;
    s.frames_ok = 4000000000u;
    s.frames_ng = 2;
    s.frames_dropped = 1;
    s.tx = (console_level_t){.active = true, .rms_cdb = -1206, .peak_cdb = -904};
    s.rx = (console_level_t){.active = true, .rms_cdb = -12000, .peak_cdb = -12000};
    s.fifo_level = 1010;
    s.fifo_overruns = 3;
    s.fifo_underruns = 4;
    s.fifo_dropped = 5;
    s.fifo_repeated = 6;
    s.has_rssi = true;
    s.rssi_dbm = -57;
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK(json_valid(g_buf));
    CHECK_HAS(g_buf, "\"hamlib\":0,\"kept\":false,\"host\":true,\"ptt\":{\"on\":true,\"cat\":true,\"line\":false,\"vox\":false},");
    CHECK_HAS(g_buf, "\"cat\":{\"freq\":14074000,\"txfreq\":14075000,\"mode\":\"USB\",\"split\":true,"
                     "\"ok\":4000000000,\"ng\":2,\"dropped\":1},");
    CHECK_HAS(g_buf, "\"audio\":{\"tx\":{\"rms\":-12.1,\"peak\":-9.0},\"rx\":{\"rms\":-120.0,\"peak\":-120.0},");
    CHECK_HAS(g_buf, "\"fifo\":{\"level\":1010,\"target\":960,\"over\":3,\"under\":4,\"dropped\":5,\"repeated\":6}},");
    CHECK_HAS(g_buf, "\"channel\":{\"muted\":true,");
    CHECK_HAS(g_buf, "\"parrot\":{\"state\":\"recording\",\"recorded\":0.5,\"capacity\":2.0,\"truncated\":false},");
    CHECK_HAS(g_buf, "\"clips\":null,\"link\":null,\"rssi\":-57}");
    CHECK(ae_config_gen(&g_engine) != gen);

    /* The link: alone, then with a peer and every counter set. */
    s.has_link = true;
    s.link.state = LINK_NO_PEER;
    s.link.target_ms = 30;
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK(json_valid(g_buf));
    CHECK_HAS(g_buf, "\"link\":{\"state\":\"no peer\",\"peer\":null,\"peer_rx\":null,\"peer_tx\":null,"
                     "\"peer_keyed\":false,\"rssi\":null,\"buffer\":0,\"target\":30,\"sent\":0,");
    s.link = (link_status_t){
        .state = LINK_RECEIVING, .peer = true, .peer_name = "mock\"radio-0002", .peer_rx_hz = 7074000,
        .peer_tx_hz = 7076000, .peer_keyed = true, .has_rssi = true, .rssi_dbm = -61, .buffer_ms = 35,
        .target_ms = 30, .sent = 1, .send_failed = 2, .received = 3, .lost = 4, .late = 5, .off_frequency = 6,
        .underruns = 7, .overruns = 8, .dropped = 9, .repeated = 10,
    };
    s.link_shared = true;
    size_t full = console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf);
    CHECK(full > 0 && json_valid(g_buf));
    CHECK_HAS(g_buf, "\"link\":{\"state\":\"receiving\",\"peer\":\"mock\\\"radio-0002\",\"peer_rx\":7074000,"
                     "\"peer_tx\":7076000,\"peer_keyed\":true,\"rssi\":-61,\"buffer\":35,\"target\":30,\"sent\":1,"
                     "\"send_failed\":2,\"received\":3,\"lost\":4,\"late\":5,\"off_frequency\":6,\"underruns\":7,"
                     "\"overruns\":8,\"dropped\":9,\"repeated\":10,\"shared\":true,\"leads\":false},\"rssi\":-57}");
    s.link.state = LINK_OFF_FREQUENCY;
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK_HAS(g_buf, "\"state\":\"off frequency\"");
    CHECK(full < CONSOLE_JSON_MAX / 2 - 512); /* the firmware keeps 2048 for a snapshot */
    s.has_link = false;

    /* Keyed past the end of the 2 s buffer, then unkeyed: waiting to replay. */
    for (int i = 0; i < 20; i++) ae_process(&g_engine, audio, audio, 4800), tone(audio, 4800, 8000.0f);
    ae_set_ptt(&g_engine, false);
    for (int i = 0; i < 2; i++) ae_process(&g_engine, NULL, audio, 4800);
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK_HAS(g_buf, "\"ptt\":{\"on\":false,");
    CHECK_HAS(g_buf, "\"parrot\":{\"state\":\"waiting\",\"recorded\":2.0,\"capacity\":2.0,\"truncated\":true},");

    /* The replay is heard: the channel has a level and a gain to report. */
    for (int i = 0; i < 12; i++) ae_process(&g_engine, NULL, audio, 4800);
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK(json_valid(g_buf));
    CHECK_HAS(g_buf, "\"parrot\":{\"state\":\"playing\",");
    CHECK_HAS(g_buf, "\"channel\":{\"muted\":false,\"level\":-1");
    CHECK(!strstr(g_buf, "\"level\":null"));

    /* PTT sources are told apart. */
    fresh();
    CHECK(apply("{\"line\":\"rts\"}") == CONSOLE_OK);
    ae_set_lines(&g_engine, false, true);
    ae_process(&g_engine, NULL, audio, 480);
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, sizeof g_buf) > 0);
    CHECK_HAS(g_buf, "\"ptt\":{\"on\":true,\"cat\":false,\"line\":true,\"vox\":false},");

    /* Too small a buffer gives nothing. */
    CHECK(console_telemetry_json(&g_ctl, &s, g_buf, 200) == 0);
    CHECK(g_buf[0] == '\0');
    CHECK(n < CONSOLE_JSON_MAX / 2);
}

static void test_cost(void)
{
    static console_cost_t c;
    console_health_t h;

    console_cost_init(&c);
    memset(&h, 0, sizeof h);
    console_cost_read(&c, &h);
    CHECK(!h.has_cost && h.cost_blocks == 0);

    /* 99 blocks of 10 ms: not yet a whole second, so nothing but the counts. */
    for (int i = 0; i < 99; i++) console_cost_add(&c, i == 40 ? 900u : 500u, 480);
    console_cost_read(&c, &h);
    CHECK(!h.has_cost);
    CHECK(h.cost_blocks == 99 && h.cost_block_samples == 480 && h.cost_worst_us == 900);

    /* The hundredth completes it: 504 us on average, 5.0 % of the 10 ms a block lasts. */
    console_cost_add(&c, 500u, 480);
    console_cost_read(&c, &h);
    CHECK(h.has_cost);
    CHECK(h.cost_avg_us == 504 && h.cost_peak_us == 900 && h.cost_load_permille == 50);

    /* The next second is judged afresh; the longest ever stays. */
    for (int i = 0; i < 100; i++) console_cost_add(&c, 2000u, 480);
    console_cost_read(&c, &h);
    CHECK(h.cost_avg_us == 2000 && h.cost_peak_us == 2000 && h.cost_load_permille == 200);
    CHECK(h.cost_worst_us == 2000 && h.cost_blocks == 200);
    for (int i = 0; i < 100; i++) console_cost_add(&c, 100u, 480);
    console_cost_read(&c, &h);
    CHECK(h.cost_avg_us == 100 && h.cost_peak_us == 100 && h.cost_worst_us == 2000);

    /* Blocks of another length, and work slower than the audio it makes. */
    console_cost_init(&c);
    for (int i = 0; i < 1000; i++) console_cost_add(&c, 3000u, 48);
    console_cost_read(&c, &h);
    CHECK(h.has_cost && h.cost_block_samples == 48 && h.cost_load_permille == 3000);

    /* Nothing sensible, but nothing that wraps or divides by zero either. */
    console_cost_init(&c);
    console_cost_add(&c, 123u, 0);
    console_cost_read(&c, &h);
    CHECK(h.cost_blocks == 0);
    for (int i = 0; i < 3; i++) console_cost_add(&c, UINT32_MAX, 100);
    console_cost_add(&c, UINT32_MAX, 100000);
    console_cost_read(&c, &h);
    CHECK(h.has_cost && h.cost_peak_us == UINT32_MAX && h.cost_load_permille == 9999);
}

static void test_health(void)
{
    static console_health_t h;

    /* A board that has measured nothing yet. */
    memset(&h, 0, sizeof h);
    h.uptime_ms = 5000;
    h.heap_free = 200000;
    h.heap_min = 190000;
    h.heap_largest = 100000;
    h.running = 3;
    size_t n = console_health_json(&h, g_buf, sizeof g_buf);
    CHECK(n > 0 && n == strlen(g_buf));
    CHECK(json_valid(g_buf));
    CHECK_STR(g_buf, "{\"t\":5000,\"reset\":null,\"heap\":{\"free\":200000,\"min\":190000,\"largest\":100000},"
                     "\"psram\":null,\"audio\":null,\"wifi\":null,\"cpu\":null,\"running\":3,\"tasks\":null}");

    /* Everything there is. */
    h.reset = "crash (panic)";
    h.has_psram = true;
    h.psram_free = 7000000;
    h.has_cost = true;
    h.cost_blocks = 1234;
    h.cost_block_samples = 480;
    h.cost_avg_us = 610;
    h.cost_peak_us = 850;
    h.cost_worst_us = 1830;
    h.cost_load_permille = 61;
    h.has_wifi = true;
    h.wifi_drops = 2;
    h.n_cores = 2;
    h.busy_permille[0] = 125;
    h.busy_permille[1] = 1000;
    h.has_tasks = true;
    h.tasks_age_ms = 1500;
    h.tasks_took_us = 740;
    h.n_tasks = 2;
    memcpy(h.tasks[0].name, "IDLE0", 6);
    h.tasks[0].stack_free = 1100;
    h.tasks[0].has_cpu = true;
    h.tasks[0].cpu_permille = 875;
    memcpy(h.tasks[1].name, "a \"long\" name!!", CONSOLE_TASK_NAME_MAX); /* fills the field: no zero after it */
    h.tasks[1].stack_free = 96;
    CHECK(console_health_json(&h, g_buf, sizeof g_buf) > 0);
    CHECK(json_valid(g_buf));
    CHECK_STR(g_buf, "{\"t\":5000,\"reset\":\"crash (panic)\",\"heap\":{\"free\":200000,\"min\":190000,\"largest\":100000},"
                     "\"psram\":7000000,"
                     "\"audio\":{\"blocks\":1234,\"block\":480,\"avg\":610,\"max\":850,\"worst\":1830,\"load\":6.1},"
                     "\"wifi\":{\"drops\":2},\"cpu\":[12.5,100.0],\"running\":3,"
                     "\"tasks\":{\"age\":1500,\"took\":740,\"missed\":0,\"list\":["
                     "{\"name\":\"IDLE0\",\"stack\":1100,\"cpu\":87.5},"
                     "{\"name\":\"a \\\"long\\\" name!!\",\"stack\":96,\"cpu\":null}]}}");

    /* One core, and a table read with nothing in it. */
    h.n_cores = 1;
    h.n_tasks = 0;
    h.tasks_missed = 41;
    CHECK(console_health_json(&h, g_buf, sizeof g_buf) > 0);
    CHECK(json_valid(g_buf));
    CHECK_HAS(g_buf, "\"cpu\":[12.5],");
    CHECK_HAS(g_buf, "\"tasks\":{\"age\":1500,\"took\":740,\"missed\":41,\"list\":[]}}");

    /* Counts beyond what the reading holds are not followed off its end. */
    h.n_cores = 7;
    h.n_tasks = CONSOLE_HEALTH_TASKS + 5;
    for (size_t i = 0; i < CONSOLE_HEALTH_TASKS; i++) {
        memset(&h.tasks[i], 0, sizeof h.tasks[i]);
        memset(h.tasks[i].name, 'w', CONSOLE_TASK_NAME_MAX);
        h.tasks[i].stack_free = UINT32_MAX;
        h.tasks[i].has_cpu = true;
        h.tasks[i].cpu_permille = 1000;
    }
    n = console_health_json(&h, g_buf, sizeof g_buf);
    CHECK(n > 0 && json_valid(g_buf));
    CHECK_HAS(g_buf, "\"cpu\":[12.5,100.0],");
    size_t listed = 0;
    for (const char *p = g_buf; (p = strstr(p, "{\"name\":")) != NULL; p++) listed++;
    CHECK(listed == CONSOLE_HEALTH_TASKS);
    CHECK(n < CONSOLE_JSON_MAX - 1000); /* the fullest reading fits the server's buffer, with room */

    /* Too small a buffer gives nothing, not a cut-off reading. */
    CHECK(console_health_json(&h, g_buf, 300) == 0);
    CHECK(g_buf[0] == '\0');
}

int main(void)
{
    test_settings_read();
    test_settings_apply();
    test_settings_refused();
    test_parrot_needs_buffer();
    test_same_settings_as_civ();
    test_info();
    test_personality();
    test_kept_and_clips();
    test_json_string();
    test_meter();
    test_mode_names();
    test_telemetry();
    test_cost();
    test_health();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
