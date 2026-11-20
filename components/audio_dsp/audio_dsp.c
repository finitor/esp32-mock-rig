/* audio_dsp: see audio_dsp.h. */
#include "audio_dsp.h"

#include <math.h>
#include <string.h>

static int16_t sat16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int32_t abs16(int16_t s)
{
    return s < 0 ? -(int32_t)s : (int32_t)s;
}

/* ---- resampler ----------------------------------------------------------
 *
 * Kaiser-windowed sinc, Q15, unity gain at DC. Regenerate with
 * host/tools/gen_resampler_fir.py --table; do not edit by hand.
 * The taps' absolute values sum to 1.72, so a full-scale input cannot
 * overflow the 32-bit accumulator.
 */
static const int16_t rs_coef[DSP_RS_TAPS] = {
         1,      1,      0,     -2,     -4,     -4,     -2,      4,
        10,     13,     10,     -1,    -18,    -31,    -32,    -15,
        18,     54,     73,     57,      5,    -68,   -127,   -135,
       -73,     47,    177,    248,    208,     46,   -183,   -377,
      -424,   -261,     84,    478,    728,    666,    230,   -466,
     -1147,  -1453,  -1075,    110,   1962,   4085,   5943,   7024,
      7024,   5943,   4085,   1962,    110,  -1075,  -1453,  -1147,
      -466,    230,    666,    728,    478,     84,   -261,   -424,
      -377,   -183,     46,    208,    248,    177,     47,    -73,
      -135,   -127,    -68,      5,     57,     73,     54,     18,
       -15,    -32,    -31,    -18,     -1,     10,     13,     10,
         4,     -2,     -4,     -4,     -2,      0,      1,      1,
};

void dsp_decim4_init(dsp_decim4_t *d)
{
    memset(d, 0, sizeof *d);
}

size_t dsp_decim4(dsp_decim4_t *d, const int16_t *in, size_t n_in, int16_t *out)
{
    size_t n_out = 0;
    for (size_t i = 0; i < n_in; i++) {
        d->hist[d->pos] = in[i];
        d->hist[d->pos + DSP_RS_TAPS] = in[i];
        d->pos = (d->pos + 1) % DSP_RS_TAPS;

        if (++d->phase < DSP_RS_FACTOR) continue;
        d->phase = 0;

        /* The last DSP_RS_TAPS inputs, oldest first. The filter is
         * symmetric, so the direction of the dot product does not matter. */
        const int16_t *w = &d->hist[d->pos];
        int32_t acc = 0;
        for (int k = 0; k < DSP_RS_TAPS; k++) acc += (int32_t)rs_coef[k] * w[k];
        out[n_out++] = sat16((acc + (1 << 14)) >> 15);
    }
    return n_out;
}

#define RS_PHASE_TAPS (DSP_RS_TAPS / DSP_RS_FACTOR)

void dsp_interp4_init(dsp_interp4_t *u)
{
    memset(u, 0, sizeof *u);
}

void dsp_interp4(dsp_interp4_t *u, const int16_t *in, size_t n_in, int16_t *out)
{
    for (size_t i = 0; i < n_in; i++) {
        u->hist[u->pos] = in[i];
        u->hist[u->pos + RS_PHASE_TAPS] = in[i];
        u->pos = (u->pos + 1) % RS_PHASE_TAPS;

        /* w[RS_PHASE_TAPS - 1 - k] is the input k samples ago. Output
         * 4n + p is the sum over k of coef[p + 4k] * x[n - k], times 4 to
         * make up for the three zeros between input samples. */
        const int16_t *w = &u->hist[u->pos];
        for (int p = 0; p < DSP_RS_FACTOR; p++) {
            int32_t acc = 0;
            for (int k = 0; k < RS_PHASE_TAPS; k++)
                acc += (int32_t)rs_coef[p + DSP_RS_FACTOR * k] * w[RS_PHASE_TAPS - 1 - k];
            *out++ = sat16((acc + (1 << 12)) >> 13);
        }
    }
}

