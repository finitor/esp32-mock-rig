/* mock_ctl: see mock_ctl.h for the command table. */
#include "mock_ctl.h"

#include <math.h>

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
    {MOCK_CTL_PARROT_DELAY, 2, false, 0, (int32_t)(AE_PARROT_DELAY_MAX_MS / 100)},
    {MOCK_CTL_PARROT_PERIOD, 2, false, 0, (int32_t)(AE_PARROT_PERIOD_MAX_MS / 100)},
    {MOCK_CTL_LINK_BUFFER, 2, false, (int32_t)AE_LINK_BUFFER_MIN_MS, (int32_t)AE_LINK_BUFFER_MAX_MS},
    {MOCK_CTL_LINK_MATCH, 0, false, 0, 1},
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

void mock_ctl_init(mock_ctl_t *m, ae_t *engine, const ae_config_t *power_up)
{
    m->engine = engine;
    m->power_up = *power_up;
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
    case MOCK_CTL_RESET:
        if (f->len == 0) {
            mock_ctl_reset(m);
            civ_reply_ok(c, f);
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
