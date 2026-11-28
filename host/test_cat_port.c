/*
 * Unit tests for cat_port, the CAT serial port's replies on their way to the
 * host, and for CAT under load: the CI-V parser feeding it while the host
 * polls hard, keys PTT and stops reading for a while, as a Hamlib rigctld
 * facade did on the radio bench on Nov 27, 2026.
 */
#include <stdio.h>
#include <string.h>

#include "cat_civ.h"
#include "cat_port.h"
#include "rig_ic705.h"

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

static int g_locks, g_held;

static void test_lock(void *ctx)
{
    (void)ctx;
    CHECK(!g_held);
    g_held = 1;
    g_locks++;
}

static void test_unlock(void *ctx)
{
    (void)ctx;
    CHECK(g_held);
    g_held = 0;
}

static cat_port_status_t status(cat_port_t *p)
{
    cat_port_status_t st;
    cat_port_get_status(p, &st);
    return st;
}

/* A reply of n bytes whose bytes all say which one it is. */
static void put_n(cat_port_t *p, uint8_t id, size_t n, uint32_t now)
{
    uint8_t b[CAT_PORT_FRAME_MAX + 1];
    memset(b, id, sizeof b);
    CHECK(cat_port_put(p, b, n, now));
}

static void test_order_and_size(void)
{
    static cat_port_t p;
    uint8_t out[3 * CAT_PORT_FRAME_MAX];
    cat_port_init(&p, test_lock, test_unlock, NULL);

    /* Nothing waiting: nothing out. */
    CHECK(cat_port_take(&p, out, CAT_PORT_FRAME_MAX, 0) == 0);

    /* Whole replies, oldest first, as many as fit. */
    put_n(&p, 1, 11, 0);
    put_n(&p, 2, 11, 0);
    put_n(&p, 3, 50, 0);
    CHECK(cat_port_waiting(&p) == 3);
    size_t n = cat_port_take(&p, out, CAT_PORT_FRAME_MAX, 5);
    CHECK(n == 22 && out[0] == 1 && out[10] == 1 && out[11] == 2 && out[21] == 2);
    CHECK(cat_port_waiting(&p) == 1);
    n = cat_port_take(&p, out, CAT_PORT_FRAME_MAX, 5);
    CHECK(n == 50 && out[0] == 3 && out[49] == 3);
    CHECK(status(&p).queued == 3 && status(&p).sent == 3 && status(&p).waiting == 0);

    /* One packet's worth always holds the first, even at the longest. */
    put_n(&p, 4, CAT_PORT_FRAME_MAX, 0);
    CHECK(cat_port_take(&p, out, CAT_PORT_FRAME_MAX, 0) == CAT_PORT_FRAME_MAX);

    /* Empty and too long are refused; a cap of 0 sends nothing. */
    uint8_t big[CAT_PORT_FRAME_MAX + 1] = {0};
    CHECK(!cat_port_put(&p, big, 0, 0));
    CHECK(!cat_port_put(&p, NULL, 5, 0));
    CHECK(!cat_port_put(&p, big, sizeof big, 0));
    CHECK(status(&p).too_long == 1);
    put_n(&p, 5, 8, 0);
    CHECK(cat_port_take(&p, out, 0, 0) == 0 && cat_port_waiting(&p) == 1);

    /* Every put and take is made under the lock, and releases it. */
    CHECK(g_locks > 0 && !g_held);

    /* Opened afresh: what waits was for someone else. */
    put_n(&p, 6, 8, 0);
    cat_port_discard(&p);
    CHECK(cat_port_waiting(&p) == 0 && status(&p).discarded == 2);
    CHECK(cat_port_take(&p, out, CAT_PORT_FRAME_MAX, 0) == 0);
}

