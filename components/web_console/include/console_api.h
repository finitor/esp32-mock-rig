/*
 * console_api: the web console's settings and telemetry, as JSON text.
 *
 * Portable C with no ESP-IDF dependencies, so it is tested on the host. The
 * HTTP server in this component (web_console.c, ESP-IDF only) is a thin layer
 * over these functions; so is the simulator in host/.
 *
 * Settings go through mock_ctl's own functions, by the names host/tools/
 * mockctl.py uses for them, so the console and the CI-V controls share one
 * table of ranges and one set of power-up values. A value outside its range
 * is refused, not clamped, and a request that holds one refused value changes
 * nothing.
 *
 *   settings:   {"route":"monitor","noise":"off","snr":10.0,"bw":2500, ...}
 *   a change:   {"noise":"on","snr":-12.5}        any subset, in any order
 *
 * Every function that writes JSON returns its length, with a terminating
 * zero after it, or 0 if cap is too small. CONSOLE_JSON_MAX is enough for
 * any of them.
 *
 * Threads: the settings functions read, change and write back the engine's
 * settings in three steps, like mock_ctl's commands. The caller keeps them,
 * and the CI-V parser, from running at the same time.
 */
#ifndef CONSOLE_API_H
#define CONSOLE_API_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_dsp.h"
#include "link.h"
#include "mock_ctl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONSOLE_JSON_MAX 6144
#define CONSOLE_ERR_MAX  128
#define CONSOLE_API_VERSION 1

/* ---- identity and the list of settings ------------------------------------ */

typedef struct {
    const char *name; /* the board's name on the network */
    const char *rig;  /* the personality's name and model, used only if the */
    int hamlib_model; /* controls were given no list of personalities */
} console_info_t;

/* Who this is and what can be set: the radio imitated and the ones it could
 * be instead, the layout of the waterfall's rows ("scope"), and each setting's
 * name, label, group, and either its choices (with a label for each, where
 * the word alone does not say enough) or its unit, range and step. A setting
 * that is in force only while another has one value says so ("when": the
 * custom fading's numbers, {"fade":"custom"}). The page builds its form from
 * this. */
size_t console_info_json(mock_ctl_t *m, const console_info_t *info, char *buf, size_t cap);

/* ---- settings ---------------------------------------------------------------- */

size_t console_settings_json(mock_ctl_t *m, char *buf, size_t cap);

typedef enum {
    CONSOLE_OK = 0,
    CONSOLE_BAD_REQUEST, /* not a flat JSON object, an unknown name, a wrong type or a value out of range */
    CONSOLE_UNAVAILABLE, /* well formed, but not possible now: the parrot route without a buffer */
} console_result_t;

/* Apply a change: json is a zero-terminated JSON object of setting names and
 * values. All of it is applied, or none of it. On anything but CONSOLE_OK,
 * err says what was wrong, in plain words, for the person at the browser. */
console_result_t console_settings_apply(mock_ctl_t *m, const char *json, char *err, size_t err_cap);

/* Change the radio imitated: json is {"hamlib":3088}, the personality's
 * Hamlib model number. CONSOLE_UNAVAILABLE if the mock has no such
 * personality. The new radio starts from its power-up state. */
console_result_t console_rig_apply(mock_ctl_t *m, const char *json, char *err, size_t err_cap);

/* A clip operation: json is {"op":"save","clip":1}, with op one of "save"
 * (the parrot's recording into the clip), "play" and "erase". It is begun,
 * not finished, when this returns CONSOLE_OK: the telemetry says when it is
 * done. CONSOLE_UNAVAILABLE with the reason if it cannot be begun now. */
console_result_t console_clip_apply(mock_ctl_t *m, const char *json, char *err, size_t err_cap);

