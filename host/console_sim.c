/*
 * console_sim: two simulated mock radios behind a line protocol, so the web
 * console's page and the two-board link can be tried in a browser with no
 * ESP32. tools/console_sim.py puts an HTTP server in front of each board
 * and serves the page.
 *
 * One command per line on stdin, one reply line on stdout: an HTTP status,
 * a space, and the JSON body. Commands for one board start with its letter:
 *
 *   a info | a settings | a telemetry | a reset | a replay
 *   a health | a tasks  the board's health; tasks reads the table of tasks first
 *   a apply <json>      change settings
 *   a rig <json>        change the radio imitated: {"hamlib":3088}
 *   a keep | a forget   keep the settings for power-up, or forget them
 *   a clip <json>       a clip operation: {"op":"save","clip":1}
 *   a play on|off       the host plays a tone, or stops playing
 *   a record on|off     the host records, or stops (on at the start)
 *   a civ <hex bytes>   bytes from the host's CAT port; the body is the reply in hex
 *   tick <ms>           run both radios for this long (the caller keeps time)
 *   loss <percent>      how many of the link's packets the path loses
 *
 * The engine, the controls, the link and the JSON are the firmware's own
 * code. The path between the boards delivers at once, the audio FIFO's
 * numbers are made up, and the flash is memory: kept settings and clips last
 * as long as the simulator runs, and a clip operation is done at once.
 *
 * The board's health is made up from end to end, and says so where the page
 * shows why the board last restarted: there is no ESP32 here to measure.
 * Only the layout of the reading is the firmware's.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine.h"
#include "cat_civ.h"
#include "clip_store.h"
#include "console_api.h"
#include "coord.h"
#include "link.h"
#include "mock_ctl.h"
#include "rig_g90.h"
#include "rig_ic705.h"

#define BLOCK 240 /* 5 ms, as the firmware's capture side delivers it */
#define PARROT_SECONDS 30
#define BOARDS 2
#define CLIPS 2

_Static_assert(AE_LINK_BUFFER_MIN_MS == LINK_BUFFER_MIN_MS && AE_LINK_BUFFER_MAX_MS == LINK_BUFFER_MAX_MS,
               "the engine's range for the link buffer must be the link's");

typedef struct {
    int index;
    char name[20];
    uint8_t addr[LINK_ADDR_LEN];
    ae_t engine;
    mock_ctl_t ctl;
    civ_t civ;
    link_t link;
    coord_t coord;
    console_meter_t tx_meter, rx_meter;
    uint32_t tx_seen, rx_seen;
    int16_t parrot[PARROT_SECONDS * AE_PARROT_FS_HZ];
    uint8_t kept[MOCK_CTL_BLOB_MAX];
    size_t kept_len;
    uint8_t flash[CLIPS * CLIP_SLOT_BYTES];
    clip_store_t clips;
    bool clip_failed;
    bool playing;
    bool recording;
    float phase;
    bool tasks_read;
    uint64_t tasks_at;
} board_t;

static board_t g_board[BOARDS];
static char g_json[CONSOLE_JSON_MAX];
static uint8_t g_reply[256];
static size_t g_reply_len;
static uint64_t g_samples;
static int g_loss_percent;

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
    board_t *b = ctx;
    ae_set_ptt(&b->engine, on);
}

/* The path between the boards: there at once, or lost. */
static bool path_send(void *ctx, const uint8_t *dst, const uint8_t *data, size_t len)
{
    board_t *from = ctx;
    for (int i = 0; i < BOARDS; i++) {
        board_t *to = &g_board[i];
        if (to == from || (dst && memcmp(dst, to->addr, LINK_ADDR_LEN) != 0)) continue;
        if (rand() % 100 < g_loss_percent) continue;
        link_receive(&to->link, from->addr, data, len, -48 - rand() % 3);
    }
    return true;
}

/* ---- the pretend flash: kept settings and clips ------------------------------ */

static bool kept_save(void *ctx, const uint8_t *blob, size_t len)
{
    board_t *b = ctx;
    if (len > sizeof b->kept) return false;
    memcpy(b->kept, blob, len);
    b->kept_len = len;
    return true;
}

static bool kept_forget(void *ctx)
{
    board_t *b = ctx;
    b->kept_len = 0;
    return true;
}

static bool flash_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    board_t *b = ctx;
    if (off > sizeof b->flash || len > sizeof b->flash - off) return false;
    memcpy(buf, &b->flash[off], len);
    return true;
}

static bool flash_write(void *ctx, uint32_t off, const void *buf, size_t len)
{
    board_t *b = ctx;
    if (off > sizeof b->flash || len > sizeof b->flash - off) return false;
    memcpy(&b->flash[off], buf, len);
    return true;
}

