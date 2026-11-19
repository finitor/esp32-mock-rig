/*
 * audio_engine: what the mock radio does to receive audio.
 *
 * Portable C with no ESP-IDF dependencies, so it is tested on the host.
 * Audio is 48 kHz 16-bit mono, processed a block at a time. Nothing here
 * allocates.
 *
 * The caller hands in the audio arriving at the receiver (the receive
 * source: looped-back transmit audio now, parrot replay or the link later)
 * and gets back what the host should record:
 *
 *   - receive mute while keyed, and for a turnaround time after unkeying
 *   - a noise floor at a fixed level, with the signal scaled so that it sits
 *     at the configured SNR whatever level the sender drove it at
 *   - extra attenuation on top
 *
 * Threads: ae_configure(), ae_get_config(), ae_set_ptt() and ae_get_status()
 * may be called from any task. ae_rx_process() belongs to one audio task.
 */
#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AE_FS_HZ 48000

/* With noise on, the signal is never raised above this RMS level; a higher
 * SNR is reached by lowering the noise floor instead. Leaves 12 dB for peaks. */
#define AE_SIGNAL_MAX_DBFS (-12.0f)

/* The most a quiet signal is amplified to reach the SNR. Beyond this the
 * SNR comes out lower than asked and the status says so. */
#define AE_GAIN_MAX_DB 30.0f

/* Source audio below this RMS level (measured over 1 ms) counts as silence
 * and does not move the level estimate. */
#define AE_SILENCE_DBFS (-60.0f)

typedef enum {
    AE_ROUTE_MONITOR = 0, /* receive is always open; PTT is ignored */
    AE_ROUTE_RADIO,       /* receive is muted while keyed and for the turnaround time after */
} ae_route_t;

typedef struct {
    ae_route_t route;
    bool noise_on;          /* off: the signal passes with atten_db only */
    float snr_db;           /* signal to noise, in ref_bw_hz */
    float ref_bw_hz;        /* bandwidth the SNR is quoted in: 2500 (WSJT-X, Data2G) or 3000 (VARA) */
    float noise_floor_dbfs; /* RMS of the noise across the whole 0..24 kHz band */
    float atten_db;         /* extra signal loss, 0 or more; with noise on it lowers the SNR by as much */
    uint32_t turnaround_ms; /* radio route: how long receive stays muted after unkeying */
} ae_config_t;

/* Monitor route, noise off, no attenuation: audio passes unchanged.
 * SNR 10 dB in 2500 Hz, floor -40 dBFS and 30 ms turnaround once enabled. */
void ae_config_defaults(ae_config_t *cfg);

typedef struct {
    float signal_dbfs; /* level estimate of the receive source; DSP_SILENCE_DBFS if none yet */
    float gain_db;     /* gain applied to the signal in the last block */
    bool gain_limited; /* the source is too quiet to reach the SNR within AE_GAIN_MAX_DB */
    bool rx_muted;     /* the last block was muted, wholly or in part */
} ae_status_t;

typedef struct {
    /* settings and PTT: written by any task, read by the audio task */
    _Atomic uint32_t cfg_gen;
    _Atomic int32_t cfg_route;
    _Atomic int32_t cfg_noise_on;
    _Atomic int32_t cfg_snr_cdb; /* dB values are held in hundredths of a dB */
    _Atomic int32_t cfg_ref_bw_hz;
    _Atomic int32_t cfg_floor_cdb;
    _Atomic int32_t cfg_atten_cdb;
    _Atomic uint32_t cfg_turnaround_ms;
    _Atomic bool ptt;

    /* status: written by the audio task, read by any task */
    _Atomic uint32_t st_level_ms; /* mean square of the source, 0 = none yet */
    _Atomic int32_t st_gain_q24;
    _Atomic bool st_gain_limited;
    _Atomic bool st_rx_muted;

    /* everything below belongs to the audio task */
    uint32_t seen_gen;
    ae_route_t route;
    bool noise_on;
    float noise_rms;  /* sample units */
    float target_rms; /* where the signal is put, before atten */
    float atten_lin;
    uint32_t turnaround_samples;
    bool plan_dirty;

    bool was_keyed;
    uint32_t mute_left; /* samples of turnaround mute still to play */

    float level_ms;    /* mean square of the source over its loud parts */
    uint32_t level_n;  /* samples behind level_ms, capped at the averaging window */
    uint32_t quiet_n;  /* consecutive silent samples */
    uint64_t chunk_sum;
    uint32_t chunk_n;

    dsp_chan_t chan;
} ae_t;

/* Start with ae_config_defaults(). seed starts the noise generator. */
void ae_init(ae_t *e, uint32_t seed);

/* Change the settings; out-of-range values are clamped. Takes effect at the
 * next block the audio task processes. */
void ae_configure(ae_t *e, const ae_config_t *cfg);
void ae_get_config(ae_t *e, ae_config_t *cfg);

/* The radio's transmit state. It is sampled once per block, so the
 * turnaround time can run long by up to one block. */
void ae_set_ptt(ae_t *e, bool keyed);

void ae_get_status(ae_t *e, ae_status_t *st);

/* Produce n samples of receive audio for the host. src is the audio arriving
 * at the receiver, or NULL when there is none (the output is then the noise
 * floor, or silence with noise off). src and out may be the same buffer. */
void ae_rx_process(ae_t *e, const int16_t *src, int16_t *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_ENGINE_H */
