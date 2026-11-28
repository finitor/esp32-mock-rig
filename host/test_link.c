/*
 * Unit tests for link: two (or three) boards joined by a simulated path that
 * can lose, delay, repeat and reorder packets, with sample clocks that
 * differ.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"

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

/* ---- the simulated path --------------------------------------------------------- */

#define NODES 3
#define STEP_MS 5
#define STEP 240 /* samples at 48 kHz in STEP_MS */
#define QUEUE 512
#define OUT_MAX (48000 * 130)

typedef struct {
    int at_ms; /* when it arrives */
    int order; /* among those arriving together */
    int to, from;
    size_t len;
    uint8_t data[LINK_MAX_PACKET];
    bool used;
} packet_t;

typedef struct {
    link_t link;
    uint8_t addr[LINK_ADDR_LEN];
    int index;
    bool keyed;
    bool tone;        /* what the host plays */
    float phase;
    double drift_ppm; /* this board's sample clock against the true one */
    double drift_acc;
    int16_t *out;     /* everything link_rx produced, silence where it gave nothing */
    size_t out_n;
    size_t heard_n;   /* samples for which link_rx said there was something */
} node_t;

/* What the path does to packets; all zero is a perfect path. */
typedef struct {
    int delay_ms;
    int jitter_ms;       /* extra delay, 0..jitter_ms, never reordering */
    bool duplicate;      /* every audio packet arrives twice */
    bool drop_end;       /* lose the last packet of each transmission */
    int drop_from, drop_n; /* lose drop_n audio packets starting with the drop_from'th sent by node 0 */
    int swap_at;         /* deliver node 0's swap_at'th audio packet after the one behind it; 0 = off */
    int loss_percent;    /* lose this share of node 0's audio packets, at random */
    int hold_every_ms, hold_ms; /* every hold_every_ms the path holds what it carries for hold_ms, then
                                 * delivers it all at once: bunched arrival, as ESP-NOW does sharing the radio */
} path_t;

static node_t g_node[NODES];
static packet_t g_queue[QUEUE];
static path_t g_path;
static int g_ms;
static int g_order;
static int g_audio_sent;   /* audio packets node 0 has sent */
static int g_starts;       /* of those, marked as the first of a transmission */
static int g_hellos[NODES];
static size_t g_longest;
static int g_last_at[NODES];
static uint32_t g_rand = 12345u;

static uint32_t rnd(void)
{
    g_rand = g_rand * 1664525u + 1013904223u;
    return g_rand >> 8;
}

static void enqueue(int to, int from, const uint8_t *data, size_t len, int at_ms)
{
    for (int i = 0; i < QUEUE; i++) {
        if (g_queue[i].used) continue;
        g_queue[i] = (packet_t){.at_ms = at_ms, .order = g_order++, .to = to, .from = from, .len = len, .used = true};
        memcpy(g_queue[i].data, data, len);
        return;
    }
    fail(__FILE__, __LINE__, "the test's packet queue is full");
}

static bool node_send(void *ctx, const uint8_t *dst, const uint8_t *data, size_t len)
{
    node_t *me = ctx;
    bool audio = len >= 2 && (data[1] & 0x0F) == 2;

    if (len > g_longest) g_longest = len;
    if (!audio) g_hellos[me->index]++;
    if (audio && me->index == 0) {
        g_audio_sent++;
        if (data[2] & 0x01) g_starts++;
        if (g_path.drop_end && (data[2] & 0x02)) return true;
        if (g_path.drop_n && g_audio_sent >= g_path.drop_from && g_audio_sent < g_path.drop_from + g_path.drop_n) return true;
        if (g_path.loss_percent && !(data[2] & 0x03) && (int)(rnd() % 100u) < g_path.loss_percent) return true;
    }
    for (int i = 0; i < NODES; i++) {
        if (i == me->index) continue;
        if (dst && memcmp(dst, g_node[i].addr, LINK_ADDR_LEN) != 0) continue;
        int at = g_ms + g_path.delay_ms;
        if (g_path.jitter_ms) at += (int)(rnd() % (uint32_t)(g_path.jitter_ms + 1));
        if (g_path.hold_ms && at % g_path.hold_every_ms < g_path.hold_ms) {
            at += g_path.hold_ms - at % g_path.hold_every_ms;
        }
        if (at < g_last_at[i]) at = g_last_at[i]; /* jitter delays; it does not reorder */
        if (audio && me->index == 0 && g_path.swap_at && g_audio_sent == g_path.swap_at) at += 15;
        else g_last_at[i] = at;
        enqueue(i, me->index, data, len, at);
        if (audio && g_path.duplicate) enqueue(i, me->index, data, len, at);
    }
    return true;
}

static void sim_reset(void)
{
    static int16_t out_store[NODES][OUT_MAX];
    memset(g_queue, 0, sizeof g_queue);
    memset(&g_path, 0, sizeof g_path);
    memset(g_hellos, 0, sizeof g_hellos);
    memset(g_last_at, 0, sizeof g_last_at);
    g_ms = 0;
    g_audio_sent = 0;
    g_starts = 0;
    for (int i = 0; i < NODES; i++) {
        char name[32];
        node_t *nd = &g_node[i];
        memset(nd, 0, offsetof(node_t, out));
        nd->index = i;
        nd->out = out_store[i];
        nd->out_n = 0;
        nd->heard_n = 0;
        memset(nd->addr, 0x10 * (i + 1), LINK_ADDR_LEN);
        snprintf(name, sizeof name, "mockradio-000%d", i + 1);
        link_init(&nd->link, name, node_send, nd);
        link_set_radio(&nd->link, 7074000u, 7074000u);
    }
}