/* ---- the front panel ----------------------------------------------------------
 *
 * The radio imitated, as an operator at its front panel has it: read what
 * the controller set over CAT, and change things at the radio to see whether
 * the controller notices. The panel is generic. It is built from the
 * personality's own tables, so it shows whichever radio is imitated with no
 * knowledge of its own.
 *
 *   {"rig":"Icom IC-705","hamlib":3085,"gen":2166136261,
 *    "vfo":"A","split":false,
 *    "a":{"freq":14074000,"mode":"USB","data":true,"filter":1},"b":{...},
 *    "modes":["LSB","USB",...],"filters":3,"can":{"data":true,"equal":true},
 *    "duplex":"simplex","rit":{"on":false,"hz":0},"tuner":false,
 *    "att":{"on":false,"byte":"00","set":true},"step":"00","width":"34","power":true,
 *    "rx":[[30000,199999999],...],"tx":[[1800000,2000000],...],
 *    "levels":[{"id":"14.01","name":"AF gain","value":128},...],
 *    "funcs":[{"id":"16.12","name":"AGC","value":2,"choices":["off","fast","mid","slow"]},...],
 *    "meters":[{"id":"15.02","name":"S-meter","value":12,"max":255},...]}
 *
 * What the radio lacks is null (duplex, rit, tuner, att, step, width,
 * power). An item's id is its CI-V command and sub-command. "gen" is
 * civ_state_hash(), which the telemetry carries too ("rgen").
 *
 * A change is a flat object of any of: "freq" (Hz, the selected VFO), "vfo"
 * ("A"/"B"), "op" ("equal": the selected VFO into the other; "swap"), "mode",
 * "data", "filter", "split", "duplex" ("simplex"/"dup-"/"dup+"), "rit",
 * "rit_hz", "tuner", "att", "ptt" (the key on the front panel), and items by
 * id: {"14.01":200,"16.12":3,"15.02":120}.
 *
 * Everything CAT can set goes to the radio as the CI-V command itself
 * (civ_local), so the panel and CAT cannot disagree about what is allowed.
 * The meters, which CAT can only read, and the key are the panel's own. All
 * of a change is applied, or none of it.
 */

/* c is the parser, or a copy of it taken with the parser's lock held. */
size_t console_radio_json(const civ_t *c, char *buf, size_t cap);

/* CONSOLE_UNAVAILABLE for what this radio does not have, CONSOLE_BAD_REQUEST
 * for the rest. The caller keeps the CI-V parser from running meanwhile. */
console_result_t console_radio_apply(mock_ctl_t *m, const char *json, char *err, size_t err_cap);

/* {"error":"<message>"}, with the message escaped. */
size_t console_error_json(const char *message, char *buf, size_t cap);

/* {"<key>":"<value>", ...} for n keys and values, the values escaped. The
 * keys are the caller's own plain names. */
size_t console_strings_json(const char *const *keys, const char *const *values, size_t n, char *buf, size_t cap);

/* The string value of one member of a flat JSON object, unescaped. False if
 * the object is malformed, the member is missing or not a string, or it does
 * not fit. For the few requests that are not settings (the Wi-Fi form). */
bool console_json_string(const char *json, const char *name, char *out, size_t cap);

/* ---- level meters ---------------------------------------------------------------
 *
 * A level for the console to show: RMS and peak over the last
 * CONSOLE_METER_WINDOW samples (0.1 s). One audio task adds samples; one
 * other task reads. A meter that has had nothing added since the last read
 * reads as not active, which is how the page knows that the host has
 * stopped playing or recording.
 */
#define CONSOLE_METER_WINDOW 4800u

typedef struct {
    dsp_meter_t acc; /* the audio task's */
    _Atomic int32_t rms_cdb; /* hundredths of a dB, full scale = 0 */
    _Atomic int32_t peak_cdb;
    _Atomic uint32_t seq; /* windows published so far */
} console_meter_t;

typedef struct {
    bool active;      /* samples have arrived since the last read */
    int32_t rms_cdb;  /* hundredths of a dBFS; DSP_SILENCE_DBFS * 100 for silence */
    int32_t peak_cdb;
} console_level_t;

void console_meter_init(console_meter_t *m);

/* samples NULL counts as n samples of silence. */
void console_meter_add(console_meter_t *m, const int16_t *samples, size_t n);

/* seen is the reader's own memory of the meter, zero to start with. */
void console_meter_read(console_meter_t *m, uint32_t *seen, console_level_t *out);

/* ---- the waterfall ----------------------------------------------------------------
 *
 * The spectrum of the audio the host plays and of the audio it records, up
 * to 3 kHz, a row at a time for the page to scroll. Two parts, kept apart
 * because of who runs them:
 *
 *   console_scope_t   a ring of the newest samples. The audio task adds to
 *                     it, and that is all the waterfall costs the audio: a
 *                     copy. One for each direction.
 *   console_scope_row a row for the page, made from the two rings by one
 *                     other task: the transforms are done there, and only
 *                     while some page is showing the waterfall.
 *
 * A row is bytes, not JSON, to keep it small beside the link's audio on the
 * same radio:
 *
 *   byte 0   CONSOLE_SCOPE_FORMAT
 *   byte 1   flags: CONSOLE_SCOPE_RX, _TX: that lane follows; _KEYED: the
 *            radio is transmitting
 *   byte 2   bins in a lane, DSP_SPEC_BINS
 *   byte 3   0
 *   then the receive lane and the transmit lane, whichever are there, a byte
 *   for each bin: the level in half decibels below full scale (0 is 0 dBFS,
 *   255 is -127.5 dBFS or less), on the level meters' scale.
 *
 * A lane is left out while the host is not streaming that way. Bin k is at
 * k * fs / n hertz; /api/info gives fs, n, the bins and the rows a second.
 */