static bool flash_erase(void *ctx, uint32_t off, uint32_t len)
{
    board_t *b = ctx;
    if (off > sizeof b->flash || len > sizeof b->flash - off) return false;
    memset(&b->flash[off], 0xFF, len);
    return true;
}

static bool clips_start(void *ctx, mock_ctl_clip_op_t op, unsigned slot)
{
    board_t *b = ctx;
    /* No audio runs while a command is handled, so there is nothing to wait for. */
    b->clip_failed = !clip_run(&b->clips, &b->engine, (clip_op_t)op, slot, NULL, NULL);
    return true;
}

static void clips_status(void *ctx, mock_ctl_clips_status_t *st)
{
    board_t *b = ctx;
    memset(st, 0, sizeof *st);
    st->slots = clip_store_slots(&b->clips);
    st->failed = b->clip_failed;
    for (unsigned i = 0; i < st->slots; i++) {
        st->length_ms[i] = clip_store_length(&b->clips, i + 1) / (CLIP_FS_HZ / 1000u);
    }
}

static void link_tx_op(void *ctx, const int16_t *tx, size_t n, bool keyed)
{
    link_tx(ctx, tx, n, keyed);
}

static bool link_rx_op(void *ctx, int16_t *out, size_t n)
{
    return link_rx(ctx, out, n);
}

static void board_init(board_t *b, int index)
{
    ae_config_t power_up;

    b->index = index;
    b->recording = true;
    snprintf(b->name, sizeof b->name, "mockradio-sim%c", 'a' + index);
    memset(b->addr, 0xA0 + index, sizeof b->addr);

    ae_init(&b->engine, 20261119u + (uint32_t)index);
    ae_set_parrot_buffer(&b->engine, b->parrot, sizeof b->parrot / sizeof b->parrot[0]);
    link_init(&b->link, b->name, path_send, b);
    const ae_link_t ops = {.tx = link_tx_op, .rx = link_rx_op, .ctx = &b->link};
    ae_set_link(&b->engine, &ops);
    ae_config_defaults(&power_up);
    ae_configure(&b->engine, &power_up);
    mock_ctl_init(&b->ctl, &b->engine, &power_up);
    static const civ_rig_t *const rigs[] = {&rig_ic705, &rig_g90};
    civ_init(&b->civ, &rig_ic705, civ_tx, on_ptt, b);
    civ_set_private(&b->civ, MOCK_CTL_ADDR, mock_ctl_command, &b->ctl);
    mock_ctl_set_rigs(&b->ctl, &b->civ, rigs, sizeof rigs / sizeof rigs[0], NULL, NULL);
    coord_init(&b->coord, &b->ctl, &b->link, 20261119u + 77u * (uint32_t)index);

    const mock_ctl_store_t store = {.save = kept_save, .forget = kept_forget, .ctx = b};
    const mock_ctl_clips_t clips = {.start = clips_start, .status = clips_status, .ctx = b};
    const clip_dev_t flash = {
        .read = flash_read, .write = flash_write, .erase = flash_erase, .ctx = b, .size = sizeof b->flash,
    };
    clip_store_init(&b->clips, &flash);
    mock_ctl_set_store(&b->ctl, &store);
    mock_ctl_set_clips(&b->ctl, &clips);
    console_meter_init(&b->tx_meter);
    console_meter_init(&b->rx_meter);
}

/* What the firmware's status task does for the link, every block here. */
static void board_housekeeping(board_t *b)
{
    ae_config_t cfg;
    ae_get_config(&b->engine, &cfg);
    link_configure(&b->link, cfg.route == AE_ROUTE_LINK, cfg.link_buffer_ms, cfg.link_match);
    link_set_radio(&b->link, (uint32_t)civ_rx_freq(&b->civ), (uint32_t)civ_tx_freq(&b->civ));
    coord_tick(&b->coord, (uint32_t)(g_samples / 48u));
    link_tick(&b->link, (uint32_t)(g_samples / 48u));
}

static void run(uint32_t ms)
{
    int16_t tx[BLOCK], rx[BLOCK];
    for (uint64_t end = g_samples + (uint64_t)ms * 48u; g_samples < end; g_samples += BLOCK) {
        for (int i = 0; i < BOARDS; i++) {
            board_t *b = &g_board[i];
            board_housekeeping(b);
            if (b->playing) {
                /* 1500 Hz at -12 dBFS peak; the second board a little lower, to tell them apart */
                float amp = i ? 5827.0f : 8231.0f;
                for (int k = 0; k < BLOCK; k++) {
                    tx[k] = (int16_t)lrintf(amp * sinf(b->phase));
                    b->phase += 2.0f * 3.14159265f * 1500.0f / 48000.0f;
                    if (b->phase > 2.0f * 3.14159265f) b->phase -= 2.0f * 3.14159265f;
                }
                console_meter_add(&b->tx_meter, tx, BLOCK);
            }
            if (b->recording) {
                /* Both boards have the same clock here; on real ones the
                 * shared clock is what brings them together. */
                uint32_t seed, clock;
                coord_fade_clock(&b->coord, (uint32_t)(g_samples / 48u), &seed, &clock);
                ae_fade_clock(&b->engine, seed, clock);
                ae_process(&b->engine, b->playing ? tx : NULL, rx, BLOCK);
                console_meter_add(&b->rx_meter, rx, BLOCK);
            }
        }
    }
}