static void enable(int i, uint32_t buffer_ms, bool match)
{
    link_configure(&g_node[i].link, true, buffer_ms, match);
}

/* Run every enabled board for ms milliseconds. */
static void run(int ms)
{
    for (int end = g_ms + ms; g_ms < end; g_ms += STEP_MS) {
        for (int i = 0; i < NODES; i++) {
            node_t *nd = &g_node[i];
            int16_t tx[STEP + 8], rx[STEP + 8];

            /* This board's idea of 5 ms, in its own samples. */
            size_t n = STEP;
            nd->drift_acc += nd->drift_ppm * STEP / 1e6;
            if (nd->drift_acc >= 1.0) {
                n++;
                nd->drift_acc -= 1.0;
            } else if (nd->drift_acc <= -1.0) {
                n--;
                nd->drift_acc += 1.0;
            }

            for (size_t k = 0; k < n; k++) {
                tx[k] = (int16_t)lrintf(8000.0f * sinf(nd->phase));
                nd->phase += 2.0f * 3.14159265f * 1000.0f / 48000.0f;
                if (nd->phase > 2.0f * 3.14159265f) nd->phase -= 2.0f * 3.14159265f;
            }
            link_tick(&nd->link, (uint32_t)g_ms);
            link_tx(&nd->link, nd->tone ? tx : NULL, n, nd->keyed);

            for (int q = 0; q < QUEUE; q++) {
                /* In order of arrival: the earliest due packet first. */
                int best = -1;
                for (int p = 0; p < QUEUE; p++) {
                    if (!g_queue[p].used || g_queue[p].to != i || g_queue[p].at_ms > g_ms) continue;
                    if (best < 0 || g_queue[p].at_ms < g_queue[best].at_ms ||
                        (g_queue[p].at_ms == g_queue[best].at_ms && g_queue[p].order < g_queue[best].order)) {
                        best = p;
                    }
                }
                if (best < 0) break;
                g_queue[best].used = false;
                link_receive(&nd->link, g_node[g_queue[best].from].addr, g_queue[best].data, g_queue[best].len, -40 - i);
            }

            bool heard = link_rx(&nd->link, rx, n);
            if (!heard) memset(rx, 0, n * sizeof rx[0]);
            if (heard) nd->heard_n += n;
            if (nd->out_n + n <= OUT_MAX) {
                memcpy(nd->out + nd->out_n, rx, n * sizeof rx[0]);
                nd->out_n += n;
            }
        }
    }
}

/* Board 0 transmits a tone for ms milliseconds, then everything settles. */
static void transmit(int ms)
{
    g_node[0].keyed = true;
    g_node[0].tone = true;
    run(ms);
    g_node[0].keyed = false;
    g_node[0].tone = false;
    run(600);
}

static link_status_t status(int i)
{
    link_status_t st;
    link_get_status(&g_node[i].link, &st);
    return st;
}

/* ---- looking at what came out ---------------------------------------------------- */

#define LOUD 400 /* a sample this large is the tone, not filter ringing */

static long first_loud(const node_t *nd, size_t from)
{
    for (size_t i = from; i < nd->out_n; i++) {
        if (abs(nd->out[i]) >= LOUD) return (long)i;
    }
    return -1;
}

static long last_loud(const node_t *nd)
{
    for (size_t i = nd->out_n; i > 0; i--) {
        if (abs(nd->out[i - 1]) >= LOUD) return (long)(i - 1);
    }
    return -1;
}

static double rms(const int16_t *x, size_t n)
{
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)x[i] * x[i];
    return sqrt(sum / (double)n);
}

/* The quietest 10 ms window between two points: a dropout shows here. */
static double quietest(const node_t *nd, long from, long to)
{
    double low = 1e9;
    for (long i = from; i + 480 <= to; i += 480) {
        double r = rms(nd->out + i, 480);
        if (r < low) low = r;
    }
    return low;
}

/* The longest run of near-silence between two points, in samples. */
static long longest_gap(const node_t *nd, long from, long to)
{
    long best = 0, run_len = 0;
    for (long i = from; i < to; i++) {
        run_len = abs(nd->out[i]) < LOUD ? run_len + 1 : 0;
        if (run_len > best) best = run_len;
    }
    return best;
}

#define TONE_RMS (8000.0 / 1.41421356)
#define MS(samples) ((double)(samples) / 48.0)

/* ---- tests -------------------------------------------------------------------- */

