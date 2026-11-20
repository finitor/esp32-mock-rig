/*
 * cat_civ: Icom CI-V protocol core. See cat_civ.h.
 *
 * Nothing in this file is specific to one radio. Radio differences come in
 * through civ_rig_t: tables, capability flags and the optional extra() hook.
 */
#include "cat_civ.h"

#include <string.h>

/* ---- BCD helpers ------------------------------------------------------ */

static bool bcd_byte(uint8_t b, unsigned *out)
{
    unsigned hi = b >> 4, lo = b & 0x0F;
    if (hi > 9 || lo > 9) return false;
    *out = hi * 10 + lo;
    return true;
}

bool civ_bcd_le_to_u64(const uint8_t *bcd, size_t nbytes, uint64_t *out)
{
    uint64_t v = 0, mul = 1;
    for (size_t i = 0; i < nbytes; i++) {
        unsigned d;
        if (!bcd_byte(bcd[i], &d)) return false;
        v += d * mul;
        mul *= 100;
    }
    *out = v;
    return true;
}

void civ_u64_to_bcd_le(uint64_t v, uint8_t *bcd, size_t nbytes)
{
    for (size_t i = 0; i < nbytes; i++) {
        unsigned d = (unsigned)(v % 100);
        bcd[i] = (uint8_t)(((d / 10) << 4) | (d % 10));
        v /= 100;
    }
}

bool civ_bcd_be_to_u16(const uint8_t *bcd, size_t nbytes, uint16_t *out)
{
    unsigned v = 0;
    for (size_t i = 0; i < nbytes; i++) {
        unsigned d;
        if (!bcd_byte(bcd[i], &d)) return false;
        v = v * 100 + d;
        if (v > 0xFFFF) return false;
    }
    *out = (uint16_t)v;
    return true;
}

void civ_u16_to_bcd_be(uint16_t v, uint8_t *bcd, size_t nbytes)
{
    for (size_t i = nbytes; i-- > 0;) {
        unsigned d = v % 100;
        bcd[i] = (uint8_t)(((d / 10) << 4) | (d % 10));
        v /= 100;
    }
}

/* ---- replies ---------------------------------------------------------- */

static void send_frame(civ_t *c, const civ_frame_t *f, const uint8_t *payload, size_t len)
{
    uint8_t out[CIV_MAX_FRAME];
    if (len + 5 > sizeof out) return;
    out[0] = CIV_PREAMBLE;
    out[1] = CIV_PREAMBLE;
    out[2] = f->from; /* back to the controller */
    out[3] = f->to;   /* from whatever address it called us by */
    memcpy(&out[4], payload, len);
    out[4 + len] = CIV_END;
    c->tx(c->ctx, out, len + 5);
}

void civ_reply_ok(civ_t *c, const civ_frame_t *f)
{
    const uint8_t ok = CIV_OK;
    c->frames_ok++;
    send_frame(c, f, &ok, 1);
}

void civ_reply_ng(civ_t *c, const civ_frame_t *f)
{
    const uint8_t ng = CIV_NG;
    c->frames_ng++;
    send_frame(c, f, &ng, 1);
}

/* A data reply: counts as a success. */
void civ_reply(civ_t *c, const civ_frame_t *f, const uint8_t *payload, size_t len)
{
    c->frames_ok++;
    send_frame(c, f, payload, len);
}

/* ---- state helpers ---------------------------------------------------- */

static bool in_ranges(const civ_range_t *r, size_t n, uint64_t hz)
{
    for (size_t i = 0; i < n; i++)
        if (hz >= r[i].lo_hz && hz <= r[i].hi_hz) return true;
    return false;
}

static bool mode_ok(const civ_rig_t *rig, uint8_t mode)
{
    for (size_t i = 0; i < rig->n_modes; i++)
        if (rig->modes[i] == mode) return true;
    return false;
}

static bool filter_ok(const civ_rig_t *rig, uint8_t filter)
{
    return filter >= 1 && filter <= rig->max_filter;
}

static int find_item(const civ_item_t *items, size_t n, uint8_t sub)
{
    for (size_t i = 0; i < n; i++)
        if (items[i].sub == sub) return (int)i;
    return -1;
}

static int find_setting(const civ_rig_t *rig, uint16_t item)
{
    for (size_t i = 0; i < rig->n_settings && i < CIV_MAX_SETTINGS; i++)
        if (rig->settings[i].item == item) return (int)i;
    return -1;
}

