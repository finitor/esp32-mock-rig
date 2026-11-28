/*
 * Unit tests for coord: two (or three) boards with clocks of their own,
 * joined by the link, agreeing on one channel.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine.h"
#include "coord.h"
#include "link.h"
#include "mock_ctl.h"

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

/* ---- the boards ----------------------------------------------------------------- */

#define BOARDS 3
#define TICK_MS 20
#define FS 48000
#define BLOCK 240 /* 5 ms */

typedef struct {
    int index;
    uint8_t addr[LINK_ADDR_LEN];
    ae_t engine;
    mock_ctl_t ctl;
    link_t link;
    coord_t coord;
    double clock_base; /* this board's clock when the test's is 0, ms */
    double clock_ppm;  /* and how much faster than true time it runs */
    bool audio;        /* run its receive audio */
    int16_t *out;
    size_t out_n;
} board_t;

static board_t g_b[BOARDS];
static double g_ms;   /* true time */
static bool g_cut;    /* nothing gets through between the boards */
static int g_states;  /* state packets sent, by anyone */
static uint64_t g_sample;

static uint32_t board_clock(void *ctx)
{
    const board_t *b = ctx;
    return (uint32_t)(int64_t)(b->clock_base + g_ms * (1.0 + b->clock_ppm * 1e-6));
}

static bool board_send(void *ctx, const uint8_t *dst, const uint8_t *data, size_t len)
{
    board_t *from = ctx;
    if (len >= 2 && (data[1] & 0x0F) == 3) g_states++;
    if (g_cut) return true;
    for (int i = 0; i < BOARDS; i++) {
        if (&g_b[i] == from || (dst && memcmp(dst, g_b[i].addr, LINK_ADDR_LEN) != 0)) continue;
        link_receive(&g_b[i].link, from->addr, data, len, -50);
    }
    return true;
}

/* A stand-in for the link's audio: every board hears the same steady tone. */
static void no_tx(void *ctx, const int16_t *tx, size_t n, bool keyed)
{
    (void)ctx;
    (void)tx;
    (void)n;
    (void)keyed;
}

static bool tone_rx(void *ctx, int16_t *out, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++) out[i] = (int16_t)lrint(6000.0 * sin(2 * 3.14159265358979 * 1500.0 * (double)(g_sample + i) / FS));
    return true;
}

static void board_start(int i, uint32_t random)
{
    static const ae_link_t ops = {.tx = no_tx, .rx = tone_rx};
    board_t *b = &g_b[i];
    ae_config_t cfg;
    char name[20];

    b->index = i;
    memset(b->addr, 0x10 * (i + 1), LINK_ADDR_LEN);
    ae_init(&b->engine, 100u + (uint32_t)i);
    ae_set_link(&b->engine, &ops);
    ae_config_defaults(&cfg);
    cfg.turnaround_ms = 0;
    ae_configure(&b->engine, &cfg);
    mock_ctl_init(&b->ctl, &b->engine, &cfg);
    snprintf(name, sizeof name, "mockradio-000%d", i + 1);
    link_init(&b->link, name, board_send, b);
    link_set_clock(&b->link, board_clock, b);
    coord_init(&b->coord, &b->ctl, &b->link, random);
}

static void sim_reset(void)
{
    static int16_t out_store[BOARDS][FS * 70];
    g_ms = 0;
    g_cut = false;
    g_states = 0;
    g_sample = 0;
    for (int i = 0; i < BOARDS; i++) {
        memset(&g_b[i], 0, sizeof g_b[i]);
        g_b[i].out = out_store[i];
        g_b[i].clock_base = 1000000.0 * (i + 1) + 333.0 * i; /* started at different times */
        board_start(i, 1000u + 7u * (uint32_t)i);
    }
}

static ae_config_t config(int i)
{
    ae_config_t cfg;
    ae_get_config(&g_b[i].engine, &cfg);
    return cfg;
}

static void set(int i, uint8_t cmd, int32_t value)
{
    ae_config_t cfg = config(i);
    g_checks++;
    if (mock_ctl_setting_put(&g_b[i].ctl, &cfg, cmd, value) != MOCK_CTL_SET_OK) fail(__FILE__, __LINE__, "setting refused");
    ae_configure(&g_b[i].engine, &cfg);
}

static coord_status_t status(int i)
{
    coord_status_t st;
    coord_get_status(&g_b[i].coord, &st);
    return st;
}