static void telemetry(board_t *b)
{
    console_sample_t s;
    memset(&s, 0, sizeof s);
    s.uptime_ms = (uint32_t)(g_samples / 48u);
    s.host = true;
    s.freq_hz = civ_rx_freq(&b->civ);
    s.tx_freq_hz = civ_tx_freq(&b->civ);
    s.mode = civ_selected_vfo(&b->civ)->mode;
    s.data = civ_selected_vfo(&b->civ)->data;
    s.split = b->civ.split;
    s.frames_ok = b->civ.frames_ok;
    s.frames_ng = b->civ.frames_ng;
    s.frames_dropped = b->civ.frames_dropped;
    console_meter_read(&b->tx_meter, &b->tx_seen, &s.tx);
    console_meter_read(&b->rx_meter, &b->rx_seen, &s.rx);
    s.fifo_target = 960;
    s.fifo_level = b->playing ? 960u + (uint32_t)(rand() % 97) - 48u : 0u;
    s.has_link = true;
    link_get_status(&b->link, &s.link);
    coord_status_t co;
    coord_get_status(&b->coord, &co);
    s.link_shared = co.sharing;
    s.link_leads = co.leads;
    s.has_rssi = true;
    s.rssi_dbm = -52 - rand() % 4;
    console_telemetry_json(&b->ctl, &s, g_json, sizeof g_json);
    printf("200 %s\n", g_json);
}

/* Numbers of about the size a board might give, so that the page has
 * something to lay out. None of them is measured. */
static void health(board_t *b, bool scan_tasks)
{
    static const struct {
        const char *name;
        uint32_t stack;
        uint16_t cpu;
    } made_up[] = {
        {"IDLE0", 1100, 870},  {"IDLE1", 1100, 955},   {"usb_audio", 2200, 68}, {"tinyusb", 2600, 21},
        {"wifi", 3900, 18},    {"httpd", 3300, 9},     {"status", 2500, 6},     {"esp_timer", 2900, 4},
        {"tiT", 1800, 3},      {"sys_evt", 1400, 1},   {"console_push", 1500, 1}, {"clips", 5600, 0},
        {"health", 1900, 0},   {"ipc0", 600, 0},       {"ipc1", 180, 0},
    };
    enum { N = sizeof made_up / sizeof made_up[0] };
    static console_health_t h;
    uint32_t now = (uint32_t)(g_samples / 48u);

    memset(&h, 0, sizeof h);
    h.uptime_ms = now;
    h.reset = "simulated board: every number in this section is made up";
    h.heap_free = 214000u + (uint32_t)(rand() % 2000);
    h.heap_min = 201500;
    h.heap_largest = 131072;
    h.has_psram = true;
    h.psram_free = 8u * 1024u * 1024u - PARROT_SECONDS * AE_PARROT_FS_HZ * 2u;
    if (b->recording) {
        h.has_cost = true;
        h.cost_blocks = now / 10u;
        h.cost_block_samples = 480;
        h.cost_avg_us = 610u + (uint32_t)(rand() % 40);
        h.cost_peak_us = h.cost_avg_us + 240u;
        h.cost_worst_us = 1830;
        h.cost_load_permille = h.cost_avg_us / 10u;
    }
    h.has_wifi = true;
    h.wifi_drops = (uint32_t)b->index; /* board B has lost its network once, to show what that looks like */
    h.n_cores = 2;
    h.busy_permille[0] = (uint16_t)(120 + rand() % 20);
    h.busy_permille[1] = (uint16_t)(40 + rand() % 10);
    h.running = N;
    if (scan_tasks) {
        b->tasks_read = true;
        b->tasks_at = g_samples;
    }
    if (b->tasks_read) {
        h.has_tasks = true;
        h.tasks_age_ms = (uint32_t)((g_samples - b->tasks_at) / 48u);
        h.tasks_took_us = 740;
        h.n_tasks = N;
        for (size_t i = 0; i < N; i++) {
            snprintf(h.tasks[i].name, sizeof h.tasks[i].name, "%s", made_up[i].name);
            h.tasks[i].stack_free = made_up[i].stack;
            h.tasks[i].has_cpu = true;
            h.tasks[i].cpu_permille = made_up[i].cpu;
        }
    }
    console_health_json(&h, g_json, sizeof g_json);
    printf("200 %s\n", g_json);
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void feed_civ(board_t *b, const char *hex)
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
    civ_feed(&b->civ, in, n);
    printf("200 \"");
    for (size_t i = 0; i < g_reply_len; i++) printf("%s%02x", i ? " " : "", g_reply[i]);
    printf("\"\n");
}

