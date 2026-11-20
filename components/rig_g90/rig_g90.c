/*
 * rig_g90: Xiegu G90 personality for cat_civ.
 *
 * Built from documents, not from a radio: no G90 was at hand. Where each
 * part comes from:
 *
 *   [M] Radioddity, "Xiegu G90 CAT control and digital modes" v1.0
 *       (2021-06-23), section 6, "CI-V implementation": the command table.
 *   [H] Hamlib, rigs/icom/xiegu.c, in 4.7.1 and in master (mid-2026), whose
 *       G90 notes cite [M], the Radioddity specification sheet, and
 *       observations on stock firmware v1.81.
 *   [I] Hamlib issue 1547: a log of a G90 with firmware 20240504.8.
 *
 * Known differences from the radio, none of which Hamlib minds:
 *   - The radio echoes each command before answering it [I]. The mock does
 *     not, like an Icom with echo off.
 *   - The radio answers some commands with the command instead of FB: PTT
 *     (older firmware) and the attenuator [H]. The mock answers FB.
 *   - The attenuator command toggles, whatever its data [M], and reads back
 *     0C for on [H]. The mock stores the byte it is given.
 *   - Some replies of the radio hold bytes that are not BCD [H]. The mock's
 *     are always BCD.
 */
#include "rig_g90.h"

#define KHZ(x) ((uint64_t)(x) * 1000u)
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* 19 00. Not in [M]. Hamlib reads the byte after "19 00" and takes 70 for a
 * G90 [H]; 70 is also the radio's default address [M]. */
static const uint8_t g90_id[] = {0x70};

/* LSB USB AM CW from [M] table 2-2, FM (05, narrow) from [H], CW-R from [M].
 * The data modes U-D and L-D are USB and LSB with the data flag (1A 06). */
static const uint8_t g90_modes[] = {0x00, 0x01, 0x02, 0x03, 0x05, 0x07};

/* 0.5 to 30 MHz [H, from the specification sheet]. */
static const civ_range_t g90_rx[] = {
    {KHZ(500), KHZ(30000)},
};

/* The amateur bands from 160 to 10 m, USA limits. */
static const civ_range_t g90_tx[] = {
    {KHZ(1800), KHZ(2000)},   {KHZ(3500), KHZ(4000)},   {KHZ(5255), KHZ(5405)},   {KHZ(7000), KHZ(7300)},
    {KHZ(10100), KHZ(10150)}, {KHZ(14000), KHZ(14350)}, {KHZ(18068), KHZ(18168)}, {KHZ(21000), KHZ(21450)},
    {KHZ(24890), KHZ(24990)}, {KHZ(28000), KHZ(29700)},
};

/* 14 xx levels, 0..255 for 0..100 % [M]. Any other sub-command reads as 255
 * [M]: see g90_extra(). */
static const civ_item_t g90_levels[] = {
    {0x01, 64},  /* AF level */
    {0x02, 255}, /* RF gain: not in [M], reported working in [H] */
    {0x03, 0},   /* squelch */
    {0x09, 128}, /* CW sidetone frequency */
    {0x0A, 255}, /* transmit power: 255 = 20 W */
    {0x0C, 130}, /* CW key speed */
};

/* 15 xx [M] */
static const civ_item_t g90_meters[] = {
    {0x01, 0},  /* squelch status */
    {0x02, 12}, /* S-meter */
    {0x11, 0},  /* power meter */
    {0x12, 0},  /* SWR meter: 1.0 */
};

/* 16 xx [M] */
static const civ_item_t g90_funcs[] = {
    {0x02, 0}, /* preamplifier: 00 off, 01 or 02 on */
    {0x12, 1}, /* AGC: 00 off, 01 fast, 02 middle, 03 slow */
    {0x22, 0}, /* noise blanker */
    {0x44, 0}, /* speech compressor */
    {0x50, 0}, /* dial lock */
};

