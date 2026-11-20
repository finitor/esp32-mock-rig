/*
 * USB mock radio firmware entry point.
 *
 * The USB serial port feeds the CI-V parser. Audio the host plays goes
 * through a FIFO to the audio engine, which decides what the receiver hears
 * (the transmit audio, nothing, or the parrot's replay of the last
 * transmission from PSRAM), applies the receive channel (mute while keyed,
 * noise at a set SNR, attenuation) and hands the result back as audio the
 * host can record. With the default settings that is a plain loopback. CI-V
 * frames to the mock's own address change the settings at run time. The
 * radio transmits while CAT, a selected serial control line or the VOX keys
 * it, and the RGB LED shows what it is doing. In the link route the transmit
 * audio goes to a second board and that board's comes back as receive
 * audio. A web console over Wi-Fi changes the same settings and shows live
 * telemetry. The radio imitated is an Icom IC-705 or a Xiegu G90, chosen over
 * either control path and remembered in flash, as are the settings when
 * asked, and recordings kept as clips. Before USB starts, a short self-check
 * proves the portable modules run on the target.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "audio_dsp.h"
#include "audio_engine.h"
#include "cat_civ.h"
#include "mock_ctl.h"
#include "rig_g90.h"
#include "rig_ic705.h"
#include "status_led.h"
#include "storage.h"
#include "ui_led.h"
#include "usb_composite.h"
#ifdef CONFIG_MOCK_WEB_CONSOLE
#include "console_api.h"
#include "web_console.h"
#endif
#ifndef CONFIG_MOCK_LINK_NONE
#include "link.h"
#include "link_transport.h"
#define HAVE_LINK 1
#endif

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

/* The CI-V parser's state and the settings have two users: the USB task,
 * which feeds the parser (and through it the mock's own controls), and the
 * web console's task. Each holds this while it works on them. */
static SemaphoreHandle_t s_cat_lock;

static void cat_lock(void *ctx)
{
    (void)ctx;
    if (s_cat_lock) xSemaphoreTake(s_cat_lock, portMAX_DELAY);
}

static void cat_unlock(void *ctx)
{
    (void)ctx;
    if (s_cat_lock) xSemaphoreGive(s_cat_lock);
}

/* ---- the link to a second board ---------------------------------------------
 *
 * The engine hands the link transmit audio and takes receive audio from it,
 * in the audio task. The status task below keeps the link's settings in
 * step with the engine's and lets it announce the board; the frequencies it
 * compares come from the CI-V parser whenever CAT bytes arrive.
 */
#ifdef HAVE_LINK
_Static_assert(AE_LINK_BUFFER_MIN_MS == LINK_BUFFER_MIN_MS && AE_LINK_BUFFER_MAX_MS == LINK_BUFFER_MAX_MS,
               "the engine's range for the link buffer must be the link's");

static link_t s_link;

static void link_tx_op(void *ctx, const int16_t *tx, size_t n, bool keyed)
{
    link_tx(ctx, tx, n, keyed);
}

static bool link_rx_op(void *ctx, int16_t *out, size_t n)
{
    return link_rx(ctx, out, n);
}
#endif

/* ---- the radio imitated ------------------------------------------------------
 *
 * One of these personalities, chosen with the mock's own control 60 or in
 * the web console and kept in flash, so that a board stays the radio it was
 * made. A board that has never been told starts as the build's choice.
 */
static const civ_rig_t *const s_rigs[] = {&rig_ic705, &rig_g90};
#define N_RIGS (sizeof s_rigs / sizeof s_rigs[0])

#define RIG_KEY "rig"

static const civ_rig_t *rig_at_start(void)
{
#ifdef CONFIG_MOCK_RIG_G90
    const civ_rig_t *rig = &rig_g90;
#else
    const civ_rig_t *rig = &rig_ic705;
#endif
    uint16_t model = 0;

    if (storage_get_u16(RIG_KEY, &model)) {
        for (size_t i = 0; i < N_RIGS; i++) {
            if (s_rigs[i]->hamlib_model == model) rig = s_rigs[i];
        }
    }
    return rig;
}

/* Runs on the task that changed the radio: the USB task for CAT, the web
 * console's for the page. A flash write takes a few milliseconds. */
