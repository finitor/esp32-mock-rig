/* audio_engine: see audio_engine.h. */
#include "audio_engine.h"

#include <math.h>
#include <string.h>

#define FULL_SCALE 32768.0f

/* The source level is measured in 1 ms pieces so that a transmission
 * starting part-way through a block is measured over its loud part only. */
#define CHUNK 48u

/* The level estimate is a plain average until it covers this much signal,
 * then a running average over about this long. */
#define LEVEL_WINDOW (AE_FS_HZ) /* 1 s */

/* This much silence ends a transmission: the next one starts a new estimate. */
#define LEVEL_GAP (AE_FS_HZ / 5) /* 200 ms */

#define PAR_TAIL (AE_PARROT_TAIL_MS * (AE_FS_HZ / 1000)) /* samples at 48 kHz */

/* Down to 12 kHz and back up delays audio by 92 samples at 48 kHz, which is
 * 23 stored samples. The replay skips that many at the start, so it is not
 * late, and feeds the interpolator a filter length of zeros at the end, so
 * the last of the recording still comes out. */
#define PAR_SKIP  23u
#define PAR_FLUSH (DSP_RS_TAPS / DSP_RS_FACTOR)

static float db_to_lin(float db)
{
    return powf(10.0f, db / 20.0f);
}

static float clampf(float x, float lo, float hi)
{
    if (!(x >= lo)) return lo; /* also catches NaN */
    if (x > hi) return hi;
    return x;
}

static int32_t to_cdb(float db, float lo, float hi)
{
    return (int32_t)lroundf(clampf(db, lo, hi) * 100.0f);
}

static float from_cdb(int32_t cdb)
{
    return (float)cdb / 100.0f;
}

