/*
 * coord: two linked boards agreeing on one channel.
 *
 * Portable C with no ESP-IDF dependencies, so it is tested on the host. Each
 * board applies the channel to what it receives. For the two directions of a
 * link to be one path, as a real one is, the boards must agree on what the
 * path is. With the "shared channel" setting on, in the link route, they do:
 *
 *   settings   Noise on or off, SNR, its bandwidth, the noise floor, the
 *              attenuation and the fading are the pair's, not a board's. A
 *              change on either board reaches the other within half a
 *              second. Each board's set carries a version that goes up with
 *              every change made on it; the higher version wins, and when
 *              two boards meet with the same one, one of them is picked
 *              (by a number drawn at start-up). The other settings stay
 *              each board's own, the frequency offset among them.
 *   fading     Both boards fade from one seed and one clock, so the path
 *              fades alike both ways. The board with the higher start-up
 *              number leads: the other takes its seed and steers its clock
 *              to it, a millisecond per tick, or at once if it is far out.
 *
 * It rides on the link's state packets, sent to the peer four times a
 * second, and changes settings through mock_ctl, with its range checks: a
 * value this board cannot use is passed over, and that setting stays as it
 * was. Each board still makes its own noise: a receiver hears its noise
 * floor whether or not anyone is transmitting.
 *
 * Threads: coord_tick() belongs with link_tick(), in the periodic task, and
 * changes settings as mock_ctl's commands do; the caller keeps it from
 * running at the same time as they. coord_fade_clock() and
 * coord_get_status() may be called from any task.
 */
#ifndef COORD_H
#define COORD_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "link.h"
#include "mock_ctl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A peer's state older than this is not acted on. */
#define COORD_STALE_MS 2000u

/* The follower's clock is set at once if it is further out than this, and
 * otherwise moved a millisecond per tick. */
#define COORD_JUMP_MS 200

typedef struct {
    bool sharing;      /* a peer is sharing its channel with this board */
    bool leads;        /* the pair fades by this board's clock and seed */
    int32_t offset_ms; /* the shared clock, less this board's own */
    uint32_t version;  /* of the shared settings */
} coord_status_t;

typedef struct {
    mock_ctl_t *ctl;
    link_t *link;
    uint32_t boot_id;  /* this board, this time it started */
    uint32_t own_seed; /* its own fading */

    /* the periodic task */
    uint32_t version, nonce;
    bool have_last;
    uint8_t n_last;
    int32_t last[LINK_STATE_ITEMS]; /* the shared settings as last seen, to notice a change */

    /* read by any task */
    _Atomic int32_t offset_ms;
    _Atomic uint32_t seed;
    _Atomic bool sharing;
    _Atomic bool leads;
    _Atomic uint32_t st_version;
} coord_t;

/* random: any number that differs between boards and between starts. */
void coord_init(coord_t *c, mock_ctl_t *ctl, link_t *link, uint32_t random);

/* Every 10 to 50 ms, before link_tick(), with the same millisecond clock. */
void coord_tick(coord_t *c, uint32_t now_ms);

/* What to give ae_fade_clock(): the fading's seed, and the shared clock at
 * local_ms of this board's own. */
void coord_fade_clock(coord_t *c, uint32_t local_ms, uint32_t *seed, uint32_t *clock_ms);

void coord_get_status(coord_t *c, coord_status_t *st);

#ifdef __cplusplus
}
#endif

#endif /* COORD_H */
