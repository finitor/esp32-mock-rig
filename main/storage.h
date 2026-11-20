/*
 * storage: what the firmware keeps in flash.
 *
 * Small things go in NVS, by key: the radio imitated and the kept power-up
 * settings. Clips, which are large, go in a partition of their own through
 * the portable clip_store, and are written by a task of their own, because
 * erasing and writing megabytes of flash takes seconds.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_engine.h"
#include "mock_ctl.h"

/* Open NVS. False if flash storage is not usable: nothing is kept then, and
 * the other functions fail harmlessly. */
bool storage_start(void);

bool storage_get_u16(const char *key, uint16_t *value);
bool storage_set_u16(const char *key, uint16_t value);

/* Returns the blob's length, or 0 if there is none or it does not fit. */
size_t storage_get_blob(const char *key, uint8_t *buf, size_t cap);
bool storage_set_blob(const char *key, const uint8_t *buf, size_t len);
bool storage_erase(const char *key); /* true also if there was nothing */

/* Find the clips partition and start the task that works on it. engine is
 * the one whose parrot the clips are saved from and played into. Returns the
 * number of clips there is room for: 0 if the flash has no such partition. */
unsigned storage_clips_start(ae_t *engine);

/* For mock_ctl_set_clips(): begin an operation, and say how things stand. */
bool storage_clip_begin(void *ctx, mock_ctl_clip_op_t op, unsigned slot);
void storage_clip_status(void *ctx, mock_ctl_clips_status_t *st);

#endif /* STORAGE_H */
