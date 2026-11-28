/* link: see link.h. */
#include "link.h"

#include <string.h>

/* ---- packets ------------------------------------------------------------------
 *
 * Little-endian. Every packet starts with the magic byte and a byte holding
 * the format version (high half) and the packet type (low half).
 *
 *   hello:  4D 11 <flags> <rx Hz, 4> <tx Hz, 4> <name, 16, zero padded>
 *             flags: 01 transmitting
 *   audio:  4D 12 <flags> <seq, 2> <tx Hz, 4> <samples, 2 each, 0..120>
 *             flags: 01 first of a transmission, 02 last
 *   state:  4D 13 <boot id, 4> <version, 4> <nonce, 4> <clock ms, 4> <seed, 4>
 *             <n> then n times <id> <value, 4>
 */
#define MAGIC 0x4D
#define VERSION 1
#define TYPE_HELLO 1
#define TYPE_AUDIO 2
#define TYPE_STATE 3

#define HELLO_LEN (11 + LINK_NAME_MAX)
#define AUDIO_HDR 9
#define STATE_HDR 23
#define FLAG_KEYED 0x01
#define FLAG_START 0x01
#define FLAG_END 0x02

#define TAIL_SAMPLES (LINK_TAIL_MS * 48u)
#define STALL_SAMPLES (LINK_STALL_MS * 48u)
#define STEER_GAP_SAMPLES (LINK_STEER_MS * 48u)
#define FLUSH_SAMPLES ((uint32_t)(DSP_RS_TAPS / DSP_RS_FACTOR)) /* zeros that push the last audio out of the interpolator */
#define PIECE 64
#define LATE_RESYNC 5

_Static_assert(AUDIO_HDR + 2 * LINK_FRAME_SAMPLES <= LINK_MAX_PACKET, "an audio packet must fit one ESP-NOW frame");
_Static_assert(HELLO_LEN <= LINK_MAX_PACKET, "hello too long");
_Static_assert(STATE_HDR + 5 * LINK_STATE_ITEMS <= LINK_MAX_PACKET, "state too long");

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    put_u16(p, (uint16_t)v);
    put_u16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)get_u16(p) | ((uint32_t)get_u16(p + 2) << 16);
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- set-up and settings ------------------------------------------------------ */

void link_init(link_t *l, const char *name, link_send_fn send, void *ctx)
{
    memset(l, 0, sizeof *l);
    l->send = send;
    l->ctx = ctx;
    /* zero padded by the memset; no terminator if it fills the field */
    for (size_t i = 0; name && i < LINK_NAME_MAX && name[i]; i++) l->name[i] = name[i];
    dsp_fifo_init(&l->fifo, l->fifo_store, LINK_FIFO_SAMPLES);
    dsp_interp4_init(&l->up);
    dsp_decim4_init(&l->down);
    atomic_store(&l->match, true);
    atomic_store(&l->target, LINK_BUFFER_DEFAULT_MS * (LINK_FS_HZ / 1000u));
}

void link_configure(link_t *l, bool enabled, uint32_t buffer_ms, bool match)
{
    atomic_store(&l->target, clamp_u32(buffer_ms, LINK_BUFFER_MIN_MS, LINK_BUFFER_MAX_MS) * (LINK_FS_HZ / 1000u));
    atomic_store(&l->match, match);
    atomic_store(&l->enabled, enabled);
    if (!enabled) atomic_store(&l->have_peer, false);
}

void link_set_radio(link_t *l, uint32_t rx_hz, uint32_t tx_hz)
{
    atomic_store(&l->rx_hz, rx_hz);
    atomic_store(&l->tx_hz, tx_hz);
}

/* The peer's address, if there is one. */
static bool peer_addr(link_t *l, uint8_t addr[LINK_ADDR_LEN])
{
    for (int tries = 0; tries < 4; tries++) {
        uint32_t gen = atomic_load(&l->peer_gen);
        if (gen & 1u) continue; /* being written */
        memcpy(addr, l->peer_addr, LINK_ADDR_LEN);
        if (atomic_load(&l->peer_gen) == gen) return atomic_load(&l->have_peer);
    }
    return false;
}

/* ---- sending -------------------------------------------------------------------- */