/* ---- noise and channel -------------------------------------------------- */

void dsp_rng_seed(dsp_rng_t *r, uint32_t seed)
{
    r->state = seed ? seed : 0x6D2B79F5u; /* xorshift must not start at zero */
}

static uint32_t rng_next(dsp_rng_t *r)
{
    uint32_t x = r->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->state = x;
    return x;
}

int32_t dsp_gauss(dsp_rng_t *r)
{
    /* Twelve uniform 16-bit values: the sum has variance 65536^2 and is
     * Gaussian to well past 3 sigma, which is all channel noise needs. */
    int32_t sum = 0;
    for (int i = 0; i < 6; i++) {
        uint32_t x = rng_next(r);
        sum += (int32_t)(x >> 16) + (int32_t)(x & 0xFFFFu);
    }
    return sum - 393210; /* 12 * 32767.5 */
}

#define DSP_GAIN_ONE (1 << 24)

void dsp_chan_init(dsp_chan_t *c, uint32_t seed)
{
    dsp_rng_seed(&c->rng, seed);
    c->gain_q24 = DSP_GAIN_ONE;
    c->noise_rms_q8 = 0;
}

void dsp_chan_set_gain_db(dsp_chan_t *c, float db)
{
    if (db >= 0.0f) c->gain_q24 = DSP_GAIN_ONE;
    else if (db < -120.0f) c->gain_q24 = 0;
    else c->gain_q24 = (int32_t)lroundf((float)DSP_GAIN_ONE * powf(10.0f, db / 20.0f));
}

void dsp_chan_set_gain(dsp_chan_t *c, float gain)
{
    if (!(gain > 0.0f)) gain = 0.0f; /* also catches NaN */
    if (gain > DSP_CHAN_GAIN_MAX) gain = DSP_CHAN_GAIN_MAX;
    c->gain_q24 = (int32_t)lroundf((float)DSP_GAIN_ONE * gain);
}

void dsp_chan_set_noise_rms(dsp_chan_t *c, float rms)
{
    if (!(rms > 0.0f)) rms = 0.0f; /* also catches NaN */
    if (rms > 32767.0f) rms = 32767.0f;
    c->noise_rms_q8 = (uint32_t)lroundf(rms * 256.0f);
}

void dsp_chan_process(dsp_chan_t *c, const int16_t *in, int16_t *out, size_t n)
{
    /* Signal and noise are added with 8 fractional bits and rounded once, so
     * a signal of only a few counts is dithered by the noise instead of
     * being rounded on its own first. */
    for (size_t i = 0; i < n; i++) {
        int64_t acc = 0;
        if (in) acc = ((int64_t)in[i] * c->gain_q24 + (1 << 15)) >> 16;
        if (c->noise_rms_q8) {
            /* gauss has sigma 2^16, the level is scaled by 2^8 */
            int64_t nz = (int64_t)dsp_gauss(&c->rng) * (int64_t)c->noise_rms_q8;
            acc += (nz + (1 << 15)) >> 16;
        }
        int64_t s = (acc + (1 << 7)) >> 8;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
    }
}

float dsp_noise_rms_for_snr(float signal_rms, float snr_db, float fs_hz, float ref_bw_hz)
{
    if (!(signal_rms > 0.0f) || !(ref_bw_hz > 0.0f) || !(fs_hz > 0.0f)) return 0.0f;
    /* Noise power inside the reference bandwidth is the total times
     * ref_bw / (fs / 2), so the total has to be larger by the inverse. */
    float in_band = signal_rms / powf(10.0f, snr_db / 20.0f);
    return in_band * sqrtf((fs_hz / 2.0f) / ref_bw_hz);
}

/* ---- level meter and VOX ------------------------------------------------ */

void dsp_meter_reset(dsp_meter_t *m)
{
    memset(m, 0, sizeof *m);
}