static void error(int status, const char *message)
{
    console_error_json(message, g_json, sizeof g_json);
    printf("%d %s\n", status, g_json);
}

static void board_command(board_t *b, char *line)
{
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = '\0';
    if (!arg) arg = line + strlen(line);

    if (strcmp(line, "info") == 0) {
        const console_info_t info = {.name = b->name};
        console_info_json(&b->ctl, &info, g_json, sizeof g_json);
        printf("200 %s\n", g_json);
    } else if (strcmp(line, "settings") == 0) {
        console_settings_json(&b->ctl, g_json, sizeof g_json);
        printf("200 %s\n", g_json);
    } else if (strcmp(line, "apply") == 0) {
        char err[CONSOLE_ERR_MAX];
        console_result_t r = console_settings_apply(&b->ctl, arg, err, sizeof err);
        if (r == CONSOLE_OK) {
            console_settings_json(&b->ctl, g_json, sizeof g_json);
            printf("200 %s\n", g_json);
        } else {
            error(r == CONSOLE_UNAVAILABLE ? 409 : 400, err);
        }
    } else if (strcmp(line, "rig") == 0) {
        char err[CONSOLE_ERR_MAX];
        console_result_t r = console_rig_apply(&b->ctl, arg, err, sizeof err);
        if (r == CONSOLE_OK) {
            printf("200 {}\n");
        } else {
            error(r == CONSOLE_UNAVAILABLE ? 409 : 400, err);
        }
    } else if (strcmp(line, "keep") == 0 || strcmp(line, "forget") == 0) {
        bool ok = line[0] == 'k' ? mock_ctl_keep_settings(&b->ctl) : mock_ctl_forget_settings(&b->ctl);
        if (ok) {
            printf("200 {}\n");
        } else {
            error(500, "the board could not write its flash");
        }
    } else if (strcmp(line, "clip") == 0) {
        char err[CONSOLE_ERR_MAX];
        console_result_t r = console_clip_apply(&b->ctl, arg, err, sizeof err);
        if (r == CONSOLE_OK) {
            printf("200 {}\n");
        } else {
            error(r == CONSOLE_UNAVAILABLE ? 409 : 400, err);
        }
    } else if (strcmp(line, "reset") == 0) {
        mock_ctl_reset(&b->ctl);
        console_settings_json(&b->ctl, g_json, sizeof g_json);
        printf("200 %s\n", g_json);
    } else if (strcmp(line, "replay") == 0) {
        if (ae_parrot_replay(&b->engine)) {
            printf("200 {}\n");
        } else {
            error(409, "nothing to replay: it needs the parrot route, a recording and PTT off");
        }
    } else if (strcmp(line, "telemetry") == 0) {
        telemetry(b);
    } else if (strcmp(line, "health") == 0 || strcmp(line, "tasks") == 0) {
        health(b, line[0] == 't');
    } else if (strcmp(line, "play") == 0) {
        b->playing = strcmp(arg, "on") == 0;
        printf("200 {}\n");
    } else if (strcmp(line, "record") == 0) {
        b->recording = strcmp(arg, "on") == 0;
        printf("200 {}\n");
    } else if (strcmp(line, "civ") == 0) {
        feed_civ(b, arg);
    } else {
        error(404, "no such command");
    }
}

int main(void)
{
    static char line[2048];

    for (int i = 0; i < BOARDS; i++) board_init(&g_board[i], i);

    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "tick ", 5) == 0) {
            long ms = strtol(line + 5, NULL, 10);
            if (ms > 0 && ms <= 10000) run((uint32_t)ms);
            printf("200 {}\n");
        } else if (strncmp(line, "loss ", 5) == 0) {
            long percent = strtol(line + 5, NULL, 10);
            g_loss_percent = percent < 0 ? 0 : percent > 100 ? 100 : (int)percent;
            printf("200 {}\n");
        } else if (line[0] >= 'a' && line[0] < 'a' + BOARDS && line[1] == ' ') {
            board_command(&g_board[line[0] - 'a'], line + 2);
        } else {
            error(404, "no such command");
        }
        fflush(stdout);
    }
    return 0;
}