static void send_frame(link_t *l, uint8_t flags)
{
    uint8_t pkt[AUDIO_HDR + 2 * LINK_FRAME_SAMPLES];
    uint8_t dst[LINK_ADDR_LEN];

    if (l->tx_start) flags |= FLAG_START;
    l->tx_start = false;
    pkt[0] = MAGIC;
    pkt[1] = (VERSION << 4) | TYPE_AUDIO;
    pkt[2] = flags;
    put_u16(&pkt[3], l->seq++);
    put_u32(&pkt[5], atomic_load(&l->tx_hz));
    for (uint32_t i = 0; i < l->fill; i++) put_u16(&pkt[AUDIO_HDR + 2 * i], (uint16_t)l->frame[i]);
    size_t len = AUDIO_HDR + 2 * (size_t)l->fill;
    l->fill = 0;

    if (!peer_addr(l, dst)) {
        /* Nobody to hear it. But a transmission that was being heard still
         * gets its end, even if the link has just been switched off, which
         * forgets the peer: the address is still there. */
        if (!(flags & FLAG_END) || !l->tx_heard) return;
        memcpy(dst, l->peer_addr, LINK_ADDR_LEN);
    }
    l->tx_heard = !(flags & FLAG_END);
    atomic_fetch_add(&l->n_sent, 1u);
    if (!l->send(l->ctx, dst, pkt, len)) atomic_fetch_add(&l->n_send_failed, 1u);
}

/* Add n samples at 48 kHz to the transmission. */
static void feed(link_t *l, const int16_t *tx, size_t n)
{
    static const int16_t silence[PIECE];

    while (n) {
        size_t k = n < PIECE ? n : PIECE;
        int16_t low[PIECE / DSP_RS_FACTOR];
        size_t got = dsp_decim4(&l->down, tx ? tx : silence, k, low);
        for (size_t i = 0; i < got; i++) {
            l->frame[l->fill++] = low[i];
            if (l->fill == LINK_FRAME_SAMPLES) send_frame(l, 0);
        }
        if (tx) tx += k;
        n -= k;
    }
}

static void finish(link_t *l)
{
    send_frame(l, FLAG_END); /* with whatever part of a frame is left, even none */
    l->tx_active = false;
    l->tx_tail = false;
    atomic_store(&l->st_sending, false);
}

void link_tx(link_t *l, const int16_t *tx, size_t n, bool keyed)
{
    if (n == 0 || !atomic_load(&l->enabled)) {
        if (l->tx_active) finish(l);
        return;
    }
    if (keyed) {
        if (!l->tx_active) {
            l->tx_active = true;
            l->tx_start = true;
            l->fill = 0;
            dsp_decim4_init(&l->down);
            atomic_store(&l->st_sending, true);
        }
        l->tx_tail = false; /* keyed again inside the tail: the same transmission goes on */
        feed(l, tx, n);
    } else if (l->tx_active) {
        if (!l->tx_tail) {
            l->tx_tail = true;
            l->tx_tail_left = TAIL_SAMPLES;
        }
        size_t k = l->tx_tail_left < n ? l->tx_tail_left : n;
        feed(l, tx, k);
        l->tx_tail_left -= (uint32_t)k;
        if (!l->tx_tail_left) finish(l);
    }
}

void link_send_failed(link_t *l)
{
    atomic_fetch_add(&l->n_send_failed, 1u);
}

/* ---- announcing, and the peer --------------------------------------------------- */

static uint32_t clock_now(link_t *l)
{
    return l->clock ? l->clock(l->clock_ctx) : atomic_load(&l->now_ms);
}

void link_set_clock(link_t *l, uint32_t (*clock)(void *ctx), void *ctx)
{
    l->clock = clock;
    l->clock_ctx = ctx;
}

void link_set_shared(link_t *l, const link_shared_t *mine, int32_t clock_offset_ms)
{
    l->have_mine = mine != NULL;
    if (mine) l->mine = *mine;
    if (l->mine.n > LINK_STATE_ITEMS) l->mine.n = LINK_STATE_ITEMS;
    l->mine_offset_ms = clock_offset_ms;
}