void dsp_meter_add(dsp_meter_t *m, const int16_t *in, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int32_t a = abs16(in[i]);
        m->sum_sq += (uint64_t)((int64_t)a * a);
        if (a > m->peak) m->peak = a;
        if (a >= 32767) m->clipped++;
    }
    m->count += (uint32_t)n;
}

float dsp_meter_rms(const dsp_meter_t *m)
{
    if (m->count == 0) return 0.0f;
    return (float)sqrt((double)m->sum_sq / (double)m->count);
}

static float to_dbfs(float v)
{
    if (!(v > 0.0f)) return DSP_SILENCE_DBFS;
    float db = 20.0f * log10f(v / 32768.0f);
    return db < DSP_SILENCE_DBFS ? DSP_SILENCE_DBFS : db;
}

float dsp_meter_rms_dbfs(const dsp_meter_t *m)
{
    return to_dbfs(dsp_meter_rms(m));
}

float dsp_meter_peak_dbfs(const dsp_meter_t *m)
{
    return to_dbfs((float)m->peak);
}

void dsp_vox_init(dsp_vox_t *v, float threshold_dbfs, uint32_t hang_ms, uint32_t fs_hz)
{
    float t = 32768.0f * powf(10.0f, threshold_dbfs / 20.0f);
    if (!(t >= 1.0f)) t = 1.0f;
    if (t > 32768.0f) t = 32768.0f;
    v->threshold = (int32_t)lroundf(t);
    v->hang_samples = (uint32_t)(((uint64_t)hang_ms * fs_hz) / 1000u);
    v->hang_left = 0;
    v->on = false;
}

bool dsp_vox_process(dsp_vox_t *v, const int16_t *in, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (abs16(in[i]) >= v->threshold) {
            v->on = true;
            v->hang_left = v->hang_samples;
        } else {
            if (v->hang_left > 0) v->hang_left--;
            if (v->hang_left == 0) v->on = false;
        }
    }
    return v->on;
}

/* ---- fading ---------------------------------------------------------------
 *
 * The Hilbert transformer's taps at +1, +3, ... +63 from the middle, Q15;
 * those at -k are the negatives, and the even ones are zero. Then the filter
 * that shapes the gains' spectrum. Both from host/tools/gen_hilbert_fir.py
 * --table; do not edit by hand.
 */
static const int16_t hilbert_coef[DSP_HILBERT_PAIRS] = {
     20849,   6918,   4114,   2899,   2214,   1771,   1458,   1223,
      1040,    892,    770,    667,    579,    504,    437,    379,
       328,    283,    244,    208,    177,    150,    125,    104,
        86,     70,     56,     44,     34,     25,     18,     12,
};
static const float fade_gain_coef[DSP_FADE_GAIN_TAPS] = {
    5.972436973e-05f, 1.034535186e-04f, 1.757791050e-04f, 2.929662034e-04f,
    4.789565190e-04f, 7.680737910e-04f, 1.208197950e-03f, 1.864238767e-03f,
    2.821585837e-03f, 4.189028259e-03f, 6.100446251e-03f, 8.714414465e-03f,
    1.221077164e-02f, 1.678325909e-02f, 2.262756286e-02f, 2.992454479e-02f,
    3.881911382e-02f, 4.939602169e-02f, 6.165476118e-02f, 7.548654681e-02f,
    9.065688428e-02f, 1.067973093e-01f, 1.234093669e-01f, 1.398827718e-01f,
    1.555280138e-01f, 1.696216619e-01f, 1.814605795e-01f, 1.904195632e-01f,
    1.960058923e-01f, 1.979041865e-01f, 1.960058923e-01f, 1.904195632e-01f,
    1.814605795e-01f, 1.696216619e-01f, 1.555280138e-01f, 1.398827718e-01f,
    1.234093669e-01f, 1.067973093e-01f, 9.065688428e-02f, 7.548654681e-02f,
    6.165476118e-02f, 4.939602169e-02f, 3.881911382e-02f, 2.992454479e-02f,
    2.262756286e-02f, 1.678325909e-02f, 1.221077164e-02f, 8.714414465e-03f,
    6.100446251e-03f, 4.189028259e-03f, 2.821585837e-03f, 1.864238767e-03f,
    1.208197950e-03f, 7.680737910e-04f, 4.789565190e-04f, 2.929662034e-04f,
    1.757791050e-04f, 1.034535186e-04f, 5.972436973e-05f,
};

