/* mock_ctl: see mock_ctl.h for the command table. */
#include "mock_ctl.h"

#include <math.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* A setting as it travels. A number is nbytes of BCD, then a sign byte if
 * signed; lo and hi are in wire units (tenths of a dB, Hz, ms or tenths of a
 * second). A choice (nbytes 0) is one plain byte, 00..hi. */
typedef struct {
    uint8_t cmd;
    uint8_t nbytes;
    bool is_signed;
    int32_t lo, hi;
} setting_t;

static const setting_t settings[] = {
    {MOCK_CTL_ROUTE, 0, false, 0, AE_ROUTE_LINK},
    {MOCK_CTL_NOISE, 0, false, 0, 1},
    {MOCK_CTL_SNR, 2, true, (int32_t)(AE_SNR_MIN_DB * 10), (int32_t)(AE_SNR_MAX_DB * 10)},
    {MOCK_CTL_REF_BW, 3, false, (int32_t)AE_REF_BW_MIN_HZ, (int32_t)AE_REF_BW_MAX_HZ},
    {MOCK_CTL_FLOOR, 2, false, (int32_t)(-AE_FLOOR_MAX_DBFS * 10), (int32_t)(-AE_FLOOR_MIN_DBFS * 10)},
    {MOCK_CTL_ATTEN, 2, false, 0, (int32_t)(AE_ATTEN_MAX_DB * 10)},
    {MOCK_CTL_TURNAROUND, 2, false, 0, (int32_t)AE_TURNAROUND_MAX_MS},
    {MOCK_CTL_FADE, 0, false, 0, AE_FADE_COUNT - 1},
    {MOCK_CTL_DOPPLER, 2, false, 1, (int32_t)(AE_DOPPLER_MAX_HZ * 100)},
    {MOCK_CTL_FADE_DELAY, 2, false, 0, (int32_t)(AE_FADE_DELAY_MAX_MS * 10)},
    {MOCK_CTL_FREQ_OFFSET, 2, true, (int32_t)(-AE_OFFSET_MAX_HZ * 10), (int32_t)(AE_OFFSET_MAX_HZ * 10)},
    {MOCK_CTL_PARROT_DELAY, 2, false, 0, (int32_t)(AE_PARROT_DELAY_MAX_MS / 100)},
    {MOCK_CTL_PARROT_PERIOD, 2, false, 0, (int32_t)(AE_PARROT_PERIOD_MAX_MS / 100)},
    {MOCK_CTL_LINK_BUFFER, 2, false, (int32_t)AE_LINK_BUFFER_MIN_MS, (int32_t)AE_LINK_BUFFER_MAX_MS},
    {MOCK_CTL_LINK_MATCH, 0, false, 0, 1},
    {MOCK_CTL_LINK_COORD, 0, false, 0, 1},
    {MOCK_CTL_PTT_LINE, 0, false, 0, AE_LINE_DTR},
    {MOCK_CTL_VOX, 0, false, 0, 1},
    {MOCK_CTL_VOX_THRESHOLD, 2, false, (int32_t)(-AE_VOX_MAX_DBFS * 10), (int32_t)(-AE_VOX_MIN_DBFS * 10)},
    {MOCK_CTL_VOX_HANG, 2, false, 0, (int32_t)AE_VOX_HANG_MAX_MS},
};

static const setting_t *find_setting(uint8_t cmd)
{
    for (size_t i = 0; i < ARRAY_LEN(settings); i++) {
        if (settings[i].cmd == cmd) return &settings[i];
    }
    return NULL;
}

static int32_t tenths(float db)
{
    return (int32_t)lroundf(db * 10.0f);
}

/* Milliseconds to tenths of a second, rounded. */
static int32_t tenths_of_s(uint32_t ms)
{
    return (int32_t)((ms + 50) / 100);
}

bool mock_ctl_setting_range(uint8_t cmd, int32_t *lo, int32_t *hi)
{
    const setting_t *s = find_setting(cmd);
    if (!s) return false;
    *lo = s->lo;
    *hi = s->hi;
    return true;
}

