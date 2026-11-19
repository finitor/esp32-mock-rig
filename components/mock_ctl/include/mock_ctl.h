/*
 * mock_ctl: controls that belong to the mock, not to the radio it imitates.
 *
 * Portable C with no ESP-IDF dependencies. The controls are CI-V frames sent
 * to a private address, MOCK_CTL_ADDR, on the same serial port as the radio's
 * CAT. Frames to any other address are still the radio's.
 *
 *   command:  FE FE F0 <controller> <cmd> [<data>...] FD
 *   reply:    FE FE <controller> F0 <cmd> <data>... FD     to a read
 *             FE FE <controller> F0 FB FD                  to a set that was accepted
 *             FE FE <controller> F0 FA FD                  to anything else
 *
 * A command with no data reads; with data it sets. Numbers are BCD, most
 * significant byte first, like the radio's levels (14 xx): 12.5 is "01 25"
 * in tenths. A signed number has one more byte: 00 plus, 01 minus. A value
 * outside its range is refused with FA, not clamped.
 *
 *   00            identity, read only: <protocol version> <Hamlib model, 2 bytes>
 *   01            put every setting back to its power-up value
 *   10 <00|01>    receive route: 00 monitor, 01 radio
 *   11 <00|01>    noise off / on
 *   12 <2> <sign> SNR, tenths of a dB, -60.0 .. +80.0
 *   13 <3>        bandwidth the SNR is quoted in, Hz, 100 .. 24000
 *   14 <2>        noise floor, tenths of a dB below full scale, 12.0 .. 90.0
 *   15 <2>        attenuation, tenths of a dB, 0 .. 120.0
 *   16 <2>        turnaround time, ms, 0 .. 5000
 *   20            status, read only: <flags> <level, 2> <gain, 2> <sign>
 *                   flags: 01 receive muted, 02 gain limited
 *                   level: the receive source, tenths of a dB below full
 *                          scale; 99 99 if nothing has been measured yet
 *                   gain:  what the channel applies to it, tenths of a dB
 *
 * Reserved for parts that do not exist yet, answered FA: 30 and 31 (parrot:
 * replay, play clip), 40 (link mode), 50 (PTT source), 60 (personality).
 */
#ifndef MOCK_CTL_H
#define MOCK_CTL_H

#include "audio_engine.h"
#include "cat_civ.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Nothing else uses this address: in Hamlib 4.7.1 the radios' default
 * addresses run up to B6, one receiver sits at E1 and the controller is E0.
 * FA..FF are protocol bytes. */
#define MOCK_CTL_ADDR 0xF0

#define MOCK_CTL_VERSION 0x01

enum {
    MOCK_CTL_ID = 0x00,
    MOCK_CTL_RESET = 0x01,
    MOCK_CTL_ROUTE = 0x10,
    MOCK_CTL_NOISE = 0x11,
    MOCK_CTL_SNR = 0x12,
    MOCK_CTL_REF_BW = 0x13,
    MOCK_CTL_FLOOR = 0x14,
    MOCK_CTL_ATTEN = 0x15,
    MOCK_CTL_TURNAROUND = 0x16,
    MOCK_CTL_STATUS = 0x20,
};

enum {
    MOCK_CTL_FLAG_RX_MUTED = 0x01,
    MOCK_CTL_FLAG_GAIN_LIMITED = 0x02,
};

typedef struct {
    ae_t *engine;
    ae_config_t power_up; /* what MOCK_CTL_RESET restores */
} mock_ctl_t;

/* power_up is copied. It is not applied here: the caller has already
 * configured the engine with it. */
void mock_ctl_init(mock_ctl_t *m, ae_t *engine, const ae_config_t *power_up);

/* The civ_private_fn: civ_set_private(civ, MOCK_CTL_ADDR, mock_ctl_command, m).
 * Runs on the task that feeds the CI-V parser. */
void mock_ctl_command(civ_t *c, const civ_frame_t *f, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_CTL_H */