/* The shared clock as board i has it now. */
static double shared_clock(int i)
{
    uint32_t seed, clock;
    coord_fade_clock(&g_b[i].coord, board_clock(&g_b[i]), &seed, &clock);
    return (double)clock;
}

static uint32_t fade_seed(int i)
{
    uint32_t seed, clock;
    coord_fade_clock(&g_b[i].coord, 0, &seed, &clock);
    return seed;
}

static void run(int ms)
{
    for (double end = g_ms + ms; g_ms < end; g_ms += 5) {
        for (int i = 0; i < BOARDS; i++) {
            board_t *b = &g_b[i];
            if ((long)g_ms % TICK_MS == 0) {
                /* as the firmware's status task: the link follows the engine's settings */
                ae_config_t cfg = config(i);
                link_configure(&b->link, cfg.route == AE_ROUTE_LINK, cfg.link_buffer_ms, cfg.link_match);
                coord_tick(&b->coord, board_clock(b));
                link_tick(&b->link, board_clock(b));
            }
            if (b->audio && b->out_n + BLOCK <= (size_t)FS * 70) {
                uint32_t seed, clock;
                coord_fade_clock(&b->coord, board_clock(b), &seed, &clock);
                ae_fade_clock(&b->engine, seed, clock);
                ae_process(&b->engine, NULL, b->out + b->out_n, BLOCK);
                b->out_n += BLOCK;
            }
        }
        g_sample += BLOCK;
    }
}

static void link_up(int i)
{
    set(i, MOCK_CTL_ROUTE, AE_ROUTE_LINK);
}

/* ---- tests -------------------------------------------------------------------- */

static void test_settings(void)
{
    sim_reset();
    /* Not in the link route: nothing is sent and nothing is shared. A
     * change is counted all the same: board 0 is at version 1. */
    run(200);
    set(0, MOCK_CTL_SNR, 50);
    run(2000);
    CHECK(g_states == 0 && !status(0).sharing && !status(1).sharing);
    CHECK(mock_ctl_setting_get(&(ae_config_t){0}, MOCK_CTL_SNR) == 0); /* (the helper reads what it is given) */

    /* Both in the link route. Board 0's SNR was changed once, board 1's
     * never: board 0's settings become the pair's. */
    link_up(0);
    link_up(1);
    run(1500);
    CHECK(status(0).sharing && status(1).sharing);
    CHECK(fabsf(config(1).snr_db - 5.0f) < 0.001f && fabsf(config(0).snr_db - 5.0f) < 0.001f);
    CHECK(status(0).version == 1 && status(1).version == 1);

    /* A change on either reaches the other within half a second. */
    set(1, MOCK_CTL_NOISE, 1);
    set(1, MOCK_CTL_FADE, AE_FADE_MPP);
    run(500);
    CHECK(config(0).noise_on && config(0).fade == AE_FADE_MPP);
    CHECK(status(0).version == status(1).version && status(0).version >= 2);
    set(0, MOCK_CTL_ATTEN, 60);
    set(0, MOCK_CTL_REF_BW, 3000);
    set(0, MOCK_CTL_FLOOR, 500);
    set(0, MOCK_CTL_DOPPLER, 15);
    set(0, MOCK_CTL_FADE_DELAY, 75);
    run(500);
    ae_config_t c1 = config(1);
    CHECK(fabsf(c1.atten_db - 6.0f) < 0.001f && fabsf(c1.ref_bw_hz - 3000.0f) < 0.5f);
    CHECK(fabsf(c1.noise_floor_dbfs - -50.0f) < 0.001f);
    CHECK(fabsf(c1.fade_doppler_hz - 0.15f) < 0.0001f && fabsf(c1.fade_delay_ms - 7.5f) < 0.001f);
    CHECK(c1.noise_on && c1.fade == AE_FADE_MPP); /* what was not changed stays */

    /* Settled, nothing more changes hands: the versions stand still. */
    uint32_t version = status(0).version;
    uint32_t gen0 = ae_config_gen(&g_b[0].engine), gen1 = ae_config_gen(&g_b[1].engine);
    run(3000);
    CHECK(status(0).version == version && status(1).version == version);
    CHECK(ae_config_gen(&g_b[0].engine) == gen0 && ae_config_gen(&g_b[1].engine) == gen1);

    /* What is each board's own stays its own. */
    set(0, MOCK_CTL_TURNAROUND, 250);
    set(0, MOCK_CTL_FREQ_OFFSET, -125);
    set(0, MOCK_CTL_VOX, 1);
    set(0, MOCK_CTL_LINK_BUFFER, 120);
    set(0, MOCK_CTL_PARROT_DELAY, 25);
    run(1000);
    c1 = config(1);
    CHECK(c1.turnaround_ms == 0 && c1.freq_offset_hz == 0.0f && !c1.vox_on && c1.link_buffer_ms == 80);
    CHECK(c1.parrot_delay_ms == 1000 && status(1).version == version);

    /* Both changed between two ticks: one of the two wins, and both agree. */
    set(0, MOCK_CTL_SNR, 100);
    set(1, MOCK_CTL_SNR, -100);
    run(1000);
    CHECK(config(0).snr_db == config(1).snr_db);
    CHECK(fabsf(config(0).snr_db) == 10.0f);

    /* Sharing switched off on one board: it neither tells nor is told, and
     * the other is on its own too. Switched on again, the pair is one. */
    set(1, MOCK_CTL_LINK_COORD, 0);
    run(2500);
    CHECK(!status(1).sharing && !status(0).sharing);
    set(0, MOCK_CTL_ATTEN, 200);
    set(1, MOCK_CTL_ATTEN, 30);
    run(100); /* changes are counted a tick at a time */
    set(1, MOCK_CTL_ATTEN, 40);
    run(100);
    set(1, MOCK_CTL_ATTEN, 50);
    run(1000);
    CHECK(fabsf(config(0).atten_db - 20.0f) < 0.001f && fabsf(config(1).atten_db - 5.0f) < 0.001f);
    set(1, MOCK_CTL_LINK_COORD, 1);
    run(1000);
    CHECK(status(0).sharing && status(1).sharing);
    /* Board 1 made more changes meanwhile: its version is the higher. */
    CHECK(fabsf(config(0).atten_db - 5.0f) < 0.001f && fabsf(config(1).atten_db - 5.0f) < 0.001f);

    /* Leaving the link route ends it as well. */
    set(0, MOCK_CTL_ROUTE, AE_ROUTE_RADIO);
    run(2500);
    CHECK(!status(0).sharing && !status(1).sharing);
    set(1, MOCK_CTL_SNR, 33);
    run(1000);
    CHECK(fabsf(config(0).snr_db - 3.3f) > 0.01f);
}

