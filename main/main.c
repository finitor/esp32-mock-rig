/*
 * USB mock radio firmware entry point.
 *
 * The USB serial port feeds the CI-V parser. Audio the host plays goes
 * through a FIFO to the audio engine, which applies the receive channel
 * (mute while keyed, noise at a set SNR, attenuation) and hands the result
 * back as audio the host can record. With the default settings that is a
 * plain loopback. CI-V frames to the mock's own address change the channel
 * settings at run time. Before USB starts, a short self-check proves the
 * portable modules run on the target.
 */
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "audio_dsp.h"
#include "audio_engine.h"
#include "cat_civ.h"
#include "mock_ctl.h"
#include "rig_ic705.h"
#include "usb_composite.h"

static const char *TAG = "mock-radio";

static uint8_t s_reply[CIV_MAX_FRAME];
static size_t s_reply_len;

static void capture_tx(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    if (len <= sizeof s_reply) {
        memcpy(s_reply, bytes, len);
        s_reply_len = len;
    }
}

/* Read the frequency over CI-V and compare with the power-up state. */
static bool check_cat(void)
{
    static const uint8_t read_freq[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x03, 0xFD};
    static const uint8_t want[] = {0xFE, 0xFE, 0xE0, 0xA4, 0x03, 0x00, 0x40, 0x07, 0x14, 0x00, 0xFD};
    static civ_t civ;

    civ_init(&civ, &rig_ic705, capture_tx, NULL, NULL);
    s_reply_len = 0;
    civ_feed(&civ, read_freq, sizeof read_freq);
    return s_reply_len == sizeof want && memcmp(s_reply, want, sizeof want) == 0;
}

/* A steady level must come through the 48 -> 12 -> 48 kHz path unchanged. */
static bool check_dsp(void)
{
    enum { N = 480, LEVEL = 10000 };
    static int16_t in[N], mid[N / 4], out[N];
    static dsp_decim4_t down;
    static dsp_interp4_t up;

    for (int i = 0; i < N; i++) in[i] = LEVEL;
    dsp_decim4_init(&down);
    dsp_interp4_init(&up);
    if (dsp_decim4(&down, in, N, mid) != N / 4) return false;
    dsp_interp4(&up, mid, N / 4, out);

    int error = out[N - 1] - LEVEL;
    return error >= -8 && error <= 8;
}

/* ---- CAT over the USB serial port ---------------------------------------- */

static civ_t s_civ;
static ae_t s_engine;
static mock_ctl_t s_ctl;

static void cat_tx(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    usb_composite_serial_write(bytes, len);
}

static void cat_ptt(void *ctx, bool on)
{
    (void)ctx;
    ae_set_ptt(&s_engine, on);
    ESP_LOGI(TAG, "PTT %s (CAT)", on ? "on" : "off");
}

static void serial_rx(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    civ_feed(&s_civ, bytes, len);
}

static void line_state(void *ctx, bool dtr, bool rts)
{
    (void)ctx;
    /* Hosts raise both lines on open. Line keying is a selectable mode that
     * does not exist yet, so this is information only. */
    ESP_LOGI(TAG, "serial lines: DTR=%d RTS=%d (not used for PTT)", dtr, rts);
}

/* ---- audio ----------------------------------------------------------------
 *
 * Playback audio arrives 1 ms at a time on the host's clock; capture audio
 * is fetched 10 ms at a time on this board's clock. A FIFO sits between
 * them, held near LOOP_TARGET samples by drift steering. Nothing is taken
 * from it until it first reaches the target, and it re-buffers after an
 * underrun, so a stream starting up does not stutter.
 *
 * What the FIFO delivers is the receive source in the monitor route. The
 * radio route has no receive source until parrot mode or the board link
 * exists, so the host then gets the engine's noise floor alone.
 */
#define LOOP_CAPACITY  4096
#define LOOP_TARGET    960 /* 20 ms at 48 kHz, measured when capture fetches */
#define LOOP_TOLERANCE 240

static int16_t s_loop_store[LOOP_CAPACITY];
static dsp_fifo_t s_loop;
static bool s_loop_primed;