#define CONSOLE_SCOPE_RING 4096u /* a power of two, with room to spare over DSP_SPEC_INPUT */
#define CONSOLE_SCOPE_FORMAT 0x01
#define CONSOLE_SCOPE_ROWS_PER_S 10
#define CONSOLE_SCOPE_ROW_MAX (4 + 2 * DSP_SPEC_BINS)

enum {
    CONSOLE_SCOPE_RX = 0x01,
    CONSOLE_SCOPE_TX = 0x02,
    CONSOLE_SCOPE_KEYED = 0x04,
};

typedef struct {
    int16_t ring[CONSOLE_SCOPE_RING]; /* the audio task's */
    _Atomic uint32_t written;         /* samples added so far */
} console_scope_t;

void console_scope_init(console_scope_t *s);

/* The audio task. samples NULL counts as n samples of silence. */
void console_scope_add(console_scope_t *s, const int16_t *samples, size_t n);

/* The newest DSP_SPEC_INPUT samples into out, the newest last. seen is the
 * reader's own memory of the ring, zero to start with. False, and out is not
 * to be used, if nothing has been added since the last call (the host has
 * stopped streaming that way) or there are not that many samples yet. */
bool console_scope_take(console_scope_t *s, uint32_t *seen, int16_t *out);

/* What making rows needs to keep: about 15 KB, so one of them, not on a stack. */
typedef struct {
    dsp_spec_t spec;
    int16_t pcm[DSP_SPEC_INPUT];
    float db[DSP_SPEC_BINS];
    uint32_t rx_seen, tx_seen;
} console_scope_work_t;

void console_scope_work_init(console_scope_work_t *w);

/* One row. rx is what the host records and tx what it plays; either may be
 * NULL. Returns its length, or 0 if cap is below CONSOLE_SCOPE_ROW_MAX. */
size_t console_scope_row(console_scope_work_t *w, console_scope_t *rx, console_scope_t *tx, bool keyed, uint8_t *out,
                         size_t cap);

/* ---- telemetry ------------------------------------------------------------------ */

/* What the console shows that the engine does not know: the caller collects
 * it from the CI-V parser, the USB device, the audio FIFO and the meters. */
typedef struct {
    uint32_t uptime_ms;
    bool host; /* a USB host has configured the device */

    uint64_t freq_hz;    /* receive frequency */
    uint64_t tx_freq_hz; /* differs when split */
    uint8_t mode;        /* the CI-V mode number */
    bool data;
    bool split;
    uint32_t radio_gen;  /* civ_state_hash(): changes when the front panel should be read again */
    uint32_t frames_ok, frames_ng, frames_dropped;
    /* the serial port's replies (cat_port) and what arrived with no room for it */
    uint32_t replies_stale, replies_full, replies_discarded, port_recovered, rx_lost;

    console_level_t tx; /* audio the host plays */
    console_level_t rx; /* audio the host records */
    uint32_t fifo_level, fifo_target;
    uint32_t fifo_overruns, fifo_underruns, fifo_dropped, fifo_repeated;

    bool has_link; /* the board was built with the two-board link */
    link_status_t link;
    bool link_shared; /* the linked board shares its channel with this one */
    bool link_leads;  /* and the pair fades by this board's clock */

    bool has_rssi; /* joined to a Wi-Fi network */
    int rssi_dbm;
} console_sample_t;

/* One snapshot: PTT and its sources, the receive channel's status, the
 * parrot, the link, and everything in the sample. "rgen" changes when the
 * radio's own state does (see the front panel). "gen" changes when the settings do,
 * however they were changed, so the page knows to read them again, and
 * "hamlib" is the radio imitated now, for the same reason. "kept" says
 * whether the power-up settings are ones kept in flash, and "clips" lists
 * the recordings kept there, or is null on a mock without any. */
size_t console_telemetry_json(mock_ctl_t *m, const console_sample_t *s, char *buf, size_t cap);