static void test_peer(void)
{
    sim_reset();
    /* Not enabled: silent, and deaf. */
    run(1000);
    CHECK(g_hellos[0] == 0 && g_hellos[1] == 0);
    CHECK(status(0).state == LINK_OFF && !status(0).peer);

    /* One board alone announces itself four times a second and has no peer. */
    enable(0, 30, true);
    run(1000);
    CHECK(g_hellos[0] == 4);
    CHECK(status(0).state == LINK_NO_PEER);
    CHECK(status(1).state == LINK_OFF);

    /* A second one: each takes the other. */
    enable(1, 30, true);
    link_set_radio(&g_node[1].link, 7074000u, 7075000u);
    run(500);
    link_status_t a = status(0), b = status(1);
    CHECK(a.state == LINK_IDLE && a.peer && strcmp(a.peer_name, "mockradio-0002") == 0);
    CHECK(b.state == LINK_IDLE && b.peer && strcmp(b.peer_name, "mockradio-0001") == 0);
    CHECK(a.peer_rx_hz == 7074000u && a.peer_tx_hz == 7075000u && !a.peer_keyed);
    CHECK(a.has_rssi && a.rssi_dbm == -40 && b.rssi_dbm == -41);
    CHECK(a.target_ms == 30 && a.buffer_ms == 0);

    /* A third board is heard by both and taken by neither. */
    enable(2, 30, true);
    run(1000);
    CHECK(strcmp(status(0).peer_name, "mockradio-0002") == 0);
    CHECK(strcmp(status(1).peer_name, "mockradio-0001") == 0);
    CHECK(status(2).peer); /* it pairs with whichever it heard first, one-sidedly */
    g_node[2].keyed = g_node[2].tone = true;
    run(500);
    g_node[2].keyed = g_node[2].tone = false;
    run(500);
    CHECK(status(0).received == 0 && g_node[0].heard_n == 0);
    CHECK(status(1).received == 0 && g_node[1].heard_n == 0);
    link_configure(&g_node[2].link, false, 30, true);

    /* The peer's PTT shows while it transmits. */
    g_node[1].keyed = true;
    run(400);
    CHECK(status(0).peer_keyed && status(1).state == LINK_SENDING);
    g_node[1].keyed = false;
    run(400);
    CHECK(!status(0).peer_keyed);

    /* A peer that goes quiet is dropped after 3 s, and taken again when back. */
    link_configure(&g_node[1].link, false, 30, true);
    CHECK(status(1).state == LINK_OFF);
    run(2500);
    CHECK(status(0).peer);
    run(700);
    CHECK(!status(0).peer && status(0).state == LINK_NO_PEER && status(0).peer_name[0] == '\0');
    enable(1, 30, true);
    run(300);
    CHECK(status(0).peer && status(1).peer);

    /* A name that fills the field, and none at all. */
    link_t odd;
    link_init(&odd, "exactly-16-chars", node_send, &g_node[2]);
    CHECK(memcmp(odd.name, "exactly-16-chars", 16) == 0);
    link_init(&odd, "a-name-longer-than-the-field", node_send, &g_node[2]);
    CHECK(memcmp(odd.name, "a-name-longer-th", 16) == 0);
    link_init(&odd, NULL, node_send, &g_node[2]);
    CHECK(odd.name[0] == '\0');
    CHECK(g_longest <= LINK_MAX_PACKET);
}

static void test_clean_transfer(void)
{
    sim_reset();
    enable(0, 30, true);
    enable(1, 30, true);
    run(500);
    size_t start = g_node[1].out_n;

    g_node[0].keyed = g_node[0].tone = true;
    run(1000);
    CHECK(status(0).state == LINK_SENDING);
    CHECK(status(1).state == LINK_RECEIVING);
    CHECK(status(1).buffer_ms >= 20 && status(1).buffer_ms <= 50);
    run(1000);
    g_node[0].keyed = g_node[0].tone = false;
    run(600);

    const node_t *b = &g_node[1];
    long first = first_loud(b, start), last = last_loud(b);
    CHECK(first >= 0);
    /* Heard about 30 ms after it was said, on a path with no delay of its
     * own: the buffer's 30 ms fills with the third packet, and the resampler
     * adds 2. On a board the path, and USB's own buffering, come on top. */
    double latency = MS(first - (long)start);
    CHECK(latency >= 25.0 && latency <= 45.0);
    /* The whole 2 s comes out, at the level it went in, with no dropout. */
    double length = MS(last - first);
    CHECK(length >= 1997.0 && length <= 2003.0);
    CHECK(fabs(20.0 * log10(rms(b->out + first + 4800, 48000) / TONE_RMS)) < 0.1);
    CHECK(quietest(b, first + 480, last - 480) > 0.97 * TONE_RMS);

    link_status_t a = status(0), bs = status(1);
    /* 2.05 s of audio in 10 ms packets, and the one that ends it. */
    CHECK(a.sent == 206 && a.send_failed == 0 && g_starts == 1);
    CHECK(bs.received == 206 && bs.lost == 0 && bs.late == 0 && bs.underruns == 0 && bs.overruns == 0);
    CHECK(bs.dropped == 0 && bs.repeated == 0 && bs.off_frequency == 0);
    CHECK(a.state == LINK_IDLE && bs.state == LINK_IDLE && bs.buffer_ms == 0);
    /* Nothing came the other way. */
    CHECK(g_node[0].heard_n == 0 && a.received == 0 && bs.sent == 0);
    CHECK(g_longest <= LINK_MAX_PACKET);

    /* A second transmission is as good as the first. */
    start = g_node[1].out_n;
    transmit(500);
    first = first_loud(b, start);
    CHECK(first >= 0 && MS(first - (long)start) >= 25.0 && MS(first - (long)start) <= 45.0);
    CHECK(MS(last_loud(b) - first) >= 497.0 && MS(last_loud(b) - first) <= 503.0);
    CHECK(status(1).lost == 0 && status(1).underruns == 0 && g_starts == 2);

    /* Both directions at once: each still gets the other's audio. */
    size_t a_start = g_node[0].out_n;
    g_node[0].keyed = g_node[0].tone = true;
    g_node[1].keyed = g_node[1].tone = true;
    run(500);
    g_node[0].keyed = g_node[0].tone = false;
    g_node[1].keyed = g_node[1].tone = false;
    run(600);
    CHECK(first_loud(&g_node[0], a_start) >= 0);
    CHECK(status(0).underruns == 0 && status(1).underruns == 0);
}