static void audio_from_host(void *ctx, const int16_t *samples, size_t n)
{
    (void)ctx;
    dsp_fifo_write(&s_loop, samples, n);
}

static void audio_to_host(void *ctx, int16_t *samples, size_t n)
{
    (void)ctx;
    bool have_audio = false;
    if (!s_loop_primed && dsp_fifo_level(&s_loop) >= LOOP_TARGET) s_loop_primed = true;
    if (s_loop_primed) {
        if (dsp_fifo_read_steered(&s_loop, samples, n, LOOP_TARGET, LOOP_TOLERANCE) < n) {
            s_loop_primed = false; /* ran dry: playback stopped or stalled */
        }
        have_audio = true;
    }
    ae_config_t cfg;
    ae_get_config(&s_engine, &cfg);
    bool monitor = cfg.route == AE_ROUTE_MONITOR;
    ae_rx_process(&s_engine, have_audio && monitor ? samples : NULL, samples, n);
}

/* The receive channel as configured at build time (menuconfig). These are
 * the power-up settings; the mock's own CAT controls change them from there
 * and can put them back. */
static void engine_start(void)
{
    ae_config_t cfg;
    ae_init(&s_engine, esp_random());
    ae_config_defaults(&cfg);
#ifdef CONFIG_MOCK_ROUTE_RADIO
    cfg.route = AE_ROUTE_RADIO;
    cfg.turnaround_ms = CONFIG_MOCK_TURNAROUND_MS;
#endif
#ifdef CONFIG_MOCK_NOISE_ON
    cfg.noise_on = true;
    cfg.snr_db = (float)CONFIG_MOCK_SNR_DB;
    cfg.ref_bw_hz = (float)CONFIG_MOCK_SNR_REF_BW_HZ;
    cfg.noise_floor_dbfs = (float)CONFIG_MOCK_NOISE_FLOOR_DBFS;
#endif
    cfg.atten_db = (float)CONFIG_MOCK_ATTEN_DB;
    ae_configure(&s_engine, &cfg);
    mock_ctl_init(&s_ctl, &s_engine, &cfg);

    ESP_LOGI(TAG, "receive channel: %s route, %s, attenuation %d dB",
             cfg.route == AE_ROUTE_RADIO ? "radio" : "monitor",
             cfg.noise_on ? "noise on" : "noise off", CONFIG_MOCK_ATTEN_DB);
    if (cfg.noise_on) {
        ESP_LOGI(TAG, "  SNR %d dB in %d Hz, noise floor %d dBFS", (int)cfg.snr_db, (int)cfg.ref_bw_hz,
                 (int)cfg.noise_floor_dbfs);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "USB mock radio: %s personality (Hamlib model %d)", rig_ic705.name, rig_ic705.hamlib_model);

    size_t psram = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    if (psram) {
        ESP_LOGI(TAG, "PSRAM: %u KB", (unsigned)(psram / 1024));
    } else {
        ESP_LOGW(TAG, "No PSRAM found: parrot mode will have nowhere to record");
    }

    bool cat_ok = check_cat();
    bool dsp_ok = check_dsp();
    ESP_LOGI(TAG, "self-check: CAT %s, DSP %s", cat_ok ? "ok" : "FAILED", dsp_ok ? "ok" : "FAILED");

    engine_start();
    civ_init(&s_civ, &rig_ic705, cat_tx, cat_ptt, NULL);
    civ_set_private(&s_civ, MOCK_CTL_ADDR, mock_ctl_command, &s_ctl);
    ESP_LOGI(TAG, "mock controls answer at CI-V address %02X (host/tools/mockctl.py)", MOCK_CTL_ADDR);
    dsp_fifo_init(&s_loop, s_loop_store, LOOP_CAPACITY);

    const usb_composite_config_t usb = {
        .serial_rx = serial_rx,
        .line_state = line_state,
        .audio_from_host = audio_from_host,
        .audio_to_host = audio_to_host,
    };
    esp_err_t err = usb_composite_init(&usb);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "USB device started on the native USB port: sound card + CAT serial port");
    } else {
        ESP_LOGE(TAG, "USB device failed to start: %s", esp_err_to_name(err));
    }
}