/* The name of a CI-V mode number ("USB", "CW-R"), with "-D" added for data.
 * These numbers are common to CI-V radios. An unknown one reads as two hex
 * digits. out holds at least 12 characters. */
void console_mode_name(uint8_t mode, bool data, char *out, size_t cap);

/* ---- the board's health ------------------------------------------------------------
 *
 * What a first bring-up asks about the board and not about the radio: is
 * there memory left, how busy is each processor core, how much of its time
 * does the audio work take, does Wi-Fi keep dropping, is a task's stack
 * about to run out. It is asked for on its own (GET /api/health) and is not
 * part of the telemetry, so that the five-times-a-second stream stays small.
 *
 * The table of tasks is the one part that costs something to read: the board
 * holds everything else up while it looks through every stack. It is read
 * only when asked for, and a reading says how long that took and how old the
 * table is.
 *
 * The cost of the audio work: the time each block took, averaged and at its
 * longest over every CONSOLE_COST_WINDOW samples (1 s), and the longest ever.
 * One audio task adds; any number of others read, since reading changes
 * nothing.
 */
#define CONSOLE_COST_WINDOW 48000u

typedef struct {
    uint32_t sum_us, max_us, samples, blocks; /* the audio task's, this window */
    _Atomic uint32_t avg_us;        /* per block, over the last whole window */
    _Atomic uint32_t peak_us;       /* the longest block in it */
    _Atomic uint32_t load_permille; /* time taken / time the samples last */
    _Atomic uint32_t block_samples; /* the length of the latest block */
    _Atomic uint32_t worst_us;      /* the longest block since start */
    _Atomic uint32_t total_blocks;
    _Atomic uint32_t windows;       /* whole windows measured so far */
} console_cost_t;

void console_cost_init(console_cost_t *c);

/* One block of n samples took us microseconds. */
void console_cost_add(console_cost_t *c, uint32_t us, size_t n);

#define CONSOLE_HEALTH_CORES 2
#define CONSOLE_HEALTH_TASKS 32
#define CONSOLE_TASK_NAME_MAX 16

typedef struct {
    char name[CONSOLE_TASK_NAME_MAX];
    uint32_t stack_free;   /* bytes of its stack never used so far */
    bool has_cpu;
    uint16_t cpu_permille; /* of one processor core, between the last two readings of the table */
} console_task_t;

typedef struct {
    uint32_t uptime_ms;
    const char *reset; /* why the board last restarted, in plain words; NULL if unknown */

    uint32_t heap_free;    /* internal memory, bytes: free now, */
    uint32_t heap_min;     /* the least there has been since start, */
    uint32_t heap_largest; /* and the largest single piece free now */
    bool has_psram;
    uint32_t psram_free;

    bool has_cost; /* the audio work has been timed for at least one window */
    uint32_t cost_blocks, cost_block_samples;
    uint32_t cost_avg_us, cost_peak_us, cost_worst_us, cost_load_permille;

    bool has_wifi;       /* built with Wi-Fi */
    uint32_t wifi_drops; /* times a joined network was lost */

    /* How busy each core was since the reader last asked. 0 cores: not
     * measured (the first reading, or a build without run-time statistics). */
    unsigned n_cores;
    uint16_t busy_permille[CONSOLE_HEALTH_CORES];

    uint32_t running; /* tasks that exist now */

    bool has_tasks;         /* the table has been read at least once */
    uint32_t tasks_age_ms;  /* how long ago */
    uint32_t tasks_took_us; /* how long the board was held up for it */
    uint32_t tasks_missed;  /* tasks there was no room for */
    size_t n_tasks;
    console_task_t tasks[CONSOLE_HEALTH_TASKS];
} console_health_t;

/* The cost's part of a health reading. */
void console_cost_read(console_cost_t *c, console_health_t *out);

/* {"t":...,"reset":...,"heap":{"free":...,"min":...,"largest":...},"psram":...,
 *  "audio":{"blocks":...,"block":...,"avg":...,"max":...,"worst":...,"load":...},
 *  "wifi":{"drops":...},"cpu":[12.5,3.0],"running":21,
 *  "tasks":{"age":...,"took":...,"missed":0,"list":[{"name":...,"stack":...,"cpu":...}, ...]}}
 * with null for what the board does not have or has not measured. Times are
 * in microseconds (age in milliseconds), memory in bytes, cpu and load in
 * percent. */
size_t console_health_json(const console_health_t *h, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_API_H */