bool link_peer_shared(link_t *l, link_shared_t *theirs, uint32_t *heard_ms)
{
    for (int tries = 0; tries < 4; tries++) {
        uint32_t gen = atomic_load(&l->pst_gen);
        if (gen & 1u) continue; /* being written */
        *theirs = l->pst;
        *heard_ms = l->pst_heard_ms;
        if (atomic_load(&l->pst_gen) == gen) return atomic_load(&l->pst_have) && atomic_load(&l->have_peer);
    }
    return false;
}

static void send_state(link_t *l)
{
    uint8_t pkt[STATE_HDR + 5 * LINK_STATE_ITEMS];
    uint8_t dst[LINK_ADDR_LEN];
    if (!l->have_mine || !peer_addr(l, dst)) return; /* it is for the peer and no one else */

    pkt[0] = MAGIC;
    pkt[1] = (VERSION << 4) | TYPE_STATE;
    put_u32(&pkt[2], l->mine.boot_id);
    put_u32(&pkt[6], l->mine.version);
    put_u32(&pkt[10], l->mine.nonce);
    put_u32(&pkt[14], clock_now(l) + (uint32_t)l->mine_offset_ms);
    put_u32(&pkt[18], l->mine.seed);
    pkt[22] = l->mine.n;
    for (unsigned i = 0; i < l->mine.n; i++) {
        pkt[STATE_HDR + 5 * i] = l->mine.item[i].id;
        put_u32(&pkt[STATE_HDR + 5 * i + 1], (uint32_t)l->mine.item[i].value);
    }
    (void)l->send(l->ctx, dst, pkt, STATE_HDR + 5 * (size_t)l->mine.n);
}

void link_tick(link_t *l, uint32_t now_ms)
{
    atomic_store(&l->now_ms, now_ms);
    if (!atomic_load(&l->enabled)) {
        l->hello_sent = false;
        return;
    }
    if (atomic_load(&l->have_peer) && now_ms - atomic_load(&l->peer_heard_ms) > LINK_PEER_TIMEOUT_MS) {
        atomic_store(&l->have_peer, false);
    }
    if (l->hello_sent && now_ms - l->last_hello_ms < LINK_HELLO_MS) return;
    l->hello_sent = true;
    l->last_hello_ms = now_ms;

    uint8_t pkt[HELLO_LEN];
    pkt[0] = MAGIC;
    pkt[1] = (VERSION << 4) | TYPE_HELLO;
    pkt[2] = atomic_load(&l->st_sending) ? FLAG_KEYED : 0;
    put_u32(&pkt[3], atomic_load(&l->rx_hz));
    put_u32(&pkt[7], atomic_load(&l->tx_hz));
    memcpy(&pkt[11], l->name, LINK_NAME_MAX);
    (void)l->send(l->ctx, NULL, pkt, sizeof pkt);
    send_state(l);
}

/* ---- receiving: the transport's side ---------------------------------------------- */

static void heard(link_t *l, int rssi_dbm)
{
    atomic_store(&l->peer_heard_ms, atomic_load(&l->now_ms));
    if (rssi_dbm != 0) {
        atomic_store(&l->rssi_dbm, rssi_dbm);
        atomic_store(&l->has_rssi, true);
    }
}

static void on_hello(link_t *l, const uint8_t *src, const uint8_t *data, int rssi_dbm)
{
    if (!atomic_load(&l->have_peer)) {
        /* The first board heard becomes the peer. */
        atomic_fetch_add(&l->peer_gen, 1u);
        memcpy(l->peer_addr, src, LINK_ADDR_LEN);
        memcpy(l->peer_name, &data[11], LINK_NAME_MAX);
        l->peer_name[LINK_NAME_MAX] = '\0';
        atomic_fetch_add(&l->peer_gen, 1u);
        l->have_expected = false;
        atomic_store(&l->has_rssi, false);
        atomic_store(&l->pst_have, false); /* another board's state is not this one's */
        atomic_store(&l->peer_heard_ms, atomic_load(&l->now_ms));
        atomic_store(&l->have_peer, true);
    } else if (memcmp(src, l->peer_addr, LINK_ADDR_LEN) != 0) {
        return; /* a third board: not ours */
    }
    atomic_store(&l->peer_keyed, (data[2] & FLAG_KEYED) != 0);
    atomic_store(&l->peer_rx_hz, get_u32(&data[3]));
    atomic_store(&l->peer_tx_hz, get_u32(&data[7]));
    heard(l, rssi_dbm);
}