/* The commands only this radio has, or answers in its own way. */
static civ_result_t g90_extra(civ_t *c, const civ_frame_t *f)
{
    /* 02: the frequency edges, lower and higher with '-' between [M table 2-3]. */
    if (f->cmd == 0x02 && f->len == 0) {
        uint8_t p[12] = {0x02};
        civ_u64_to_bcd_le(g90_rx[0].lo_hz, &p[1], 5);
        p[6] = '-';
        civ_u64_to_bcd_le(g90_rx[0].hi_hz, &p[7], 5);
        civ_reply(c, f, p, sizeof p);
        return CIV_DONE;
    }

    /* 14 xx read, for a level the radio does not have: "always return 255" [M]. */
    if (f->cmd == 0x14 && f->len == 1) {
        for (size_t i = 0; i < ARRAY_LEN(g90_levels); i++) {
            if (g90_levels[i].sub == f->data[0]) return CIV_PASS;
        }
        const uint8_t p[4] = {0x14, f->data[0], 0x02, 0x55};
        civ_reply(c, f, p, sizeof p);
        return CIV_DONE;
    }

    /* 1A 06 read: two bytes, the data mode switch and then always 0. From a
     * note on the G90 in Xiegu's X6100 CI-V table, seen only as quoted by a
     * search result, so the weakest of the sources here. Setting it is the
     * core's. */
    if (f->cmd == 0x1A && f->len == 1 && f->data[0] == 0x06) {
        const uint8_t p[4] = {0x1A, 0x06, (uint8_t)(civ_selected_vfo(c)->data ? 0x01 : 0x00), 0x00};
        civ_reply(c, f, p, sizeof p);
        return CIV_DONE;
    }

    /* 1D 19: Xiegu's own identity command on newer firmware, 00 90 for a G90
     * [H, master]. Hamlib 4.7.1 does not send it. */
    if (f->cmd == 0x1D && f->len == 1 && f->data[0] == 0x19) {
        static const uint8_t p[4] = {0x1D, 0x19, 0x00, 0x90};
        civ_reply(c, f, p, sizeof p);
        return CIV_DONE;
    }
    return CIV_PASS;
}

const civ_rig_t rig_g90 = {
    .name = "Xiegu G90",
    .hamlib_model = 3088,
    .default_addr = 0x70, /* [M]; it also answers 88 and others, as the mock answers any */

    .id = g90_id,
    .id_len = sizeof g90_id,

    .modes = g90_modes,
    .n_modes = sizeof g90_modes,
    .max_filter = 3,

    .rx = g90_rx,
    .n_rx = ARRAY_LEN(g90_rx),
    .tx = g90_tx,
    .n_tx = ARRAY_LEN(g90_tx),

    .levels = g90_levels,
    .n_levels = ARRAY_LEN(g90_levels),
    .meters = g90_meters,
    .n_meters = ARRAY_LEN(g90_meters),
    .funcs = g90_funcs,
    .n_funcs = ARRAY_LEN(g90_funcs),
    .settings = 0,
    .n_settings = 0,

    /* 25/26 are not in [M], but firmware 20240504.8 has them, with faults
     * [I], and Hamlib tries them on a G90: if they are refused, the first
     * mode command of every session fails (seen with rigctl 4.7.1 against
     * this personality). So the mock has them, without the faults. RIT (21)
     * is not in [M] either, but [H] sets it on a G90 that was tested. No
     * 1A 03, 10 or 18, which [M] does not list and Hamlib does not send. */
    .caps = CIV_CAP_X25_X26 | CIV_CAP_DATA_MODE | CIV_CAP_TUNER | CIV_CAP_RIT | CIV_CAP_ATT,

    .init_freq = {14074000, 7074000},
    .init_mode = 0x01, /* USB */
    .init_filter = 1,
    .init_data = true, /* U-D: ready for digital modes */
    .init_filter_bw = 0,

    .extra = g90_extra,
};
