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
 *   - link:    what another board is transmitting; and this board's own
 *              transmit audio goes to that board. The engine only hands the
 *              audio over (ae_set_link); how it travels is not its business
 *
 * The radio transmits while any PTT source keys it: CAT (ae_set_ptt), a
 * serial control line if one is selected (ae_set_lines), or VOX if it is on,
 * which opens when the transmit audio reaches a threshold and closes a hang
 * time after it last did.
 *
 * The receive channel is then applied to the receive source:
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
 * The parrot's buffer can be lent to another task, to save the recording or
 * to load a stored one: ae_parrot_hold() asks, the audio task lets go at its
 * next block, and ae_parrot_release() gives the buffer back, saying whether
 * it now holds a different recording and whether to replay it.
 *
 * Threads: ae_configure(), ae_get_config(), ae_config_gen(), ae_set_ptt(),
 * ae_set_lines(), ae_get_ptt(), ae_get_status(), ae_get_parrot_status(),
 * ae_parrot_replay() and ae_blocks() may be called from any task. The hold
 * functions belong to one task at a time, other than the audio task.
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
#define AE_VOX_MIN_DBFS (-80.0f)
#define AE_VOX_MAX_DBFS (-6.0f)
#define AE_VOX_HANG_MAX_MS 5000u
#define AE_LINK_BUFFER_MIN_MS 10u
#define AE_LINK_BUFFER_MAX_MS 200u

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
    AE_ROUTE_LINK,        /* like radio, and receive hears the other board's transmission */
} ae_route_t;

/* Which serial control line keys the radio. Off by default: hosts raise
 * both lines when they open the port. */
typedef enum {
    AE_LINE_OFF = 0,
    AE_LINE_RTS,
    AE_LINE_DTR,
} ae_line_t;

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
    ae_line_t ptt_line;        /* the control line that keys the radio, if any */
    bool vox_on;               /* transmit audio keys the radio */
    float vox_threshold_dbfs;  /* a transmit sample this large opens the VOX (0 dBFS = full scale) */
    uint32_t vox_hang_ms;      /* the VOX stays open this long after the last such sample */
    uint32_t link_buffer_ms;   /* link: audio held before it is played, to ride out uneven arrival */
    bool link_match;           /* link: hear the other board only on this radio's receive frequency */
} ae_config_t;

/* Monitor route, noise off, no attenuation: audio passes unchanged.
 * SNR 10 dB in 2500 Hz, floor -40 dBFS and 30 ms turnaround once enabled;
 * the parrot replays 1 s after unkeying, with no period. CAT is the only
 * PTT source: no control line, VOX off (-40 dBFS and 100 ms once enabled).
 * The link holds 30 ms of audio and matches frequencies. */
void ae_config_defaults(ae_config_t *cfg);

typedef struct {
    float signal_dbfs; /* level estimate of the receive source; DSP_SILENCE_DBFS if none yet */
    float gain_db;     /* gain applied to the signal in the last block */
    bool gain_limited; /* the source is too quiet to reach the SNR within AE_GAIN_MAX_DB */
    bool rx_muted;     /* the last block was muted, wholly or in part */
} ae_status_t;

/* Which PTT sources are keying the radio now. It transmits if any is. */
typedef struct {
    bool cat;
    bool line;
    bool vox;
} ae_ptt_t;

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

/* The link route's other end. Both functions are called from the audio task,
 * in every block while the route is link.
 *   tx  n samples of transmit audio (NULL for silence) and whether the radio
 *       is transmitting. Called once with n == 0 when the route changes
 *       away from link, to end a transmission under way.
 *   rx  fill out with n samples of what the other board is sending and
 *       return true, or return false if there is nothing to hear. */