static void rig_changed(void *ctx, const civ_rig_t *rig)
{
    (void)ctx;
    ESP_LOGI(TAG, "now imitating %s (Hamlib model %d)", rig->name, rig->hamlib_model);
    if (!storage_set_u16(RIG_KEY, (uint16_t)rig->hamlib_model)) ESP_LOGW(TAG, "the choice of radio was not stored");
}

/* ---- settings kept for power-up -------------------------------------------------
 *
 * mock_ctl packs and checks them; here they are only put in flash and taken
 * out. Like the choice of radio, written on the task that asked.
 */
#define SETTINGS_KEY "settings"

static bool settings_save(void *ctx, const uint8_t *blob, size_t len)
{
    (void)ctx;
    return storage_set_blob(SETTINGS_KEY, blob, len);
}

static bool settings_forget(void *ctx)
{
    (void)ctx;
    return storage_erase(SETTINGS_KEY);
}

/* Call with the CAT lock held, or before anything else uses the parser. */
static void link_follow_radio(void)
{
#ifdef HAVE_LINK
    link_set_radio(&s_link, (uint32_t)civ_rx_freq(&s_civ), (uint32_t)civ_tx_freq(&s_civ));
#endif
}

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
    /* A read of 1C 00 in these bytes should say the radio is transmitting
     * when the line or the VOX is keying it, not only CAT. */
    ae_ptt_t ptt;
    ae_get_ptt(&s_engine, &ptt);
    cat_lock(NULL);
    civ_set_ptt(&s_civ, ptt.line || ptt.vox);
    civ_feed(&s_civ, bytes, len);
    link_follow_radio(); /* the frequency may have changed */
    cat_unlock(NULL);
}

static void line_state(void *ctx, bool dtr, bool rts)
{
    (void)ctx;
    /* Hosts raise both lines on open. They key the radio only if the PTT
     * line setting selects one. */
    ae_set_lines(&s_engine, dtr, rts);
    ESP_LOGI(TAG, "serial lines: DTR=%d RTS=%d", dtr, rts);
}

/* ---- audio ----------------------------------------------------------------
 *
 * Playback audio arrives 1 ms at a time on the host's clock; capture audio
 * is fetched 10 ms at a time on this board's clock. A FIFO sits between
 * them, held near LOOP_TARGET samples by drift steering. Nothing is taken
 * from it until it first reaches the target, and it re-buffers after an
 * underrun, so a stream starting up does not stutter.
 *
 * What the FIFO delivers is the transmit audio. The engine loops it back
 * (monitor route), ignores it (radio route) or records it while keyed and
 * replays it afterwards (parrot route). All of it runs on the capture side,
 * so the parrot records only while the host is also recording.
 */
#define LOOP_CAPACITY  4096
#define LOOP_TARGET    960 /* 20 ms at 48 kHz, measured when capture fetches */
#define LOOP_TOLERANCE 240

static int16_t s_loop_store[LOOP_CAPACITY];
static dsp_fifo_t s_loop;
static bool s_loop_primed;

#ifdef CONFIG_MOCK_WEB_CONSOLE
/* Levels for the web console: what the host plays and what it records. */
static console_meter_t s_tx_meter;
static console_meter_t s_rx_meter;
#endif

static void audio_from_host(void *ctx, const int16_t *samples, size_t n)
{
    (void)ctx;
    dsp_fifo_write(&s_loop, samples, n);
#ifdef CONFIG_MOCK_WEB_CONSOLE
    console_meter_add(&s_tx_meter, samples, n);
#endif
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
    ae_process(&s_engine, have_audio ? samples : NULL, samples, n);
#ifdef CONFIG_MOCK_WEB_CONSOLE
    console_meter_add(&s_rx_meter, samples, n);
#endif
}

/* The parrot records into PSRAM. Without it the parrot route is refused. */
static bool parrot_buffer_start(void)
{
    uint32_t samples = (uint32_t)CONFIG_MOCK_PARROT_SECONDS * AE_PARROT_FS_HZ;
    int16_t *store = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!store) {
        ESP_LOGW(TAG, "parrot: no PSRAM for a %d s buffer, the parrot route is not available",
                 CONFIG_MOCK_PARROT_SECONDS);
        return false;
    }
    ae_set_parrot_buffer(&s_engine, store, samples);
    ESP_LOGI(TAG, "parrot: %d s buffer in PSRAM (%u KB)", CONFIG_MOCK_PARROT_SECONDS,
             (unsigned)(samples * sizeof(int16_t) / 1024));
    return true;
}

