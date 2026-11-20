/*
 * link: the path between two mock radios.
 *
 * Portable C with no ESP-IDF dependencies, so it is tested on the host. What
 * one board transmits becomes what the other receives: half duplex, keyed by
 * PTT, like two radios on one frequency. Audio travels at 12 kHz, 16-bit
 * mono, in packets of 10 ms. How packets travel is the caller's: it gives a
 * send function and calls link_receive() with what arrives (ESP-NOW or a
 * UART on the board, a function call in the host tests).
 *
 *   sending    While the radio is keyed, and for LINK_TAIL_MS after, transmit
 *              audio is resampled to 12 kHz and sent 120 samples at a time,
 *              each packet with a sequence number and the transmit frequency.
 *              The last packet of a transmission says so.
 *   receiving  Packets go into a jitter buffer. Playing starts when it holds
 *              the configured time (30 ms by default) and stops when the
 *              transmission ends. A lost packet is replaced by silence of
 *              the same length, so what follows is not shifted in time; if
 *              the buffer runs empty first, the silence played meanwhile
 *              counts towards it. The two
 *              boards' sample clocks differ by a few parts per million; the
 *              buffer absorbs that over a transmission, and if its fill ever
 *              drifts far from the target one sample is dropped or repeated.
 *   frequency  A packet is only heard if it was sent on the frequency this
 *              radio is listening on, unless matching is switched off.
 *   peer       Boards announce themselves four times a second. A board
 *              links to the first one it hears and stays with it until it
 *              has been silent for LINK_PEER_TIMEOUT_MS. Audio goes to the
 *              peer only and is taken from the peer only.
 *
 * Threads. Three may be involved, and each function belongs to one:
 *   the audio task       link_tx(), link_rx()
 *   the transport's task link_receive(), link_send_failed()
 *   a slow periodic task link_tick()
 * link_configure(), link_set_radio() and link_get_status() may be called
 * from any task. The send function is called from the audio task (audio)
 * and from the periodic task (announcements), so it must cope with both.
 */
#ifndef LINK_H
#define LINK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_FS_HZ 12000
#define LINK_FRAME_SAMPLES 120 /* 10 ms */
#define LINK_ADDR_LEN 6
#define LINK_NAME_MAX 16
#define LINK_MAX_PACKET 250 /* what one ESP-NOW frame carries */

#define LINK_BUFFER_MIN_MS 10u
#define LINK_BUFFER_MAX_MS 200u
#define LINK_BUFFER_DEFAULT_MS 30u

/* Transmit audio still on its way through the host and the USB buffers when
 * PTT drops is sent for this long after. */
#define LINK_TAIL_MS 50u

#define LINK_HELLO_MS 250u
#define LINK_PEER_TIMEOUT_MS 3000u

/* Audio left in the jitter buffer below the starting level is played anyway
 * once nothing has arrived for this long: the end of a transmission whose
 * last packet was lost. */
#define LINK_STALL_MS 100u

/* The most lost packets replaced by silence. A longer gap is taken as a new
 * start. */
#define LINK_CONCEAL_MAX 10

#define LINK_FIFO_SAMPLES 8192u /* the jitter buffer's storage, at 12 kHz */

/* Send one packet: to the board at dst, or to every board in range if dst is
 * NULL. Returns false if it could not be sent. */
typedef bool (*link_send_fn)(void *ctx, const uint8_t *dst, const uint8_t *data, size_t len);

typedef enum {
    LINK_OFF = 0,       /* not enabled */
    LINK_NO_PEER,       /* enabled, no other board heard */
    LINK_IDLE,          /* linked, nobody transmitting */
    LINK_SENDING,       /* sending transmit audio to the peer */
    LINK_RECEIVING,     /* playing the peer's audio */
    LINK_OFF_FREQUENCY, /* the peer is transmitting on another frequency: not heard */
} link_state_t;

typedef struct {
    link_state_t state;
    bool peer;
    char peer_name[LINK_NAME_MAX + 1];
    uint32_t peer_rx_hz, peer_tx_hz;
    bool peer_keyed;
    bool has_rssi; /* the transport reports signal strength */
    int rssi_dbm;

    uint32_t buffer_ms; /* audio waiting in the jitter buffer now */
    uint32_t target_ms;

    uint32_t sent, send_failed;  /* audio packets */
    uint32_t received;           /* audio packets taken */
    uint32_t lost;               /* gaps in the sequence */
    uint32_t late;               /* arrived after their turn, or twice: dropped */
    uint32_t off_frequency;      /* dropped for the frequency */
    uint32_t underruns;          /* times the buffer ran empty mid-transmission */
    uint32_t overruns;           /* samples that did not fit in the buffer */
    uint32_t dropped, repeated;  /* single samples, to follow the peer's clock */
} link_status_t;