static bool has(const civ_t *c, uint32_t cap)
{
    return (c->rig->caps & cap) != 0;
}

/* PTT as commanded over CAT. */
static void set_ptt(civ_t *c, bool on)
{
    if (c->ptt == on) return;
    c->ptt = on;
    if (c->on_ptt) c->on_ptt(c->ctx, on);
}

bool civ_ptt(const civ_t *c) { return c->ptt || c->ptt_other; }

void civ_set_ptt(civ_t *c, bool on) { c->ptt_other = on; }

const civ_vfo_t *civ_selected_vfo(const civ_t *c) { return &c->vfo[c->sel]; }

uint64_t civ_rx_freq(const civ_t *c) { return c->vfo[c->sel].freq; }

uint64_t civ_tx_freq(const civ_t *c)
{
    return c->split ? c->vfo[c->sel ^ 1].freq : c->vfo[c->sel].freq;
}

bool civ_in_tx_range(const civ_t *c, uint64_t hz)
{
    return in_ranges(c->rig->tx, c->rig->n_tx, hz);
}

bool civ_set_meter(civ_t *c, uint8_t sub, uint16_t value)
{
    int i = find_item(c->rig->meters, c->rig->n_meters, sub);
    if (i < 0) return false;
    c->meter[i] = value;
    return true;
}

void civ_set_private(civ_t *c, uint8_t addr, civ_private_fn fn, void *ctx)
{
    c->private_addr = addr;
    c->private_fn = fn;
    c->private_ctx = ctx;
}

/* The radio as the personality has it at power-up. */
static void power_up(civ_t *c, const civ_rig_t *rig)
{
    c->rig = rig;
    c->sel = 0;
    c->split = false;
    c->ptt = false;
    c->tuner = 0;
    c->att = 0;
    c->ts = 0;
    c->rit_on = false;
    c->rit_hz = 0;
    memset(c->level, 0, sizeof c->level);
    memset(c->meter, 0, sizeof c->meter);
    memset(c->func, 0, sizeof c->func);
    memset(c->setting, 0, sizeof c->setting);

    for (int v = 0; v < 2; v++) {
        c->vfo[v].freq = rig->init_freq[v];
        c->vfo[v].mode = rig->init_mode;
        c->vfo[v].filter = rig->init_filter;
        c->vfo[v].data = rig->init_data;
    }
    c->duplex = 0x10;
    c->power = true;
    c->filter_bw = rig->init_filter_bw;
    for (size_t i = 0; i < rig->n_levels && i < CIV_MAX_LEVELS; i++) c->level[i] = rig->levels[i].initial;
    for (size_t i = 0; i < rig->n_meters && i < CIV_MAX_METERS; i++) c->meter[i] = rig->meters[i].initial;
    for (size_t i = 0; i < rig->n_funcs && i < CIV_MAX_FUNCS; i++) c->func[i] = rig->funcs[i].initial;
    for (size_t i = 0; i < rig->n_settings && i < CIV_MAX_SETTINGS; i++) c->setting[i] = rig->settings[i].initial;
}

void civ_init(civ_t *c, const civ_rig_t *rig, civ_tx_fn tx, civ_ptt_fn on_ptt, void *ctx)
{
    memset(c, 0, sizeof *c);
    c->tx = tx;
    c->on_ptt = on_ptt;
    c->ctx = ctx;
    power_up(c, rig);
}

void civ_set_rig(civ_t *c, const civ_rig_t *rig)
{
    bool was_keyed = c->ptt;
    power_up(c, rig);
    if (was_keyed && c->on_ptt) c->on_ptt(c->ctx, false); /* the new radio starts in receive */
}

/* ---- command handlers ------------------------------------------------- */

static bool set_freq(civ_t *c, int vfo, const uint8_t *bcd, size_t len)
{
    uint64_t hz;
    if (len != 5 || !civ_bcd_le_to_u64(bcd, 5, &hz)) return false;
    if (!in_ranges(c->rig->rx, c->rig->n_rx, hz)) return false;
    c->vfo[vfo].freq = hz;
    return true;
}