int32_t mock_ctl_setting_get(const ae_config_t *cfg, uint8_t cmd)
{
    switch (cmd) {
    case MOCK_CTL_ROUTE:
        return (int32_t)cfg->route;
    case MOCK_CTL_NOISE:
        return cfg->noise_on ? 1 : 0;
    case MOCK_CTL_SNR:
        return tenths(cfg->snr_db);
    case MOCK_CTL_REF_BW:
        return (int32_t)lroundf(cfg->ref_bw_hz);
    case MOCK_CTL_FLOOR:
        return tenths(-cfg->noise_floor_dbfs);
    case MOCK_CTL_ATTEN:
        return tenths(cfg->atten_db);
    case MOCK_CTL_TURNAROUND:
        return (int32_t)cfg->turnaround_ms;
    case MOCK_CTL_PARROT_DELAY:
        return tenths_of_s(cfg->parrot_delay_ms);
    case MOCK_CTL_PARROT_PERIOD:
        return tenths_of_s(cfg->parrot_period_ms);
    case MOCK_CTL_FADE:
        return (int32_t)cfg->fade;
    case MOCK_CTL_DOPPLER:
        return (int32_t)lroundf(cfg->fade_doppler_hz * 100.0f);
    case MOCK_CTL_FADE_DELAY:
        return tenths(cfg->fade_delay_ms);
    case MOCK_CTL_FREQ_OFFSET:
        return tenths(cfg->freq_offset_hz);
    case MOCK_CTL_LINK_COORD:
        return cfg->link_coord ? 1 : 0;
    case MOCK_CTL_LINK_BUFFER:
        return (int32_t)cfg->link_buffer_ms;
    case MOCK_CTL_LINK_MATCH:
        return cfg->link_match ? 1 : 0;
    case MOCK_CTL_PTT_LINE:
        return (int32_t)cfg->ptt_line;
    case MOCK_CTL_VOX:
        return cfg->vox_on ? 1 : 0;
    case MOCK_CTL_VOX_THRESHOLD:
        return tenths(-cfg->vox_threshold_dbfs);
    case MOCK_CTL_VOX_HANG:
        return (int32_t)cfg->vox_hang_ms;
    default:
        return 0;
    }
}

mock_ctl_set_t mock_ctl_setting_put(mock_ctl_t *m, ae_config_t *cfg, uint8_t cmd, int32_t w)
{
    const setting_t *s = find_setting(cmd);
    if (!s) return MOCK_CTL_SET_UNKNOWN;
    if (w < s->lo || w > s->hi) return MOCK_CTL_SET_RANGE;

    switch (cmd) {
    case MOCK_CTL_ROUTE: {
        ae_parrot_status_t par;
        ae_get_parrot_status(m->engine, &par);
        if (w == AE_ROUTE_PARROT && par.capacity_ms == 0) return MOCK_CTL_SET_UNAVAILABLE; /* nowhere to record */
        if (w == AE_ROUTE_LINK && !ae_has_link(m->engine)) return MOCK_CTL_SET_UNAVAILABLE;   /* nothing to link through */
        cfg->route = (ae_route_t)w;
        break;
    }
    case MOCK_CTL_NOISE:
        cfg->noise_on = w == 1;
        break;
    case MOCK_CTL_SNR:
        cfg->snr_db = (float)w / 10.0f;
        break;
    case MOCK_CTL_REF_BW:
        cfg->ref_bw_hz = (float)w;
        break;
    case MOCK_CTL_FLOOR:
        cfg->noise_floor_dbfs = (float)-w / 10.0f;
        break;
    case MOCK_CTL_ATTEN:
        cfg->atten_db = (float)w / 10.0f;
        break;
    case MOCK_CTL_TURNAROUND:
        cfg->turnaround_ms = (uint32_t)w;
        break;
    case MOCK_CTL_PARROT_DELAY:
        cfg->parrot_delay_ms = (uint32_t)w * 100u;
        break;
    case MOCK_CTL_PARROT_PERIOD:
        cfg->parrot_period_ms = (uint32_t)w * 100u;
        break;
    case MOCK_CTL_FADE:
        cfg->fade = (ae_fade_t)w;
        break;
    case MOCK_CTL_DOPPLER:
        cfg->fade_doppler_hz = (float)w / 100.0f;
        break;
    case MOCK_CTL_FADE_DELAY:
        cfg->fade_delay_ms = (float)w / 10.0f;
        break;
    case MOCK_CTL_FREQ_OFFSET:
        cfg->freq_offset_hz = (float)w / 10.0f;
        break;
    case MOCK_CTL_LINK_COORD:
        cfg->link_coord = w == 1;
        break;
    case MOCK_CTL_LINK_BUFFER:
        cfg->link_buffer_ms = (uint32_t)w;
        break;
    case MOCK_CTL_LINK_MATCH:
        cfg->link_match = w == 1;
        break;
    case MOCK_CTL_PTT_LINE:
        cfg->ptt_line = (ae_line_t)w;
        break;
    case MOCK_CTL_VOX:
        cfg->vox_on = w == 1;
        break;
    case MOCK_CTL_VOX_THRESHOLD:
        cfg->vox_threshold_dbfs = (float)-w / 10.0f;
        break;
    case MOCK_CTL_VOX_HANG:
        cfg->vox_hang_ms = (uint32_t)w;
        break;
    default:
        return MOCK_CTL_SET_UNKNOWN;
    }
    return MOCK_CTL_SET_OK;
}