static void on_audio(link_t *l, const uint8_t *src, const uint8_t *data, size_t len, int rssi_dbm)
{
    static const int16_t silence[LINK_FRAME_SAMPLES];
    int16_t samples[LINK_FRAME_SAMPLES];
    size_t n = (len - AUDIO_HDR) / 2;
    uint8_t flags = data[2];
    uint16_t seq = get_u16(&data[3]);

    if (!atomic_load(&l->have_peer) || memcmp(src, l->peer_addr, LINK_ADDR_LEN) != 0) return;
    heard(l, rssi_dbm);

    if (atomic_load(&l->match) && get_u32(&data[5]) != atomic_load(&l->rx_hz)) {
        /* Sent on a frequency this radio is not listening on. */
        atomic_fetch_add(&l->n_off_frequency, 1u);
        atomic_store(&l->off_freq_ms, atomic_load(&l->now_ms));
        atomic_store(&l->off_freq_seen, true);
        l->have_expected = false;
        return;
    }

    int16_t gap = l->have_expected ? (int16_t)(uint16_t)(seq - l->expected) : 0;
    if (gap < 0) {
        /* Behind its turn: a copy, or overtaken on the way. A run of them
         * is a peer that started over, so follow it. */
        atomic_fetch_add(&l->n_late, 1u);
        if (++l->late_run < LATE_RESYNC) return;
        l->have_expected = false;
    }
    l->late_run = 0;

    /* Silence the player has already put out because the buffer was empty.
     * It stands for lost packets, if packets were lost, and otherwise for
     * the start of what arrives now, late. A new transmission owes none. */
    bool continues = l->have_expected && !l->after_end && !(flags & FLAG_START);
    uint32_t debt = atomic_exchange(&l->rx_debt, 0u);
    if (!continues) debt = 0;
    if (continues && gap > 0) {
        atomic_fetch_add(&l->n_lost, (uint32_t)gap);
        /* Silence in place of what was lost keeps the rest in time. */
        if (gap <= LINK_CONCEAL_MAX) {
            uint32_t fill = (uint32_t)gap * LINK_FRAME_SAMPLES;
            uint32_t paid = debt < fill ? debt : fill;
            fill -= paid;
            debt -= paid;
            while (fill) {
                uint32_t k = fill < LINK_FRAME_SAMPLES ? fill : LINK_FRAME_SAMPLES;
                dsp_fifo_write(&l->fifo, silence, k);
                fill -= k;
            }
        }
    }
    atomic_store(&l->rx_ended, false);

    /* Audio that arrives after its time was played as silence is skipped
     * by as much, so that what follows is played in time, as after a lost
     * packet. Played late instead, it would leave the buffer too full, and
     * the clock steering would take that for a fast peer and drop samples
     * until it was back: up to one every millisecond, which a modem hears
     * as the timing and frequency jumping. */
    uint32_t skip = debt < n ? debt : (uint32_t)n;
    debt -= skip;
    if (debt) atomic_fetch_add(&l->rx_debt, debt); /* the next packet pays the rest */
    if (skip) atomic_fetch_add(&l->n_skipped, skip);

    for (size_t i = 0; i < n; i++) samples[i] = (int16_t)get_u16(&data[AUDIO_HDR + 2 * i]);
    dsp_fifo_write(&l->fifo, samples + skip, n - skip);
    atomic_fetch_add(&l->n_received, 1u);
    l->expected = (uint16_t)(seq + 1u);
    l->have_expected = true;
    /* After an end the next packet starts a transmission, even if the one
     * marked as its first is lost: nothing between the two is missing. */
    l->after_end = (flags & FLAG_END) != 0;
    if (l->after_end) atomic_store(&l->rx_ended, true);
}