typedef struct {
    link_send_fn send;
    void *ctx;
    char name[LINK_NAME_MAX];

    /* settings and the radio's state: any task */
    _Atomic bool enabled;
    _Atomic bool match;
    _Atomic uint32_t target; /* samples at 12 kHz */
    _Atomic uint32_t rx_hz, tx_hz;
    _Atomic uint32_t now_ms;

    /* the peer: written by the transport's task; peer_gen is odd while the
     * address and name are being written */
    _Atomic uint32_t peer_gen;
    uint8_t peer_addr[LINK_ADDR_LEN];
    char peer_name[LINK_NAME_MAX + 1];
    _Atomic bool have_peer;
    _Atomic uint32_t peer_heard_ms;
    _Atomic uint32_t peer_rx_hz, peer_tx_hz;
    _Atomic bool peer_keyed;
    _Atomic bool has_rssi;
    _Atomic int32_t rssi_dbm;

    /* receiving: the transport's task writes the buffer, the audio task reads it */
    int16_t fifo_store[LINK_FIFO_SAMPLES];
    dsp_fifo_t fifo;
    _Atomic bool rx_ended;        /* the last packet of a transmission is in the buffer */
    _Atomic uint32_t off_freq_ms; /* when a packet was last dropped for its frequency */
    _Atomic bool off_freq_seen;
    bool have_expected; /* the transport's task */
    bool after_end;
    uint16_t expected;
    uint32_t late_run;

    _Atomic uint32_t rx_debt; /* silence played because the buffer was empty, in 12 kHz samples */

    bool primed; /* the audio task */
    bool draining;
    int32_t level_avg_q8; /* the buffer's fill, averaged, times 256 */
    bool dry; /* empty in mid-transmission */
    uint32_t dry_n;
    uint32_t stall_level, stall_n;
    uint32_t flush_left;
    dsp_interp4_t up;
    int16_t frac[DSP_RS_FACTOR];
    uint32_t frac_n;
    _Atomic bool st_receiving;

    /* sending: the audio task */
    bool tx_active, tx_tail, tx_start;
    bool tx_heard; /* packets of this transmission have gone to the peer */
    uint32_t tx_tail_left; /* samples at 48 kHz */
    dsp_decim4_t down;
    int16_t frame[LINK_FRAME_SAMPLES];
    uint32_t fill;
    uint16_t seq;
    _Atomic bool st_sending;

    uint32_t last_hello_ms; /* the periodic task */
    bool hello_sent;

    /* counters: each written by one task */
    _Atomic uint32_t n_sent, n_send_failed, n_received, n_lost, n_late, n_off_frequency, n_underruns;
} link_t;

/* name is the board's own, shown on the other board; send must not be NULL.
 * Starts disabled, with the default buffer and frequency matching on. */
void link_init(link_t *l, const char *name, link_send_fn send, void *ctx);

/* enabled: the radio is in the link route. Disabled, it announces nothing,
 * ignores what arrives and forgets its peer. buffer_ms is clamped. */
void link_configure(link_t *l, bool enabled, uint32_t buffer_ms, bool match);

/* The frequencies the radio is listening and transmitting on now. */
void link_set_radio(link_t *l, uint32_t rx_hz, uint32_t tx_hz);

/* Transmit audio: n samples at 48 kHz (silence if tx is NULL), in every block
 * while the link is in use, keyed or not. n == 0 ends a transmission at
 * once, for when the radio leaves the link route mid-way. */
void link_tx(link_t *l, const int16_t *tx, size_t n, bool keyed);

/* Receive audio: fills n samples at 48 kHz and returns true, or returns
 * false and leaves out alone when there is nothing to hear. */
bool link_rx(link_t *l, int16_t *out, size_t n);

/* Call every 10 to 50 ms with a millisecond clock: announces the board and
 * drops a peer that has gone quiet. */
void link_tick(link_t *l, uint32_t now_ms);

/* A packet arrived from the board at src. rssi_dbm is 0 if the transport
 * does not measure it. Anything malformed is ignored. */
void link_receive(link_t *l, const uint8_t *src, const uint8_t *data, size_t len, int rssi_dbm);

/* For a transport that learns later that a packet it accepted was not
 * delivered. */
void link_send_failed(link_t *l);

void link_get_status(link_t *l, link_status_t *st);

/* ---- framing for a byte stream (the UART transport) ------------------------
 *
 * A packet on a wire needs a start, an end and a check: C0, the packet and a
 * CRC-16 with C0 and DB escaped (SLIP), C0. A frame that fails the check is
 * dropped, and the decoder finds the next C0 again after any garbage.
 */
#define LINK_WIRE_MAX (2 * (LINK_MAX_PACKET + 2) + 2)

/* Returns the frame's length, or 0 if it does not fit in cap or the packet
 * is longer than LINK_MAX_PACKET. LINK_WIRE_MAX always fits. */
size_t link_wire_encode(const uint8_t *pkt, size_t len, uint8_t *out, size_t cap);

typedef struct {
    uint8_t buf[LINK_MAX_PACKET + 2];
    size_t len;
    bool esc;
    bool overflow;
    uint32_t bad; /* frames dropped: failed the check, too long or too short */
} link_wire_t;

typedef void (*link_wire_fn)(void *ctx, const uint8_t *pkt, size_t len);

void link_wire_init(link_wire_t *w);

/* Feed received bytes; fn is called once for each good packet. */
void link_wire_feed(link_wire_t *w, const uint8_t *bytes, size_t n, link_wire_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LINK_H */