void mock_ctl_reset(mock_ctl_t *m)
{
    ae_configure(m->engine, &m->power_up);
}

/* Append a number to a reply: magnitude, then the sign byte if signed. */
static size_t put_num(uint8_t *p, int32_t w, size_t nbytes, bool is_signed)
{
    int32_t mag = w < 0 ? -w : w;
    civ_u16_to_bcd_be((uint16_t)mag, p, nbytes);
    if (is_signed) p[nbytes] = (uint8_t)(w < 0 ? 0x01 : 0x00);
    return nbytes + (is_signed ? 1u : 0u);
}

/* Read or set one setting. */
static void cmd_setting(mock_ctl_t *m, civ_t *c, const civ_frame_t *f, const setting_t *s)
{
    ae_config_t cfg;
    ae_get_config(m->engine, &cfg);

    if (f->len == 0) {
        uint8_t p[5] = {f->cmd};
        int32_t now = mock_ctl_setting_get(&cfg, f->cmd);
        size_t len = 2;
        if (s->nbytes == 0) {
            p[1] = (uint8_t)now;
        } else {
            len = 1 + put_num(&p[1], now, s->nbytes, s->is_signed);
        }
        civ_reply(c, f, p, len);
        return;
    }

    int32_t w;
    if (s->nbytes == 0) {
        if (f->len != 1) {
            civ_reply_ng(c, f);
            return;
        }
        w = f->data[0];
    } else {
        uint16_t mag;
        if (f->len != (size_t)s->nbytes + (s->is_signed ? 1u : 0u) || !civ_bcd_be_to_u16(f->data, s->nbytes, &mag)) {
            civ_reply_ng(c, f);
            return;
        }
        w = mag;
        if (s->is_signed) {
            if (f->data[s->nbytes] > 0x01) {
                civ_reply_ng(c, f);
                return;
            }
            if (f->data[s->nbytes] == 0x01) w = -w;
        }
    }
    if (mock_ctl_setting_put(m, &cfg, f->cmd, w) != MOCK_CTL_SET_OK) {
        civ_reply_ng(c, f);
        return;
    }
    ae_configure(m->engine, &cfg);
    civ_reply_ok(c, f);
}

static void cmd_status(mock_ctl_t *m, civ_t *c, const civ_frame_t *f)
{
    ae_status_t st;
    ae_get_status(m->engine, &st);

    uint8_t p[7] = {MOCK_CTL_STATUS};
    p[1] = (uint8_t)((st.rx_muted ? MOCK_CTL_FLAG_RX_MUTED : 0) | (st.gain_limited ? MOCK_CTL_FLAG_GAIN_LIMITED : 0));

    int32_t level = 9999; /* nothing measured yet */
    if (st.signal_dbfs > DSP_SILENCE_DBFS) {
        level = tenths(-st.signal_dbfs);
        if (level < 0) level = 0;
        if (level > 9998) level = 9998;
    }
    put_num(&p[2], level, 2, false);

    int32_t gain = tenths(st.gain_db);
    if (gain > 9999) gain = 9999;
    if (gain < -9999) gain = -9999;
    put_num(&p[4], gain, 2, true);

    civ_reply(c, f, p, sizeof p);
}