static void send_freq(civ_t *c, const civ_frame_t *f, const uint8_t *head, size_t head_len, int vfo)
{
    uint8_t p[8];
    memcpy(p, head, head_len);
    civ_u64_to_bcd_le(c->vfo[vfo].freq, &p[head_len], 5);
    civ_reply(c, f, p, head_len + 5);
}

/* 06 <mode> [<filter>] and the transceive form 01 <mode> [<filter>]. */
static bool set_mode(civ_t *c, int vfo, const uint8_t *d, size_t len)
{
    if (len < 1 || len > 2) return false;
    if (!mode_ok(c->rig, d[0])) return false;
    if (len == 2 && !filter_ok(c->rig, d[1])) return false;
    c->vfo[vfo].mode = d[0];
    if (len == 2) c->vfo[vfo].filter = d[1];
    return true;
}

static void cmd_vfo(civ_t *c, const civ_frame_t *f)
{
    if (f->len == 0) { /* "VFO mode" */
        civ_reply_ok(c, f);
        return;
    }
    if (f->len != 1) {
        civ_reply_ng(c, f);
        return;
    }
    switch (f->data[0]) {
    case 0x00:
        c->sel = 0;
        break;
    case 0x01:
        c->sel = 1;
        break;
    case 0xA0: /* selected -> unselected */
        if (!has(c, CIV_CAP_VFO_EQUAL)) {
            civ_reply_ng(c, f);
            return;
        }
        c->vfo[c->sel ^ 1] = c->vfo[c->sel];
        break;
    case 0xB0: { /* exchange */
        civ_vfo_t t = c->vfo[0];
        c->vfo[0] = c->vfo[1];
        c->vfo[1] = t;
        break;
    }
    default:
        civ_reply_ng(c, f);
        return;
    }
    civ_reply_ok(c, f);
}

static void cmd_split(civ_t *c, const civ_frame_t *f)
{
    if (f->len == 0) {
        const uint8_t p[2] = {0x0F, (uint8_t)(c->split ? 0x01 : 0x00)};
        civ_reply(c, f, p, 2);
        return;
    }
    if (f->len == 1) {
        uint8_t v = f->data[0];
        if (v <= 0x01) {
            c->split = v == 0x01;
            civ_reply_ok(c, f);
            return;
        }
        if (v >= 0x10 && v <= 0x12 && has(c, CIV_CAP_DUPLEX)) {
            c->duplex = v;
            civ_reply_ok(c, f);
            return;
        }
    }
    civ_reply_ng(c, f);
}

/* A one-byte setting: <cmd> reads, <cmd> <value> writes. */
static void cmd_byte(civ_t *c, const civ_frame_t *f, uint8_t *value)
{
    if (f->len == 0) {
        const uint8_t p[2] = {f->cmd, *value};
        civ_reply(c, f, p, 2);
    } else if (f->len == 1) {
        *value = f->data[0];
        civ_reply_ok(c, f);
    } else {
        civ_reply_ng(c, f);
    }
}

/* 14 <sub> [<hi> <lo>] : levels, 0000..0255 */
static void cmd_level(civ_t *c, const civ_frame_t *f)
{
    int i = f->len >= 1 ? find_item(c->rig->levels, c->rig->n_levels, f->data[0]) : -1;
    if (i < 0 || i >= CIV_MAX_LEVELS) {
        civ_reply_ng(c, f);
        return;
    }
    if (f->len == 1) {
        uint8_t p[4] = {0x14, f->data[0]};
        civ_u16_to_bcd_be(c->level[i], &p[2], 2);
        civ_reply(c, f, p, 4);
        return;
    }
    uint16_t v;
    if (f->len == 3 && civ_bcd_be_to_u16(&f->data[1], 2, &v) && v <= 255) {
        c->level[i] = v;
        civ_reply_ok(c, f);
        return;
    }
    civ_reply_ng(c, f);
}

/* 15 <sub> : meters and status. Status items are one byte, meters two. */
static void cmd_meter(civ_t *c, const civ_frame_t *f)
{
    int i = f->len == 1 ? find_item(c->rig->meters, c->rig->n_meters, f->data[0]) : -1;
    if (i < 0 || i >= CIV_MAX_METERS) {
        civ_reply_ng(c, f);
        return;
    }
    uint8_t sub = f->data[0];
    uint8_t p[4] = {0x15, sub};
    if (sub == 0x01 || sub == 0x05 || sub == 0x07) { /* squelch, tone squelch, overflow */
        p[2] = (uint8_t)(c->meter[i] ? 0x01 : 0x00);
        civ_reply(c, f, p, 3);
    } else {
        civ_u16_to_bcd_be(c->meter[i], &p[2], 2);
        civ_reply(c, f, p, 4);
    }
}

