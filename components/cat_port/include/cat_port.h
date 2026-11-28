/*
 * cat_port: the CAT serial port's replies on their way to the host.
 *
 * Portable C with no ESP-IDF dependencies, so it is tested on the host. The
 * CI-V parser's replies wait here until the USB serial port's IN endpoint is
 * free, and go out from here a packet at a time, so nothing waits in
 * TinyUSB's own buffer, where it could not be dropped:
 *
 *   stale    A reply that has waited CAT_PORT_STALE_MS answers a request the
 *            host has given up on (Hamlib's Icom timeout is a second). It is
 *            dropped, not sent: a host that stops reading for a while and
 *            comes back gets the replies to what it asks next, not a backlog.
 *   full     With CAT_PORT_SLOTS replies waiting the oldest goes, so the
 *            newest are always the ones kept. Nothing waits for room: the
 *            parser, and a PTT command behind a stream of reads, is never held
 *            up by a host that is not reading.
 *   open     When the host opens the port afresh, whatever waits is for the
 *            last program that had it, and is dropped (cat_port_discard).
 *   stuck    cat_port_watch() notices an IN endpoint that will never send
 *            again by itself, which leaves the port taking CAT and answering
 *            nothing until a replug. Two ways are known:
 *              lost     the USB stack thinks it busy while the hardware has
 *                       finished with it: the completion was lost on the way
 *                       to the USB task. The caller hands the stack the
 *                       completion it missed.
 *              starved  the transfer is armed in the hardware, but its data
 *                       never went into the endpoint's FIFO and the FIFO's
 *                       interrupt, which would put it there, is masked: the
 *                       host is answered NAK for ever. (TinyUSB's DWC2 driver
 *                       sets and clears that mask with unlocked
 *                       read-modify-writes from tasks and the interrupt; on
 *                       two cores one can undo the other.) The caller
 *                       unmasks it.
 *
 * Threads. cat_port_put() belongs to the task that runs the parser,
 * cat_port_take() and cat_port_discard() to the USB task, cat_port_watch()
 * to one periodic task; cat_port_get_status() may be called from any. The
 * caller gives a lock that puts and takes are made under: each holds it for
 * one copy of a reply at most.
 */
#ifndef CAT_PORT_H
#define CAT_PORT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAT_PORT_SLOTS 16       /* replies waiting at most */
#define CAT_PORT_FRAME_MAX 64   /* the longest reply: CIV_MAX_FRAME, and one USB packet */
#define CAT_PORT_STALE_MS 1000u /* a reply this old is dropped */
#define CAT_PORT_STUCK_MS 1000u /* stuck this long, in the same way, before anything is done */

typedef struct {
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;

    struct {
        uint32_t at_ms;
        uint8_t len;
        uint8_t bytes[CAT_PORT_FRAME_MAX];
    } slot[CAT_PORT_SLOTS];
    uint32_t head, count; /* under the lock */

    _Atomic uint32_t n_queued;    /* replies taken in */
    _Atomic uint32_t n_sent;      /* replies handed to USB */
    _Atomic uint32_t n_stale;     /* dropped for their age */
    _Atomic uint32_t n_full;      /* dropped, the oldest, to make room */
    _Atomic uint32_t n_discarded; /* dropped when the port was opened afresh */
    _Atomic uint32_t n_too_long;  /* refused: longer than CAT_PORT_FRAME_MAX */
    _Atomic uint32_t n_recovered; /* lost completions noticed */
    _Atomic uint32_t n_rearmed;   /* starved transfers noticed */
} cat_port_t;

typedef struct {
    uint32_t waiting;
    uint32_t queued, sent, stale, full, discarded, too_long, recovered, rearmed;
} cat_port_status_t;

/* lock and unlock may be NULL where only one task uses the port. */
void cat_port_init(cat_port_t *p, void (*lock)(void *ctx), void (*unlock)(void *ctx), void *ctx);

/* A reply from the parser, whole. Never waits. False if it was refused
 * (empty or too long); a full port drops its oldest instead. */
bool cat_port_put(cat_port_t *p, const uint8_t *bytes, size_t len, uint32_t now_ms);

/* Drops what has gone stale, then copies out whole replies, oldest first,
 * as many as fit in cap bytes (CAT_PORT_FRAME_MAX always holds the first).
 * Returns the bytes copied; 0 if nothing is waiting. With cap 0 it only
 * drops what has gone stale. */
size_t cat_port_take(cat_port_t *p, uint8_t *out, size_t cap, uint32_t now_ms);

/* Drop everything waiting: the port has been opened afresh. */
void cat_port_discard(cat_port_t *p);

uint32_t cat_port_waiting(cat_port_t *p);

/* ---- the IN endpoint -------------------------------------------------------- */

/* The endpoint as the USB stack and the hardware see it. */
typedef struct {
    bool busy;      /* the stack waits for a transfer to complete */
    bool armed;     /* the hardware has a transfer enabled (DIEPCTL.EPENA) */
    uint32_t unsent; /* bytes of it not yet written into the FIFO (DIEPTSIZ.XferSize) */
    bool fifo_irq;  /* the FIFO-empty interrupt that writes them is unmasked (DIEPEMPMSK) */
} cat_port_in_t;

typedef enum {
    CAT_PORT_IN_OK = 0,   /* idle, sending, or waiting for a host that is not reading */
    CAT_PORT_IN_LOST,     /* hand the stack the completion it missed */
    CAT_PORT_IN_STARVED,  /* unmask the FIFO-empty interrupt */
} cat_port_fault_t;

typedef struct {
    cat_port_fault_t seen;
    uint32_t since_ms;
} cat_port_watch_t;

/* Call now and then (every 20 ms, say) with the endpoint's state. Returns a
 * fault, once, when it has been there CAT_PORT_STUCK_MS on end, and again
 * after as long if the remedy did not help. A host that is not reading
 * leaves its transfer armed with everything written (unsent 0), and is
 * never taken for a fault. */
cat_port_fault_t cat_port_watch(cat_port_t *p, cat_port_watch_t *w, const cat_port_in_t *in, uint32_t now_ms);

void cat_port_get_status(cat_port_t *p, cat_port_status_t *st);

#ifdef __cplusplus
}
#endif

#endif /* CAT_PORT_H */