static void cmd_parrot_status(mock_ctl_t *m, civ_t *c, const civ_frame_t *f)
{
    ae_parrot_status_t st;
    ae_get_parrot_status(m->engine, &st);

    uint8_t p[7] = {MOCK_CTL_PARROT_STATUS};
    p[1] = (uint8_t)st.state;
    p[2] = (uint8_t)(st.truncated ? MOCK_CTL_PARROT_FLAG_TRUNCATED : 0);
    int32_t recorded = tenths_of_s(st.recorded_ms);
    int32_t capacity = tenths_of_s(st.capacity_ms);
    put_num(&p[3], recorded > 9999 ? 9999 : recorded, 2, false);
    put_num(&p[5], capacity > 9999 ? 9999 : capacity, 2, false);

    civ_reply(c, f, p, sizeof p);
}

/* ---- power-up settings kept in flash -------------------------------------------
 *
 *   "MS" <version 01> <count> then count times <command> <value, 4, little-endian>
 *   and a CRC-16 of all before it
 */
#define BLOB_VERSION 1

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF; /* CCITT-FALSE, as on the link's wire */
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

size_t mock_ctl_settings_pack(const ae_config_t *cfg, uint8_t *out, size_t cap)
{
    size_t len = 4 + 5 * ARRAY_LEN(settings) + 2;
    if (cap < len) return 0;
    out[0] = 'M';
    out[1] = 'S';
    out[2] = BLOB_VERSION;
    out[3] = (uint8_t)ARRAY_LEN(settings);
    for (size_t i = 0; i < ARRAY_LEN(settings); i++) {
        uint8_t *p = &out[4 + 5 * i];
        uint32_t v = (uint32_t)mock_ctl_setting_get(cfg, settings[i].cmd);
        p[0] = settings[i].cmd;
        p[1] = (uint8_t)v;
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)(v >> 16);
        p[4] = (uint8_t)(v >> 24);
    }
    uint16_t crc = crc16(out, len - 2);
    out[len - 2] = (uint8_t)(crc >> 8);
    out[len - 1] = (uint8_t)crc;
    return len;
}

void mock_ctl_set_store(mock_ctl_t *m, const mock_ctl_store_t *store)
{
    if (store) {
        m->store = *store;
    } else {
        memset(&m->store, 0, sizeof m->store);
    }
}

bool mock_ctl_restore(mock_ctl_t *m, const uint8_t *blob, size_t len)
{
    if (!blob || len < 6 || blob[0] != 'M' || blob[1] != 'S' || blob[2] != BLOB_VERSION) return false;
    size_t count = blob[3];
    if (len != 4 + 5 * count + 2) return false;
    if (crc16(blob, len - 2) != (uint16_t)((blob[len - 2] << 8) | blob[len - 1])) return false;

    ae_config_t cfg = m->built_in;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *p = &blob[4 + 5 * i];
        uint32_t v = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
        /* A setting this firmware does not have, a value outside today's
         * range, a route this board cannot do: each is skipped, and that
         * setting keeps the build's value. */
        (void)mock_ctl_setting_put(m, &cfg, p[0], (int32_t)v);
    }
    m->power_up = cfg;
    m->kept = true;
    ae_configure(m->engine, &cfg);
    return true;
}

bool mock_ctl_keep_settings(mock_ctl_t *m)
{
    uint8_t blob[MOCK_CTL_BLOB_MAX];
    ae_config_t cfg;
    if (!m->store.save) return false;
    ae_get_config(m->engine, &cfg);
    size_t len = mock_ctl_settings_pack(&cfg, blob, sizeof blob);
    if (!len || !m->store.save(m->store.ctx, blob, len)) return false;
    m->power_up = cfg;
    m->kept = true;
    return true;
}

bool mock_ctl_forget_settings(mock_ctl_t *m)
{
    if (!m->store.forget || !m->store.forget(m->store.ctx)) return false;
    m->power_up = m->built_in;
    m->kept = false;
    return true;
}

bool mock_ctl_settings_kept(const mock_ctl_t *m)
{
    return m->kept;
}

/* ---- clips ----------------------------------------------------------------------- */

void mock_ctl_set_clips(mock_ctl_t *m, const mock_ctl_clips_t *clips)
{
    if (clips && clips->start && clips->status) {
        m->clips = *clips;
    } else {
        memset(&m->clips, 0, sizeof m->clips);
    }
}

bool mock_ctl_clips_status(mock_ctl_t *m, mock_ctl_clips_status_t *st)
{
    memset(st, 0, sizeof *st);
    if (!m->clips.status) return false;
    m->clips.status(m->clips.ctx, st);
    if (st->slots > MOCK_CTL_CLIPS_MAX) st->slots = MOCK_CTL_CLIPS_MAX;
    return st->slots > 0;
}

