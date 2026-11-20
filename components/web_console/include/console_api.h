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

#define CONSOLE_JSON_MAX 4096
#define CONSOLE_ERR_MAX  128
#define CONSOLE_API_VERSION 1

/* ---- identity and the list of settings ------------------------------------ */

typedef struct {
    const char *name; /* the board's name on the network */
    const char *rig;  /* the personality's name */
    int hamlib_model;
} console_info_t;

/* Who this is and what can be set: each setting's name, label, group, and
 * either its choices or its unit, range and step. The page builds its form
 * from this. */
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
    uint32_t frames_ok, frames_ng, frames_dropped;

    console_level_t tx; /* audio the host plays */
    console_level_t rx; /* audio the host records */
    uint32_t fifo_level, fifo_target;
    uint32_t fifo_overruns, fifo_underruns, fifo_dropped, fifo_repeated;

    bool has_link; /* the board was built with the two-board link */
    link_status_t link;

    bool has_rssi; /* joined to a Wi-Fi network */
    int rssi_dbm;
} console_sample_t;

/* One snapshot: PTT and its sources, the receive channel's status, the
 * parrot, the link, and everything in the sample. "gen" changes when the settings do,
 * however they were changed, so the page knows to read them again. */
size_t console_telemetry_json(mock_ctl_t *m, const console_sample_t *s, char *buf, size_t cap);

/* The name of a CI-V mode number ("USB", "CW-R"), with "-D" added for data.
 * These numbers are common to CI-V radios. An unknown one reads as two hex
 * digits. out holds at least 12 characters. */
void console_mode_name(uint8_t mode, bool data, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_API_H */