/* 16 <sub> [<value>] : on/off functions */
static void cmd_func(civ_t *c, const civ_frame_t *f)
{
    int i = f->len >= 1 ? find_item(c->rig->funcs, c->rig->n_funcs, f->data[0]) : -1;
    if (i < 0 || i >= CIV_MAX_FUNCS) {
        civ_reply_ng(c, f);
        return;
    }
    if (f->len == 1) {
        const uint8_t p[3] = {0x16, f->data[0], (uint8_t)c->func[i]};
        civ_reply(c, f, p, 3);
    } else if (f->len == 2) {
        c->func[i] = f->data[1];
        civ_reply_ok(c, f);
    } else {
        civ_reply_ng(c, f);
    }
}

static void cmd_1a(civ_t *c, const civ_frame_t *f)
{
    if (f->len < 1) {
        civ_reply_ng(c, f);
        return;
    }
    civ_vfo_t *v = &c->vfo[c->sel];
    switch (f->data[0]) {
    case 0x03: /* IF filter width index */
        if (!has(c, CIV_CAP_FILTER_BW)) break;
        if (f->len == 1) {
            const uint8_t p[3] = {0x1A, 0x03, c->filter_bw};
            civ_reply(c, f, p, 3);
            return;
        }
        if (f->len == 2) {
            c->filter_bw = f->data[1];
            civ_reply_ok(c, f);
            return;
        }
        break;
    case 0x05: { /* menu setting: 1A 05 <item, two bytes> [<value>] */
        if (f->len != 3 && f->len != 4) break;
        int i = find_setting(c->rig, (uint16_t)((f->data[1] << 8) | f->data[2]));
        if (i < 0) break;
        if (f->len == 3) {
            const uint8_t p[5] = {0x1A, 0x05, f->data[1], f->data[2], c->setting[i]};
            civ_reply(c, f, p, 5);
            return;
        }
        unsigned d;
        if (!bcd_byte(f->data[3], &d) || f->data[3] > c->rig->settings[i].max) break;
        c->setting[i] = f->data[3];
        civ_reply_ok(c, f);
        return;
    }
    case 0x06: /* data mode: 1A 06 <00|01> [<filter>] */
        if (!has(c, CIV_CAP_DATA_MODE)) break;
        if (f->len == 1) {
            const uint8_t p[4] = {0x1A, 0x06, (uint8_t)(v->data ? 0x01 : 0x00), (uint8_t)(v->data ? v->filter : 0x00)};
            civ_reply(c, f, p, 4);
            return;
        }
        if ((f->len == 2 || f->len == 3) && f->data[1] <= 0x01) {
            bool on = f->data[1] == 0x01;
            if (f->len == 3 && on) {
                if (!filter_ok(c->rig, f->data[2])) break;
                v->filter = f->data[2];
            }
            v->data = on;
            civ_reply_ok(c, f);
            return;
        }
        break;
    default:
        break;
    }
    civ_reply_ng(c, f);
}

static void cmd_1c(civ_t *c, const civ_frame_t *f)
{
    if (f->len < 1) {
        civ_reply_ng(c, f);
        return;
    }
    switch (f->data[0]) {
    case 0x00: /* PTT */
        if (f->len == 1) {
            const uint8_t p[3] = {0x1C, 0x00, (uint8_t)(civ_ptt(c) ? 0x01 : 0x00)};
            civ_reply(c, f, p, 3);
            return;
        }
        if (f->len == 2 && f->data[1] <= 0x01) {
            set_ptt(c, f->data[1] == 0x01);
            civ_reply_ok(c, f);
            return;
        }
        break;
    case 0x01: /* antenna tuner: 00 off, 01 on, 02 start tuning */
        if (!has(c, CIV_CAP_TUNER)) break;
        if (f->len == 1) {
            const uint8_t p[3] = {0x1C, 0x01, c->tuner};
            civ_reply(c, f, p, 3);
            return;
        }
        if (f->len == 2 && f->data[1] <= 0x02) {
            c->tuner = f->data[1] == 0x02 ? 0x01 : f->data[1]; /* tuning completes at once */
            civ_reply_ok(c, f);
            return;
        }
        break;
    default:
        break;
    }
    civ_reply_ng(c, f);
}