mock_ctl_clip_result_t mock_ctl_clip(mock_ctl_t *m, mock_ctl_clip_op_t op, unsigned slot)
{
    mock_ctl_clips_status_t st;
    ae_parrot_status_t par;
    ae_config_t cfg;
    ae_ptt_t ptt;

    if (!mock_ctl_clips_status(m, &st) || slot < 1 || slot > st.slots) return MOCK_CTL_CLIP_NONE;
    if (st.busy) return MOCK_CTL_CLIP_BUSY;
    ae_get_parrot_status(m->engine, &par);
    ae_get_config(m->engine, &cfg);
    ae_get_ptt(m->engine, &ptt);

    switch (op) {
    case MOCK_CTL_CLIP_SAVE:
        if (par.capacity_ms == 0 || par.recorded_ms == 0) return MOCK_CTL_CLIP_NOTHING;
        if (par.state == AE_PARROT_RECORDING) return MOCK_CTL_CLIP_STATE;
        break;
    case MOCK_CTL_CLIP_PLAY:
        if (st.length_ms[slot - 1] == 0) return MOCK_CTL_CLIP_NOTHING;
        if (cfg.route != AE_ROUTE_PARROT || ptt.cat || ptt.line || ptt.vox || par.state == AE_PARROT_RECORDING) {
            return MOCK_CTL_CLIP_STATE;
        }
        break;
    case MOCK_CTL_CLIP_ERASE:
        break;
    default:
        return MOCK_CTL_CLIP_NONE;
    }
    return m->clips.start(m->clips.ctx, op, slot) ? MOCK_CTL_CLIP_OK : MOCK_CTL_CLIP_BUSY;
}

static void cmd_clip_status(mock_ctl_t *m, civ_t *c, const civ_frame_t *f)
{
    mock_ctl_clips_status_t st;
    uint8_t p[3 + 2 * MOCK_CTL_CLIPS_MAX] = {MOCK_CTL_CLIP_STATUS};

    (void)mock_ctl_clips_status(m, &st); /* none: no clips, and it says so */
    p[1] = (uint8_t)((st.busy ? MOCK_CTL_CLIP_FLAG_BUSY : 0) | (st.failed ? MOCK_CTL_CLIP_FLAG_FAILED : 0));
    p[2] = (uint8_t)st.slots;
    for (unsigned i = 0; i < st.slots; i++) {
        int32_t tenths_s = tenths_of_s(st.length_ms[i]);
        put_num(&p[3 + 2 * i], tenths_s > 9999 ? 9999 : tenths_s, 2, false);
    }
    civ_reply(c, f, p, 3 + 2 * (size_t)st.slots);
}

void mock_ctl_init(mock_ctl_t *m, ae_t *engine, const ae_config_t *power_up)
{
    m->engine = engine;
    m->power_up = *power_up;
    m->built_in = *power_up;
    m->kept = false;
    memset(&m->store, 0, sizeof m->store);
    memset(&m->clips, 0, sizeof m->clips);
    m->civ = NULL;
    m->rigs = NULL;
    m->n_rigs = 0;
    m->rig_changed = NULL;
    m->rig_ctx = NULL;
}

void mock_ctl_set_rigs(mock_ctl_t *m, civ_t *civ, const civ_rig_t *const *rigs, size_t n_rigs,
                       mock_ctl_rig_fn changed, void *ctx)
{
    m->civ = civ;
    m->rigs = rigs;
    m->n_rigs = n_rigs;
    m->rig_changed = changed;
    m->rig_ctx = ctx;
}

const civ_rig_t *mock_ctl_rig(const mock_ctl_t *m)
{
    return m->civ ? m->civ->rig : NULL;
}

bool mock_ctl_select_rig(mock_ctl_t *m, int hamlib_model)
{
    if (!m->civ) return false;
    for (size_t i = 0; i < m->n_rigs; i++) {
        if (m->rigs[i]->hamlib_model != hamlib_model) continue;
        if (m->civ->rig != m->rigs[i]) {
            civ_set_rig(m->civ, m->rigs[i]);
            if (m->rig_changed) m->rig_changed(m->rig_ctx, m->rigs[i]);
        }
        return true;
    }
    return false;
}