typedef struct {
    void (*tx)(void *ctx, const int16_t *tx, size_t n, bool keyed);
    bool (*rx)(void *ctx, int16_t *out, size_t n);
    void *ctx;
} ae_link_t;

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
    _Atomic int32_t cfg_ptt_line;
    _Atomic int32_t cfg_vox_on;
    _Atomic int32_t cfg_vox_cdb;
    _Atomic uint32_t cfg_vox_hang_ms;
    _Atomic uint32_t cfg_link_buffer_ms;
    _Atomic int32_t cfg_link_match;
    _Atomic bool ptt; /* CAT */
    _Atomic bool dtr;
    _Atomic bool rts;
    _Atomic bool replay_req;
    _Atomic bool par_hold;      /* another task wants the buffer */
    _Atomic bool rel_loaded;    /* what ae_parrot_release() said */
    _Atomic uint32_t rel_len;
    _Atomic bool rel_play;

    /* status: written by the audio task, read by any task */
    _Atomic uint32_t st_level_ms; /* mean square of the source, 0 = none yet */
    _Atomic int32_t st_gain_q24;
    _Atomic bool st_gain_limited;
    _Atomic bool st_rx_muted;
    _Atomic bool st_vox;
    _Atomic int32_t st_parrot_state;
    _Atomic uint32_t st_parrot_len; /* samples at 12 kHz */
    _Atomic bool st_parrot_truncated;
    _Atomic bool st_par_held; /* the audio task has let go of the buffer */
    _Atomic uint32_t st_blocks;

    /* set once, before the audio task starts */
    int16_t *par_buf;
    uint32_t par_cap; /* samples at 12 kHz */
    ae_link_t link;
    bool has_link;

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

    bool vox_enabled;
    int32_t vox_cdb; /* the settings the detector was last set up with */
    uint32_t vox_hang_ms;
    dsp_vox_t vox;

    bool was_keyed;
    uint32_t mute_left; /* samples of turnaround mute still to play */

    float level_ms;    /* mean square of the source over its loud parts */
    uint32_t level_n;  /* samples behind level_ms, capped at the averaging window */
    uint32_t quiet_n;  /* consecutive silent samples */
    uint64_t chunk_sum;
    uint32_t chunk_n;

    dsp_chan_t chan;

    uint64_t now; /* samples ae_process() has produced */
    bool par_lent; /* the buffer is with another task */
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

/* A number that changes each time ae_configure() is called, so a display
 * can tell that the settings may have changed without comparing them. */
uint32_t ae_config_gen(ae_t *e);

/* PTT as commanded over CAT. The transmit state is sampled once per block,
 * so the turnaround time can run long by up to one block. */
void ae_set_ptt(ae_t *e, bool keyed);

/* The serial port's control lines, as the host last set them. They key the
 * radio only if ptt_line selects one. */
void ae_set_lines(ae_t *e, bool dtr, bool rts);

/* What is keying the radio now. The VOX state is from the last block the
 * audio task processed. */
void ae_get_ptt(ae_t *e, ae_ptt_t *p);

void ae_get_status(ae_t *e, ae_status_t *st);

/* Give the parrot somewhere to record: samples of storage at 12 kHz (12000
 * per second). Call after ae_init() and before the audio task starts.
 * Without a buffer the parrot route behaves like the radio route. */
void ae_set_parrot_buffer(ae_t *e, int16_t *storage, uint32_t samples);

void ae_get_parrot_status(ae_t *e, ae_parrot_status_t *st);

/* Give the link route its other end. Call after ae_init() and before the
 * audio task starts. Without one the link route behaves like the radio
 * route. */
void ae_set_link(ae_t *e, const ae_link_t *link);
bool ae_has_link(ae_t *e);

/* Play the recording again, from its start, at the next block. Returns false
 * and does nothing unless the route is parrot, there is a recording and the
 * radio is not transmitting. */
bool ae_parrot_replay(ae_t *e);

/* ---- lending the parrot's buffer ------------------------------------------
 *
 * To read the recording out, or put another in, from a task that is not the
 * audio task:
 *
 *   ae_parrot_hold(e);
 *   wait until ae_parrot_held(e), or until ae_blocks(e) has stood still for
 *     a few block times: an audio task that is not running holds nothing,
 *     and if it starts it sees the hold at once
 *   use ae_parrot_buffer()
 *   ae_parrot_release(e, ...);
 *
 * While held the parrot neither records nor replays; a transmission made
 * meanwhile is not recorded.
 */
void ae_parrot_hold(ae_t *e);
bool ae_parrot_held(ae_t *e);

/* How many blocks the audio task has processed. */
uint32_t ae_blocks(ae_t *e);

/* The buffer, the length of the recording in it and its capacity, in 12 kHz
 * samples. NULL if the parrot has none. Only for the task holding it. */
int16_t *ae_parrot_buffer(ae_t *e, uint32_t *len, uint32_t *cap);

/* Give the buffer back. loaded: it now holds a recording of len samples,
 * which replaces the old one. play: replay it at once, if the route is
 * parrot and the radio is not transmitting. */
void ae_parrot_release(ae_t *e, bool loaded, uint32_t len, bool play);

/* One block of audio through the radio: n samples. tx is what the host is
 * transmitting, or NULL when it sends nothing; out is what it should record.
 * tx and out may be the same buffer. */
void ae_process(ae_t *e, const int16_t *tx, int16_t *out, size_t n);

/* The receive half alone, for a caller that brings its own receive source:
 * src is the audio arriving at the receiver, or NULL when there is none (the
 * output is then the noise floor, or silence with noise off). The route only
 * decides whether PTT mutes; the parrot does not run, and neither does the
 * VOX, which has no transmit audio to hear. src and out may be the same
 * buffer. Use this or ae_process() on an engine, not both. */
void ae_rx_process(ae_t *e, const int16_t *src, int16_t *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_ENGINE_H */
