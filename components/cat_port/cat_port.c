/* cat_port: see cat_port.h. */
#include "cat_port.h"

#include <string.h>

static void lock(cat_port_t *p)
{
    if (p->lock) p->lock(p->ctx);
}

static void unlock(cat_port_t *p)
{
    if (p->unlock) p->unlock(p->ctx);
}

void cat_port_init(cat_port_t *p, void (*lock_fn)(void *ctx), void (*unlock_fn)(void *ctx), void *ctx)
{
    memset(p, 0, sizeof *p);
    p->lock = lock_fn;
    p->unlock = unlock_fn;
    p->ctx = ctx;
}

bool cat_port_put(cat_port_t *p, const uint8_t *bytes, size_t len, uint32_t now_ms)
{
    if (!bytes || len == 0) return false;
    if (len > CAT_PORT_FRAME_MAX) {
        atomic_fetch_add(&p->n_too_long, 1u);
        return false;
    }
    lock(p);
    if (p->count == CAT_PORT_SLOTS) {
        /* The host is not reading: the newest replies are the useful ones. */
        p->head = (p->head + 1) % CAT_PORT_SLOTS;
        p->count--;
        atomic_fetch_add(&p->n_full, 1u);
    }
    uint32_t at = (p->head + p->count) % CAT_PORT_SLOTS;
    p->slot[at].at_ms = now_ms;
    p->slot[at].len = (uint8_t)len;
    memcpy(p->slot[at].bytes, bytes, len);
    p->count++;
    unlock(p);
    atomic_fetch_add(&p->n_queued, 1u);
    return true;
}

size_t cat_port_take(cat_port_t *p, uint8_t *out, size_t cap, uint32_t now_ms)
{
    size_t used = 0;
    lock(p);
    while (p->count && now_ms - p->slot[p->head].at_ms >= CAT_PORT_STALE_MS) {
        p->head = (p->head + 1) % CAT_PORT_SLOTS;
        p->count--;
        atomic_fetch_add(&p->n_stale, 1u);
    }
    while (p->count && out && used + p->slot[p->head].len <= cap) {
        memcpy(out + used, p->slot[p->head].bytes, p->slot[p->head].len);
        used += p->slot[p->head].len;
        p->head = (p->head + 1) % CAT_PORT_SLOTS;
        p->count--;
        atomic_fetch_add(&p->n_sent, 1u);
    }
    unlock(p);
    return used;
}

void cat_port_discard(cat_port_t *p)
{
    lock(p);
    atomic_fetch_add(&p->n_discarded, p->count);
    p->head = 0;
    p->count = 0;
    unlock(p);
}

uint32_t cat_port_waiting(cat_port_t *p)
{
    lock(p);
    uint32_t n = p->count;
    unlock(p);
    return n;
}

bool cat_port_watch(cat_port_t *p, cat_port_watch_t *w, bool busy, bool hw_armed, uint32_t now_ms)
{
    if (!busy || hw_armed) {
        /* Idle, or a transfer the host has yet to read: nothing lost. */
        w->armed = false;
        return false;
    }
    if (!w->armed) {
        w->armed = true;
        w->since_ms = now_ms;
        return false;
    }
    if (now_ms - w->since_ms < CAT_PORT_LOST_MS) return false;
    w->armed = false; /* once; again after as long if it did not help */
    atomic_fetch_add(&p->n_recovered, 1u);
    return true;
}

void cat_port_get_status(cat_port_t *p, cat_port_status_t *st)
{
    st->waiting = cat_port_waiting(p);
    st->queued = atomic_load(&p->n_queued);
    st->sent = atomic_load(&p->n_sent);
    st->stale = atomic_load(&p->n_stale);
    st->full = atomic_load(&p->n_full);
    st->discarded = atomic_load(&p->n_discarded);
    st->too_long = atomic_load(&p->n_too_long);
    st->recovered = atomic_load(&p->n_recovered);
}
