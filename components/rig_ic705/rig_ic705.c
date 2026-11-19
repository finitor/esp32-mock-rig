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
    {0x01, 128}, /* AF gain */
    {0x02, 255}, /* RF gain */
    {0x03, 0},   /* squelch */
    {0x06, 0},   /* NR level */
    {0x07, 128}, /* twin PBT inside */
    {0x08, 128}, /* twin PBT outside */
    {0x09, 128}, /* CW pitch */
    {0x0A, 255}, /* RF power: 255 = 10 W */
    {0x0B, 128}, /* mic gain */
    {0x0C, 130}, /* key speed */
    {0x0D, 128}, /* notch position */
    {0x0E, 0},   /* compressor level */
    {0x0F, 128}, /* break-in delay */
    {0x12, 128}, /* NB level */
    {0x15, 0},   /* monitor gain */
    {0x16, 128}, /* VOX gain */
    {0x17, 128}, /* anti-VOX gain */
    {0x19, 255}, /* LCD backlight */
};

/* 15 xx meters and status */
static const civ_item_t ic705_meters[] = {
    {0x01, 0},   /* squelch status: closed */
    {0x02, 12},  /* S-meter: about S1 */
    {0x05, 0},   /* tone squelch status */
    {0x07, 0},   /* overflow */
    {0x11, 0},   /* power output */
    {0x12, 0},   /* SWR: 1.0 */
    {0x13, 0},   /* ALC */
    {0x14, 0},   /* COMP */
    {0x15, 203}, /* Vd: about 13.8 V */
    {0x16, 0},   /* Id */
};

/* 16 xx functions */
static const civ_item_t ic705_funcs[] = {
    {0x02, 0}, /* preamp */
    {0x12, 2}, /* AGC: MID */
    {0x22, 0}, /* noise blanker */
    {0x40, 0}, /* noise reduction */
    {0x41, 0}, /* auto notch */
    {0x42, 0}, /* repeater tone */
    {0x43, 0}, /* tone squelch */
    {0x44, 0}, /* speech compressor */
    {0x45, 0}, /* monitor */
    {0x46, 0}, /* VOX */
    {0x47, 0}, /* break-in */
    {0x48, 0}, /* manual notch */
    {0x4B, 0}, /* DTCS */
    {0x4F, 0}, /* twin peak filter */
    {0x50, 0}, /* dial lock */
    {0x56, 0}, /* DSP IF filter shape */
    {0x57, 0}, /* manual notch width */
    {0x58, 0}, /* SSB TX bandwidth */
    {0x5B, 0}, /* DSQL / CSQL */
    {0x5D, 0}, /* tone squelch type */
    {0x65, 0}, /* IP plus */
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

    .init_freq = {14074000, 7074000},
    .init_mode = 0x01, /* USB */
    .init_filter = 1,
    .init_data = true, /* USB-D: ready for digital modes */
    .init_filter_bw = 0x34, /* index 34 = 3.0 kHz in SSB */

    .extra = 0,
};