static void test_short_and_lost_end(void)
{
    /* A transmission shorter than the buffer is played when its end arrives. */
    sim_reset();
    enable(0, 200, true);
    enable(1, 200, true);
    run(500);
    size_t start = g_node[1].out_n;
    transmit(50); /* 50 ms keyed and 50 ms of tail: 100 ms in all */
    const node_t *b = &g_node[1];
    long first = first_loud(b, start);
    CHECK(first >= 0);
    CHECK(MS(first - (long)start) >= 95.0 && MS(first - (long)start) <= 115.0);
    CHECK(MS(last_loud(b) - first) >= 47.0 && MS(last_loud(b) - first) <= 53.0);
    CHECK(status(1).underruns == 0);

    /* With its last packet lost, it is played once nothing more has come
     * for 100 ms. */
    g_path.drop_end = true;
    start = b->out_n;
    transmit(50);
    first = first_loud(b, start);
    CHECK(first >= 0);
    CHECK(MS(first - (long)start) >= 190.0 && MS(first - (long)start) <= 215.0);
    CHECK(MS(last_loud(b) - first) >= 47.0 && MS(last_loud(b) - first) <= 53.0);
    CHECK(status(1).underruns == 0 && status(1).buffer_ms == 0);

    /* And the one after starts clean, with nothing left over. */
    g_path.drop_end = false;
    start = b->out_n;
    transmit(1000);
    first = first_loud(b, start);
    CHECK(MS(first - (long)start) >= 195.0 && MS(first - (long)start) <= 220.0);
    CHECK(MS(last_loud(b) - first) >= 997.0 && MS(last_loud(b) - first) <= 1003.0);
    CHECK(quietest(b, first + 480, last_loud(b) - 480) > 0.97 * TONE_RMS);
    CHECK(status(1).lost == 0);
}

static void test_tail_and_abort(void)
{
    sim_reset();
    enable(0, 30, true);
    enable(1, 30, true);
    run(500);

    /* Audio still arriving after PTT drops is sent for 50 ms more. */
    const node_t *b = &g_node[1];
    size_t start = b->out_n;
    g_node[0].keyed = g_node[0].tone = true;
    run(200);
    g_node[0].keyed = false; /* the tone goes on */
    run(600);
    long first = first_loud(b, start);
    CHECK(MS(last_loud(b) - first) >= 247.0 && MS(last_loud(b) - first) <= 253.0);
    CHECK(status(0).state == LINK_IDLE);

    /* Keyed again inside the tail: one transmission, not two. */
    g_node[0].tone = true;
    g_starts = 0;
    start = b->out_n;
    g_node[0].keyed = true;
    run(100);
    g_node[0].keyed = false;
    run(20);
    g_node[0].keyed = true;
    run(100);
    g_node[0].keyed = false;
    run(600);
    first = first_loud(b, start);
    CHECK(g_starts == 1);
    CHECK(MS(last_loud(b) - first) >= 267.0 && MS(last_loud(b) - first) <= 273.0);
    CHECK(quietest(b, first + 480, last_loud(b) - 480) > 0.97 * TONE_RMS);

    /* Leaving the link mid-transmission ends it at once, and the other side
     * plays what it got. */
    start = b->out_n;
    g_node[0].keyed = true;
    run(200);
    uint32_t sent = status(0).sent;
    link_tx(&g_node[0].link, NULL, 0, true);
    CHECK(status(0).sent == sent + 1 && status(0).state == LINK_IDLE);
    link_tx(&g_node[0].link, NULL, 0, true); /* again: nothing more to end */
    CHECK(status(0).sent == sent + 1);
    g_node[0].keyed = g_node[0].tone = false;
    run(600);
    first = first_loud(b, start);
    CHECK(MS(last_loud(b) - first) >= 195.0 && MS(last_loud(b) - first) <= 203.0);
    CHECK(status(1).underruns == 0);

    /* Disabling does the same. */
    g_node[0].keyed = g_node[0].tone = true;
    run(200);
    link_configure(&g_node[0].link, false, 30, true);
    run(20);
    CHECK(status(0).state == LINK_OFF);
    g_node[0].keyed = g_node[0].tone = false;
    run(600);
    CHECK(status(1).underruns == 0 && status(1).buffer_ms == 0);
}

