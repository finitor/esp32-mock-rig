/*
 * Unit tests for clip_store: recordings kept in flash, on a pretend flash
 * chip that behaves like the real kind (erase to all ones in whole sectors,
 * write only clears bits) and can fail part-way, as at a power cut.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine.h"
#include "clip_store.h"

static int g_failed;
static int g_checks;

static void fail(const char *file, int line, const char *what)
{
    g_failed++;
    printf("FAIL %s:%d %s\n", file, line, what);
}

#define CHECK(cond)                                    \
    do {                                               \
        g_checks++;                                    \
        if (!(cond)) fail(__FILE__, __LINE__, #cond);  \
    } while (0)

/* ---- the pretend flash ---------------------------------------------------------- */

#define FLASH_SLOTS 3
#define FLASH_BYTES (FLASH_SLOTS * CLIP_SLOT_BYTES + 5000u) /* three slots and a remainder */

static uint8_t g_flash[FLASH_BYTES];
static int g_writes_left = -1; /* fail when this reaches 0; -1 never */
static int g_misuse;           /* writes over unerased bytes, erases off a sector, anything out of range */
static uint32_t g_erased_bytes;

static bool flash_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    (void)ctx;
    if (off > FLASH_BYTES || len > FLASH_BYTES - off) {
        g_misuse++;
        return false;
    }
    memcpy(buf, &g_flash[off], len);
    return true;
}

static bool flash_write(void *ctx, uint32_t off, const void *buf, size_t len)
{
    (void)ctx;
    if (off > FLASH_BYTES || len > FLASH_BYTES - off) {
        g_misuse++;
        return false;
    }
    if (g_writes_left == 0) return false;
    if (g_writes_left > 0) g_writes_left--;
    for (size_t i = 0; i < len; i++) {
        if (g_flash[off + i] != 0xFF) g_misuse++;
        g_flash[off + i] &= ((const uint8_t *)buf)[i];
    }
    return true;
}

static bool flash_erase(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    if (off % CLIP_SECTOR || len % CLIP_SECTOR || len == 0 || off > FLASH_BYTES || len > FLASH_BYTES - off) {
        g_misuse++;
        return false;
    }
    memset(&g_flash[off], 0xFF, len);
    g_erased_bytes += len;
    return true;
}

static const clip_dev_t g_dev = {.read = flash_read, .write = flash_write, .erase = flash_erase, .size = FLASH_BYTES};

static void flash_fresh(clip_store_t *s)
{
    /* A chip never used holds whatever it holds: not all ones here. */
    for (size_t i = 0; i < FLASH_BYTES; i++) g_flash[i] = (uint8_t)(i * 31u + 7u);
    g_writes_left = -1;
    g_misuse = 0;
    g_erased_bytes = 0;
    clip_store_init(s, &g_dev);
}

static int16_t sample(uint32_t i, int seed)
{
    return (int16_t)((i * 37u + (uint32_t)seed * 1009u) % 60001u) - 30000;
}

static void fill(int16_t *buf, uint32_t n, int seed)
{
    for (uint32_t i = 0; i < n; i++) buf[i] = sample(i, seed);
}

static bool same(const int16_t *buf, uint32_t n, int seed)
{
    for (uint32_t i = 0; i < n; i++) {
        if (buf[i] != sample(i, seed)) return false;
    }
    return true;
}

static int16_t g_a[CLIP_MAX_SAMPLES + 100], g_b[CLIP_MAX_SAMPLES + 100];

/* ---- tests -------------------------------------------------------------------- */