static void test_first_meeting(void)
{
    /* Two boards that have never been changed but power up with different
     * settings (kept ones, say): one set is picked, the same way each time,
     * and both end up with it. */
    for (int round = 0; round < 2; round++) {
        sim_reset();
        ae_config_t cfg = config(0);
        cfg.route = AE_ROUTE_LINK;
        cfg.snr_db = 3.0f;
        ae_configure(&g_b[0].engine, &cfg);
        cfg.snr_db = 17.0f;
        ae_configure(&g_b[1].engine, &cfg);
        if (round) { /* the same two boards, started with each other's numbers */
            coord_init(&g_b[0].coord, &g_b[0].ctl, &g_b[0].link, 1007u);
            coord_init(&g_b[1].coord, &g_b[1].ctl, &g_b[1].link, 1000u);
        }
        run(1500);
        CHECK(status(0).sharing && status(1).sharing && status(0).version == 0 && status(1).version == 0);
        CHECK(config(0).snr_db == config(1).snr_db);
        /* The board with the higher start-up number is the one followed. */
        int leader = g_b[0].coord.boot_id > g_b[1].coord.boot_id ? 0 : 1;
        CHECK(fabsf(config(0).snr_db - (leader == 0 ? 3.0f : 17.0f)) < 0.001f);
        CHECK(status(leader).leads && !status(1 - leader).leads);
    }

    /* A board that restarts takes up the pair's settings again. */
    sim_reset();
    link_up(0);
    link_up(1);
    set(0, MOCK_CTL_SNR, -55);
    set(0, MOCK_CTL_FADE, AE_FADE_HILAT_D);
    run(1000);
    CHECK(config(1).fade == AE_FADE_HILAT_D);
    board_start(1, 424242u); /* power cycle: defaults, version 0 */
    CHECK(config(1).fade == AE_FADE_OFF);
    link_up(1);
    run(4500); /* board 0 drops the old peer after 3 s and takes the new one */
    CHECK(status(0).sharing && status(1).sharing);
    CHECK(config(1).fade == AE_FADE_HILAT_D && fabsf(config(1).snr_db - -5.5f) < 0.001f);

    /* A third board in the link route is no part of the pair. */
    link_up(2);
    set(2, MOCK_CTL_SNR, 77);
    set(2, MOCK_CTL_SNR, 78);
    set(2, MOCK_CTL_SNR, 79);
    set(2, MOCK_CTL_SNR, 80);
    run(2000);
    CHECK(fabsf(config(0).snr_db - -5.5f) < 0.001f && fabsf(config(1).snr_db - -5.5f) < 0.001f);
    CHECK(fabsf(config(2).snr_db - 8.0f) < 0.001f); /* it hears the pair's announcements, not their state */
}