static const char *route_name(ae_route_t route)
{
    return route == AE_ROUTE_LINK ? "link" : route == AE_ROUTE_PARROT ? "parrot" : route == AE_ROUTE_RADIO ? "radio" : "monitor";
}

/* The engine as configured at build time (menuconfig). These are the
 * power-up settings; the mock's own CAT controls change them from there and
 * can put them back. */
static void engine_start(void)
{
    ae_config_t cfg;
    ae_init(&s_engine, esp_random());
    bool have_parrot = parrot_buffer_start();
#ifdef HAVE_LINK
    /* The link exists from here on, so the route can be chosen; its
     * transport starts later, once Wi-Fi is up. */
    uint8_t mac[6] = {0};
    char name[LINK_NAME_MAX + 1];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(name, sizeof name, "mockradio-%02x%02x", mac[4], mac[5]); /* as wifi_net names the board */
    link_init(&s_link, name, link_transport_send, NULL);
    const ae_link_t link_ops = {.tx = link_tx_op, .rx = link_rx_op, .ctx = &s_link};
    ae_set_link(&s_engine, &link_ops);
#endif
    ae_config_defaults(&cfg);
#if defined(CONFIG_MOCK_ROUTE_LINK) && defined(HAVE_LINK)
    cfg.route = AE_ROUTE_LINK;
#elif defined(CONFIG_MOCK_ROUTE_PARROT)
    cfg.route = have_parrot ? AE_ROUTE_PARROT : AE_ROUTE_RADIO;
#elif defined(CONFIG_MOCK_ROUTE_RADIO)
    cfg.route = AE_ROUTE_RADIO;
#endif
    (void)have_parrot;
    cfg.turnaround_ms = CONFIG_MOCK_TURNAROUND_MS;
    cfg.parrot_delay_ms = CONFIG_MOCK_PARROT_DELAY_MS;
    cfg.parrot_period_ms = CONFIG_MOCK_PARROT_PERIOD_MS;
#ifdef HAVE_LINK
    cfg.link_buffer_ms = CONFIG_MOCK_LINK_BUFFER_MS;
#ifndef CONFIG_MOCK_LINK_MATCH
    cfg.link_match = false;
#endif
#endif
#if defined(CONFIG_MOCK_PTT_LINE_RTS)
    cfg.ptt_line = AE_LINE_RTS;
#elif defined(CONFIG_MOCK_PTT_LINE_DTR)
    cfg.ptt_line = AE_LINE_DTR;
#endif
#ifdef CONFIG_MOCK_VOX
    cfg.vox_on = true;
#endif
    cfg.vox_threshold_dbfs = (float)CONFIG_MOCK_VOX_THRESHOLD_DBFS;
    cfg.vox_hang_ms = CONFIG_MOCK_VOX_HANG_MS;
#ifdef CONFIG_MOCK_NOISE_ON
    cfg.noise_on = true;
    cfg.snr_db = (float)CONFIG_MOCK_SNR_DB;
    cfg.ref_bw_hz = (float)CONFIG_MOCK_SNR_REF_BW_HZ;
    cfg.noise_floor_dbfs = (float)CONFIG_MOCK_NOISE_FLOOR_DBFS;
#endif
    cfg.atten_db = (float)CONFIG_MOCK_ATTEN_DB;
    ae_configure(&s_engine, &cfg);
    mock_ctl_init(&s_ctl, &s_engine, &cfg);

    /* Settings kept in flash replace the build's, above, as the power-up
     * settings. mock_ctl checks them: what it cannot use it leaves alone. */
    static const mock_ctl_store_t store = {.save = settings_save, .forget = settings_forget};
    uint8_t blob[MOCK_CTL_BLOB_MAX];
    size_t blob_len = storage_get_blob(SETTINGS_KEY, blob, sizeof blob);
    mock_ctl_set_store(&s_ctl, &store);
    if (blob_len) {
        if (mock_ctl_restore(&s_ctl, blob, blob_len)) {
            ae_get_config(&s_engine, &cfg);
            ESP_LOGI(TAG, "power-up settings are the ones kept in flash");
        } else {
            ESP_LOGW(TAG, "the settings kept in flash are damaged: using the build's");
        }
    }

    /* Clips: recordings kept in their own partition. */
    static const mock_ctl_clips_t clips = {.start = storage_clip_begin, .status = storage_clip_status};
    unsigned n_clips = storage_clips_start(&s_engine);
    if (n_clips) {
        mock_ctl_set_clips(&s_ctl, &clips);
        ESP_LOGI(TAG, "room for %u clips of up to 120 s in flash", n_clips);
    }

    /* As in use: the build's values, or the kept ones where there are any. */
    ESP_LOGI(TAG, "receive channel: %s route, %s, attenuation %d dB", route_name(cfg.route),
             cfg.noise_on ? "noise on" : "noise off", (int)cfg.atten_db);
    if (cfg.route == AE_ROUTE_PARROT) {
        ESP_LOGI(TAG, "  parrot replays %u ms after unkeying, period %u ms (0 = off)", (unsigned)cfg.parrot_delay_ms,
                 (unsigned)cfg.parrot_period_ms);
    }
    if (cfg.noise_on) {
        ESP_LOGI(TAG, "  SNR %d dB in %d Hz, noise floor %d dBFS", (int)cfg.snr_db, (int)cfg.ref_bw_hz,
                 (int)cfg.noise_floor_dbfs);
    }
    ESP_LOGI(TAG, "PTT: CAT, control line %s, VOX %s",
             cfg.ptt_line == AE_LINE_RTS ? "RTS" : cfg.ptt_line == AE_LINE_DTR ? "DTR" : "off",
             cfg.vox_on ? "on" : "off");
}

