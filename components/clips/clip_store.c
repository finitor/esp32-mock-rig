/* clip_store: see clip_store.h. */
#include "clip_store.h"

#include <string.h>

#define MAGIC "MRCL"
#define VERSION 1u
#define HEADER_LEN 24u
#define CHUNK 1024u /* samples moved at a time: 2 KB */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* CRC-32 (IEEE 802.3), a bit at a time: speed does not matter beside flash.
 * "123456789" gives CBF43926. */
static uint32_t crc32_add(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Samples as they are stored: little-endian, whatever this machine is. */
static void to_le(const int16_t *in, uint8_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        out[2 * i] = (uint8_t)(uint16_t)in[i];
        out[2 * i + 1] = (uint8_t)((uint16_t)in[i] >> 8);
    }
}

static void from_le(const uint8_t *in, int16_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) out[i] = (int16_t)(uint16_t)(in[2 * i] | (in[2 * i + 1] << 8));
}

void clip_store_init(clip_store_t *s, const clip_dev_t *dev)
{
    memset(s, 0, sizeof *s);
    if (!dev || !dev->read || !dev->write || !dev->erase) return;
    s->dev = *dev;
    s->slots = dev->size / CLIP_SLOT_BYTES;
    if (s->slots > CLIP_SLOTS_MAX) s->slots = CLIP_SLOTS_MAX;
}

unsigned clip_store_slots(const clip_store_t *s)
{
    return s->slots;
}

/* Where the slot starts, or false if there is no such slot. */
static bool slot_base(const clip_store_t *s, unsigned slot, uint32_t *base)
{
    if (slot < 1 || slot > s->slots) return false;
    *base = (uint32_t)(slot - 1) * CLIP_SLOT_BYTES;
    return true;
}

/* The slot's header, if it is a good one. */
static bool read_header(clip_store_t *s, uint32_t base, uint32_t *samples, uint32_t *data_crc)
{
    uint8_t h[HEADER_LEN];
    if (!s->dev.read(s->dev.ctx, base, h, sizeof h)) return false;
    if (memcmp(h, MAGIC, 4) != 0 || get_u32(&h[4]) != VERSION || get_u32(&h[8]) != CLIP_FS_HZ) return false;
    if (get_u32(&h[20]) != crc32_add(0, h, 20)) return false;
    *samples = get_u32(&h[12]);
    *data_crc = get_u32(&h[16]);
    return *samples >= 1 && *samples <= CLIP_MAX_SAMPLES;
}

uint32_t clip_store_length(clip_store_t *s, unsigned slot)
{
    uint32_t base, samples, crc;
    if (!slot_base(s, slot, &base) || !read_header(s, base, &samples, &crc)) return 0;
    return samples;
}

bool clip_store_erase(clip_store_t *s, unsigned slot)
{
    uint32_t base;
    if (!slot_base(s, slot, &base)) return false;
    return s->dev.erase(s->dev.ctx, base, CLIP_SECTOR); /* without its header a clip is not there */
}

bool clip_store_save(clip_store_t *s, unsigned slot, const int16_t *samples, uint32_t n)
{
    uint8_t buf[CHUNK * 2];
    uint8_t h[HEADER_LEN];
    uint32_t base, crc = 0;

    if (!slot_base(s, slot, &base) || !samples || n == 0) return false;
    if (n > CLIP_MAX_SAMPLES) n = CLIP_MAX_SAMPLES;

    /* The header first, so the old clip is gone whatever happens next; then
     * as much of the data area as the new one needs. */
    uint32_t data_bytes = (n * 2u + CLIP_SECTOR - 1u) / CLIP_SECTOR * CLIP_SECTOR;
    if (!s->dev.erase(s->dev.ctx, base, CLIP_SECTOR + data_bytes)) return false;

    for (uint32_t done = 0; done < n;) {
        uint32_t k = n - done < CHUNK ? n - done : CHUNK;
        to_le(samples + done, buf, k);
        crc = crc32_add(crc, buf, k * 2u);
        if (!s->dev.write(s->dev.ctx, base + CLIP_SECTOR + done * 2u, buf, k * 2u)) return false;
        done += k;
    }

    memcpy(h, MAGIC, 4);
    put_u32(&h[4], VERSION);
    put_u32(&h[8], CLIP_FS_HZ);
    put_u32(&h[12], n);
    put_u32(&h[16], crc);
    put_u32(&h[20], crc32_add(0, h, 20));
    return s->dev.write(s->dev.ctx, base, h, sizeof h);
}

uint32_t clip_store_load(clip_store_t *s, unsigned slot, int16_t *out, uint32_t cap)
{
    uint8_t buf[CHUNK * 2];
    uint32_t base, samples, want_crc, crc = 0;

    if (!slot_base(s, slot, &base) || !out || !read_header(s, base, &samples, &want_crc)) return 0;

    /* All of it is read, to check it, even if only part of it fits. */
    for (uint32_t done = 0; done < samples;) {
        uint32_t k = samples - done < CHUNK ? samples - done : CHUNK;
        if (!s->dev.read(s->dev.ctx, base + CLIP_SECTOR + done * 2u, buf, k * 2u)) return 0;
        crc = crc32_add(crc, buf, k * 2u);
        if (done < cap) from_le(buf, out + done, cap - done < k ? cap - done : k);
        done += k;
    }
    if (crc != want_crc) return 0;
    return samples < cap ? samples : cap;
}

/* ---- whole operations ------------------------------------------------------ */

/* Borrow the parrot's buffer. An audio task that is running lets go at its
 * next block; one that is not running has nothing to let go of, which shows
 * as its block count standing still. */
static void borrow(ae_t *e, void (*wait)(void *ctx), void *ctx)
{
    ae_parrot_hold(e);
    uint32_t blocks = ae_blocks(e);
    int still = 0;
    for (int i = 0; i < 200 && !ae_parrot_held(e) && still < 5; i++) {
        if (wait) wait(ctx);
        uint32_t now = ae_blocks(e);
        still = now == blocks ? still + 1 : 0;
        blocks = now;
    }
}

bool clip_run(clip_store_t *s, ae_t *e, clip_op_t op, unsigned slot, void (*wait)(void *ctx), void *ctx)
{
    uint32_t len, cap;
    bool ok = false;

    if (slot < 1 || slot > s->slots) return false;
    if (op == CLIP_ERASE) return clip_store_erase(s, slot);
    if (op != CLIP_SAVE && op != CLIP_PLAY) return false;
    if (!ae_parrot_buffer(e, &len, &cap)) return false;
    if (op == CLIP_PLAY && clip_store_length(s, slot) == 0) return false; /* nothing there: leave the parrot alone */

    borrow(e, wait, ctx);
    int16_t *buf = ae_parrot_buffer(e, &len, &cap);
    if (op == CLIP_SAVE) {
        ok = len > 0 && clip_store_save(s, slot, buf, len);
        ae_parrot_release(e, false, 0, false);
    } else {
        uint32_t got = clip_store_load(s, slot, buf, cap);
        ok = got > 0;
        /* A clip that failed its check may have overwritten the recording
         * that was there: what is left is not a recording. */
        ae_parrot_release(e, true, got, ok);
    }
    return ok;
}