static void test_stale_and_full(void)
{
    static cat_port_t p;
    uint8_t out[CAT_PORT_FRAME_MAX];
    cat_port_init(&p, NULL, NULL, NULL);

    /* A reply that waited a second is not sent; a fresher one behind it is. */
    put_n(&p, 1, 9, 1000);
    put_n(&p, 2, 9, 1600);
    CHECK(cat_port_take(&p, out, sizeof out, 1999) == 18);
    put_n(&p, 3, 9, 3000);
    put_n(&p, 4, 9, 3600);
    CHECK(cat_port_take(&p, out, sizeof out, 4000) == 9 && out[0] == 4);
    CHECK(status(&p).stale == 1);

    /* Dropping by age needs no room to send into. */
    put_n(&p, 5, 9, 5000);
    CHECK(cat_port_take(&p, NULL, 0, 6000) == 0 && cat_port_waiting(&p) == 0 && status(&p).stale == 2);

    /* Full: the oldest goes, never the newest, and put never waits. */
    for (int i = 0; i < CAT_PORT_SLOTS + 5; i++) put_n(&p, (uint8_t)(10 + i), 9, 7000);
    CHECK(cat_port_waiting(&p) == CAT_PORT_SLOTS && status(&p).full == 5);
    CHECK(cat_port_take(&p, out, 9, 7001) == 9 && out[0] == 15);
    while (cat_port_take(&p, out, 9, 7001)) {
    }
    CHECK(out[0] == 10 + CAT_PORT_SLOTS + 4);

    /* The clock wraps without making anything stale or fresh by mistake. */
    put_n(&p, 7, 9, 0xFFFFFF00u);
    CHECK(cat_port_take(&p, out, sizeof out, 0x00000010u) == 9);
    put_n(&p, 8, 9, 0xFFFFFF00u);
    CHECK(cat_port_take(&p, out, sizeof out, 0xFFFFFF00u + CAT_PORT_STALE_MS) == 0);
}

static void test_watch(void)
{
    static cat_port_t p;
    cat_port_watch_t w = {0};
    cat_port_init(&p, NULL, NULL, NULL);

    /* A host that is not reading leaves the transfer armed: never lost. */
    for (uint32_t t = 0; t < 60000; t += 20) CHECK(!cat_port_watch(&p, &w, true, true, t));
    /* Idle: nothing to watch. */
    for (uint32_t t = 60000; t < 70000; t += 20) CHECK(!cat_port_watch(&p, &w, false, false, t));

    /* Busy with the hardware idle for a moment is a completion on its way. */
    CHECK(!cat_port_watch(&p, &w, true, false, 70000));
    CHECK(!cat_port_watch(&p, &w, true, false, 70040));
    CHECK(!cat_port_watch(&p, &w, false, false, 70060));
    /* For a second: lost. Once, then again a second later if still so. */
    int fired = 0;
    uint32_t first = 0;
    for (uint32_t t = 80000; t <= 82500; t += 20) {
        if (cat_port_watch(&p, &w, true, false, t)) {
            if (!fired) first = t;
            fired++;
        }
    }
    CHECK(fired == 2 && first == 80000 + CAT_PORT_LOST_MS);
    CHECK(status(&p).recovered == 2);
    /* The hardware arming again in between starts the wait over. */
    w.armed = false;
    CHECK(!cat_port_watch(&p, &w, true, false, 90000));
    CHECK(!cat_port_watch(&p, &w, true, true, 90500));
    CHECK(!cat_port_watch(&p, &w, true, false, 90600));
    CHECK(!cat_port_watch(&p, &w, true, false, 91500));
    CHECK(cat_port_watch(&p, &w, true, false, 91600));
}

/* ---- CAT under load ------------------------------------------------------------- */

static cat_port_t g_port;
static uint32_t g_now;
static int g_frames_fed; /* frames handed to the parser so far */
static int g_ptt_at[4];  /* the frame count at each PTT event */
static bool g_ptt_on[4];
static int g_ptt_n;

static void port_tx(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    CHECK(cat_port_put(&g_port, b, n, g_now));
}

static void port_ptt(void *ctx, bool on)
{
    (void)ctx;
    if (g_ptt_n < 4) {
        g_ptt_at[g_ptt_n] = g_frames_fed;
        g_ptt_on[g_ptt_n] = on;
    }
    g_ptt_n++;
}