static void test_untrusted(void)
{
    /* What a peer sends is checked like anything else: values this board
     * cannot use and settings that are not the path's are passed over. */
    sim_reset();
    link_up(0);
    link_up(1);
    run(1000);
    uint8_t pkt[23 + 5 * 6] = {0x4D, 0x13};
    uint32_t words[5] = {0xFFFFFFF0u /* boot id */, 900 /* version */, 5 /* nonce */, 0 /* clock */, 42 /* seed */};
    for (int w = 0; w < 5; w++) {
        for (int k = 0; k < 4; k++) pkt[2 + 4 * w + k] = (uint8_t)(words[w] >> (8 * k));
    }
    const struct {
        uint8_t id;
        int32_t value;
    } items[6] = {
        {MOCK_CTL_SNR, 99999},        /* out of range */
        {MOCK_CTL_ATTEN, 120},        /* fine: 12.0 dB */
        {MOCK_CTL_ROUTE, 0},          /* not the path's */
        {MOCK_CTL_VOX, 1},            /* not the path's */
        {0x7E, 1},                    /* no such setting */
        {MOCK_CTL_FADE, AE_FADE_MPD}, /* fine */
    };
    pkt[22] = 6;
    for (int i = 0; i < 6; i++) {
        pkt[23 + 5 * i] = items[i].id;
        for (int k = 0; k < 4; k++) pkt[24 + 5 * i + k] = (uint8_t)((uint32_t)items[i].value >> (8 * k));
    }
    g_cut = true; /* only the made-up packet arrives at board 0, as if from its peer */
    link_receive(&g_b[0].link, g_b[1].addr, pkt, sizeof pkt, -50);
    run(100);
    ae_config_t c0 = config(0);
    CHECK(fabsf(c0.atten_db - 12.0f) < 0.001f && c0.fade == AE_FADE_MPD);
    CHECK(fabsf(c0.snr_db - 10.0f) < 0.001f && c0.route == AE_ROUTE_LINK && !c0.vox_on);
    CHECK(status(0).version == 900);
    /* Nor does it take them a second time, or call the result a change of its own. */
    uint32_t gen = ae_config_gen(&g_b[0].engine);
    link_receive(&g_b[0].link, g_b[1].addr, pkt, sizeof pkt, -50);
    run(200);
    CHECK(ae_config_gen(&g_b[0].engine) == gen && status(0).version == 900);

    /* Malformed state packets are dropped. */
    pkt[22] = 13; /* more items than there can be */
    link_receive(&g_b[0].link, g_b[1].addr, pkt, sizeof pkt, -50);
    pkt[22] = 6;
    link_receive(&g_b[0].link, g_b[1].addr, pkt, sizeof pkt - 1, -50);
    link_receive(&g_b[0].link, g_b[1].addr, pkt, 22, -50);
    link_receive(&g_b[0].link, g_b[2].addr, pkt, sizeof pkt, -50); /* and from a stranger */
    run(100);
    CHECK(status(0).version == 900);
}