/* Noise for the gains: a value for each seed, stream and gain sample, the
 * same wherever and whenever it is asked for. Four 16-bit pieces of a
 * 64-bit hash, summed: near enough to Gaussian once 59 of them have been
 * through the filter, and exact integer arithmetic, so two boards agree. */
static float fade_noise(uint32_t seed, unsigned stream, uint64_t k)
{
    uint64_t z = k * 4u + stream + ((uint64_t)seed << 32 | (seed ^ 0x9E3779B9u)) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    int32_t sum = (int32_t)(z & 0xFFFF) + (int32_t)((z >> 16) & 0xFFFF) + (int32_t)((z >> 32) & 0xFFFF) +
                  (int32_t)(z >> 48) - 2 * 65535;
    return (float)sum * (1.0f / 37837.2f); /* unit variance: 65536 * sqrt(4 / 12) */
}

/* The gain at sample k + which (0 or 1), for one stream. */
static float fade_gain(const dsp_fade_t *f, int stream, int which)
{
    const float *w = &f->noise[stream][which];
    float acc = 0.0f;
    for (int j = 0; j < DSP_FADE_GAIN_TAPS; j++) acc += fade_gain_coef[j] * w[j];
    return acc;
}

/* Everything about the gains follows from the seed, the spread and t. */
static void fade_place(dsp_fade_t *f)
{
    /* t * inc, a 96-bit product, in two halves: the gain sample and the
     * fraction. Integer arithmetic, so stepping sample by sample from any
     * start arrives at the same values. */
    uint64_t lo = (f->t & 0xFFFFFFFFu) * (uint64_t)f->inc;
    f->k = (f->t >> 32) * (uint64_t)f->inc + (lo >> 32);
    f->frac = (uint32_t)lo;

    const uint64_t first = f->k - (DSP_FADE_GAIN_TAPS - 1) / 2; /* wraps below zero, which the hash does not mind */
    for (int s = 0; s < 4; s++) {
        for (int j = 0; j <= DSP_FADE_GAIN_TAPS; j++) f->noise[s][j] = fade_noise(f->seed, (unsigned)s, first + (uint64_t)j);
        f->g0[s] = fade_gain(f, s, 0);
        f->g1[s] = fade_gain(f, s, 1);
    }
}

/* On to the next gain sample. */
static void fade_step(dsp_fade_t *f)
{
    f->k++;
    const uint64_t last = f->k + (DSP_FADE_GAIN_TAPS - 1) / 2 + 1;
    for (int s = 0; s < 4; s++) {
        memmove(&f->noise[s][0], &f->noise[s][1], DSP_FADE_GAIN_TAPS * sizeof(float));
        f->noise[s][DSP_FADE_GAIN_TAPS] = fade_noise(f->seed, (unsigned)s, last);
        f->g0[s] = f->g1[s];
        f->g1[s] = fade_gain(f, s, 1);
    }
}

void dsp_fade_init(dsp_fade_t *f, uint32_t seed)
{
    memset(f, 0, sizeof *f);
    f->seed = seed;
    f->rot_re = 1.0f;
    f->step_re = 1.0f;
    fade_place(f);
}