static void feed(civ_t *c, const uint8_t *frame, size_t len)
{
    g_frames_fed++;
    civ_feed(c, frame, len);
}

static void test_cat_under_load(void)
{
    static civ_t c;
    static const uint8_t read_freq[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x03, 0xFD};
    static const uint8_t read_mode[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x04, 0xFD};
    static const uint8_t read_s[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x15, 0x02, 0xFD};
    static const uint8_t read_swr[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x15, 0x12, 0xFD};
    static const uint8_t read_alc[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x15, 0x13, 0xFD};
    static const uint8_t read_po[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x15, 0x11, 0xFD};
    static const uint8_t ptt_on[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x1C, 0x00, 0x01, 0xFD};
    static const uint8_t ptt_off[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x1C, 0x00, 0x00, 0xFD};
    static const uint8_t *const polls[] = {read_freq, read_mode, read_s, read_swr, read_alc, read_po};
    static const size_t poll_len[] = {6, 6, 7, 7, 7, 7};
    uint8_t out[CAT_PORT_FRAME_MAX];

    cat_port_init(&g_port, NULL, NULL, NULL);
    civ_init(&c, &rig_ic705, port_tx, port_ptt, NULL);

    /* The host polls six readings every 100 ms (far harder than the bench's
     * once a second) and never reads a reply: 30 s of it, keyed for the
     * middle 10 s, with a PTT command arriving in one piece with a poll. */
    for (int tick = 0; tick < 300; tick++) {
        g_now = (uint32_t)tick * 100u;
        if (tick == 100) feed(&c, ptt_on, sizeof ptt_on);
        if (tick == 200) feed(&c, ptt_off, sizeof ptt_off);
        for (int k = 0; k < 6; k++) feed(&c, polls[k], poll_len[k]);
        /* The USB task, seeing the endpoint busy, only drops what is stale. */
        (void)cat_port_take(&g_port, NULL, 0, g_now);
    }
    /* Each PTT command acted on as it was parsed, behind 600 reads, not
     * after their replies had gone anywhere. */
    CHECK(g_ptt_n == 2);
    CHECK(g_ptt_on[0] && g_ptt_at[0] == 601);
    CHECK(!g_ptt_on[1] && g_ptt_at[1] == 1202);
    CHECK(c.frames_ok == 1802 && c.frames_ng == 0 && c.frames_dropped == 0);

    /* Nothing piled up: what waits is the newest few, none older than a
     * second, however long the host was away. */
    cat_port_status_t st = status(&g_port);
    CHECK(st.queued == 1802 && st.sent == 0);
    CHECK(st.waiting <= CAT_PORT_SLOTS && st.waiting > 0);
    CHECK(st.stale + st.full + st.waiting == 1802);

    /* The host comes back 2 s later and asks for the frequency: the first
     * reply it gets is that one, not a backlog. */
    g_now += 2000;
    (void)cat_port_take(&g_port, NULL, 0, g_now);
    CHECK(cat_port_waiting(&g_port) == 0);
    feed(&c, read_freq, sizeof read_freq);
    size_t n = cat_port_take(&g_port, out, sizeof out, g_now + 1);
    static const uint8_t want[] = {0xFE, 0xFE, 0xE0, 0xA4, 0x03, 0x00, 0x40, 0x07, 0x14, 0x00, 0xFD};
    CHECK(n == sizeof want && memcmp(out, want, sizeof want) == 0);

    /* One reply the host abandons does not hold up the next: it goes stale
     * and the next goes out. */
    feed(&c, read_mode, sizeof read_mode);
    g_now += CAT_PORT_STALE_MS;
    feed(&c, read_freq, sizeof read_freq);
    n = cat_port_take(&g_port, out, sizeof out, g_now);
    CHECK(n == sizeof want && memcmp(out, want, sizeof want) == 0);
}

int main(void)
{
    test_order_and_size();
    test_stale_and_full();
    test_watch();
    test_cat_under_load();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