static void test_loss(void)
{
    sim_reset();
    enable(0, 30, true);
    enable(1, 30, true);
    run(500);
    const node_t *b = &g_node[1];

    /* One packet lost: 10 ms of silence in its place, the buffer never
     * empty, and everything after it still on time. */
    g_path.drop_from = 50;
    g_path.drop_n = 1;
    size_t start = b->out_n;
    transmit(1000);
    long first = first_loud(b, start), last = last_loud(b);
    CHECK(status(1).lost == 1 && status(1).received == 106 - 1 && status(1).underruns == 0);
    CHECK(MS(last - first) >= 997.0 && MS(last - first) <= 1003.0);
    CHECK(MS(longest_gap(b, first, last)) >= 7.0 && MS(longest_gap(b, first, last)) <= 11.0);

    /* Three in a row, as long as the buffer: it runs empty before the next
     * packet shows how many were lost, and the silence played meanwhile is
     * counted, so there are still 30 ms of it and no more. */
    g_path.drop_from = g_audio_sent + 50;
    g_path.drop_n = 3;
    start = b->out_n;
    transmit(1000);
    first = first_loud(b, start);
    last = last_loud(b);
    CHECK(status(1).lost == 4 && status(1).received == 2 * 106 - 4);
    CHECK(status(1).underruns <= 1 && status(1).late == 0);
    CHECK(MS(last - first) >= 997.0 && MS(last - first) <= 1003.0);
    long gap = longest_gap(b, first, last);
    CHECK(MS(gap) >= 27.0 && MS(gap) <= 31.0);
    /* The silence sits where the lost packets were: 490 to 520 ms in. */
    CHECK(quietest(b, first + 480, first + 48 * 485) > 0.97 * TONE_RMS);
    CHECK(rms(b->out + first + 48 * 495, 48 * 20) < 0.02 * TONE_RMS);
    CHECK(quietest(b, first + 48 * 525, last - 480) > 0.97 * TONE_RMS);

    /* The first packet of a transmission lost: it starts 10 ms in. */
    uint32_t under = status(1).underruns;
    g_path.drop_from = g_audio_sent + 1;
    g_path.drop_n = 1;
    start = b->out_n;
    transmit(500);
    first = first_loud(b, start);
    last = last_loud(b);
    CHECK(MS(last - first) >= 487.0 && MS(last - first) <= 493.0);
    CHECK(status(1).underruns == under);

    /* A gap too long to fill: the buffer runs dry, and it picks up again. */
    uint32_t lost = status(1).lost;
    under = status(1).underruns;
    g_path.drop_from = g_audio_sent + 30;
    g_path.drop_n = 15;
    start = b->out_n;
    transmit(1000);
    first = first_loud(b, start);
    last = last_loud(b);
    CHECK(status(1).lost == lost + 15 && status(1).underruns == under + 1);
    CHECK(quietest(b, last - 48 * 400, last - 480) > 0.97 * TONE_RMS);
    CHECK(status(1).buffer_ms == 0 && status(1).state == LINK_IDLE);

    /* Every packet twice: the copies are dropped and nothing is heard twice. */
    g_path.drop_n = 0;
    g_path.duplicate = true;
    uint32_t late = status(1).late, got = status(1).received;
    start = b->out_n;
    transmit(500);
    first = first_loud(b, start);
    last = last_loud(b);
    CHECK(status(1).late - late == 56 && status(1).received - got == 56);
    CHECK(MS(last - first) >= 497.0 && MS(last - first) <= 503.0);
    CHECK(quietest(b, first + 480, last - 480) > 0.97 * TONE_RMS);

    /* A peer that starts over, its count back at zero: its packets look
     * old at first, and after a few it is followed again. */
    g_path.duplicate = false;
    late = status(1).late;
    g_node[0].link.seq = 0;
    start = b->out_n;
    transmit(500);
    first = first_loud(b, start);
    last = last_loud(b);
    /* Four or five are dropped: the copy that ended the case above counts
     * towards the run. */
    CHECK(status(1).late - late >= 4 && status(1).late - late <= 5);
    CHECK(MS(last - first) >= 447.0 && MS(last - first) <= 473.0);
    CHECK(quietest(b, first + 480, last - 480) > 0.97 * TONE_RMS);

    /* Two packets swapped on the way: the one that comes late is dropped,
     * and silence stands in for it. */
    g_path.duplicate = false;
    g_path.swap_at = g_audio_sent + 20;
    late = status(1).late;
    lost = status(1).lost;
    start = b->out_n;
    transmit(500);
    first = first_loud(b, start);
    last = last_loud(b);
    CHECK(status(1).late - late == 1 && status(1).lost - lost == 1);
    CHECK(MS(last - first) >= 497.0 && MS(last - first) <= 503.0);
    CHECK(MS(longest_gap(b, first, last)) >= 7.0 && MS(longest_gap(b, first, last)) <= 11.0);
}

/* A poor path for a minute: one packet in twenty lost, and uneven arrival. */
static void test_poor_path(void)
{
    sim_reset();
    g_path.loss_percent = 5;
    g_path.jitter_ms = 12;
    enable(0, 30, true);
    enable(1, 30, true);
    run(500);
    const node_t *b = &g_node[1];
    size_t start = b->out_n;
    transmit(60000);
    long first = first_loud(b, start), last = last_loud(b);
    link_status_t st = status(1);

    CHECK(st.lost > 200 && st.lost < 400 && st.received + st.lost == 6006);
    /* Loss empties the buffer for a moment now and then. It is not mistaken
     * for a clock that differs: no sample is dropped or repeated, and the
     * audio ends when it should, a minute after it began. */
    CHECK(st.dropped == 0 && st.repeated == 0 && st.overruns == 0 && st.late == 0);
    CHECK(fabs(MS(last - first) - 60000.0) <= 25.0);
    CHECK(st.buffer_ms == 0 && st.state == LINK_IDLE);
    /* Between the gaps the tone is whole: over any second, the level is what
     * the loss leaves of it. */
    for (long at = first + 48000; at + 48000 < last; at += 10 * 48000) {
        double level = rms(b->out + at, 48000) / TONE_RMS;
        CHECK(level > 0.92 && level < 1.001);
    }
}