static void test_store(void)
{
    clip_store_t s;
    flash_fresh(&s);
    CHECK(clip_store_slots(&s) == FLASH_SLOTS);
    CHECK(CLIP_SLOT_BYTES == 4096u + 2883584u); /* a header sector and 120 s, rounded up to sectors */

    /* Nothing is found in flash that was never written. */
    for (unsigned slot = 0; slot <= FLASH_SLOTS + 1; slot++) CHECK(clip_store_length(&s, slot) == 0);
    CHECK(clip_store_load(&s, 1, g_b, CLIP_MAX_SAMPLES) == 0);

    /* A 15 s clip, as an FT8 transmission is. Only what it needs is erased. */
    fill(g_a, 180000, 1);
    CHECK(clip_store_save(&s, 2, g_a, 180000));
    CHECK(g_erased_bytes == CLIP_SECTOR + 360448u);
    CHECK(clip_store_length(&s, 2) == 180000);
    CHECK(clip_store_length(&s, 1) == 0 && clip_store_length(&s, 3) == 0);
    memset(g_b, 0, sizeof g_b);
    CHECK(clip_store_load(&s, 2, g_b, CLIP_MAX_SAMPLES) == 180000 && same(g_b, 180000, 1));
    CHECK(g_b[180000] == 0); /* and not a sample more */

    /* Stored little-endian, whatever the machine. */
    uint32_t data = CLIP_SLOT_BYTES + CLIP_SECTOR;
    CHECK(g_flash[data] == (uint8_t)(uint16_t)g_a[0] && g_flash[data + 1] == (uint8_t)((uint16_t)g_a[0] >> 8));
    CHECK(memcmp(&g_flash[CLIP_SLOT_BYTES], "MRCL", 4) == 0);

    /* One sample; an odd length; the longest; and longer than that, cut. */
    fill(g_a, CLIP_MAX_SAMPLES + 100, 2);
    CHECK(clip_store_save(&s, 1, g_a, 1));
    CHECK(clip_store_length(&s, 1) == 1 && clip_store_load(&s, 1, g_b, 10) == 1 && g_b[0] == g_a[0]);
    CHECK(clip_store_save(&s, 1, g_a, 12345));
    CHECK(clip_store_load(&s, 1, g_b, CLIP_MAX_SAMPLES) == 12345 && same(g_b, 12345, 2));
    CHECK(clip_store_save(&s, 3, g_a, CLIP_MAX_SAMPLES));
    CHECK(clip_store_load(&s, 3, g_b, CLIP_MAX_SAMPLES) == CLIP_MAX_SAMPLES && same(g_b, CLIP_MAX_SAMPLES, 2));
    CHECK(clip_store_save(&s, 3, g_a, CLIP_MAX_SAMPLES + 100));
    CHECK(clip_store_length(&s, 3) == CLIP_MAX_SAMPLES);
    /* The last slot's end is inside the flash, and its neighbours are whole. */
    CHECK(clip_store_load(&s, 2, g_b, CLIP_MAX_SAMPLES) == 180000 && same(g_b, 180000, 1));
    CHECK(clip_store_load(&s, 1, g_b, CLIP_MAX_SAMPLES) == 12345 && same(g_b, 12345, 2));

    /* A shorter clip over a longer one; a buffer too small takes the start. */
    fill(g_a, 500, 3);
    CHECK(clip_store_save(&s, 3, g_a, 500));
    CHECK(clip_store_load(&s, 3, g_b, CLIP_MAX_SAMPLES) == 500 && same(g_b, 500, 3));
    memset(g_b, 0, sizeof g_b);
    CHECK(clip_store_load(&s, 2, g_b, 1000) == 1000 && same(g_b, 1000, 1) && g_b[1000] == 0);
    CHECK(clip_store_load(&s, 2, g_b, 0) == 0);

    /* Erase, and what is refused. */
    CHECK(clip_store_erase(&s, 2) && clip_store_length(&s, 2) == 0);
    CHECK(clip_store_load(&s, 2, g_b, CLIP_MAX_SAMPLES) == 0);
    CHECK(clip_store_erase(&s, 2)); /* again: still empty, no harm */
    CHECK(clip_store_length(&s, 1) == 12345 && clip_store_length(&s, 3) == 500);
    CHECK(!clip_store_save(&s, 0, g_a, 10) && !clip_store_save(&s, FLASH_SLOTS + 1, g_a, 10));
    CHECK(!clip_store_save(&s, 1, g_a, 0) && !clip_store_save(&s, 1, NULL, 10));
    CHECK(clip_store_length(&s, 1) == 12345); /* a refused save changed nothing */
    CHECK(!clip_store_erase(&s, 0) && !clip_store_erase(&s, FLASH_SLOTS + 1));
    CHECK(clip_store_load(&s, 1, NULL, 10) == 0);
    CHECK(g_misuse == 0);

    /* A store survives a restart: it is all in the flash. */
    clip_store_t again;
    clip_store_init(&again, &g_dev);
    CHECK(clip_store_length(&again, 1) == 12345 && clip_store_length(&again, 3) == 500);
}