/* 21 00 [<lo> <hi> <sign>] RIT frequency, 21 01 [<on>] RIT, 21 02 [<on>] delta-TX */
static void cmd_rit(civ_t *c, const civ_frame_t *f)
{
    if (!has(c, CIV_CAP_RIT) || f->len < 1) {
        civ_reply_ng(c, f);
        return;
    }
    switch (f->data[0]) {
    case 0x00:
        if (f->len == 1) {
            uint8_t p[5] = {0x21, 0x00};
            int hz = c->rit_hz < 0 ? -c->rit_hz : c->rit_hz;
            civ_u64_to_bcd_le((uint64_t)hz, &p[2], 2);
            p[4] = (uint8_t)(c->rit_hz < 0 ? 0x01 : 0x00);
            civ_reply(c, f, p, 5);
            return;
        }
        if (f->len == 4 && f->data[3] <= 0x01) {
            uint64_t hz;
            if (!civ_bcd_le_to_u64(&f->data[1], 2, &hz)) break;
            c->rit_hz = (int16_t)(f->data[3] ? -(int)hz : (int)hz);
            civ_reply_ok(c, f);
            return;
        }
        break;
    case 0x01:
    case 0x02: /* delta-TX shares the RIT flag here: the mock has no RF to offset */
        if (f->len == 1) {
            const uint8_t p[3] = {0x21, f->data[0], (uint8_t)(c->rit_on && f->data[0] == 0x01 ? 0x01 : 0x00)};
            civ_reply(c, f, p, 3);
            return;
        }
        if (f->len == 2 && f->data[1] <= 0x01) {
            if (f->data[0] == 0x01) c->rit_on = f->data[1] == 0x01;
            civ_reply_ok(c, f);
            return;
        }
        break;
    default:
        break;
    }
    civ_reply_ng(c, f);
}

/* 25 <00 selected | 01 unselected> [<5 BCD>] */
static void cmd_x25(civ_t *c, const civ_frame_t *f)
{
    if (!has(c, CIV_CAP_X25_X26) || f->len < 1 || f->data[0] > 0x01) {
        civ_reply_ng(c, f);
        return;
    }
    int vfo = f->data[0] == 0x00 ? c->sel : c->sel ^ 1;
    if (f->len == 1) {
        const uint8_t head[2] = {0x25, f->data[0]};
        send_freq(c, f, head, 2, vfo);
    } else if (set_freq(c, vfo, &f->data[1], f->len - 1)) {
        civ_reply_ok(c, f);
    } else {
        civ_reply_ng(c, f);
    }
}

/* 26 <00|01> [<mode> [<data> [<filter>]]] */
static void cmd_x26(civ_t *c, const civ_frame_t *f)
{
    if (!has(c, CIV_CAP_X25_X26) || f->len < 1 || f->data[0] > 0x01) {
        civ_reply_ng(c, f);
        return;
    }
    civ_vfo_t *v = &c->vfo[f->data[0] == 0x00 ? c->sel : c->sel ^ 1];
    if (f->len == 1) {
        const uint8_t p[5] = {0x26, f->data[0], v->mode, (uint8_t)(v->data ? 0x01 : 0x00), v->filter};
        civ_reply(c, f, p, 5);
        return;
    }
    if (f->len > 4 || !mode_ok(c->rig, f->data[1])) {
        civ_reply_ng(c, f);
        return;
    }
    if (f->len >= 3 && f->data[2] > 0x01) {
        civ_reply_ng(c, f);
        return;
    }
    if (f->len == 4 && !filter_ok(c->rig, f->data[3])) {
        civ_reply_ng(c, f);
        return;
    }
    v->mode = f->data[1];
    if (f->len >= 3) v->data = f->data[2] == 0x01;
    if (f->len == 4) v->filter = f->data[3];
    civ_reply_ok(c, f);
}

