/*
 * cat_civ: Icom CI-V protocol core for the mock radio.
 *
 * Portable C with no ESP-IDF dependencies. Bytes in through civ_feed(),
 * reply bytes out through a callback. Everything that differs between
 * radios lives in a personality (civ_rig_t); this core never names a radio.
 */
#ifndef CAT_CIV_H
#define CAT_CIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CIV_PREAMBLE 0xFE
#define CIV_END      0xFD
#define CIV_OK       0xFB
#define CIV_NG       0xFA

#define CIV_MAX_FRAME  64 /* longest frame accepted or sent, FE FE ... FD */
#define CIV_MAX_LEVELS 32
#define CIV_MAX_FUNCS  32
#define CIV_MAX_METERS 16

typedef struct civ civ_t;

/* One received command, already stripped of preamble and terminator. */
typedef struct {
    uint8_t to;          /* radio address the controller used */
    uint8_t from;        /* controller address */
    uint8_t cmd;
    const uint8_t *data; /* bytes after cmd: sub-command first, if any */
    size_t len;
} civ_frame_t;

typedef struct {
    uint64_t lo_hz, hi_hz;
} civ_range_t;

/* A 0x14 level, 0x15 meter or 0x16 function the radio has. */
typedef struct {
    uint8_t sub;
    uint16_t initial;
} civ_item_t;

/* Optional commands. A radio without the capability answers NG. */
enum {
    CIV_CAP_X25_X26   = 1u << 0, /* 25/26: per-VFO frequency and mode */
    CIV_CAP_DATA_MODE = 1u << 1, /* 1A 06: data mode flag */
    CIV_CAP_VFO_EQUAL = 1u << 2, /* 07 A0: copy selected VFO to the other */
    CIV_CAP_DUPLEX    = 1u << 3, /* 0F 10..12: simplex / DUP- / DUP+ */
    CIV_CAP_POWER     = 1u << 4, /* 18: power off / on */
    CIV_CAP_TUNER     = 1u << 5, /* 1C 01: antenna tuner */
    CIV_CAP_RIT       = 1u << 6, /* 21: RIT / delta-TX */
    CIV_CAP_FILTER_BW = 1u << 7, /* 1A 03: IF filter width */
    CIV_CAP_ATT       = 1u << 8, /* 11: attenuator */
    CIV_CAP_TS        = 1u << 9, /* 10: tuning step */
};

/* What a personality's extra handler did with a command. */
typedef enum {
    CIV_PASS = 0, /* not mine: let the core handle it */
    CIV_DONE,     /* handled, reply already sent (or deliberately none) */
} civ_result_t;

/* A radio personality. All of it is constant data plus one optional hook. */
typedef struct civ_rig {
    const char *name;
    int hamlib_model;
    uint8_t default_addr;

    const uint8_t *id; /* data bytes of the 19 00 reply, after "19 00" */
    size_t id_len;

    const uint8_t *modes; /* mode bytes the radio accepts */
    size_t n_modes;
    uint8_t max_filter; /* filters are 1..max_filter */

    const civ_range_t *rx; /* frequencies the radio accepts */
    size_t n_rx;
    const civ_range_t *tx; /* frequencies it will transmit on */
    size_t n_tx;

    const civ_item_t *levels; /* 14 xx */
    size_t n_levels;
    const civ_item_t *meters; /* 15 xx */
    size_t n_meters;
    const civ_item_t *funcs; /* 16 xx */
    size_t n_funcs;

    uint32_t caps;

    /* Power-up state. */
    uint64_t init_freq[2];
    uint8_t init_mode;
    uint8_t init_filter;
    bool init_data;
    uint8_t init_filter_bw; /* 1A 03 width index, as the BCD byte on the wire */

    /* Radio-specific commands, tried before the core. May be NULL. */
    civ_result_t (*extra)(civ_t *c, const civ_frame_t *f);
} civ_rig_t;

typedef struct {
    uint64_t freq;
    uint8_t mode;
    uint8_t filter;
    bool data;
} civ_vfo_t;

typedef void (*civ_tx_fn)(void *ctx, const uint8_t *bytes, size_t len);
typedef void (*civ_ptt_fn)(void *ctx, bool on);

struct civ {
    const civ_rig_t *rig;
    civ_tx_fn tx;
    civ_ptt_fn on_ptt;
    void *ctx;

    /* receive framing */
    uint8_t rx[CIV_MAX_FRAME];
    size_t rx_len;
    uint8_t preamble; /* consecutive FE seen */
    bool in_frame;
    bool overflow;

    /* radio state */
    civ_vfo_t vfo[2];
    uint8_t sel; /* selected VFO, 0 = A */
    bool split;
    uint8_t duplex; /* 0x10 simplex, 0x11 DUP-, 0x12 DUP+ */
    bool ptt;
    bool power;
    uint8_t tuner;
    uint8_t att;
    uint8_t ts;
    uint8_t filter_bw;
    bool rit_on;
    int16_t rit_hz;
    uint16_t level[CIV_MAX_LEVELS];
    uint16_t meter[CIV_MAX_METERS];
    uint16_t func[CIV_MAX_FUNCS];

    /* counters, for tests and diagnostics */
    uint32_t frames_ok;
    uint32_t frames_ng;
    uint32_t frames_dropped;
};

/* Reset to the personality's power-up state. tx must not be NULL. */
void civ_init(civ_t *c, const civ_rig_t *rig, civ_tx_fn tx, civ_ptt_fn on_ptt, void *ctx);

/* Feed received serial bytes. Replies are sent from inside this call. */
void civ_feed(civ_t *c, const uint8_t *bytes, size_t len);

/* State other modules need. */
bool civ_ptt(const civ_t *c);
uint64_t civ_rx_freq(const civ_t *c); /* selected VFO */
uint64_t civ_tx_freq(const civ_t *c); /* the other VFO when split */
const civ_vfo_t *civ_selected_vfo(const civ_t *c);

/* PTT from a source other than CAT (control line, VOX), so 1C 00 reads true. */
void civ_set_ptt(civ_t *c, bool on);

/* Drive a 15 xx meter (for example the S-meter from the audio engine). */
bool civ_set_meter(civ_t *c, uint8_t sub, uint16_t value);

/* Whether the radio would transmit on this frequency. */
bool civ_in_tx_range(const civ_t *c, uint64_t hz);

/* ---- helpers for personalities --------------------------------------- */

/* Reply FB / FA to the command in f. */
void civ_reply_ok(civ_t *c, const civ_frame_t *f);
void civ_reply_ng(civ_t *c, const civ_frame_t *f);

/* Reply with a data frame: FE FE <from> <to> <payload...> FD.
 * payload starts with the command byte. */
void civ_reply(civ_t *c, const civ_frame_t *f, const uint8_t *payload, size_t len);

/* Frequency BCD, least significant byte first (CI-V wire order). */
bool civ_bcd_le_to_u64(const uint8_t *bcd, size_t nbytes, uint64_t *out);
void civ_u64_to_bcd_le(uint64_t v, uint8_t *bcd, size_t nbytes);

/* Level / meter BCD, most significant byte first: 0255 -> 02 55. */
bool civ_bcd_be_to_u16(const uint8_t *bcd, size_t nbytes, uint16_t *out);
void civ_u16_to_bcd_be(uint16_t v, uint8_t *bcd, size_t nbytes);

#ifdef __cplusplus
}
#endif

#endif /* CAT_CIV_H */