static void test_damage(void)
{
    clip_store_t s;
    flash_fresh(&s);
    fill(g_a, 50000, 4);
    CHECK(clip_store_save(&s, 1, g_a, 50000));

    /* One bit of the audio gone: the clip is not played. */
    g_flash[CLIP_SECTOR + 77777] ^= 0x10;
    CHECK(clip_store_length(&s, 1) == 50000); /* the header alone cannot tell */
    CHECK(clip_store_load(&s, 1, g_b, CLIP_MAX_SAMPLES) == 0);
    g_flash[CLIP_SECTOR + 77777] ^= 0x10;
    CHECK(clip_store_load(&s, 1, g_b, CLIP_MAX_SAMPLES) == 50000);

    /* Any byte of the header gone: there is no clip. */
    for (uint32_t i = 0; i < 24; i++) {
        g_flash[i] ^= 0x01;
        g_checks++;
        if (clip_store_length(&s, 1) != 0 || clip_store_load(&s, 1, g_b, CLIP_MAX_SAMPLES) != 0) {
            fail(__FILE__, __LINE__, "a damaged header was accepted");
        }
        g_flash[i] ^= 0x01;
    }
    CHECK(clip_store_length(&s, 1) == 50000);

    /* Power lost during a save, at every stage of it: an empty slot, never
     * a wrong clip, and the next save works. */
    fill(g_a, 5000, 5);
    for (int writes = 0; writes <= 5; writes++) { /* 5000 samples are 5 writes, and the header a 6th */
        g_writes_left = writes;
        g_checks++;
        if (clip_store_save(&s, 1, g_a, 5000) || clip_store_length(&s, 1) != 0) {
            fail(__FILE__, __LINE__, "a save cut short left a clip");
        }
    }
    g_writes_left = -1;
    CHECK(clip_store_save(&s, 1, g_a, 5000));
    CHECK(clip_store_load(&s, 1, g_b, CLIP_MAX_SAMPLES) == 5000 && same(g_b, 5000, 5));
    CHECK(g_misuse == 0);

    /* No flash, flash too small, half a device: a store with no slots, and
     * every operation refused. */
    clip_store_t none;
    clip_dev_t small = g_dev;
    small.size = CLIP_SLOT_BYTES - 1;
    clip_store_init(&none, &small);
    CHECK(clip_store_slots(&none) == 0 && !clip_store_save(&none, 1, g_a, 10) && clip_store_length(&none, 1) == 0);
    clip_store_init(&none, NULL);
    CHECK(clip_store_slots(&none) == 0 && !clip_store_erase(&none, 1));
    clip_dev_t broken = g_dev;
    broken.erase = NULL;
    clip_store_init(&none, &broken);
    CHECK(clip_store_slots(&none) == 0);
    /* More flash than eight slots is eight slots. */
    clip_dev_t big = g_dev;
    big.size = 20u * CLIP_SLOT_BYTES;
    clip_store_init(&none, &big);
    CHECK(clip_store_slots(&none) == CLIP_SLOTS_MAX);
}

/* ---- with the engine: save the parrot's recording, play a clip back ----------- */

enum { BLK = 480, PAR_CAP = 3 * AE_PARROT_FS_HZ };

static ae_t g_engine;
static int16_t g_parrot[PAR_CAP];
static int g_waits;
static bool g_audio_running;

static void run_audio(int blocks, bool tone)
{
    int16_t tx[BLK], out[BLK];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < BLK; i++) tx[i] = (int16_t)((i / 24) % 2 ? 5000 : -5000);
        ae_process(&g_engine, tone ? tx : NULL, out, BLK);
    }
}

/* What a task delay is on the board: time for the audio task to run. */
static void wait_10ms(void *ctx)
{
    (void)ctx;
    g_waits++;
    if (g_audio_running) run_audio(1, false);
}

static ae_parrot_status_t parrot(void)
{
    ae_parrot_status_t st;
    ae_get_parrot_status(&g_engine, &st);
    return st;
}

