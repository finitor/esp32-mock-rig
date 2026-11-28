/* coord: see coord.h. */
#include "coord.h"

#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* The settings that are the path's, by mock_ctl's command numbers. */
static const uint8_t shared[] = {
    MOCK_CTL_NOISE, MOCK_CTL_SNR,     MOCK_CTL_REF_BW,     MOCK_CTL_FLOOR,
    MOCK_CTL_ATTEN, MOCK_CTL_FADE,    MOCK_CTL_DOPPLER,    MOCK_CTL_FADE_DELAY,
    MOCK_CTL_LINK_MATCH, /* strict frequencies: the two ends of one path follow one rule */
};

_Static_assert(sizeof shared <= LINK_STATE_ITEMS, "the shared settings must fit a state packet");

static bool is_shared(uint8_t cmd)
{
    for (size_t i = 0; i < ARRAY_LEN(shared); i++) {
        if (shared[i] == cmd) return true;
    }
    return false;
}

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

void coord_init(coord_t *c, mock_ctl_t *ctl, link_t *link, uint32_t random)
{
    memset(c, 0, sizeof *c);
    c->ctl = ctl;
    c->link = link;
    c->boot_id = mix(random) | 1u; /* never 0 */
    c->own_seed = mix(random ^ 0xA5A5A5A5u);
    c->nonce = c->boot_id;
    atomic_store(&c->seed, c->own_seed);
    atomic_store(&c->leads, true);
}

static void read_shared(const ae_config_t *cfg, int32_t *values)
{
    for (size_t i = 0; i < ARRAY_LEN(shared); i++) values[i] = mock_ctl_setting_get(cfg, shared[i]);
}

/* The pair is on its own again: this board's own fading, from where its
 * clock stands, so that nothing jumps. */
static void alone(coord_t *c)
{
    atomic_store(&c->sharing, false);
    atomic_store(&c->leads, true);
    atomic_store(&c->seed, c->own_seed);
}

void coord_tick(coord_t *c, uint32_t now_ms)
{
    ae_config_t cfg;
    int32_t now_values[LINK_STATE_ITEMS];
    link_shared_t mine, theirs;
    uint32_t heard_ms;

    /* A change made on this board, by either control path, is a new version. */
    ae_get_config(c->ctl->engine, &cfg);
    read_shared(&cfg, now_values);
    if (!c->have_last || memcmp(now_values, c->last, sizeof(int32_t) * ARRAY_LEN(shared)) != 0) {
        if (c->have_last) {
            c->version++;
            c->nonce = c->boot_id;
        }
        memcpy(c->last, now_values, sizeof(int32_t) * ARRAY_LEN(shared));
        c->have_last = true;
    }
    atomic_store(&c->st_version, c->version);

    if (cfg.route != AE_ROUTE_LINK || !cfg.link_coord) {
        link_set_shared(c->link, NULL, 0);
        alone(c);
        return;
    }

    memset(&mine, 0, sizeof mine);
    mine.boot_id = c->boot_id;
    mine.version = c->version;
    mine.nonce = c->nonce;
    mine.seed = atomic_load(&c->seed);
    mine.n = (uint8_t)ARRAY_LEN(shared);
    for (size_t i = 0; i < ARRAY_LEN(shared); i++) {
        mine.item[i].id = shared[i];
        mine.item[i].value = c->last[i];
    }
    link_set_shared(c->link, &mine, atomic_load(&c->offset_ms));

    if (!link_peer_shared(c->link, &theirs, &heard_ms) || now_ms - heard_ms > COORD_STALE_MS) {
        alone(c);
        return;
    }
    atomic_store(&c->sharing, true);

    /* Settings: the newer set is the pair's. */
    if (theirs.version > c->version || (theirs.version == c->version && theirs.nonce > c->nonce)) {
        for (unsigned i = 0; i < theirs.n && i < LINK_STATE_ITEMS; i++) {
            /* Only the path's settings are taken from a peer, and each
             * through the range check: one this board cannot use stays. */
            if (is_shared(theirs.item[i].id)) {
                (void)mock_ctl_setting_put(c->ctl, &cfg, theirs.item[i].id, theirs.item[i].value);
            }
        }
        ae_configure(c->ctl->engine, &cfg);
        c->version = theirs.version;
        c->nonce = theirs.nonce;
        ae_get_config(c->ctl->engine, &cfg);
        read_shared(&cfg, c->last); /* as taken: not a change of this board's own */
        atomic_store(&c->st_version, c->version);
    }

    /* Fading: the board with the higher start-up number leads. */
    if (theirs.boot_id > c->boot_id) {
        int32_t offset = atomic_load(&c->offset_ms);
        int32_t want = (int32_t)(theirs.clock_ms - heard_ms); /* their shared clock, less this board's own */
        int32_t err = (int32_t)((uint32_t)want - (uint32_t)offset);
        if (err > COORD_JUMP_MS || err < -COORD_JUMP_MS) {
            offset = want;
        } else if (err > 0) {
            offset++;
        } else if (err < 0) {
            offset--;
        }
        atomic_store(&c->offset_ms, offset);
        atomic_store(&c->seed, theirs.seed);
        atomic_store(&c->leads, false);
    } else {
        atomic_store(&c->seed, c->own_seed);
        atomic_store(&c->leads, true);
    }
}

void coord_fade_clock(coord_t *c, uint32_t local_ms, uint32_t *seed, uint32_t *clock_ms)
{
    *seed = atomic_load(&c->seed);
    *clock_ms = local_ms + (uint32_t)atomic_load(&c->offset_ms);
}

void coord_get_status(coord_t *c, coord_status_t *st)
{
    st->sharing = atomic_load(&c->sharing);
    st->leads = atomic_load(&c->leads);
    st->offset_ms = atomic_load(&c->offset_ms);
    st->version = atomic_load(&c->st_version);
}