static void test_jitter(void)
{
    sim_reset();
    g_path.delay_ms = 2;
    g_path.jitter_ms = 15;
    enable(0, 30, true);
    enable(1, 30, true);
    run(500);
    const node_t *b = &g_node[1];
    size_t start = b->out_n;
    transmit(5000);
    long first = first_loud(b, start), last = last_loud(b);
    CHECK(status(1).underruns == 0 && status(1).lost == 0 && status(1).late == 0);
    CHECK(fabs(MS(last - first) - 5000.0) <= 20.0); /* the first and last packets have their own delays */
    CHECK(quietest(b, first + 480, last - 480) > 0.97 * TONE_RMS);

    /* More jitter than the buffer holds: it runs dry. A longer buffer rides
     * it out. */
    g_path.jitter_ms = 60;
    transmit(5000);
    CHECK(status(1).underruns > 0);
    uint32_t before = status(1).underruns;
    link_configure(&g_node[1].link, true, 100, true);
    CHECK(status(1).target_ms == 100);
    start = b->out_n;
    transmit(5000);
    first = first_loud(b, start);
    last = last_loud(b);
    CHECK(status(1).underruns == before);
    CHECK(quietest(b, first + 480, last - 480) > 0.97 * TONE_RMS);

    /* The buffer setting is clamped. */
    link_configure(&g_node[1].link, true, 1, true);
    CHECK(status(1).target_ms == LINK_BUFFER_MIN_MS);
    link_configure(&g_node[1].link, true, 5000, true);
    CHECK(status(1).target_ms == LINK_BUFFER_MAX_MS);
}

/* Packets held up and then delivered together, as on the bench on Nov 27,
 * 2026: no packet lost or late, but the buffer runs dry during each hold
 * and then gets everything at once. */
static void bunched_case(uint32_t buffer_ms, int hold_ms, bool expect_dry)
{
    sim_reset();
    g_path.hold_every_ms = 1000;
    g_path.hold_ms = hold_ms;
    enable(0, buffer_ms, true);
    enable(1, buffer_ms, true);
    run(500);
    const node_t *b = &g_node[1];
    size_t start = b->out_n;
    transmit(25000);
    long first = first_loud(b, start), last = last_loud(b);
    link_status_t st = status(1);

    CHECK(st.lost == 0 && st.late == 0 && st.overruns == 0);
    CHECK(st.received == 2506);
    if (expect_dry) {
        CHECK(st.underruns >= 20 && st.skipped > 0);
    } else {
        CHECK(st.underruns == 0 && st.skipped == 0);
        CHECK(quietest(b, first + 480, last - 480) > 0.97 * TONE_RMS);
    }
    /* The audio stays in time: what came late after the buffer ran dry is
     * not played late, so the clocks are not judged to differ and no single
     * sample is dropped or repeated to pull the buffer back. */
    CHECK(st.dropped == 0 && st.repeated == 0);
    CHECK(fabs(MS(last - first) - 25000.0) <= 25.0);
    /* Each hold costs at most its own length of audio, not more. */
    if (expect_dry) CHECK(MS(longest_gap(b, first, last)) <= (double)hold_ms + 5.0);
    CHECK(st.buffer_ms == 0 && st.state == LINK_IDLE);
}

static void test_bunched(void)
{
    /* The default buffer on a path with no delay: heard after its 80 ms. */
    sim_reset();
    enable(0, LINK_BUFFER_DEFAULT_MS, true);
    enable(1, LINK_BUFFER_DEFAULT_MS, true);
    run(500);
    size_t start = g_node[1].out_n;
    transmit(1000);
    double latency = MS(first_loud(&g_node[1], start) - (long)start);
    CHECK(latency >= 75.0 && latency <= 95.0);

    /* Holds of 50 ms: longer than the old 30 ms buffer, inside 80 ms. */
    bunched_case(30, 50, true);
    bunched_case(LINK_BUFFER_DEFAULT_MS, 50, false);
    bunched_case(80, 50, false);
    /* Longer than any buffer one would choose. */
    bunched_case(80, 150, true);
}

static void drift_case(double ppm, int seconds, bool expect_steering)
{
    sim_reset();
    g_node[1].drift_ppm = ppm;
    enable(0, 30, true);
    enable(1, 30, true);
    run(500);
    const node_t *b = &g_node[1];
    size_t start = b->out_n;
    transmit(seconds * 1000);
    long first = first_loud(b, start), last = last_loud(b);
    link_status_t st = status(1);

    CHECK(st.underruns == 0 && st.overruns == 0 && st.lost == 0);
    CHECK(quietest(b, first + 480, last - 480) > 0.97 * TONE_RMS);
    /* In this board's own samples the transmission is longer or shorter by
     * the difference between the clocks, less what the buffer took up. */
    CHECK(fabs(MS(last - first) - seconds * 1000.0 * (1.0 + ppm / 1e6)) < 40.0);
    if (!expect_steering) {
        CHECK(st.dropped == 0 && st.repeated == 0);
    } else if (ppm > 0) {
        /* This board's clock is fast: it uses samples up, so some are played twice. */
        CHECK(st.repeated > 0 && st.dropped == 0);
    } else {
        CHECK(st.dropped > 0 && st.repeated == 0);
    }
}