/* ---- status: LED and PTT log ------------------------------------------------
 *
 * A slow task watches the engine and the USB connection. It logs the control
 * line and the VOX keying and unkeying (CAT keying is logged where it
 * happens) and keeps the LED on the colour ui_led picks.
 */
#define STATUS_PERIOD_MS 20

#ifdef CONFIG_MOCK_LED
#if (CONFIG_MOCK_LED_GPIO >= 35 && CONFIG_MOCK_LED_GPIO <= 37) || CONFIG_MOCK_LED_GPIO == 19 || CONFIG_MOCK_LED_GPIO == 20
#error "MOCK_LED_GPIO: GPIO35-37 belong to the octal PSRAM and GPIO19/20 to USB"
#endif
#endif

static void status_task(void *arg)
{
    (void)arg;
    ae_ptt_t was = {0};
    int shown = -1;

    for (;;) {
        ae_ptt_t ptt;
        ae_parrot_status_t parrot;
        ae_get_ptt(&s_engine, &ptt);
        ae_get_parrot_status(&s_engine, &parrot);

        if (ptt.line != was.line) ESP_LOGI(TAG, "PTT %s (control line)", ptt.line ? "on" : "off");
        if (ptt.vox != was.vox) ESP_LOGI(TAG, "PTT %s (VOX)", ptt.vox ? "on" : "off");
        was = ptt;

        bool hearing = false;
#ifdef HAVE_LINK
        /* The link follows the engine's settings, announces the board and
         * notices a peer that has gone. */
        static bool had_peer;
        ae_config_t cfg;
        link_status_t link;
        ae_get_config(&s_engine, &cfg);
        link_configure(&s_link, cfg.route == AE_ROUTE_LINK, cfg.link_buffer_ms, cfg.link_match);
        link_tick(&s_link, (uint32_t)(esp_timer_get_time() / 1000));
        link_get_status(&s_link, &link);
        hearing = link.state == LINK_RECEIVING;
        if (link.peer != had_peer) {
            had_peer = link.peer;
            if (link.peer) {
                ESP_LOGI(TAG, "link: joined with %s", link.peer_name);
            } else {
                ESP_LOGI(TAG, "link: no other board");
            }
        }
#endif

        const ui_led_input_t in = {
            .host = usb_composite_mounted(),
            .transmitting = ptt.cat || ptt.line || ptt.vox || parrot.state == AE_PARROT_RECORDING,
            .replay_waiting = parrot.state == AE_PARROT_WAITING,
            .replaying = parrot.state == AE_PARROT_PLAYING || hearing, /* green: audio is coming back */
        };
        int state = (int)ui_led_state(&in);
        if (state != shown) {
            shown = state;
#ifdef CONFIG_MOCK_LED
            ui_rgb_t c = ui_led_color((ui_led_state_t)state, CONFIG_MOCK_LED_BRIGHTNESS);
            status_led_set(c.r, c.g, c.b);
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));
    }
}

