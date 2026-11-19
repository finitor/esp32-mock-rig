/*
 * audio_engine: what the mock radio does to receive audio.
 *
 * Portable C with no ESP-IDF dependencies, so it is tested on the host.
 * Audio is 48 kHz 16-bit mono, processed a block at a time. Nothing here
 * allocates.
 *
 * ae_process() is the whole radio: the caller hands in the audio the host is
 * transmitting and gets back what the host should record. The route decides
 * what the receiver hears (the receive source):
 *
 *   - monitor: the transmit audio itself, whatever PTT does
 *   - radio:   nothing, so the host records the noise floor
 *   - parrot:  the last transmission, recorded while keyed and replayed
 *              after unkeying
 *
 * and the receive channel is applied to it:
 *
 *   - receive mute while keyed, and for a turnaround time after unkeying
 *     (every route but monitor)
 *   - a noise floor at a fixed level, with the signal scaled so that it sits
 *     at the configured SNR whatever level the sender drove it at
 *   - extra attenuation on top
 *
 * The parrot stores audio at 12 kHz in a buffer the caller provides (PSRAM
 * on the board). It records from key-on until AE_PARROT_TAIL_MS after
 * unkeying, silence included, and stops early if the buffer fills. The
 * replay starts parrot_delay_ms after unkeying; with parrot_period_ms set it
 * waits further, to a whole number of periods after key-on, so the audio
 * comes back at the same place in a later time slot (15 s for FT8). The
 * resampler's delay is taken out: a replayed sample leaves exactly that many
 * periods after it went in.
 *
 * Threads: ae_configure(), ae_get_config(), ae_set_ptt(), ae_get_status(),
 * ae_get_parrot_status() and ae_parrot_replay() may be called from any task.
 * ae_process() and ae_rx_process() belong to one audio task.
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

/* Setting ranges. ae_configure() clamps to them. */
#define AE_SNR_MIN_DB (-60.0f)
#define AE_SNR_MAX_DB 80.0f
#define AE_REF_BW_MIN_HZ 100.0f
#define AE_REF_BW_MAX_HZ (AE_FS_HZ / 2.0f)
#define AE_FLOOR_MIN_DBFS (-90.0f)
#define AE_FLOOR_MAX_DBFS AE_SIGNAL_MAX_DBFS
#define AE_ATTEN_MAX_DB 120.0f
#define AE_TURNAROUND_MAX_MS 5000u
#define AE_PARROT_DELAY_MAX_MS 60000u
#define AE_PARROT_PERIOD_MAX_MS 300000u

/* The parrot stores audio at this rate: a quarter of the memory, and the
 * 0..4.5 kHz it keeps is more than any HF modem uses. */
#define AE_PARROT_FS_HZ (AE_FS_HZ / DSP_RS_FACTOR)

/* The parrot keeps recording this long after unkeying, to catch transmit
 * audio still on its way through the host and the USB buffers. The replay
 * never starts sooner than this. */
#define AE_PARROT_TAIL_MS 50u

typedef enum {
    AE_ROUTE_MONITOR = 0, /* receive hears the transmit audio; PTT is ignored */
    AE_ROUTE_RADIO,       /* receive hears nothing; muted while keyed and for the turnaround time after */
    AE_ROUTE_PARROT,      /* like radio, and receive hears the last transmission replayed */
} ae_route_t;

typedef struct {
    ae_route_t route;
    bool noise_on;          /* off: the signal passes with atten_db only */
    float snr_db;           /* signal to noise, in ref_bw_hz */
    float ref_bw_hz;        /* bandwidth the SNR is quoted in: 2500 (WSJT-X, Data2G) or 3000 (VARA) */
    float noise_floor_dbfs; /* RMS of the noise across the whole 0..24 kHz band */
    float atten_db;         /* extra signal loss, 0 or more; with noise on it lowers the SNR by as much */
    uint32_t turnaround_ms; /* every route but monitor: how long receive stays muted after unkeying */
    uint32_t parrot_delay_ms;  /* parrot: wait this long after unkeying before the replay */
    uint32_t parrot_period_ms; /* parrot: 0, or start the replay a whole number of these after key-on */
} ae_config_t;

/* Monitor route, noise off, no attenuation: audio passes unchanged.
 * SNR 10 dB in 2500 Hz, floor -40 dBFS and 30 ms turnaround once enabled;
 * the parrot replays 1 s after unkeying, with no period. */
void ae_config_defaults(ae_config_t *cfg);