static void test_drift(void)
{
    /* Two crystals 40 ppm apart over a two-minute transmission: the buffer
     * takes it with nothing dropped or repeated. */
    drift_case(40.0, 120, false);
    drift_case(-40.0, 120, false);
    /* Far worse than any crystal: single samples keep the buffer in range. */
    drift_case(1000.0, 60, true);
    drift_case(-1000.0, 60, true);
}

static void test_frequency(void)
{
    sim_reset();
    enable(0, 30, true);
    enable(1, 30, true);
    link_set_radio(&g_node[0].link, 14074000u, 14074000u);
    run(500);
    const node_t *b = &g_node[1];

    /* Different frequencies: nothing is heard, and the status says why. */
    g_node[0].keyed = g_node[0].tone = true;
    run(500);
    CHECK(status(1).state == LINK_OFF_FREQUENCY);
    CHECK(status(1).peer_tx_hz == 14074000u);
    g_node[0].keyed = g_node[0].tone = false;
    run(600);
    CHECK(b->heard_n == 0 && status(1).received == 0 && status(1).off_frequency == 56);
    CHECK(status(1).state == LINK_IDLE && status(1).buffer_ms == 0);

    /* Tuned to it part-way through: heard from there, with no jump. */
    size_t start = b->out_n;
    g_node[0].keyed = g_node[0].tone = true;
    run(500);
    link_set_radio(&g_node[1].link, 14074000u, 14074000u);
    run(500);
    g_node[0].keyed = g_node[0].tone = false;
    run(600);
    long first = first_loud(b, start), last = last_loud(b);
    CHECK(first >= 0 && MS(first - (long)start) >= 520.0 && MS(first - (long)start) <= 550.0);
    CHECK(MS(last - first) >= 485.0 && MS(last - first) <= 503.0);
    CHECK(status(1).lost == 0 && status(1).underruns == 0);
    CHECK(quietest(b, first + 480, last - 480) > 0.97 * TONE_RMS);

    /* Split: what counts is the sender's transmit frequency against the
     * receiver's receive frequency. */
    link_set_radio(&g_node[0].link, 7074000u, 7076000u);
    link_set_radio(&g_node[1].link, 7076000u, 7074000u);
    uint32_t got = status(1).received;
    transmit(300);
    CHECK(status(1).received > got);
    link_set_radio(&g_node[1].link, 7074000u, 7076000u);
    got = status(1).received;
    transmit(300);
    CHECK(status(1).received == got);

    /* Matching off: heard whatever the frequencies. */
    link_configure(&g_node[1].link, true, 30, false);
    transmit(300);
    CHECK(status(1).received > got && status(1).state == LINK_IDLE);
}

static void test_malformed(void)
{
    static const uint8_t src[LINK_ADDR_LEN] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10};
    uint8_t pkt[LINK_MAX_PACKET + 10];
    int16_t out[STEP];

    sim_reset();
    enable(0, 30, true);
    enable(1, 30, true);
    run(500);
    link_t *l = &g_node[1].link;
    link_status_t before = status(1);

    memset(pkt, 0, sizeof pkt);
    link_receive(l, src, pkt, 0, 0);
    link_receive(l, src, pkt, 1, 0);
    link_receive(l, src, pkt, 40, 0); /* no magic */
    pkt[0] = 0x4D;
    pkt[1] = 0x22;                    /* a later version */
    link_receive(l, src, pkt, 40, 0);
    pkt[1] = 0x1F;                    /* an unknown type */
    link_receive(l, src, pkt, 40, 0);
    pkt[1] = 0x11;                    /* hello, wrong length */
    link_receive(l, src, pkt, 26, 0);
    link_receive(l, src, pkt, 28, 0);
    pkt[1] = 0x12;                    /* audio: too short, odd, too long */
    link_receive(l, src, pkt, 8, 0);
    link_receive(l, src, pkt, 12, 0);
    link_receive(l, src, pkt, LINK_MAX_PACKET + 1, 0);
    link_receive(l, src, pkt, sizeof pkt, 0);
    link_receive(l, NULL, pkt, 249, 0);
    link_receive(l, src, NULL, 249, 0);
    for (int i = 0; i < 2000; i++) { /* noise of every length */
        size_t len = rnd() % (sizeof pkt);
        for (size_t k = 0; k < len; k++) pkt[k] = (uint8_t)rnd();
        if (i & 1) pkt[0] = 0x4D;
        if (i & 2) pkt[1] = (uint8_t)(0x10 | (rnd() % 3));
        link_receive(l, src, pkt, len, 0);
    }
    link_status_t after = status(1);
    CHECK(strcmp(after.peer_name, before.peer_name) == 0 && after.state != LINK_OFF);

    /* An audio packet from the peer with no samples is taken and adds nothing. */
    link_configure(l, true, 30, false);
    uint8_t empty[9] = {0x4D, 0x12, 0x03, 0x00, 0x00};
    uint32_t got = status(1).received;
    link_receive(l, src, empty, sizeof empty, 0);
    CHECK(status(1).received == got + 1);

    /* Disabled: packets are ignored and nothing waits in the buffer. */
    link_configure(l, false, 30, true);
    uint8_t audio[249] = {0x4D, 0x12, 0x01};
    link_receive(l, src, audio, sizeof audio, 0);
    CHECK(!link_rx(l, out, STEP));
    CHECK(status(1).state == LINK_OFF && status(1).buffer_ms == 0);
}