static void status_start(void)
{
#ifdef CONFIG_MOCK_LED
    esp_err_t err = status_led_start(CONFIG_MOCK_LED_GPIO);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "status LED on GPIO%d", CONFIG_MOCK_LED_GPIO);
    } else {
        ESP_LOGW(TAG, "status LED on GPIO%d failed to start: %s", CONFIG_MOCK_LED_GPIO, esp_err_to_name(err));
    }
#endif
    if (xTaskCreate(status_task, "status", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "status task failed to start: no LED updates, no line or VOX PTT log");
    }
}

/* ---- web console -----------------------------------------------------------
 *
 * The console's server asks for this five times a second: everything it
 * shows that the engine does not know. The FIFO's counters each belong to
 * one audio task and are read here without a lock; a 32-bit read is whole on
 * this CPU, and a count that is one behind does not matter on a display.
 */
#ifdef CONFIG_MOCK_WEB_CONSOLE
static void console_sample(void *ctx, console_sample_t *out)
{
    static uint32_t tx_seen, rx_seen; /* only the console's task comes here */
    (void)ctx;

    out->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    out->host = usb_composite_mounted();

    cat_lock(NULL);
    const civ_vfo_t *vfo = civ_selected_vfo(&s_civ);
    out->freq_hz = civ_rx_freq(&s_civ);
    out->tx_freq_hz = civ_tx_freq(&s_civ);
    out->mode = vfo->mode;
    out->data = vfo->data;
    out->split = s_civ.split;
    out->frames_ok = s_civ.frames_ok;
    out->frames_ng = s_civ.frames_ng;
    out->frames_dropped = s_civ.frames_dropped;
    cat_unlock(NULL);

    console_meter_read(&s_tx_meter, &tx_seen, &out->tx);
    console_meter_read(&s_rx_meter, &rx_seen, &out->rx);
    out->fifo_level = dsp_fifo_level(&s_loop);
    out->fifo_target = LOOP_TARGET;
    out->fifo_overruns = s_loop.overruns;
    out->fifo_underruns = s_loop.underruns;
    out->fifo_dropped = s_loop.dropped;
    out->fifo_repeated = s_loop.repeated;
#ifdef HAVE_LINK
    out->has_link = true;
    link_get_status(&s_link, &out->link);
#endif
}

static void console_start(void)
{
    const web_console_config_t cfg = {
        .ctl = &s_ctl,
        .lock = cat_lock,
        .unlock = cat_unlock,
        .sample = console_sample,
    };
    esp_err_t err = web_console_start(&cfg);
    if (err != ESP_OK) ESP_LOGE(TAG, "web console failed to start: %s", esp_err_to_name(err));
}
#endif

void app_main(void)
{
    storage_start();
    const civ_rig_t *rig = rig_at_start();
    ESP_LOGI(TAG, "USB mock radio: %s personality (Hamlib model %d)", rig->name, rig->hamlib_model);

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
    status_start();
    s_cat_lock = xSemaphoreCreateMutex();
    if (!s_cat_lock) ESP_LOGW(TAG, "no lock for the CAT state: settings may collide between CAT and the web console");
#ifdef CONFIG_MOCK_WEB_CONSOLE
    console_meter_init(&s_tx_meter);
    console_meter_init(&s_rx_meter);
#endif
    civ_init(&s_civ, rig, cat_tx, cat_ptt, NULL);
    civ_set_private(&s_civ, MOCK_CTL_ADDR, mock_ctl_command, &s_ctl);
    mock_ctl_set_rigs(&s_ctl, &s_civ, s_rigs, N_RIGS, rig_changed, NULL);
    link_follow_radio();
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

    /* After USB, so that nothing about Wi-Fi can keep the radio from
     * appearing to the host. */
#ifdef CONFIG_MOCK_WEB_CONSOLE
    console_start();
#else
    ESP_LOGI(TAG, "web console is switched off (menuconfig)");
#endif

#ifdef HAVE_LINK
    err = link_transport_start(&s_link);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "link to a second board ready over %s; it is used in the link route", link_transport_name());
    } else {
        ESP_LOGE(TAG, "link over %s failed to start: %s", link_transport_name(), esp_err_to_name(err));
    }
#endif
}
