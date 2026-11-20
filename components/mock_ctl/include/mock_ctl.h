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
 *   02            keep the settings as they are now as the power-up values,
 *                 in flash; refused if the mock has nowhere to keep them
 *   03            forget the kept settings: the power-up values are the
 *                 build's again. The settings in use do not change
 *   04            read only: 00 the power-up values are the build's, 01 kept
 *                 ones
 *   10 <00..03>   receive route: 00 monitor, 01 radio, 02 parrot (refused
 *                 if the parrot has no buffer), 03 link (refused if the
 *                 mock was built without one)
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
 *   30            play the parrot's recording again now; refused unless the
 *                 route is parrot, there is a recording and PTT is off
 *   31 <n>        play clip n: a recording kept in flash replaces the
 *                 parrot's and is replayed. Refused unless the route is
 *                 parrot, PTT is off, clip n exists and no clip operation is
 *                 under way. n is 01 to the number of clips, as are 35, 36
 *   32 <2>        parrot delay from unkeying to the replay, tenths of a
 *                 second, 0 .. 60.0
 *   33 <2>        parrot period, tenths of a second, 0 .. 300.0: the replay
 *                 starts a whole number of periods after key-on; 0 = off
 *   34            parrot status, read only: <state> <flags> <recorded, 2>
 *                 <capacity, 2>
 *                   state: 00 idle, 01 recording, 02 waiting, 03 playing
 *                   flags: 01 the buffer filled, the recording is cut short
 *                   recorded, capacity: tenths of a second
 *   35 <n>        keep the parrot's recording as clip n, replacing what was
 *                 there. Refused if there is no recording, or one is being
 *                 made. Answered when it has begun: it takes seconds
 *   36 <n>        erase clip n
 *   37            clip status, read only: <flags> <clips> then for each clip
 *                 its length, 2 bytes, tenths of a second, 0 for none
 *                   flags: 01 an operation is under way, 02 the last failed
 *   40 <2>        link: audio held before it is played, ms, 10 .. 200
 *   41 <00|01>    link: frequency match off / on. On, the other board is
 *                 heard only when it transmits on this radio's receive
 *                 frequency
 *   50 <00..02>   control-line PTT: 00 off, 01 RTS keys, 02 DTR keys
 *   51 <00|01>    VOX off / on
 *   52 <2>        VOX threshold, tenths of a dB below full scale, 6.0 .. 80.0
 *   53 <2>        VOX hang time, ms, 0 .. 5000
 *   54            PTT status, read only: <sources keying the radio now>
 *                   01 CAT, 02 control line, 04 VOX; 00 = receiving
 *
 *   60 <2>        the radio imitated, as its Hamlib model number in BCD:
 *                 30 85 the IC-705, 30 88 the G90. Setting it is switching
 *                 one radio off and the other on: the new one starts from
 *                 its power-up state. Refused for a model the mock does not
 *                 have. It is not one of the settings 01 puts back.
 *
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
    MOCK_CTL_KEEP = 0x02,
    MOCK_CTL_FORGET = 0x03,
    MOCK_CTL_KEPT = 0x04,
    MOCK_CTL_ROUTE = 0x10,
    MOCK_CTL_NOISE = 0x11,
    MOCK_CTL_SNR = 0x12,
    MOCK_CTL_REF_BW = 0x13,
    MOCK_CTL_FLOOR = 0x14,
    MOCK_CTL_ATTEN = 0x15,
    MOCK_CTL_TURNAROUND = 0x16,
    MOCK_CTL_STATUS = 0x20,
    MOCK_CTL_PARROT_REPLAY = 0x30,
    MOCK_CTL_CLIP_PLAY_CMD = 0x31,
    MOCK_CTL_PARROT_DELAY = 0x32,
    MOCK_CTL_PARROT_PERIOD = 0x33,
    MOCK_CTL_PARROT_STATUS = 0x34,
    MOCK_CTL_CLIP_SAVE_CMD = 0x35,
    MOCK_CTL_CLIP_ERASE_CMD = 0x36,
    MOCK_CTL_CLIP_STATUS = 0x37,
    MOCK_CTL_LINK_BUFFER = 0x40,
    MOCK_CTL_LINK_MATCH = 0x41,
    MOCK_CTL_PTT_LINE = 0x50,
    MOCK_CTL_VOX = 0x51,
    MOCK_CTL_VOX_THRESHOLD = 0x52,
    MOCK_CTL_VOX_HANG = 0x53,
    MOCK_CTL_PTT_STATUS = 0x54,
    MOCK_CTL_RIG = 0x60,
};