void ae_config_defaults(ae_config_t *cfg)
{
    cfg->route = AE_ROUTE_MONITOR;
    cfg->noise_on = false;
    cfg->snr_db = 10.0f;
    cfg->ref_bw_hz = 2500.0f;
    cfg->noise_floor_dbfs = -40.0f;
    cfg->atten_db = 0.0f;
    cfg->turnaround_ms = 30;
    cfg->parrot_delay_ms = 1000;
    cfg->parrot_period_ms = 0;
    cfg->ptt_line = AE_LINE_OFF;
    cfg->vox_on = false;
    cfg->vox_threshold_dbfs = -40.0f;
    cfg->vox_hang_ms = 100;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

void ae_configure(ae_t *e, const ae_config_t *cfg)
{
    bool known = cfg->route == AE_ROUTE_RADIO || cfg->route == AE_ROUTE_PARROT;
    atomic_store(&e->cfg_route, known ? (int32_t)cfg->route : (int32_t)AE_ROUTE_MONITOR);
    atomic_store(&e->cfg_noise_on, cfg->noise_on ? 1 : 0);
    atomic_store(&e->cfg_snr_cdb, to_cdb(cfg->snr_db, AE_SNR_MIN_DB, AE_SNR_MAX_DB));
    atomic_store(&e->cfg_ref_bw_hz, (int32_t)lroundf(clampf(cfg->ref_bw_hz, AE_REF_BW_MIN_HZ, AE_REF_BW_MAX_HZ)));
    atomic_store(&e->cfg_floor_cdb, to_cdb(cfg->noise_floor_dbfs, AE_FLOOR_MIN_DBFS, AE_FLOOR_MAX_DBFS));
    atomic_store(&e->cfg_atten_cdb, to_cdb(cfg->atten_db, 0.0f, AE_ATTEN_MAX_DB));
    atomic_store(&e->cfg_turnaround_ms, min_u32(cfg->turnaround_ms, AE_TURNAROUND_MAX_MS));
    atomic_store(&e->cfg_parrot_delay_ms, min_u32(cfg->parrot_delay_ms, AE_PARROT_DELAY_MAX_MS));
    atomic_store(&e->cfg_parrot_period_ms, min_u32(cfg->parrot_period_ms, AE_PARROT_PERIOD_MAX_MS));
    bool line = cfg->ptt_line == AE_LINE_RTS || cfg->ptt_line == AE_LINE_DTR;
    atomic_store(&e->cfg_ptt_line, line ? (int32_t)cfg->ptt_line : (int32_t)AE_LINE_OFF);
    atomic_store(&e->cfg_vox_on, cfg->vox_on ? 1 : 0);
    atomic_store(&e->cfg_vox_cdb, to_cdb(cfg->vox_threshold_dbfs, AE_VOX_MIN_DBFS, AE_VOX_MAX_DBFS));
    atomic_store(&e->cfg_vox_hang_ms, min_u32(cfg->vox_hang_ms, AE_VOX_HANG_MAX_MS));
    atomic_fetch_add(&e->cfg_gen, 1u);
}

void ae_get_config(ae_t *e, ae_config_t *cfg)
{
    cfg->route = (ae_route_t)atomic_load(&e->cfg_route);
    cfg->noise_on = atomic_load(&e->cfg_noise_on) != 0;
    cfg->snr_db = from_cdb(atomic_load(&e->cfg_snr_cdb));
    cfg->ref_bw_hz = (float)atomic_load(&e->cfg_ref_bw_hz);
    cfg->noise_floor_dbfs = from_cdb(atomic_load(&e->cfg_floor_cdb));
    cfg->atten_db = from_cdb(atomic_load(&e->cfg_atten_cdb));
    cfg->turnaround_ms = atomic_load(&e->cfg_turnaround_ms);
    cfg->parrot_delay_ms = atomic_load(&e->cfg_parrot_delay_ms);
    cfg->parrot_period_ms = atomic_load(&e->cfg_parrot_period_ms);
    cfg->ptt_line = (ae_line_t)atomic_load(&e->cfg_ptt_line);
    cfg->vox_on = atomic_load(&e->cfg_vox_on) != 0;
    cfg->vox_threshold_dbfs = from_cdb(atomic_load(&e->cfg_vox_cdb));
    cfg->vox_hang_ms = atomic_load(&e->cfg_vox_hang_ms);
}

void ae_init(ae_t *e, uint32_t seed)
{
    memset(e, 0, sizeof *e);
    dsp_chan_init(&e->chan, seed);
    atomic_store(&e->st_gain_q24, 1 << 24);

    ae_config_t cfg;
    ae_config_defaults(&cfg);
    ae_configure(e, &cfg); /* generation 1; seen_gen is 0, so the first block loads it */
}

void ae_set_ptt(ae_t *e, bool keyed)
{
    atomic_store(&e->ptt, keyed);
}

void ae_set_lines(ae_t *e, bool dtr, bool rts)
{
    atomic_store(&e->dtr, dtr);
    atomic_store(&e->rts, rts);
}

void ae_get_ptt(ae_t *e, ae_ptt_t *p)
{
    int32_t line = atomic_load(&e->cfg_ptt_line);
    p->cat = atomic_load(&e->ptt);
    p->line = line == AE_LINE_RTS ? atomic_load(&e->rts) : line == AE_LINE_DTR ? atomic_load(&e->dtr) : false;
    p->vox = atomic_load(&e->st_vox);
}

void ae_get_status(ae_t *e, ae_status_t *st)
{
    uint32_t ms = atomic_load(&e->st_level_ms);
    int32_t gain = atomic_load(&e->st_gain_q24);
    st->signal_dbfs = ms ? 10.0f * log10f((float)ms / (FULL_SCALE * FULL_SCALE)) : DSP_SILENCE_DBFS;
    st->gain_db = gain > 0 ? 20.0f * log10f((float)gain / 16777216.0f) : DSP_SILENCE_DBFS;
    st->gain_limited = atomic_load(&e->st_gain_limited);
    st->rx_muted = atomic_load(&e->st_rx_muted);
}

void ae_set_parrot_buffer(ae_t *e, int16_t *storage, uint32_t samples)
{
    e->par_buf = samples ? storage : NULL;
    e->par_cap = storage ? samples : 0;
}

void ae_get_parrot_status(ae_t *e, ae_parrot_status_t *st)
{
    st->state = (ae_parrot_state_t)atomic_load(&e->st_parrot_state);
    st->recorded_ms = atomic_load(&e->st_parrot_len) / (AE_PARROT_FS_HZ / 1000);
    st->capacity_ms = e->par_cap / (AE_PARROT_FS_HZ / 1000);
    st->truncated = atomic_load(&e->st_parrot_truncated);
}

bool ae_parrot_replay(ae_t *e)
{
    if (atomic_load(&e->cfg_route) != AE_ROUTE_PARROT || atomic_load(&e->ptt)) return false;
    if (atomic_load(&e->st_parrot_len) == 0 || atomic_load(&e->st_parrot_state) == AE_PARROT_RECORDING) return false;
    atomic_store(&e->replay_req, true);
    return true;
}

/* ---- audio task ------------------------------------------------------------ */

static void load_config(ae_t *e)
{
    ae_config_t cfg;
    ae_get_config(e, &cfg);

    e->route = cfg.route;
    e->noise_on = cfg.noise_on;
    e->atten_lin = db_to_lin(-cfg.atten_db);
    e->turnaround_samples = cfg.turnaround_ms * (uint32_t)(AE_FS_HZ / 1000);
    e->delay_samples = cfg.parrot_delay_ms * (uint32_t)(AE_FS_HZ / 1000);
    e->period_samples = cfg.parrot_period_ms * (uint32_t)(AE_FS_HZ / 1000);

    if (cfg.noise_on) {
        /* The floor is fixed and the signal is placed above or below it.
         * White noise of RMS N across 0..fs/2 has N * sqrt(bw / (fs/2))
         * inside the reference bandwidth. */
        float floor_rms = FULL_SCALE * db_to_lin(cfg.noise_floor_dbfs);
        float target = floor_rms * db_to_lin(cfg.snr_db) * sqrtf(cfg.ref_bw_hz / ((float)AE_FS_HZ / 2.0f));
        float max_rms = FULL_SCALE * db_to_lin(AE_SIGNAL_MAX_DBFS);
        if (target > max_rms) {
            floor_rms *= max_rms / target; /* SNR too high for this floor: lower the floor */
            target = max_rms;
        }
        e->noise_rms = floor_rms;
        e->target_rms = target;
    } else {
        e->noise_rms = 0.0f;
        e->target_rms = 0.0f;
    }

    if (e->route == AE_ROUTE_MONITOR) {
        e->was_keyed = false;
        e->mute_left = 0;
    }
    /* Set the detector up again only if its own settings changed: doing it
     * for any change would drop the VOX, and the transmission, mid-way. */
    int32_t vox_cdb = atomic_load(&e->cfg_vox_cdb);
    if (cfg.vox_on && (!e->vox_enabled || vox_cdb != e->vox_cdb || cfg.vox_hang_ms != e->vox_hang_ms)) {
        dsp_vox_init(&e->vox, cfg.vox_threshold_dbfs, cfg.vox_hang_ms, AE_FS_HZ);
        e->vox_cdb = vox_cdb;
        e->vox_hang_ms = cfg.vox_hang_ms;
    }
    e->vox_enabled = cfg.vox_on;

    if (e->route != AE_ROUTE_PARROT) {
        /* Whatever the parrot was doing ends here. The recording stays. */
        e->par_state = AE_PARROT_IDLE;
        atomic_store(&e->st_parrot_state, AE_PARROT_IDLE);
        atomic_store(&e->replay_req, false);
    }
    e->plan_dirty = true;
}

static void sync_config(ae_t *e)
{
    uint32_t gen = atomic_load(&e->cfg_gen);
    if (gen != e->seen_gen) {
        e->seen_gen = gen;
        load_config(e);
    }
}

/* Fold n source samples into the level estimate. Returns whether it moved. */
static bool track_level(ae_t *e, const int16_t *in, size_t n)
{
    const float silence_rms = FULL_SCALE * db_to_lin(AE_SILENCE_DBFS);
    const float silence_ms = silence_rms * silence_rms;
    bool moved = false;

    for (size_t i = 0; i < n; i++) {
        int32_t s = in[i];
        e->chunk_sum += (uint64_t)((int64_t)s * s);
        if (++e->chunk_n < CHUNK) continue;

        float ms = (float)e->chunk_sum / (float)CHUNK;
        e->chunk_sum = 0;
        e->chunk_n = 0;

        if (ms < silence_ms) {
            if (e->quiet_n < LEVEL_GAP) e->quiet_n += CHUNK;
            if (e->quiet_n >= LEVEL_GAP) e->level_n = 0; /* forget: the next loud chunk starts over */
            continue;
        }
        e->quiet_n = 0;
        if (e->level_n < LEVEL_WINDOW) e->level_n += CHUNK;
        e->level_ms += (ms - e->level_ms) * ((float)CHUNK / (float)e->level_n);
        moved = true;
    }
    return moved;
}

static void apply_plan(ae_t *e)
{
    const float gain_max = db_to_lin(AE_GAIN_MAX_DB);
    float gain = 1.0f;
    bool limited = false;

    if (e->noise_on && e->level_ms > 0.0f) {
        gain = e->target_rms / sqrtf(e->level_ms);
        if (gain > gain_max) {
            gain = gain_max;
            limited = true;
        }
    }
    gain *= e->atten_lin;

    dsp_chan_set_gain(&e->chan, gain);
    dsp_chan_set_noise_rms(&e->chan, e->noise_rms);
    e->plan_dirty = false;

    atomic_store(&e->st_gain_q24, e->chan.gain_q24);
    atomic_store(&e->st_gain_limited, limited);
}

/* The receive channel for one block. keyed is the PTT state for all of it. */
static void rx_block(ae_t *e, bool keyed, const int16_t *src, int16_t *out, size_t n)
{
    /* How much of the start of this block is muted. */
    size_t skip = 0;
    if (e->route != AE_ROUTE_MONITOR) {
        if (keyed) {
            e->was_keyed = true;
            skip = n;
        } else {
            if (e->was_keyed) {
                e->was_keyed = false;
                e->mute_left = e->turnaround_samples;
            }
            skip = e->mute_left < n ? e->mute_left : n;
            e->mute_left -= (uint32_t)skip;
        }
    }
    if (skip) memset(out, 0, skip * sizeof(int16_t));
    atomic_store(&e->st_rx_muted, skip > 0);
    if (skip == n) return;

    const int16_t *in = src ? src + skip : NULL;
    size_t m = n - skip;
    if (in && track_level(e, in, m)) {
        e->plan_dirty = true;
        atomic_store(&e->st_level_ms, (uint32_t)e->level_ms);
    }
    if (e->plan_dirty) apply_plan(e);
    dsp_chan_process(&e->chan, in, out + skip, m);
}

/* ---- parrot ------------------------------------------------------------------ */

/* Add n samples of transmit audio (silence if tx is NULL) to the recording. */
static void parrot_record(ae_t *e, const int16_t *tx, size_t n)
{
    enum { PIECE = 64 };
    static const int16_t silence[PIECE];

    while (n) {
        size_t k = n < PIECE ? n : PIECE;
        int16_t low[PIECE / DSP_RS_FACTOR];
        size_t got = dsp_decim4(&e->par_down, tx ? tx : silence, k, low);
        size_t room = e->par_cap - e->par_len;
        if (got > room) {
            got = room;
            e->par_truncated = true;
        }
        memcpy(e->par_buf + e->par_len, low, got * sizeof(int16_t));
        e->par_len += (uint32_t)got;
        if (tx) tx += k;
        n -= k;
    }
}

/* When the replay of the recording just finished is due. */
static uint64_t parrot_due(const ae_t *e)
{
    uint64_t due = e->par_unkey_at + (e->delay_samples > PAR_TAIL ? e->delay_samples : PAR_TAIL);
    if (e->period_samples) {
        uint64_t periods = (due - e->par_key_at + e->period_samples - 1) / e->period_samples;
        due = e->par_key_at + periods * e->period_samples;
    }
    return due;
}

/* Up to k stored samples to replay next: the recording, then the zeros that
 * let the interpolator finish. Returns how many; 0 when the replay is over. */
static size_t parrot_next(ae_t *e, const int16_t **from, size_t k)
{
    static const int16_t zeros[PAR_FLUSH];
    uint32_t left = e->par_len - e->par_pos;

    if (left) {
        if (k > left) k = left;
        *from = e->par_buf + e->par_pos;
        e->par_pos += (uint32_t)k;
        return k;
    }
    if (k > e->par_flush) k = e->par_flush;
    *from = zeros;
    e->par_flush -= (uint32_t)k;
    return k;
}

/* Fill m samples with the replay; silence once it is over. */
static void parrot_play(ae_t *e, int16_t *out, size_t m)
{
    const int16_t *from;

    while (m) {
        if (e->par_frac_n) {
            *out++ = e->par_frac[DSP_RS_FACTOR - e->par_frac_n--];
            m--;
        } else if (m >= DSP_RS_FACTOR) {
            size_t got = parrot_next(e, &from, m / DSP_RS_FACTOR);
            if (!got) break;
            dsp_interp4(&e->par_up, from, got, out);
            out += got * DSP_RS_FACTOR;
            m -= got * DSP_RS_FACTOR;
        } else {
            if (!parrot_next(e, &from, 1)) break;
            dsp_interp4(&e->par_up, from, 1, e->par_frac);
            e->par_frac_n = DSP_RS_FACTOR;
        }
    }
    if (m) {
        memset(out, 0, m * sizeof(int16_t));
        e->par_state = AE_PARROT_IDLE;
    }
}

/* One block of the parrot route: record tx while keyed, replay into out when
 * due. Returns whether out now holds receive audio. */
static bool parrot_block(ae_t *e, bool keyed, const int16_t *tx, int16_t *out, size_t n)
{
    bool again = atomic_exchange(&e->replay_req, false);

    if (keyed) {
        if (e->par_state != AE_PARROT_RECORDING || e->par_tail_left) {
            /* A new transmission replaces the last one, whatever was going on. */
            e->par_state = AE_PARROT_RECORDING;
            e->par_tail_left = 0;
            e->par_len = 0;
            e->par_truncated = false;
            e->par_key_at = e->now;
            dsp_decim4_init(&e->par_down);
        }
        parrot_record(e, tx, n);
        return false;
    }

    if (e->par_state == AE_PARROT_RECORDING) {
        if (!e->par_tail_left) { /* just unkeyed */
            e->par_unkey_at = e->now;
            e->par_tail_left = PAR_TAIL;
        }
        size_t k = e->par_tail_left < n ? e->par_tail_left : n;
        parrot_record(e, tx, k);
        e->par_tail_left -= (uint32_t)k;
        if (e->par_tail_left) return false;
        e->par_play_at = parrot_due(e);
        e->par_state = AE_PARROT_WAITING;
    } else if (again && e->par_len) {
        e->par_play_at = e->now;
        e->par_state = AE_PARROT_WAITING;
    }

    size_t lead = 0; /* silence before the replay starts in this block */
    if (e->par_state == AE_PARROT_WAITING) {
        if (e->par_play_at >= e->now + n) return false;
        if (e->par_play_at > e->now) lead = (size_t)(e->par_play_at - e->now);
        e->par_state = AE_PARROT_PLAYING;
        e->par_pos = min_u32(PAR_SKIP, e->par_len);
        e->par_flush = PAR_FLUSH;
        e->par_frac_n = 0;
        dsp_interp4_init(&e->par_up);
        e->level_n = 0; /* measure this replay's level afresh */
        e->quiet_n = 0;
    }
    if (e->par_state != AE_PARROT_PLAYING) return false;

    memset(out, 0, lead * sizeof(int16_t));
    parrot_play(e, out + lead, n - lead);
    return true;
}

/* Whether the VOX is open after n samples of transmit audio (silence if tx
 * is NULL). */
static bool vox_block(ae_t *e, const int16_t *tx, size_t n)
{
    enum { PIECE = 64 };
    static const int16_t silence[PIECE];

    if (!e->vox_enabled) return false;
    if (tx) return dsp_vox_process(&e->vox, tx, n);
    while (n && e->vox.on) {
        size_t k = n < PIECE ? n : PIECE;
        dsp_vox_process(&e->vox, silence, k);
        n -= k;
    }
    return e->vox.on;
}

void ae_process(ae_t *e, const int16_t *tx, int16_t *out, size_t n)
{
    sync_config(e);
    atomic_store(&e->st_vox, vox_block(e, tx, n));
    ae_ptt_t ptt;
    ae_get_ptt(e, &ptt);
    bool keyed = ptt.cat || ptt.line || ptt.vox;
    const int16_t *src = NULL;

    if (e->route == AE_ROUTE_MONITOR) {
        src = tx;
    } else if (e->route == AE_ROUTE_PARROT && e->par_buf) {
        if (parrot_block(e, keyed, tx, out, n)) src = out;
        atomic_store(&e->st_parrot_state, (int32_t)e->par_state);
        atomic_store(&e->st_parrot_len, e->par_len);
        atomic_store(&e->st_parrot_truncated, e->par_truncated);
    }
    rx_block(e, keyed, src, out, n);
    e->now += n;
}

void ae_rx_process(ae_t *e, const int16_t *src, int16_t *out, size_t n)
{
    sync_config(e);
    ae_ptt_t ptt;
    ae_get_ptt(e, &ptt);
    rx_block(e, ptt.cat || ptt.line, src, out, n);
}