static void on_state(link_t *l, const uint8_t *src, const uint8_t *data, int rssi_dbm)
{
    if (!atomic_load(&l->have_peer) || memcmp(src, l->peer_addr, LINK_ADDR_LEN) != 0) return;
    heard(l, rssi_dbm);

    atomic_fetch_add(&l->pst_gen, 1u);
    l->pst.boot_id = get_u32(&data[2]);
    l->pst.version = get_u32(&data[6]);
    l->pst.nonce = get_u32(&data[10]);
    l->pst.clock_ms = get_u32(&data[14]);
    l->pst.seed = get_u32(&data[18]);
    l->pst.n = data[22];
    for (unsigned i = 0; i < l->pst.n; i++) {
        l->pst.item[i].id = data[STATE_HDR + 5 * i];
        l->pst.item[i].value = (int32_t)get_u32(&data[STATE_HDR + 5 * i + 1]);
    }
    l->pst_heard_ms = clock_now(l);
    atomic_fetch_add(&l->pst_gen, 1u);
    atomic_store(&l->pst_have, true);
}

void link_receive(link_t *l, const uint8_t *src, const uint8_t *data, size_t len, int rssi_dbm)
{
    if (!atomic_load(&l->enabled) || !src || !data || len < 2 || len > LINK_MAX_PACKET) return;
    if (data[0] != MAGIC || (data[1] >> 4) != VERSION) return;

    switch (data[1] & 0x0F) {
    case TYPE_HELLO:
        if (len == HELLO_LEN) on_hello(l, src, data, rssi_dbm);
        break;
    case TYPE_AUDIO:
        if (len >= AUDIO_HDR && (len - AUDIO_HDR) % 2 == 0 && (len - AUDIO_HDR) / 2 <= LINK_FRAME_SAMPLES) {
            on_audio(l, src, data, len, rssi_dbm);
        }
        break;
    case TYPE_STATE:
        if (len >= STATE_HDR && data[22] <= LINK_STATE_ITEMS && len == STATE_HDR + 5 * (size_t)data[22]) {
            on_state(l, src, data, rssi_dbm);
        }
        break;
    default:
        break;
    }
}

/* ---- receiving: the audio side ----------------------------------------------------- */

/* How far the buffer's fill may sit from its target before single samples
 * are dropped or repeated: wider than the 120 samples a packet moves it by. */
static uint32_t steer_tolerance(uint32_t target)
{
    return target / 2 > 180 ? target / 2 : 180;
}

/* Fill n samples at 48 kHz from 12 kHz samples: the jitter buffer's if
 * from_fifo, else zeros. Returns how many 12 kHz samples the buffer could
 * not supply, which were played as silence. */
static uint32_t play(link_t *l, int16_t *out, size_t n, bool from_fifo, bool steer)
{
    uint32_t missing = 0;
    uint32_t target = atomic_load(&l->target);
    uint32_t tolerance = steer_tolerance(target);

    while (n) {
        if (l->frac_n) {
            *out++ = l->frac[DSP_RS_FACTOR - l->frac_n--];
            n--;
            continue;
        }
        int16_t low[PIECE];
        size_t want = n >= DSP_RS_FACTOR ? n / DSP_RS_FACTOR : 1;
        if (want > PIECE) want = PIECE;
        size_t got = want;
        if (!from_fifo) {
            memset(low, 0, want * sizeof(int16_t));
        } else if (steer) {
            got = dsp_fifo_read_steered(&l->fifo, low, want, target, tolerance);
            steer = false; /* at most one sample per block */
            if (got + 1 == want) got = want; /* one short is a sample played twice, not an empty buffer */
        } else {
            got = dsp_fifo_read(&l->fifo, low, want);
        }
        missing += (uint32_t)(want - got);
        if (n >= DSP_RS_FACTOR) {
            dsp_interp4(&l->up, low, want, out);
            out += want * DSP_RS_FACTOR;
            n -= want * DSP_RS_FACTOR;
        } else {
            dsp_interp4(&l->up, low, 1, l->frac);
            l->frac_n = DSP_RS_FACTOR;
        }
    }
    return missing;
}