enum {
    MOCK_CTL_FLAG_RX_MUTED = 0x01,
    MOCK_CTL_FLAG_GAIN_LIMITED = 0x02,
};

enum {
    MOCK_CTL_PARROT_FLAG_TRUNCATED = 0x01,
};

enum {
    MOCK_CTL_PTT_CAT = 0x01,
    MOCK_CTL_PTT_LINE_KEYED = 0x02,
    MOCK_CTL_PTT_VOX = 0x04,
};

enum {
    MOCK_CTL_CLIP_FLAG_BUSY = 0x01,
    MOCK_CTL_CLIP_FLAG_FAILED = 0x02,
};

/* Somewhere to keep the power-up settings: flash on the board. Each returns
 * false on failure. */
typedef struct {
    bool (*save)(void *ctx, const uint8_t *blob, size_t len);
    bool (*forget)(void *ctx);
    void *ctx;
} mock_ctl_store_t;

#define MOCK_CTL_BLOB_MAX 160 /* a buffer this size holds the kept settings */

/* Recordings kept in flash. The operations take seconds on the board and
 * run in a task of the firmware's: start begins one and returns at once,
 * false if it could not be begun. */
#define MOCK_CTL_CLIPS_MAX 8

typedef enum {
    MOCK_CTL_CLIP_SAVE = 0, /* the parrot's recording into the clip */
    MOCK_CTL_CLIP_PLAY,     /* the clip into the parrot, and replayed */
    MOCK_CTL_CLIP_ERASE,
} mock_ctl_clip_op_t;

typedef struct {
    unsigned slots; /* clips are numbered 1..slots */
    bool busy;      /* an operation is under way */
    bool failed;    /* the last one did not succeed */
    uint32_t length_ms[MOCK_CTL_CLIPS_MAX]; /* 0: no clip */
} mock_ctl_clips_status_t;

typedef struct {
    bool (*start)(void *ctx, mock_ctl_clip_op_t op, unsigned slot);
    void (*status)(void *ctx, mock_ctl_clips_status_t *st);
    void *ctx;
} mock_ctl_clips_t;

/* Why a clip operation was not begun. */
typedef enum {
    MOCK_CTL_CLIP_OK = 0,
    MOCK_CTL_CLIP_NONE,    /* no clips on this mock, or no clip of that number */
    MOCK_CTL_CLIP_BUSY,    /* another operation is under way */
    MOCK_CTL_CLIP_NOTHING, /* nothing to save, or the clip is empty */
    MOCK_CTL_CLIP_STATE,   /* not now: recording, transmitting, or not in the parrot route */
} mock_ctl_clip_result_t;

/* Called after the radio imitated has changed, on whichever task changed it. */
typedef void (*mock_ctl_rig_fn)(void *ctx, const civ_rig_t *rig);

typedef struct {
    ae_t *engine;
    ae_config_t power_up; /* what MOCK_CTL_RESET restores */
    ae_config_t built_in; /* the power-up settings when none are kept */
    bool kept;            /* power_up came from flash */
    mock_ctl_store_t store;
    mock_ctl_clips_t clips;

    /* the personalities to choose from: mock_ctl_set_rigs() */
    civ_t *civ;
    const civ_rig_t *const *rigs;
    size_t n_rigs;
    mock_ctl_rig_fn rig_changed;
    void *rig_ctx;
} mock_ctl_t;

/* power_up is copied. It is not applied here: the caller has already
 * configured the engine with it. */
void mock_ctl_init(mock_ctl_t *m, ae_t *engine, const ae_config_t *power_up);

/* The civ_private_fn: civ_set_private(civ, MOCK_CTL_ADDR, mock_ctl_command, m).
 * Runs on the task that feeds the CI-V parser. */
void mock_ctl_command(civ_t *c, const civ_frame_t *f, void *ctx);

