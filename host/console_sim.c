/*
 * console_sim: the web console's settings and telemetry behind a line
 * protocol, with a simulated radio, so the console's page can be tried in a
 * browser with no ESP32. tools/console_sim.py puts an HTTP server in front
 * of it and serves the page.
 *
 * One command per line on stdin, one reply line on stdout: an HTTP status,
 * a space, and the JSON body.
 *
 *   info | settings | telemetry | reset | replay
 *   apply <json>      change settings
 *   tick <ms>         run the radio for this long (the caller keeps time)
 *   play on|off       the host plays a tone, or stops playing
 *   record on|off     the host records, or stops (on at the start)
 *   civ <hex bytes>   bytes from the host's CAT port; the body is the reply in hex
 *
 * The engine, the controls and the JSON are the firmware's own code. The
 * audio FIFO's numbers are made up.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine.h"
#include "cat_civ.h"
#include "console_api.h"
#include "mock_ctl.h"
#include "rig_ic705.h"

#define BLOCK 240 /* 5 ms, as the firmware's capture side delivers it */
#define PARROT_SECONDS 30

static ae_t g_engine;
static mock_ctl_t g_ctl;
static civ_t g_civ;
static console_meter_t g_tx_meter, g_rx_meter;
static uint32_t g_tx_seen, g_rx_seen;
static int16_t g_parrot[PARROT_SECONDS * AE_PARROT_FS_HZ];
static char g_json[CONSOLE_JSON_MAX];
static uint8_t g_reply[256];
static size_t g_reply_len;
static uint64_t g_samples;
static bool g_playing;
static bool g_recording = true;
static float g_phase;

static void civ_tx(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (g_reply_len + n <= sizeof g_reply) {
        memcpy(&g_reply[g_reply_len], b, n);
        g_reply_len += n;
    }
}

static void on_ptt(void *ctx, bool on)
{
    (void)ctx;
    ae_set_ptt(&g_engine, on);
}

static void run(uint32_t ms)
{
    int16_t tx[BLOCK], rx[BLOCK];
    for (uint64_t end = g_samples + (uint64_t)ms * 48u; g_samples < end; g_samples += BLOCK) {
        if (g_playing) {
            /* 1500 Hz at -12 dBFS peak */
            for (int i = 0; i < BLOCK; i++) {
                tx[i] = (int16_t)lrintf(8231.0f * sinf(g_phase));
                g_phase += 2.0f * 3.14159265f * 1500.0f / 48000.0f;
                if (g_phase > 2.0f * 3.14159265f) g_phase -= 2.0f * 3.14159265f;
            }
            console_meter_add(&g_tx_meter, tx, BLOCK);
        }
        if (g_recording) {
            ae_process(&g_engine, g_playing ? tx : NULL, rx, BLOCK);
            console_meter_add(&g_rx_meter, rx, BLOCK);
        }
    }
}

static void telemetry(void)
{
    console_sample_t s;
    memset(&s, 0, sizeof s);
    s.uptime_ms = (uint32_t)(g_samples / 48u);
    s.host = true;
    s.freq_hz = civ_rx_freq(&g_civ);
    s.tx_freq_hz = civ_tx_freq(&g_civ);
    s.mode = civ_selected_vfo(&g_civ)->mode;
    s.data = civ_selected_vfo(&g_civ)->data;
    s.split = g_civ.split;
    s.frames_ok = g_civ.frames_ok;
    s.frames_ng = g_civ.frames_ng;
    s.frames_dropped = g_civ.frames_dropped;
    console_meter_read(&g_tx_meter, &g_tx_seen, &s.tx);
    console_meter_read(&g_rx_meter, &g_rx_seen, &s.rx);
    s.fifo_target = 960;
    s.fifo_level = g_playing ? 960u + (uint32_t)(rand() % 97) - 48u : 0u;
    s.has_rssi = true;
    s.rssi_dbm = -52 - rand() % 4;
    console_telemetry_json(&g_ctl, &s, g_json, sizeof g_json);
    printf("200 %s\n", g_json);
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void feed_civ(const char *hex)
{
    uint8_t in[128];
    size_t n = 0;
    while (*hex && n < sizeof in) {
        int hi = hexval(hex[0]);
        if (hi < 0) {
            hex++;
            continue;
        }
        int lo = hexval(hex[1]);
        if (lo < 0) break;
        in[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    g_reply_len = 0;
    civ_feed(&g_civ, in, n);
    printf("200 \"");
    for (size_t i = 0; i < g_reply_len; i++) printf("%s%02x", i ? " " : "", g_reply[i]);
    printf("\"\n");
}

static void error(int status, const char *message)
{
    console_error_json(message, g_json, sizeof g_json);
    printf("%d %s\n", status, g_json);
}

int main(void)
{
    static char line[2048];
    const console_info_t info = {.name = "mockradio-sim", .rig = rig_ic705.name, .hamlib_model = rig_ic705.hamlib_model};
    ae_config_t power_up;

    ae_init(&g_engine, 20261119u);
    ae_set_parrot_buffer(&g_engine, g_parrot, sizeof g_parrot / sizeof g_parrot[0]);
    ae_config_defaults(&power_up);
    ae_configure(&g_engine, &power_up);
    mock_ctl_init(&g_ctl, &g_engine, &power_up);
    civ_init(&g_civ, &rig_ic705, civ_tx, on_ptt, NULL);
    civ_set_private(&g_civ, MOCK_CTL_ADDR, mock_ctl_command, &g_ctl);
    console_meter_init(&g_tx_meter);
    console_meter_init(&g_rx_meter);

    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = '\0';
        if (!arg) arg = line + strlen(line);

        if (strcmp(line, "info") == 0) {
            console_info_json(&g_ctl, &info, g_json, sizeof g_json);
            printf("200 %s\n", g_json);
        } else if (strcmp(line, "settings") == 0) {
            console_settings_json(&g_ctl, g_json, sizeof g_json);
            printf("200 %s\n", g_json);
        } else if (strcmp(line, "apply") == 0) {
            char err[CONSOLE_ERR_MAX];
            console_result_t r = console_settings_apply(&g_ctl, arg, err, sizeof err);
            if (r == CONSOLE_OK) {
                console_settings_json(&g_ctl, g_json, sizeof g_json);
                printf("200 %s\n", g_json);
            } else {
                error(r == CONSOLE_UNAVAILABLE ? 409 : 400, err);
            }
        } else if (strcmp(line, "reset") == 0) {
            mock_ctl_reset(&g_ctl);
            console_settings_json(&g_ctl, g_json, sizeof g_json);
            printf("200 %s\n", g_json);
        } else if (strcmp(line, "replay") == 0) {
            if (ae_parrot_replay(&g_engine)) {
                printf("200 {}\n");
            } else {
                error(409, "nothing to replay: it needs the parrot route, a recording and PTT off");
            }
        } else if (strcmp(line, "telemetry") == 0) {
            telemetry();
        } else if (strcmp(line, "tick") == 0) {
            long ms = strtol(arg, NULL, 10);
            if (ms > 0 && ms <= 10000) run((uint32_t)ms);
            printf("200 {}\n");
        } else if (strcmp(line, "play") == 0) {
            g_playing = strcmp(arg, "on") == 0;
            printf("200 {}\n");
        } else if (strcmp(line, "record") == 0) {
            g_recording = strcmp(arg, "on") == 0;
            printf("200 {}\n");
        } else if (strcmp(line, "civ") == 0) {
            feed_civ(arg);
        } else {
            error(404, "no such command");
        }
        fflush(stdout);
    }
    return 0;
}