bool link_rx(link_t *l, int16_t *out, size_t n)
{
    if (!atomic_load(&l->enabled)) {
        /* Nothing is kept for later: empty the buffer as it fills. */
        int16_t scrap[PIECE];
        while (dsp_fifo_level(&l->fifo)) dsp_fifo_read(&l->fifo, scrap, PIECE);
        l->primed = false;
        l->dry = false;
        l->flush_left = 0;
        l->stall_n = 0;
        atomic_store(&l->st_receiving, false);
        return false;
    }

    uint32_t level = dsp_fifo_level(&l->fifo);
    bool ended = atomic_load(&l->rx_ended);

    if (!l->primed) {
        bool start = false;
        if (level >= atomic_load(&l->target)) {
            start = true;
            l->draining = false;
        } else if (level > 0 && ended) {
            start = true; /* a transmission shorter than the buffer */
            l->draining = true;
        } else if (level > 0) {
            if (level != l->stall_level) {
                l->stall_level = level;
                l->stall_n = 0;
            }
            l->stall_n += (uint32_t)n;
            if (l->stall_n >= STALL_SAMPLES) {
                start = true; /* its end never arrived */
                l->draining = true;
            }
        }
        if (start) {
            l->primed = true;
            l->level_avg_q8 = (int32_t)(level << 8);
            l->dry = false;
            l->dry_n = 0;
            l->stall_n = 0;
            l->stall_level = 0;
            if (!l->flush_left) {
                dsp_interp4_init(&l->up);
                l->frac_n = 0;
            }
            l->flush_left = 0;
        }
    }

    if (l->primed) {
        if (ended) l->draining = true;
        /* The clocks are judged by the fill averaged over about a second:
         * a lost or late packet moves the fill for a moment, and that is
         * not drift. */
        uint32_t target = atomic_load(&l->target);
        /* An empty buffer in mid-transmission says nothing about the
         * clocks either: what arrives late is skipped to match. */
        if (!l->dry) l->level_avg_q8 += ((int32_t)(level << 8) - l->level_avg_q8) / 128;
        int32_t off = (l->level_avg_q8 >> 8) - (int32_t)target;
        /* Clocks a few parts per million apart need a sample now and then,
         * so one at most every LINK_STEER_MS: a modem barely notices that. */
        l->steer_wait = l->steer_wait > n ? l->steer_wait - (uint32_t)n : 0;
        bool steer = !l->draining && !l->steer_wait && (uint32_t)(off < 0 ? -off : off) > steer_tolerance(target);
        uint32_t steered = l->fifo.dropped + l->fifo.repeated;
        uint32_t missing = play(l, out, n, true, steer);
        if (l->fifo.dropped + l->fifo.repeated != steered) l->steer_wait = STEER_GAP_SAMPLES;
        if (!missing) {
            l->dry = false;
            l->dry_n = 0;
        } else if (l->draining || atomic_load(&l->rx_ended)) {
            /* Ran empty at the end of a transmission: that is how it stops. */
            l->primed = false;
            l->flush_left = FLUSH_SAMPLES;
        } else {
            /* Ran empty in the middle: packets are late or lost. Keep the
             * place, so that what comes next is not played late, and note
             * how much silence went out for the receiving side to allow
             * for. If nothing comes for long enough, it is over. */
            if (!l->dry) {
                l->dry = true;
                atomic_fetch_add(&l->n_underruns, 1u);
            }
            atomic_fetch_add(&l->rx_debt, missing);
            l->dry_n += (uint32_t)n;
            if (l->dry_n >= STALL_SAMPLES) {
                l->primed = false;
                l->dry = false;
                atomic_store(&l->rx_debt, 0u);
            }
        }
        atomic_store(&l->st_receiving, true);
        return true;
    }
    if (l->flush_left) {
        uint32_t used = (uint32_t)((n + DSP_RS_FACTOR - 1) / DSP_RS_FACTOR);
        (void)play(l, out, n, false, false);
        l->flush_left = used < l->flush_left ? l->flush_left - used : 0;
        return true;
    }
    atomic_store(&l->st_receiving, false);
    return false;
}

/* ---- status ------------------------------------------------------------------------- */

