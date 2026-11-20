/*
 * clip_store: recordings kept in flash.
 *
 * Portable C with no ESP-IDF dependencies, so it is tested on the host. A
 * clip is a recording of the parrot's, 12 kHz 16-bit mono, kept so that it
 * survives a power cycle and can be played again as receive audio: the last
 * transmission worth keeping, or a signal to test a decoder with.
 *
 * Storage is a plain run of flash (a partition on the board, an array in the
 * tests), cut into equal slots, one clip each:
 *
 *   one 4096-byte sector   header: "MRCL", version, sample rate, number of
 *                          samples, CRC-32 of the samples, CRC-32 of the header
 *   the samples            little-endian, up to CLIP_MAX_SAMPLES (120 s)
 *
 * Saving erases what the clip needs, writes the samples and writes the header
 * last, so a save cut short by a power failure leaves an empty slot, not a
 * wrong clip. Loading checks both CRCs. There is no file system and no wear
 * levelling: a slot is rewritten only when someone saves to it.
 *
 * clip_run() does one whole operation between the store and the audio
 * engine, borrowing the parrot's buffer for as long as it takes. On the
 * board that is seconds for a long clip, and flash writes hold up everything
 * else for milliseconds at a time, so it belongs in a task of its own and
 * outside a session.
 */
#ifndef CLIP_STORE_H
#define CLIP_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLIP_SECTOR 4096u
#define CLIP_FS_HZ AE_PARROT_FS_HZ
#define CLIP_MAX_SAMPLES (120u * CLIP_FS_HZ) /* 120 s, the parrot's default buffer */
#define CLIP_DATA_BYTES (((CLIP_MAX_SAMPLES * 2u) + CLIP_SECTOR - 1u) / CLIP_SECTOR * CLIP_SECTOR)
#define CLIP_SLOT_BYTES (CLIP_SECTOR + CLIP_DATA_BYTES)
#define CLIP_SLOTS_MAX 8u

/* The flash underneath. Offsets and lengths are in bytes; erase is always
 * asked for whole sectors, and write only for erased bytes. Each returns
 * false on failure. */
typedef struct {
    bool (*read)(void *ctx, uint32_t offset, void *buf, size_t len);
    bool (*write)(void *ctx, uint32_t offset, const void *buf, size_t len);
    bool (*erase)(void *ctx, uint32_t offset, uint32_t len);
    void *ctx;
    uint32_t size; /* bytes */
} clip_dev_t;

typedef struct {
    clip_dev_t dev;
    unsigned slots;
} clip_store_t;

/* The number of slots is what fits: size / CLIP_SLOT_BYTES, at most
 * CLIP_SLOTS_MAX. dev NULL, or too small for one, gives a store with none. */
void clip_store_init(clip_store_t *s, const clip_dev_t *dev);
unsigned clip_store_slots(const clip_store_t *s);

/* Slots are numbered from 1. */

/* Samples in the slot's clip; 0 if it is empty, damaged or not a slot. Reads
 * the header only. */
uint32_t clip_store_length(clip_store_t *s, unsigned slot);

/* Store n samples, of which no more than CLIP_MAX_SAMPLES are kept. n == 0
 * is refused. */
bool clip_store_save(clip_store_t *s, unsigned slot, const int16_t *samples, uint32_t n);

/* Read the clip into out, which holds cap samples. Returns the number read,
 * which is less than the clip's length if cap is; 0 if there is no good
 * clip, in which case out may have been written. */
uint32_t clip_store_load(clip_store_t *s, unsigned slot, int16_t *out, uint32_t cap);

/* Empty the slot. True also if it was empty already. */
bool clip_store_erase(clip_store_t *s, unsigned slot);

/* ---- whole operations ------------------------------------------------------ */

typedef enum {
    CLIP_SAVE = 0, /* the parrot's recording into the slot */
    CLIP_PLAY,     /* the slot's clip into the parrot, replayed at once */
    CLIP_ERASE,
} clip_op_t;

/* Do one operation. For save and play the parrot's buffer is borrowed from
 * the engine: wait is called, with ctx, between looks at whether the audio
 * task has let go, and should take about 10 ms (a task delay on the board).
 * Returns false if the operation failed or had nothing to work on: no such
 * slot, no recording, no clip, no parrot buffer. A clip longer than the
 * parrot's buffer is played as far as it fits. */
bool clip_run(clip_store_t *s, ae_t *e, clip_op_t op, unsigned slot, void (*wait)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CLIP_STORE_H */
