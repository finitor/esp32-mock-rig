/*
 * rig_ic705: Icom IC-705 personality for cat_civ.
 *
 * Constant tables plus the handful of commands only this radio has.
 * Sub-command numbers follow Icom's IC-705 CI-V Reference Guide.
 */
#include "rig_ic705.h"

#define KHZ(x) ((uint64_t)(x) * 1000u)
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* 19 00 answers with the radio's default CI-V address. */
static const uint8_t ic705_id[] = {0xA4};

/* LSB USB AM CW RTTY FM WFM CW-R RTTY-R DV */
static const uint8_t ic705_modes[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x17};

static const civ_range_t ic705_rx[] = {
    {KHZ(30), KHZ(199999) + 999},
    {KHZ(400000), KHZ(470000)},
};

/* Amateur bands, USA version. */
static const civ_range_t ic705_tx[] = {
    {KHZ(1800), KHZ(2000)},     {KHZ(3500), KHZ(4000)},     {KHZ(5255), KHZ(5405)},
    {KHZ(7000), KHZ(7300)},     {KHZ(10100), KHZ(10150)},   {KHZ(14000), KHZ(14350)},
    {KHZ(18068), KHZ(18168)},   {KHZ(21000), KHZ(21450)},   {KHZ(24890), KHZ(24990)},
    {KHZ(28000), KHZ(29700)},   {KHZ(50000), KHZ(54000)},   {KHZ(144000), KHZ(148000)},
    {KHZ(430000), KHZ(450000)},
};

/* 14 xx levels, 0..255 */
static const civ_item_t ic705_levels[] = {
    {0x01, 128, "AF gain", 0},
    {0x02, 255, "RF gain", 0},
    {0x03, 0, "Squelch", 0},
    {0x06, 0, "NR level", 0},
    {0x07, 128, "Twin PBT inside", 0},
    {0x08, 128, "Twin PBT outside", 0},
    {0x09, 128, "CW pitch", 0},
    {0x0A, 255, "RF power", 0}, /* 255 = 10 W */
    {0x0B, 128, "Mic gain", 0},
    {0x0C, 130, "Key speed", 0},
    {0x0D, 128, "Notch position", 0},
    {0x0E, 0, "Compressor level", 0},
    {0x0F, 128, "Break-in delay", 0},
    {0x12, 128, "NB level", 0},
    {0x15, 0, "Monitor gain", 0},
    {0x16, 128, "VOX gain", 0},
    {0x17, 128, "Anti-VOX gain", 0},
    {0x19, 255, "LCD backlight", 0},
};

/* 15 xx meters and status */
static const civ_item_t ic705_meters[] = {
    {0x01, 0, "Squelch status", 0}, /* closed */
    {0x02, 12, "S-meter", 0}, /* about S1 */
    {0x05, 0, "Tone squelch status", 0},
    {0x07, 0, "Overflow", 0},
    {0x11, 0, "Power output", 0},
    {0x12, 0, "SWR", 0}, /* 1.0 */
    {0x13, 0, "ALC", 0},
    {0x14, 0, "COMP", 0},
    {0x15, 203, "Vd", 0}, /* about 13.8 V */
    {0x16, 0, "Id", 0},
};

/* 16 xx functions. The values named for the front panel are the ones Hamlib
 * 4.7.1 was logged sending (rigctl -m 3085, L AGC, L PREAMP, U SBKIN and
 * U FBKIN): AGC 00 to 03, preamplifier 00 to 02, break-in 00 to 02. The rest
 * are switches there, whatever further values the radio has: Icom's guide
 * was not at hand to name them. */
static const civ_item_t ic705_funcs[] = {
    {0x02, 0, "Preamp", "off|1|2"},
    {0x12, 2, "AGC", "off|fast|mid|slow"}, /* MID */
    {0x22, 0, "Noise blanker", 0},
    {0x40, 0, "Noise reduction", 0},
    {0x41, 0, "Auto notch", 0},
    {0x42, 0, "Repeater tone", 0},
    {0x43, 0, "Tone squelch", 0},
    {0x44, 0, "Speech compressor", 0},
    {0x45, 0, "Monitor", 0},
    {0x46, 0, "VOX", 0},
    {0x47, 0, "Break-in", "off|semi|full"},
    {0x48, 0, "Manual notch", 0},
    {0x4B, 0, "DTCS", 0},
    {0x4F, 0, "Twin peak filter", 0},
    {0x50, 0, "Dial lock", 0},
    {0x56, 0, "DSP IF filter shape", 0},
    {0x57, 0, "Manual notch width", 0},
    {0x58, 0, "SSB TX bandwidth", 0},
    {0x5B, 0, "DSQL / CSQL", 0},
    {0x5D, 0, "Tone squelch type", 0},
    {0x65, 0, "IP plus", 0},
};

/* 1A 05 xx xx menu settings. Only the items a client has been seen to send. */
static const civ_setting_t ic705_settings[] = {
    /* DATA MOD, the modulation input in data mode: 00 MIC, 01 USB, 02 MIC,USB,
     * 03 WLAN. FT8CN and FT8AF set it at every PTT on. The mock's only audio
     * input is USB, so it starts on USB and the value changes nothing; the
     * real radio's factory default was not checked. */
    {0x0119, 0x03, 0x01},
};

const civ_rig_t rig_ic705 = {
    .name = "Icom IC-705",
    .hamlib_model = 3085,
    .default_addr = 0xA4,

    .id = ic705_id,
    .id_len = sizeof ic705_id,

    .modes = ic705_modes,
    .n_modes = sizeof ic705_modes,
    .max_filter = 3,

    .rx = ic705_rx,
    .n_rx = ARRAY_LEN(ic705_rx),
    .tx = ic705_tx,
    .n_tx = ARRAY_LEN(ic705_tx),

    .levels = ic705_levels,
    .n_levels = ARRAY_LEN(ic705_levels),
    .meters = ic705_meters,
    .n_meters = ARRAY_LEN(ic705_meters),
    .funcs = ic705_funcs,
    .n_funcs = ARRAY_LEN(ic705_funcs),
    .settings = ic705_settings,
    .n_settings = ARRAY_LEN(ic705_settings),

    .caps = CIV_CAP_X25_X26 | CIV_CAP_DATA_MODE | CIV_CAP_VFO_EQUAL | CIV_CAP_DUPLEX | CIV_CAP_POWER |
            CIV_CAP_TUNER | CIV_CAP_RIT | CIV_CAP_FILTER_BW | CIV_CAP_ATT | CIV_CAP_TS,
    .att_on = 0x20, /* 20 dB: what Hamlib 4.7.1 sends for L ATT 20 */

    .init_freq = {14074000, 7074000},
    .init_mode = 0x01, /* USB */
    .init_filter = 1,
    .init_data = true, /* USB-D: ready for digital modes */
    .init_filter_bw = 0x34, /* index 34 = 3.0 kHz in SSB */

    .extra = 0,
};
