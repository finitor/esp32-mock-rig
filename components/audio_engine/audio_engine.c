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
    cfg->link_buffer_ms = 80; /* LINK_BUFFER_DEFAULT_MS; the engine does not depend on the link */
    cfg->link_match = true;
    cfg->link_coord = true;
    cfg->fade = AE_FADE_OFF;
    cfg->fade_doppler_hz = 1.0f;
    cfg->fade_delay_ms = 2.0f;
    cfg->freq_offset_hz = 0.0f;
}

void ae_fade_preset(ae_fade_t fade, float *doppler_hz, float *delay_ms)
{
    static const float presets[AE_FADE_COUNT][2] = {
        [AE_FADE_MPG] = {0.1f, 0.5f},     [AE_FADE_MPP] = {1.0f, 2.0f},       [AE_FADE_MPD] = {2.0f, 4.0f},
        [AE_FADE_MPS] = {0.15f, 2.0f},    [AE_FADE_NVIS_D] = {1.0f, 7.0f},    [AE_FADE_LOWLAT_D] = {10.0f, 6.0f},
        [AE_FADE_HILAT_M] = {10.0f, 3.0f}, [AE_FADE_HILAT_D] = {30.0f, 7.0f},
    };
    bool known = fade > AE_FADE_OFF && fade < AE_FADE_CUSTOM;
    *doppler_hz = known ? presets[fade][0] : 0.0f;
    *delay_ms = known ? presets[fade][1] : 0.0f;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

void ae_configure(ae_t *e, const ae_config_t *cfg)
{
    bool known = cfg->route == AE_ROUTE_RADIO || cfg->route == AE_ROUTE_PARROT || cfg->route == AE_ROUTE_LINK;
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
    uint32_t buffer_ms = min_u32(cfg->link_buffer_ms, AE_LINK_BUFFER_MAX_MS);
    atomic_store(&e->cfg_link_buffer_ms, buffer_ms < AE_LINK_BUFFER_MIN_MS ? AE_LINK_BUFFER_MIN_MS : buffer_ms);
    atomic_store(&e->cfg_link_match, cfg->link_match ? 1 : 0);
    atomic_store(&e->cfg_link_coord, cfg->link_coord ? 1 : 0);
    bool fade_known = cfg->fade > AE_FADE_OFF && cfg->fade < AE_FADE_COUNT;
    atomic_store(&e->cfg_fade, fade_known ? (int32_t)cfg->fade : (int32_t)AE_FADE_OFF);
    atomic_store(&e->cfg_doppler_chz, to_cdb(cfg->fade_doppler_hz, AE_DOPPLER_MIN_HZ, AE_DOPPLER_MAX_HZ));
    atomic_store(&e->cfg_fade_delay_dms, (int32_t)lroundf(clampf(cfg->fade_delay_ms, 0.0f, AE_FADE_DELAY_MAX_MS) * 10.0f));
    atomic_store(&e->cfg_offset_dhz,
                 (int32_t)lroundf(clampf(cfg->freq_offset_hz, -AE_OFFSET_MAX_HZ, AE_OFFSET_MAX_HZ) * 10.0f));
    atomic_fetch_add(&e->cfg_gen, 1u);
}

uint32_t ae_config_gen(ae_t *e)
{
    return atomic_load(&e->cfg_gen);
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
    cfg->link_buffer_ms = atomic_load(&e->cfg_link_buffer_ms);
    cfg->link_match = atomic_load(&e->cfg_link_match) != 0;
    cfg->link_coord = atomic_load(&e->cfg_link_coord) != 0;
    cfg->fade = (ae_fade_t)atomic_load(&e->cfg_fade);
    cfg->fade_doppler_hz = from_cdb(atomic_load(&e->cfg_doppler_chz));
    cfg->fade_delay_ms = (float)atomic_load(&e->cfg_fade_delay_dms) / 10.0f;
    cfg->freq_offset_hz = (float)atomic_load(&e->cfg_offset_dhz) / 10.0f;
}

void ae_fade_clock(ae_t *e, uint32_t seed, uint32_t clock_ms)
{
    atomic_store(&e->fc_seed, seed);
    atomic_store(&e->fc_ms, clock_ms);
    atomic_store(&e->fc_set, true);
}

void ae_init(ae_t *e, uint32_t seed)
{
    memset(e, 0, sizeof *e);
    dsp_chan_init(&e->chan, seed);
    dsp_fade_init(&e->fade, seed);
    atomic_store(&e->st_gain_q24, 1 << 24);

    ae_config_t cfg;
    ae_config_defaults(&cfg);
    ae_configure(e, &cfg); /* generation 1; seen_gen is 0, so the first block loads it */
}

void ae_set_ptt(ae_t *e, bool keyed)
{
    atomic_store(&e->ptt, keyed);
}

void ae_set_panel_ptt(ae_t *e, bool keyed)
{
    atomic_store(&e->ptt_panel, keyed);
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
    p->panel = atomic_load(&e->ptt_panel);
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

void ae_set_link(ae_t *e, const ae_link_t *link)
{
    e->has_link = link && link->tx && link->rx;
    if (e->has_link) e->link = *link;
}

bool ae_has_link(ae_t *e)
{
    return e->has_link;
}

void ae_parrot_hold(ae_t *e)
{
    atomic_store(&e->rel_loaded, false);
    atomic_store(&e->par_hold, true);
}

bool ae_parrot_held(ae_t *e)
{
    return atomic_load(&e->st_par_held);
}

uint32_t ae_blocks(ae_t *e)
{
    return atomic_load(&e->st_blocks);
}

int16_t *ae_parrot_buffer(ae_t *e, uint32_t *len, uint32_t *cap)
{
    if (len) *len = atomic_load(&e->st_parrot_len);
    if (cap) *cap = e->par_cap;
    return e->par_buf;
}

void ae_parrot_release(ae_t *e, bool loaded, uint32_t len, bool play)
{
    if (loaded) {
        if (len > e->par_cap) len = e->par_cap;
        /* For an audio task that is not running: the status is true at once. */
        atomic_store(&e->st_parrot_len, len);
        atomic_store(&e->st_parrot_truncated, false);
    }
    atomic_store(&e->rel_len, len);
    atomic_store(&e->rel_play, play);
    atomic_store(&e->rel_loaded, loaded);
    atomic_store(&e->par_hold, false);
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
    if (atomic_load(&e->cfg_route) != AE_ROUTE_PARROT || atomic_load(&e->ptt) || atomic_load(&e->ptt_panel)) return false;
    if (atomic_load(&e->st_parrot_len) == 0 || atomic_load(&e->st_parrot_state) == AE_PARROT_RECORDING) return false;
    if (atomic_load(&e->par_hold)) return false; /* the buffer is with another task */
    atomic_store(&e->replay_req, true);
    return true;
}

/* ---- audio task ------------------------------------------------------------ */

static void load_config(ae_t *e)
{
    ae_config_t cfg;
    ae_get_config(e, &cfg);

    /* Leaving the link route: a transmission under way ends here for the
     * other board too. */
    if (e->route == AE_ROUTE_LINK && cfg.route != AE_ROUTE_LINK && e->has_link) e->link.tx(e->link.ctx, NULL, 0, false);
    e->route = cfg.route;
    e->noise_on = cfg.noise_on;
    e->atten_lin = db_to_lin(-cfg.atten_db);
    e->turnaround_samples = cfg.turnaround_ms * (uint32_t)(AE_FS_HZ / 1000);
    e->delay_samples = cfg.parrot_delay_ms * (uint32_t)(AE_FS_HZ / 1000);
    e->period_samples = cfg.parrot_period_ms * (uint32_t)(AE_FS_HZ / 1000);

    /* Fading and the frequency offset. The named conditions are numbers too. */
    float doppler_hz = cfg.fade_doppler_hz, delay_ms = cfg.fade_delay_ms;
    if (cfg.fade != AE_FADE_CUSTOM) ae_fade_preset(cfg.fade, &doppler_hz, &delay_ms);
    uint32_t spread_chz = cfg.fade == AE_FADE_OFF ? 0u : (uint32_t)lroundf(doppler_hz * 100.0f);
    int32_t offset_dhz = atomic_load(&e->cfg_offset_dhz);
    bool fade_on = spread_chz != 0 || offset_dhz != 0;
    if (fade_on && !e->fade_on) {
        /* The stage starts clean, with one sample at 12 kHz in hand: its two
         * resamplers do not always make as many samples as a block needs. */
        dsp_decim4_init(&e->fd_down);
        dsp_interp4_init(&e->fd_up);
        memset(e->fd_pend, 0, sizeof e->fd_pend);
        e->fd_pend_n = DSP_RS_FACTOR;
    }
    dsp_fade_set(&e->fade, spread_chz, (uint32_t)lroundf(delay_ms * (float)DSP_FADE_FS_HZ / 1000.0f), offset_dhz);
    e->fade_on = fade_on;
    e->fading = spread_chz != 0;

    if (cfg.noise_on) {
        /* The floor is fixed and the signal is placed above or below it.
         * White noise of RMS N across 0..fs/2 has N * sqrt(bw / (fs/2))
         * inside the reference bandwidth. */
        float floor_rms = FULL_SCALE * db_to_lin(cfg.noise_floor_dbfs);
        float target = floor_rms * db_to_lin(cfg.snr_db) * sqrtf(cfg.ref_bw_hz / ((float)AE_FS_HZ / 2.0f));
        float max_rms = FULL_SCALE * db_to_lin(e->fading ? AE_SIGNAL_MAX_FADING_DBFS : AE_SIGNAL_MAX_DBFS);
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

/* The fading stage for one block: down to 12 kHz, through the fading, and
 * back up. src NULL is silence: the stage runs on regardless, so that its
 * filters and its place in the fading carry through. src and out may be the
 * same buffer. */
static void fade_block(ae_t *e, const int16_t *src, int16_t *out, size_t n)
{
    enum { PIECE = 64 };
    static const int16_t silence[PIECE];

    /* Keep to the clock, if one is given. */
    if (atomic_load(&e->fc_set)) {
        uint32_t seed = atomic_load(&e->fc_seed);
        uint64_t want = (uint64_t)atomic_load(&e->fc_ms) * (DSP_FADE_FS_HZ / 1000u);
        uint64_t at = dsp_fade_time(&e->fade);
        int64_t ahead = (int64_t)(want - at);
        if (seed != e->fade.seed || ahead > 600 || ahead < -600) {
            dsp_fade_seek(&e->fade, seed, want); /* another fading, or 50 ms out: go there */
        } else if (ahead > 36) {
            dsp_fade_seek(&e->fade, seed, at + 1); /* 3 ms out: a sample at a time */
        } else if (ahead < -36 && at > 0) {
            dsp_fade_seek(&e->fade, seed, at - 1);
        }
    }

    while (n) {
        size_t k = n < PIECE ? n : PIECE;
        int16_t low[PIECE / DSP_RS_FACTOR + 1], up[PIECE + DSP_RS_FACTOR];
        size_t m = dsp_decim4(&e->fd_down, src ? src : silence, k, low);
        dsp_fade_process(&e->fade, low, low, m);
        dsp_interp4(&e->fd_up, low, m, up);
        m *= DSP_RS_FACTOR;

        /* Out go k samples: what was left over, then the new ones; what is
         * left of those is kept for the next piece. */
        size_t from_pend = e->fd_pend_n < k ? e->fd_pend_n : k;
        size_t from_up = k - from_pend;
        memcpy(out, e->fd_pend, from_pend * sizeof(int16_t));
        memmove(e->fd_pend, e->fd_pend + from_pend, (e->fd_pend_n - from_pend) * sizeof(int16_t));
        e->fd_pend_n -= (uint32_t)from_pend;
        memcpy(out + from_pend, up, from_up * sizeof(int16_t));
        memcpy(e->fd_pend + e->fd_pend_n, up + from_up, (m - from_up) * sizeof(int16_t));
        e->fd_pend_n += (uint32_t)(m - from_up);

        if (src) src += k;
        out += k;
        n -= k;
    }
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
    /* The level is taken from the source as it was sent, before any fading:
     * the SNR asked for is the average, and a fade falls below it. */
    if (skip < n && src && track_level(e, src + skip, n - skip)) {
        e->plan_dirty = true;
        atomic_store(&e->st_level_ms, (uint32_t)e->level_ms);
    }
    if (e->fade_on) {
        fade_block(e, src, out, n);
        src = out;
    }
    if (skip) memset(out, 0, skip * sizeof(int16_t));
    atomic_store(&e->st_rx_muted, skip > 0);
    if (skip == n) return;

    const int16_t *in = src ? src + skip : NULL;
    size_t m = n - skip;
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
        /* The fading stage delays what goes through it: start that much
         * sooner, so the replay still comes out when it is due. */
        uint64_t start = e->par_play_at;
        if (e->fade_on) start = start > AE_FADE_LATENCY ? start - AE_FADE_LATENCY : 0;
        if (start >= e->now + n) return false;
        if (start > e->now) lead = (size_t)(start - e->now);
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
    bool keyed = ae_ptt_keyed(&ptt);
    const int16_t *src = NULL;

    /* The buffer lent to another task, and taken back. */
    bool lent = atomic_load(&e->par_hold);
    if (lent && !e->par_lent) {
        e->par_state = AE_PARROT_IDLE; /* whatever it was doing ends here */
        e->par_tail_left = 0;
        atomic_store(&e->replay_req, false);
        atomic_store(&e->st_parrot_state, AE_PARROT_IDLE);
        atomic_store(&e->st_par_held, true);
    } else if (!lent) {
        if (e->par_lent) atomic_store(&e->st_par_held, false);
        /* Also when this task never saw the hold: it came and went between
         * two blocks, or while no audio was running. */
        if (atomic_exchange(&e->rel_loaded, false)) {
            e->par_len = min_u32(atomic_load(&e->rel_len), e->par_cap);
            e->par_truncated = false;
            atomic_store(&e->replay_req, false);
            if (atomic_load(&e->rel_play) && e->par_len && e->route == AE_ROUTE_PARROT && !keyed) {
                e->par_play_at = e->now;
                e->par_state = AE_PARROT_WAITING;
            }
        }
    }
    e->par_lent = lent;

    if (e->route == AE_ROUTE_MONITOR) {
        src = tx;
    } else if (e->route == AE_ROUTE_PARROT && e->par_buf && !lent) {
        if (parrot_block(e, keyed, tx, out, n)) src = out;
        atomic_store(&e->st_parrot_state, (int32_t)e->par_state);
        atomic_store(&e->st_parrot_len, e->par_len);
        atomic_store(&e->st_parrot_truncated, e->par_truncated);
    } else if (e->route == AE_ROUTE_LINK && e->has_link) {
        /* Transmit audio first: tx and out may be the same buffer. */
        e->link.tx(e->link.ctx, tx, n, keyed);
        if (e->link.rx(e->link.ctx, out, n)) src = out;
    }
    rx_block(e, keyed, src, out, n);
    e->now += n;
    atomic_fetch_add(&e->st_blocks, 1u);
}

void ae_rx_process(ae_t *e, const int16_t *src, int16_t *out, size_t n)
{
    sync_config(e);
    ae_ptt_t ptt;
    ae_get_ptt(e, &ptt);
    rx_block(e, ptt.cat || ptt.line || ptt.panel, src, out, n);
}