void mock_ctl_command(civ_t *c, const civ_frame_t *f, void *ctx)
{
    mock_ctl_t *m = ctx;

    const setting_t *s = find_setting(f->cmd);
    if (s) {
        cmd_setting(m, c, f, s);
        return;
    }

    switch (f->cmd) {
    case MOCK_CTL_ID:
        if (f->len == 0) {
            int model = c->rig->hamlib_model;
            uint8_t p[4] = {MOCK_CTL_ID, MOCK_CTL_VERSION};
            civ_u16_to_bcd_be((uint16_t)(model >= 0 && model <= 9999 ? model : 0), &p[2], 2);
            civ_reply(c, f, p, sizeof p);
            return;
        }
        break;
    case MOCK_CTL_RIG:
        if (f->len == 0) {
            int model = c->rig->hamlib_model;
            uint8_t p[3] = {MOCK_CTL_RIG};
            civ_u16_to_bcd_be((uint16_t)(model >= 0 && model <= 9999 ? model : 0), &p[1], 2);
            civ_reply(c, f, p, sizeof p);
            return;
        }
        {
            uint16_t model;
            /* The controls change the parser they were given, which is the
             * one this frame came through. */
            if (f->len == 2 && m->civ == c && civ_bcd_be_to_u16(f->data, 2, &model) && mock_ctl_select_rig(m, model)) {
                civ_reply_ok(c, f);
                return;
            }
        }
        break;
    case MOCK_CTL_RESET:
        if (f->len == 0) {
            mock_ctl_reset(m);
            civ_reply_ok(c, f);
            return;
        }
        break;
    case MOCK_CTL_KEEP:
        if (f->len == 0 && mock_ctl_keep_settings(m)) {
            civ_reply_ok(c, f);
            return;
        }
        break;
    case MOCK_CTL_FORGET:
        if (f->len == 0 && mock_ctl_forget_settings(m)) {
            civ_reply_ok(c, f);
            return;
        }
        break;
    case MOCK_CTL_KEPT:
        if (f->len == 0) {
            const uint8_t p[2] = {MOCK_CTL_KEPT, (uint8_t)(m->kept ? 0x01 : 0x00)};
            civ_reply(c, f, p, 2);
            return;
        }
        break;
    case MOCK_CTL_CLIP_PLAY_CMD:
    case MOCK_CTL_CLIP_SAVE_CMD:
    case MOCK_CTL_CLIP_ERASE_CMD: {
        uint16_t slot;
        mock_ctl_clip_op_t op = f->cmd == MOCK_CTL_CLIP_PLAY_CMD   ? MOCK_CTL_CLIP_PLAY
                                : f->cmd == MOCK_CTL_CLIP_SAVE_CMD ? MOCK_CTL_CLIP_SAVE
                                                                   : MOCK_CTL_CLIP_ERASE;
        if (f->len == 1 && civ_bcd_be_to_u16(f->data, 1, &slot) && mock_ctl_clip(m, op, slot) == MOCK_CTL_CLIP_OK) {
            civ_reply_ok(c, f);
            return;
        }
        break;
    }
    case MOCK_CTL_CLIP_STATUS:
        if (f->len == 0) {
            cmd_clip_status(m, c, f);
            return;
        }
        break;
    case MOCK_CTL_PTT_STATUS:
        if (f->len == 0) {
            ae_ptt_t ptt;
            ae_get_ptt(m->engine, &ptt);
            const uint8_t p[2] = {MOCK_CTL_PTT_STATUS,
                                  (uint8_t)((ptt.cat ? MOCK_CTL_PTT_CAT : 0) | (ptt.line ? MOCK_CTL_PTT_LINE_KEYED : 0) |
                                            (ptt.vox ? MOCK_CTL_PTT_VOX : 0))};
            civ_reply(c, f, p, 2);
            return;
        }
        break;
    case MOCK_CTL_PARROT_REPLAY:
        if (f->len == 0 && ae_parrot_replay(m->engine)) {
            civ_reply_ok(c, f);
            return;
        }
        break;
    case MOCK_CTL_PARROT_STATUS:
        if (f->len == 0) {
            cmd_parrot_status(m, c, f);
            return;
        }
        break;
    case MOCK_CTL_STATUS:
        if (f->len == 0) {
            cmd_status(m, c, f);
            return;
        }
        break;
    default:
        break;
    }
    civ_reply_ng(c, f);
}