typedef struct {
    float signal_dbfs; /* level estimate of the receive source; DSP_SILENCE_DBFS if none yet */
    float gain_db;     /* gain applied to the signal in the last block */
    bool gain_limited; /* the source is too quiet to reach the SNR within AE_GAIN_MAX_DB */
    bool rx_muted;     /* the last block was muted, wholly or in part */
} ae_status_t;

typedef enum {
    AE_PARROT_IDLE = 0,
    AE_PARROT_RECORDING, /* keyed, or in the tail after unkeying */
    AE_PARROT_WAITING,   /* recorded; the replay has not started yet */
    AE_PARROT_PLAYING,
} ae_parrot_state_t;

typedef struct {
    ae_parrot_state_t state;
    uint32_t recorded_ms; /* length of the recording, growing while it is made */
    uint32_t capacity_ms; /* 0: no buffer, the parrot route replays nothing */
    bool truncated;       /* the buffer filled; the end of the transmission is missing */
} ae_parrot_status_t;

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
    _Atomic uint32_t cfg_parrot_delay_ms;
    _Atomic uint32_t cfg_parrot_period_ms;
    _Atomic bool ptt;
    _Atomic bool replay_req;

    /* status: written by the audio task, read by any task */
    _Atomic uint32_t st_level_ms; /* mean square of the source, 0 = none yet */
    _Atomic int32_t st_gain_q24;
    _Atomic bool st_gain_limited;
    _Atomic bool st_rx_muted;
    _Atomic int32_t st_parrot_state;
    _Atomic uint32_t st_parrot_len; /* samples at 12 kHz */
    _Atomic bool st_parrot_truncated;

    /* set once, before the audio task starts */
    int16_t *par_buf;
    uint32_t par_cap; /* samples at 12 kHz */

    /* everything below belongs to the audio task */
    uint32_t seen_gen;
    ae_route_t route;
    bool noise_on;
    float noise_rms;  /* sample units */
    float target_rms; /* where the signal is put, before atten */
    float atten_lin;
    uint32_t turnaround_samples;
    uint32_t delay_samples;
    uint32_t period_samples;
    bool plan_dirty;

    bool was_keyed;
    uint32_t mute_left; /* samples of turnaround mute still to play */

    float level_ms;    /* mean square of the source over its loud parts */
    uint32_t level_n;  /* samples behind level_ms, capped at the averaging window */
    uint32_t quiet_n;  /* consecutive silent samples */
    uint64_t chunk_sum;
    uint32_t chunk_n;

    dsp_chan_t chan;

    uint64_t now; /* samples ae_process() has produced */
    ae_parrot_state_t par_state;
    uint32_t par_len;       /* samples recorded, at 12 kHz */
    bool par_truncated;
    uint32_t par_tail_left; /* samples at 48 kHz still to record after unkeying */
    uint64_t par_key_at;    /* when the recording began */
    uint64_t par_unkey_at;
    uint64_t par_play_at;   /* when the replay is due */
    uint32_t par_pos;       /* next sample to replay */
    uint32_t par_flush;     /* zeros still to feed the interpolator after the recording */
    int16_t par_frac[DSP_RS_FACTOR]; /* output samples that did not fit in the last block */
    uint32_t par_frac_n;
    dsp_decim4_t par_down;
    dsp_interp4_t par_up;
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

/* Give the parrot somewhere to record: samples of storage at 12 kHz (12000
 * per second). Call after ae_init() and before the audio task starts.
 * Without a buffer the parrot route behaves like the radio route. */
void ae_set_parrot_buffer(ae_t *e, int16_t *storage, uint32_t samples);

void ae_get_parrot_status(ae_t *e, ae_parrot_status_t *st);

/* Play the recording again, from its start, at the next block. Returns false
 * and does nothing unless the route is parrot, there is a recording and the
 * radio is not transmitting. */
bool ae_parrot_replay(ae_t *e);

/* One block of audio through the radio: n samples. tx is what the host is
 * transmitting, or NULL when it sends nothing; out is what it should record.
 * tx and out may be the same buffer. */
void ae_process(ae_t *e, const int16_t *tx, int16_t *out, size_t n);

/* The receive half alone, for a caller that brings its own receive source:
 * src is the audio arriving at the receiver, or NULL when there is none (the
 * output is then the noise floor, or silence with noise off). The route only
 * decides whether PTT mutes; the parrot does not run. src and out may be the
 * same buffer. Use this or ae_process() on an engine, not both. */
void ae_rx_process(ae_t *e, const int16_t *src, int16_t *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_ENGINE_H */