static void dispatch(civ_t *c, const civ_frame_t *f)
{
    if (c->rig->extra && c->rig->extra(c, f) == CIV_DONE) return;

    switch (f->cmd) {
    case 0x00: /* transceive: frequency, no reply */
        (void)set_freq(c, c->sel, f->data, f->len);
        break;
    case 0x01: /* transceive: mode, no reply */
        (void)set_mode(c, c->sel, f->data, f->len);
        break;
    case 0x03:
        if (f->len == 0) {
            const uint8_t head = 0x03;
            send_freq(c, f, &head, 1, c->sel);
        } else {
            civ_reply_ng(c, f);
        }
        break;
    case 0x04:
        if (f->len == 0) {
            const civ_vfo_t *v = &c->vfo[c->sel];
            const uint8_t p[3] = {0x04, v->mode, v->filter};
            civ_reply(c, f, p, 3);
        } else {
            civ_reply_ng(c, f);
        }
        break;
    case 0x05:
        if (set_freq(c, c->sel, f->data, f->len)) civ_reply_ok(c, f);
        else civ_reply_ng(c, f);
        break;
    case 0x06:
        if (set_mode(c, c->sel, f->data, f->len)) civ_reply_ok(c, f);
        else civ_reply_ng(c, f);
        break;
    case 0x07:
        cmd_vfo(c, f);
        break;
    case 0x0F:
        cmd_split(c, f);
        break;
    case 0x10:
        if (has(c, CIV_CAP_TS)) cmd_byte(c, f, &c->ts);
        else civ_reply_ng(c, f);
        break;
    case 0x11:
        if (has(c, CIV_CAP_ATT)) cmd_byte(c, f, &c->att);
        else civ_reply_ng(c, f);
        break;
    case 0x14:
        cmd_level(c, f);
        break;
    case 0x15:
        cmd_meter(c, f);
        break;
    case 0x16:
        cmd_func(c, f);
        break;
    case 0x18:
        if (has(c, CIV_CAP_POWER) && f->len == 1 && f->data[0] <= 0x01) {
            c->power = f->data[0] == 0x01;
            civ_reply_ok(c, f);
        } else {
            civ_reply_ng(c, f);
        }
        break;
    case 0x19:
        if (f->len == 1 && f->data[0] == 0x00 && c->rig->id_len + 2 <= 8) {
            uint8_t p[8] = {0x19, 0x00};
            memcpy(&p[2], c->rig->id, c->rig->id_len);
            civ_reply(c, f, p, 2 + c->rig->id_len);
        } else {
            civ_reply_ng(c, f);
        }
        break;
    case 0x1A:
        cmd_1a(c, f);
        break;
    case 0x1C:
        cmd_1c(c, f);
        break;
    case 0x21:
        cmd_rit(c, f);
        break;
    case 0x25:
        cmd_x25(c, f);
        break;
    case 0x26:
        cmd_x26(c, f);
        break;
    default:
        civ_reply_ng(c, f);
        break;
    }
}

/* ---- framing ---------------------------------------------------------- */

static void end_of_frame(civ_t *c)
{
    /* rx holds: to, from, cmd, data... */
    if (c->overflow || c->rx_len < 3) {
        c->frames_dropped++;
        return;
    }
    civ_frame_t f = {
        .to = c->rx[0],
        .from = c->rx[1],
        .cmd = c->rx[2],
        .data = &c->rx[3],
        .len = c->rx_len - 3,
    };
    /* Acknowledgements are never commands; a frame to itself is noise. */
    if (f.cmd == CIV_OK || f.cmd == CIV_NG || f.to == f.from) {
        c->frames_dropped++;
        return;
    }
    if (c->private_fn && f.to == c->private_addr) {
        c->private_fn(c, &f, c->private_ctx); /* not a radio command: the personality never sees it */
        return;
    }
    dispatch(c, &f);
}

void civ_feed(civ_t *c, const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = bytes[i];

        if (b == CIV_PREAMBLE) {
            /* Two or more FE start a frame, also in the middle of a broken one. */
            if (c->preamble < 2) c->preamble++;
            if (c->preamble == 2) {
                c->in_frame = true;
                c->rx_len = 0;
                c->overflow = false;
            }
            continue;
        }
        if (c->preamble == 1) c->in_frame = false; /* a lone FE is not a preamble */
        c->preamble = 0;

        if (!c->in_frame) continue;

        if (b == CIV_END) {
            c->in_frame = false;
            end_of_frame(c);
        } else if (c->rx_len < sizeof c->rx) {
            c->rx[c->rx_len++] = b;
        } else {
            c->overflow = true;
        }
    }
}
