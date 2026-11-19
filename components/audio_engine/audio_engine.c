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
}

void ae_configure(ae_t *e, const ae_config_t *cfg)
{
    atomic_store(&e->cfg_route, cfg->route == AE_ROUTE_RADIO ? 1 : 0);
    atomic_store(&e->cfg_noise_on, cfg->noise_on ? 1 : 0);
    atomic_store(&e->cfg_snr_cdb, to_cdb(cfg->snr_db, -60.0f, 80.0f));
    atomic_store(&e->cfg_ref_bw_hz, (int32_t)lroundf(clampf(cfg->ref_bw_hz, 100.0f, (float)AE_FS_HZ / 2.0f)));
    atomic_store(&e->cfg_floor_cdb, to_cdb(cfg->noise_floor_dbfs, -90.0f, AE_SIGNAL_MAX_DBFS));
    atomic_store(&e->cfg_atten_cdb, to_cdb(cfg->atten_db, 0.0f, 120.0f));
    atomic_store(&e->cfg_turnaround_ms, cfg->turnaround_ms > 5000u ? 5000u : cfg->turnaround_ms);
    atomic_fetch_add(&e->cfg_gen, 1u);
}

void ae_get_config(ae_t *e, ae_config_t *cfg)
{
    cfg->route = atomic_load(&e->cfg_route) ? AE_ROUTE_RADIO : AE_ROUTE_MONITOR;
    cfg->noise_on = atomic_load(&e->cfg_noise_on) != 0;
    cfg->snr_db = from_cdb(atomic_load(&e->cfg_snr_cdb));
    cfg->ref_bw_hz = (float)atomic_load(&e->cfg_ref_bw_hz);
    cfg->noise_floor_dbfs = from_cdb(atomic_load(&e->cfg_floor_cdb));
    cfg->atten_db = from_cdb(atomic_load(&e->cfg_atten_cdb));
    cfg->turnaround_ms = atomic_load(&e->cfg_turnaround_ms);
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

void ae_get_status(ae_t *e, ae_status_t *st)
{
    uint32_t ms = atomic_load(&e->st_level_ms);
    int32_t gain = atomic_load(&e->st_gain_q24);
    st->signal_dbfs = ms ? 10.0f * log10f((float)ms / (FULL_SCALE * FULL_SCALE)) : DSP_SILENCE_DBFS;
    st->gain_db = gain > 0 ? 20.0f * log10f((float)gain / 16777216.0f) : DSP_SILENCE_DBFS;
    st->gain_limited = atomic_load(&e->st_gain_limited);
    st->rx_muted = atomic_load(&e->st_rx_muted);
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

    if (e->route != AE_ROUTE_RADIO) {
        e->was_keyed = false;
        e->mute_left = 0;
    }
    e->plan_dirty = true;
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

void ae_rx_process(ae_t *e, const int16_t *src, int16_t *out, size_t n)
{
    uint32_t gen = atomic_load(&e->cfg_gen);
    if (gen != e->seen_gen) {
        e->seen_gen = gen;
        load_config(e);
    }

    /* How much of the start of this block is muted. */
    size_t skip = 0;
    if (e->route == AE_ROUTE_RADIO) {
        if (atomic_load(&e->ptt)) {
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