static void test_clock(void)
{
    sim_reset();
    g_b[0].clock_ppm = 40.0;
    g_b[1].clock_ppm = -60.0; /* 100 ppm apart: far worse than two crystals */
    link_up(0);
    link_up(1);
    int leader = g_b[0].coord.boot_id > g_b[1].coord.boot_id ? 0 : 1;
    int follower = 1 - leader;

    /* The boards' own clocks are a quarter of an hour apart. Within a second
     * of meeting, the follower has the leader's time and its fading. */
    CHECK(fabs(shared_clock(0) - shared_clock(1)) > 100000.0);
    run(1000);
    CHECK(status(leader).leads && !status(follower).leads);
    CHECK(fabs(shared_clock(0) - shared_clock(1)) <= 2.0);
    CHECK(fade_seed(0) == fade_seed(1) && fade_seed(leader) == g_b[leader].coord.own_seed);
    CHECK(status(leader).offset_ms == 0);

    /* And keeps it, while the two clocks drift apart by 30 ms over five minutes. */
    double worst = 0;
    for (int s = 0; s < 300; s++) {
        run(1000);
        double err = fabs(shared_clock(0) - shared_clock(1));
        if (err > worst) worst = err;
    }
    CHECK(worst <= 3.0);
    CHECK(status(follower).offset_ms != 0);

    /* The leader gone: the follower carries on from where its clock stands,
     * on its own fading, with no jump. Back again: in step within a second. */
    double before = shared_clock(follower);
    g_cut = true;
    run(2500);
    CHECK(!status(follower).sharing && status(follower).leads);
    CHECK(fade_seed(follower) == g_b[follower].coord.own_seed);
    CHECK(fabs(shared_clock(follower) - before - 2500.0) <= 2.0);
    g_cut = false;
    run(4500);
    CHECK(status(0).sharing && status(1).sharing);
    CHECK(fabs(shared_clock(0) - shared_clock(1)) <= 3.0 && fade_seed(0) == fade_seed(1));

    /* A small error is taken up gradually, not in one step: the fading must
     * not jump for a few milliseconds' disagreement. */
    atomic_store(&g_b[follower].coord.offset_ms, status(follower).offset_ms + 50);
    run(TICK_MS * 10);
    double err = fabs(shared_clock(0) - shared_clock(1));
    CHECK(err > 30.0 && err < 50.0);
    run(1500);
    CHECK(fabs(shared_clock(0) - shared_clock(1)) <= 3.0);
}

/* How alike two stretches of audio are in level, 10 ms at a time. */
static double level_correlation(const int16_t *a, const int16_t *b, size_t n)
{
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    size_t count = 0;
    for (size_t at = 0; at + 480 <= n; at += 480) {
        double pa = 0, pb = 0;
        for (size_t i = 0; i < 480; i++) {
            pa += (double)a[at + i] * a[at + i];
            pb += (double)b[at + i] * b[at + i];
        }
        sa += pa;
        sb += pb;
        saa += pa * pa;
        sbb += pb * pb;
        sab += pa * pb;
        count++;
    }
    double n_ = (double)count;
    return (sab - sa * sb / n_) / sqrt((saa - sa * sa / n_) * (sbb - sb * sb / n_));
}

/* The point of it all: with the channel shared, the two receivers fade
 * together; without, each fades by itself. */
static void test_fading_together(void)
{
    sim_reset();
    g_b[0].clock_ppm = 20.0;
    g_b[1].clock_ppm = -20.0;
    link_up(0);
    link_up(1);
    set(0, MOCK_CTL_FADE, AE_FADE_MPD); /* 2 Hz, 4 ms */
    run(2000);
    CHECK(config(1).fade == AE_FADE_MPD);
    g_b[0].audio = g_b[1].audio = true;
    run(60000);
    size_t n = g_b[0].out_n;
    CHECK(n == (size_t)FS * 60 && g_b[1].out_n == n);
    CHECK(level_correlation(g_b[0].out + FS, g_b[1].out + FS, n - FS) > 0.98);
    /* Both do fade. */
    double low = 1e18, high = 0;
    for (size_t at = FS; at + 480 <= n; at += 480) {
        double p = 0;
        for (size_t i = 0; i < 480; i++) p += (double)g_b[0].out[at + i] * g_b[0].out[at + i];
        if (p < low) low = p;
        if (p > high) high = p;
    }
    CHECK(10 * log10(high / low) > 25.0);

    /* Not shared: the same settings on both, and two different paths. */
    sim_reset();
    for (int i = 0; i < 2; i++) {
        link_up(i);
        set(i, MOCK_CTL_LINK_COORD, 0);
        set(i, MOCK_CTL_FADE, AE_FADE_MPD);
        g_b[i].audio = true;
    }
    run(60000);
    CHECK(fabs(level_correlation(g_b[0].out + FS, g_b[1].out + FS, g_b[0].out_n - FS)) < 0.25);
}

int main(void)
{
    test_settings();
    test_first_meeting();
    test_untrusted();
    test_clock();
    test_fading_together();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