/* ---- the settings by number, for other control paths -----------------------
 *
 * The web console changes the same settings through these, so it cannot
 * disagree with the CI-V commands about a range or a power-up value. A
 * setting is named by its command number. Its value is the number the
 * command carries: 0, 1, 2 for a choice, and wire units for the rest (tenths
 * of a dB, Hz, ms or tenths of a second; the noise floor and the VOX
 * threshold as tenths of a dB below full scale; the SNR may be negative).
 */
typedef enum {
    MOCK_CTL_SET_OK = 0,
    MOCK_CTL_SET_UNKNOWN,     /* no setting has this number */
    MOCK_CTL_SET_RANGE,       /* outside the setting's range */
    MOCK_CTL_SET_UNAVAILABLE, /* in range but not possible: the parrot route without a buffer */
} mock_ctl_set_t;

/* The range of a setting. False if cmd is not a setting. */
bool mock_ctl_setting_range(uint8_t cmd, int32_t *lo, int32_t *hi);

/* A setting's value in cfg. 0 if cmd is not a setting. */
int32_t mock_ctl_setting_get(const ae_config_t *cfg, uint8_t cmd);

/* Put a value into cfg, a copy of the engine's settings; nothing changes
 * until the caller hands cfg to ae_configure(). cfg is untouched unless the
 * result is MOCK_CTL_SET_OK. */
mock_ctl_set_t mock_ctl_setting_put(mock_ctl_t *m, ae_config_t *cfg, uint8_t cmd, int32_t value);

/* Put every setting back to its power-up value: what command 01 does. */
void mock_ctl_reset(mock_ctl_t *m);

/* ---- power-up settings kept in flash ------------------------------------------
 *
 * The settings are packed as a list of command numbers and values with a
 * check, so that firmware with more settings, or fewer, still reads what an
 * older one kept: what it does not know or cannot use it leaves at the
 * build's value.
 */

/* Give the mock somewhere to keep them. store may be NULL to take it away. */
void mock_ctl_set_store(mock_ctl_t *m, const mock_ctl_store_t *store);

/* At start-up: blob is what the store holds. If it is good, the settings in
 * it become the power-up settings and are put into use, over the ones given
 * to mock_ctl_init(). False, and nothing changes, if it is not. */
bool mock_ctl_restore(mock_ctl_t *m, const uint8_t *blob, size_t len);

/* What commands 02, 03 and 04 do. */
bool mock_ctl_keep_settings(mock_ctl_t *m);
bool mock_ctl_forget_settings(mock_ctl_t *m);
bool mock_ctl_settings_kept(const mock_ctl_t *m);

/* The packing itself. Returns the length, or 0 if cap is too small. */
size_t mock_ctl_settings_pack(const ae_config_t *cfg, uint8_t *out, size_t cap);

/* ---- clips ----------------------------------------------------------------------- */

void mock_ctl_set_clips(mock_ctl_t *m, const mock_ctl_clips_t *clips);

/* False, and st all zero, if the mock has no clips. */
bool mock_ctl_clips_status(mock_ctl_t *m, mock_ctl_clips_status_t *st);

/* Begin a clip operation, after checking that it makes sense now: what
 * commands 31, 35 and 36 do. */
mock_ctl_clip_result_t mock_ctl_clip(mock_ctl_t *m, mock_ctl_clip_op_t op, unsigned slot);

/* ---- the radio imitated ------------------------------------------------------
 *
 * civ is the parser the controls are attached to, and rigs the personalities
 * it may become; both must outlive m. changed may be NULL. Without this call
 * command 60 can be read but not set.
 */
void mock_ctl_set_rigs(mock_ctl_t *m, civ_t *civ, const civ_rig_t *const *rigs, size_t n_rigs,
                       mock_ctl_rig_fn changed, void *ctx);

/* The personality in use, or NULL before mock_ctl_set_rigs(). */
const civ_rig_t *mock_ctl_rig(const mock_ctl_t *m);

/* Become the personality with this Hamlib model number. False if there is
 * none; true, and nothing happens, if it is the one in use. Like the
 * settings functions, the caller keeps this from running at the same time as
 * the CI-V parser. */
bool mock_ctl_select_rig(mock_ctl_t *m, int hamlib_model);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_CTL_H */