void dsp_fade_set(dsp_fade_t *f, uint32_t spread_chz, uint32_t path_delay, int32_t offset_dhz)
{
    if (spread_chz > DSP_FADE_SPREAD_MAX_CHZ) spread_chz = DSP_FADE_SPREAD_MAX_CHZ;
    f->path_delay = path_delay < DSP_FADE_PATH_MAX ? path_delay : DSP_FADE_PATH_MAX - 1;
    if (spread_chz != f->spread_chz) {
        f->spread_chz = spread_chz;
        /* gain samples per audio sample: 32 * spread / 12000, times 2^32 */
        f->inc = (uint32_t)((((uint64_t)DSP_FADE_GAIN_RATE * spread_chz) << 32) / (100u * DSP_FADE_FS_HZ));
        fade_place(f);
    }

    bool shift = offset_dhz != 0;
    if (shift) {
        float w = 2.0f * 3.14159265f * ((float)offset_dhz / 10.0f) / (float)DSP_FADE_FS_HZ;
        f->step_re = cosf(w);
        f->step_im = sinf(w);
    }
    if (shift != f->shift) {
        f->rot_re = 1.0f;
        f->rot_im = 0.0f;
    }
    f->shift = shift;
}

void dsp_fade_seek(dsp_fade_t *f, uint32_t seed, uint64_t t)
{
    f->seed = seed;
    f->t = t;
    fade_place(f);
}

uint64_t dsp_fade_time(const dsp_fade_t *f)
{
    return f->t;
}

void dsp_fade_process(dsp_fade_t *f, const int16_t *in, int16_t *out, size_t n)
{
    const float frac_scale = 1.0f / 4294967296.0f;
    const float half_power = 0.70710678f; /* two paths of equal power make one of unit power */

    for (size_t i = 0; i < n; i++) {
        f->hist[f->pos] = in[i];
        f->hist[f->pos + DSP_HILBERT_TAPS] = in[i];
        f->pos = (f->pos + 1) % DSP_HILBERT_TAPS;

        /* w holds the last 127 inputs, oldest first. The real part is the
         * one in the middle; the imaginary part its Hilbert transform. */
        const int16_t *w = &f->hist[f->pos];
        int64_t acc = 0;
        for (int p = 0; p < DSP_HILBERT_PAIRS; p++) {
            int k = 2 * p + 1;
            acc += (int64_t)hilbert_coef[p] * ((int32_t)w[DSP_FADE_DELAY - k] - (int32_t)w[DSP_FADE_DELAY + k]);
        }
        float re = (float)w[DSP_FADE_DELAY];
        float im = (float)acc * (1.0f / 32768.0f);

        float y_re = re, y_im = im;
        if (f->spread_chz) {
            /* The second path is the same signal, later. */
            f->path_re[f->path_pos] = re;
            f->path_im[f->path_pos] = im;
            uint32_t at = (f->path_pos + DSP_FADE_PATH_MAX - f->path_delay) % DSP_FADE_PATH_MAX;
            float re2 = f->path_re[at], im2 = f->path_im[at];
            f->path_pos = (f->path_pos + 1) % DSP_FADE_PATH_MAX;

            float u = (float)f->frac * frac_scale;
            float a_re = f->g0[0] + (f->g1[0] - f->g0[0]) * u;
            float a_im = f->g0[1] + (f->g1[1] - f->g0[1]) * u;
            float b_re = f->g0[2] + (f->g1[2] - f->g0[2]) * u;
            float b_im = f->g0[3] + (f->g1[3] - f->g0[3]) * u;
            y_re = (re * a_re - im * a_im + re2 * b_re - im2 * b_im) * half_power;
            y_im = (re * a_im + im * a_re + re2 * b_im + im2 * b_re) * half_power;

            uint32_t before = f->frac;
            f->frac += f->inc;
            if (f->frac < before) fade_step(f);
        }
        f->t++;

        if (f->shift) {
            float o = y_re * f->rot_re - y_im * f->rot_im;
            float r = f->rot_re * f->step_re - f->rot_im * f->step_im;
            f->rot_im = f->rot_re * f->step_im + f->rot_im * f->step_re;
            f->rot_re = r;
            if (++f->rot_n >= 512) { /* keep the rotating unit vector a unit vector */
                f->rot_n = 0;
                float mag = sqrtf(f->rot_re * f->rot_re + f->rot_im * f->rot_im);
                f->rot_re /= mag;
                f->rot_im /= mag;
            }
            y_re = o;
        }
        long v = lrintf(y_re);
        out[i] = sat16(v > 32767 ? 32767 : v < -32768 ? -32768 : (int32_t)v);
    }
}