void link_get_status(link_t *l, link_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->target_ms = atomic_load(&l->target) / (LINK_FS_HZ / 1000u);
    st->buffer_ms = dsp_fifo_level(&l->fifo) / (LINK_FS_HZ / 1000u);
    st->sent = atomic_load(&l->n_sent);
    st->send_failed = atomic_load(&l->n_send_failed);
    st->received = atomic_load(&l->n_received);
    st->lost = atomic_load(&l->n_lost);
    st->late = atomic_load(&l->n_late);
    st->off_frequency = atomic_load(&l->n_off_frequency);
    st->underruns = atomic_load(&l->n_underruns);
    st->skipped = atomic_load(&l->n_skipped);
    /* Each of these belongs to one task; a plain read is whole on the CPUs
     * this runs on, and a count that is one behind does not matter here. */
    st->overruns = l->fifo.overruns;
    st->dropped = l->fifo.dropped;
    st->repeated = l->fifo.repeated;

    if (!atomic_load(&l->enabled)) {
        st->state = LINK_OFF;
        return;
    }
    for (int tries = 0; tries < 4; tries++) {
        uint32_t gen = atomic_load(&l->peer_gen);
        if (gen & 1u) continue;
        memcpy(st->peer_name, l->peer_name, sizeof st->peer_name);
        if (atomic_load(&l->peer_gen) == gen) break;
    }
    st->peer_name[LINK_NAME_MAX] = '\0';
    st->peer = atomic_load(&l->have_peer);
    if (!st->peer) {
        st->peer_name[0] = '\0';
        st->state = LINK_NO_PEER;
        return;
    }
    st->peer_rx_hz = atomic_load(&l->peer_rx_hz);
    st->peer_tx_hz = atomic_load(&l->peer_tx_hz);
    st->peer_keyed = atomic_load(&l->peer_keyed);
    st->has_rssi = atomic_load(&l->has_rssi);
    st->rssi_dbm = (int)atomic_load(&l->rssi_dbm);

    bool off_freq = atomic_load(&l->off_freq_seen) && atomic_load(&l->now_ms) - atomic_load(&l->off_freq_ms) < 500u;
    st->state = atomic_load(&l->st_sending)     ? LINK_SENDING
                : atomic_load(&l->st_receiving) ? LINK_RECEIVING
                : off_freq                      ? LINK_OFF_FREQUENCY
                                                : LINK_IDLE;
}

/* ---- framing for a byte stream -------------------------------------------------------- */

#define SLIP_END 0xC0
#define SLIP_ESC 0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

/* CRC-16/CCITT-FALSE: polynomial 1021, start FFFF. "123456789" gives 29B1. */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static size_t put_escaped(uint8_t *out, uint8_t b)
{
    if (b == SLIP_END) {
        out[0] = SLIP_ESC;
        out[1] = SLIP_ESC_END;
        return 2;
    }
    if (b == SLIP_ESC) {
        out[0] = SLIP_ESC;
        out[1] = SLIP_ESC_ESC;
        return 2;
    }
    out[0] = b;
    return 1;
}

size_t link_wire_encode(const uint8_t *pkt, size_t len, uint8_t *out, size_t cap)
{
    if (len == 0 || len > LINK_MAX_PACKET || cap < 2 * (len + 2) + 2) return 0;
    uint16_t crc = crc16(pkt, len);
    size_t n = 0;
    out[n++] = SLIP_END;
    for (size_t i = 0; i < len; i++) n += put_escaped(&out[n], pkt[i]);
    n += put_escaped(&out[n], (uint8_t)(crc >> 8));
    n += put_escaped(&out[n], (uint8_t)crc);
    out[n++] = SLIP_END;
    return n;
}

void link_wire_init(link_wire_t *w)
{
    memset(w, 0, sizeof *w);
}

void link_wire_feed(link_wire_t *w, const uint8_t *bytes, size_t n, link_wire_fn fn, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];
        if (b == SLIP_END) {
            if (w->len || w->overflow || w->esc) { /* two C0 in a row are just the gap between frames */
                bool good = !w->overflow && !w->esc && w->len > 2 &&
                            crc16(w->buf, w->len - 2) == (uint16_t)((w->buf[w->len - 2] << 8) | w->buf[w->len - 1]);
                if (good) {
                    fn(ctx, w->buf, w->len - 2);
                } else {
                    w->bad++;
                }
            }
            w->len = 0;
            w->esc = false;
            w->overflow = false;
            continue;
        }
        if (w->esc) {
            w->esc = false;
            if (b == SLIP_ESC_END) {
                b = SLIP_END;
            } else if (b == SLIP_ESC_ESC) {
                b = SLIP_ESC;
            } else {
                w->overflow = true; /* not a valid escape: the frame is spoiled */
                continue;
            }
        } else if (b == SLIP_ESC) {
            w->esc = true;
            continue;
        }
        if (w->len < sizeof w->buf) {
            w->buf[w->len++] = b;
        } else {
            w->overflow = true;
        }
    }
}