/* ---- framing for a byte stream -------------------------------------------------- */

static uint8_t g_wire_got[4][LINK_MAX_PACKET];
static size_t g_wire_len[4];
static int g_wire_n;

static void wire_packet(void *ctx, const uint8_t *pkt, size_t len)
{
    (void)ctx;
    if (g_wire_n < 4) {
        memcpy(g_wire_got[g_wire_n], pkt, len);
        g_wire_len[g_wire_n] = len;
    }
    g_wire_n++;
}

static void test_wire(void)
{
    uint8_t frame[LINK_WIRE_MAX], pkt[LINK_MAX_PACKET];
    link_wire_t w;

    /* The check is CRC-16/CCITT-FALSE: 29B1 for "123456789". */
    size_t n = link_wire_encode((const uint8_t *)"123456789", 9, frame, sizeof frame);
    CHECK(n == 13 && frame[0] == 0xC0 && frame[10] == 0x29 && frame[11] == 0xB1 && frame[12] == 0xC0);

    /* The two special bytes are escaped, in the data and in the check. */
    static const uint8_t special[] = {0xC0, 0xDB, 0x00, 0xDC, 0xDD, 0xC0};
    n = link_wire_encode(special, sizeof special, frame, sizeof frame);
    CHECK(n >= sizeof special + 3 + 4);
    for (size_t i = 1; i + 1 < n; i++) CHECK(frame[i] != 0xC0);
    link_wire_init(&w);
    g_wire_n = 0;
    link_wire_feed(&w, frame, n, wire_packet, NULL);
    CHECK(g_wire_n == 1 && g_wire_len[0] == sizeof special && memcmp(g_wire_got[0], special, sizeof special) == 0);

    /* The longest packet, of nothing but the byte that doubles. */
    memset(pkt, 0xC0, sizeof pkt);
    n = link_wire_encode(pkt, sizeof pkt, frame, sizeof frame);
    CHECK(n > 0 && n <= LINK_WIRE_MAX);
    g_wire_n = 0;
    for (size_t i = 0; i < n; i++) link_wire_feed(&w, &frame[i], 1, wire_packet, NULL); /* a byte at a time */
    CHECK(g_wire_n == 1 && g_wire_len[0] == sizeof pkt && memcmp(g_wire_got[0], pkt, sizeof pkt) == 0);
    CHECK(w.bad == 0);

    /* What does not fit is not encoded. */
    CHECK(link_wire_encode(pkt, sizeof pkt + 1, frame, sizeof frame) == 0);
    CHECK(link_wire_encode(pkt, 0, frame, sizeof frame) == 0);
    CHECK(link_wire_encode(pkt, 10, frame, 25) == 0);
    CHECK(link_wire_encode(pkt, 10, frame, 26) > 0);

    /* Garbage, then a good frame, a spoiled one, and two good ones back to
     * back: the three good ones come through. */
    uint8_t stream[1024];
    size_t len = 0;
    for (int i = 0; i < 40; i++) stream[len++] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 20; i++) pkt[i] = (uint8_t)i;
    len += link_wire_encode(pkt, 20, &stream[len], sizeof stream - len);
    size_t spoiled = len + link_wire_encode(pkt, 20, &stream[len], sizeof stream - len);
    stream[len + 5] ^= 0x01;
    len = spoiled;
    pkt[0] = 0xAA;
    len += link_wire_encode(pkt, 20, &stream[len], sizeof stream - len);
    pkt[0] = 0xBB;
    len += link_wire_encode(pkt, 3, &stream[len], sizeof stream - len);
    link_wire_init(&w);
    g_wire_n = 0;
    link_wire_feed(&w, stream, len, wire_packet, NULL);
    CHECK(g_wire_n == 3 && w.bad == 2); /* the garbage and the spoiled frame */
    CHECK(g_wire_len[0] == 20 && g_wire_got[0][0] == 0 && g_wire_got[0][19] == 19);
    CHECK(g_wire_len[1] == 20 && g_wire_got[1][0] == 0xAA);
    CHECK(g_wire_len[2] == 3 && g_wire_got[2][0] == 0xBB);

    /* Too long, a bad escape, cut off in an escape, too short for a check. */
    link_wire_init(&w);
    g_wire_n = 0;
    uint8_t byte = 0xC0;
    link_wire_feed(&w, &byte, 1, wire_packet, NULL);
    byte = 0x55;
    for (int i = 0; i < 300; i++) link_wire_feed(&w, &byte, 1, wire_packet, NULL);
    static const uint8_t rest[] = {0xC0, 0x01, 0xDB, 0x02, 0x03, 0xC0, 0x01, 0x02, 0xDB, 0xC0, 0x01, 0x02, 0xC0, 0xC0, 0xC0};
    link_wire_feed(&w, rest, sizeof rest, wire_packet, NULL);
    CHECK(g_wire_n == 0 && w.bad == 4);

    /* End to end: a link's own packets through the framing. */
    sim_reset();
    enable(0, 30, true);
    enable(1, 30, true);
    run(500);
    CHECK(g_longest <= LINK_MAX_PACKET);
}

int main(void)
{
    test_peer();
    test_clean_transfer();
    test_short_and_lost_end();
    test_tail_and_abort();
    test_loss();
    test_poor_path();
    test_jitter();
    test_bunched();
    test_drift();
    test_frequency();
    test_malformed();
    test_wire();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