/* ---- FIFO ---------------------------------------------------------------- */

bool dsp_fifo_init(dsp_fifo_t *f, int16_t *storage, uint32_t capacity)
{
    if (!storage || capacity < 2 || (capacity & (capacity - 1)) != 0) return false;
    memset(f, 0, sizeof *f);
    f->buf = storage;
    f->mask = capacity - 1;
    atomic_init(&f->wr, 0);
    atomic_init(&f->rd, 0);
    return true;
}

uint32_t dsp_fifo_level(dsp_fifo_t *f)
{
    return atomic_load_explicit(&f->wr, memory_order_acquire) - atomic_load_explicit(&f->rd, memory_order_acquire);
}

size_t dsp_fifo_write(dsp_fifo_t *f, const int16_t *in, size_t n)
{
    uint32_t wr = atomic_load_explicit(&f->wr, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&f->rd, memory_order_acquire);
    uint32_t space = (f->mask + 1) - (wr - rd);
    uint32_t k = n < space ? (uint32_t)n : space;

    uint32_t at = wr & f->mask;
    uint32_t first = (f->mask + 1) - at;
    if (first > k) first = k;
    memcpy(&f->buf[at], in, first * sizeof(int16_t));
    memcpy(&f->buf[0], in + first, (k - first) * sizeof(int16_t));

    atomic_store_explicit(&f->wr, wr + k, memory_order_release);
    f->overruns += (uint32_t)(n - k);
    return k;
}

/* Copy k samples starting at read position rd; no bounds or state changes. */
static void fifo_copy_out(const dsp_fifo_t *f, uint32_t rd, int16_t *out, uint32_t k)
{
    uint32_t at = rd & f->mask;
    uint32_t first = (f->mask + 1) - at;
    if (first > k) first = k;
    memcpy(out, &f->buf[at], first * sizeof(int16_t));
    memcpy(out + first, &f->buf[0], (k - first) * sizeof(int16_t));
}

size_t dsp_fifo_read(dsp_fifo_t *f, int16_t *out, size_t n)
{
    uint32_t rd = atomic_load_explicit(&f->rd, memory_order_relaxed);
    uint32_t wr = atomic_load_explicit(&f->wr, memory_order_acquire);
    uint32_t avail = wr - rd;
    uint32_t k = n < avail ? (uint32_t)n : avail;

    fifo_copy_out(f, rd, out, k);
    memset(out + k, 0, (n - k) * sizeof(int16_t));

    atomic_store_explicit(&f->rd, rd + k, memory_order_release);
    f->underruns += (uint32_t)(n - k);
    return k;
}

size_t dsp_fifo_read_steered(dsp_fifo_t *f, int16_t *out, size_t n, uint32_t target, uint32_t tolerance)
{
    uint32_t level = dsp_fifo_level(f);

    if (level > target + tolerance && level > n) {
        /* Too full: the writer's clock is faster. Skip one sample. */
        uint32_t rd = atomic_load_explicit(&f->rd, memory_order_relaxed);
        atomic_store_explicit(&f->rd, rd + 1, memory_order_release);
        f->dropped++;
        return dsp_fifo_read(f, out, n);
    }
    if (level + tolerance < target && n >= 2 && level >= n - 1) {
        /* Too empty: the writer's clock is slower. Play one sample twice. */
        size_t got = dsp_fifo_read(f, out, n - 1);
        out[n - 1] = out[n - 2];
        f->repeated++;
        return got;
    }
    return dsp_fifo_read(f, out, n);
}