static void test_with_engine(void)
{
    clip_store_t s;
    ae_config_t cfg;
    flash_fresh(&s);

    ae_init(&g_engine, 7u);
    ae_set_parrot_buffer(&g_engine, g_parrot, PAR_CAP);
    ae_config_defaults(&cfg);
    cfg.route = AE_ROUTE_PARROT;
    cfg.parrot_delay_ms = 60000;
    ae_configure(&g_engine, &cfg);

    /* Nothing recorded, nothing stored: nothing to do. */
    g_audio_running = true;
    CHECK(!clip_run(&s, &g_engine, CLIP_SAVE, 1, wait_10ms, NULL));
    CHECK(!clip_run(&s, &g_engine, CLIP_PLAY, 1, wait_10ms, NULL));
    CHECK(clip_store_length(&s, 1) == 0);

    /* A second of transmission, saved while audio runs: the buffer is lent
     * within a block or two and handed back. */
    ae_set_ptt(&g_engine, true);
    run_audio(100, true);
    ae_set_ptt(&g_engine, false);
    run_audio(10, false);
    CHECK(parrot().recorded_ms == 1050);
    g_waits = 0;
    CHECK(clip_run(&s, &g_engine, CLIP_SAVE, 1, wait_10ms, NULL));
    CHECK(g_waits >= 1 && g_waits <= 2);
    CHECK(clip_store_length(&s, 1) == 12600);
    run_audio(2, false);
    CHECK(!ae_parrot_held(&g_engine) && parrot().recorded_ms == 1050);
    memcpy(g_a, g_parrot, 12600 * sizeof(int16_t));

    /* Another transmission replaces the recording; the clip brings the first
     * one back and plays it. */
    ae_set_ptt(&g_engine, true);
    run_audio(30, false);
    ae_set_ptt(&g_engine, false);
    run_audio(10, false);
    CHECK(parrot().recorded_ms == 350);
    CHECK(clip_run(&s, &g_engine, CLIP_PLAY, 1, wait_10ms, NULL));
    run_audio(2, false);
    CHECK(parrot().state == AE_PARROT_PLAYING && parrot().recorded_ms == 1050);
    CHECK(memcmp(g_a, g_parrot, 12600 * sizeof(int16_t)) == 0);
    run_audio(150, false);
    CHECK(parrot().state == AE_PARROT_IDLE);

    /* With no audio running there is nobody to wait for: a few looks, and on. */
    g_audio_running = false;
    g_waits = 0;
    CHECK(clip_run(&s, &g_engine, CLIP_SAVE, 2, wait_10ms, NULL));
    CHECK(g_waits == 5 && clip_store_length(&s, 2) == 12600);
    CHECK(clip_run(&s, &g_engine, CLIP_PLAY, 2, NULL, NULL)); /* and with no way to wait at all */
    CHECK(parrot().recorded_ms == 1050);
    g_audio_running = true;
    run_audio(3, false);
    CHECK(parrot().state == AE_PARROT_PLAYING);
    run_audio(150, false);

    /* A clip longer than the parrot's buffer is played as far as it fits. */
    fill(g_a, 5 * AE_PARROT_FS_HZ, 6);
    CHECK(clip_store_save(&s, 3, g_a, 5 * AE_PARROT_FS_HZ));
    CHECK(clip_run(&s, &g_engine, CLIP_PLAY, 3, wait_10ms, NULL));
    run_audio(2, false);
    CHECK(parrot().recorded_ms == 3000 && same(g_parrot, PAR_CAP, 6));
    run_audio(400, false);

    /* A damaged clip is not played, and the recording it overwrote is gone,
     * not left half replaced. */
    g_flash[2 * CLIP_SLOT_BYTES + CLIP_SECTOR + 100] ^= 0x40;
    CHECK(!clip_run(&s, &g_engine, CLIP_PLAY, 3, wait_10ms, NULL));
    run_audio(2, false);
    CHECK(parrot().state == AE_PARROT_IDLE && parrot().recorded_ms == 0);

    /* Erase needs no buffer; a slot that is not there is refused. */
    CHECK(clip_run(&s, &g_engine, CLIP_ERASE, 1, wait_10ms, NULL) && clip_store_length(&s, 1) == 0);
    CHECK(!clip_run(&s, &g_engine, CLIP_ERASE, 9, wait_10ms, NULL));
    CHECK(!clip_run(&s, &g_engine, CLIP_SAVE, 0, wait_10ms, NULL));
    CHECK(!clip_run(&s, &g_engine, (clip_op_t)7, 1, wait_10ms, NULL));

    /* A failed save hands the buffer back all the same. */
    CHECK(clip_run(&s, &g_engine, CLIP_PLAY, 2, wait_10ms, NULL));
    run_audio(200, false);
    g_writes_left = 2;
    CHECK(!clip_run(&s, &g_engine, CLIP_SAVE, 1, wait_10ms, NULL));
    g_writes_left = -1;
    run_audio(2, false);
    CHECK(!ae_parrot_held(&g_engine) && parrot().recorded_ms == 1050 && clip_store_length(&s, 1) == 0);

    /* No parrot buffer: neither save nor play. */
    ae_t bare;
    ae_init(&bare, 1u);
    CHECK(!clip_run(&s, &bare, CLIP_SAVE, 1, NULL, NULL) && !clip_run(&s, &bare, CLIP_PLAY, 2, NULL, NULL));
    CHECK(g_misuse == 0);
}

int main(void)
{
    test_store();
    test_damage();
    test_with_engine();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
