/*
 * Unit tests for cat_civ with the IC-705 personality.
 *
 * The request/reply pairs marked "rigctl" were recorded from Hamlib 4.7.1
 * (rigctl -m 3085) talking to this parser through host/civ_pty; Hamlib
 * accepted every reply. They are kept here as regression fixtures.
 */
#include <stdio.h>
#include <string.h>

#include "cat_civ.h"
#include "rig_ic705.h"

static uint8_t g_out[512];
static size_t g_out_len;
static int g_ptt_events;
static bool g_ptt_last;
static int g_failed;
static int g_checks;

static void tx(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (g_out_len + n <= sizeof g_out) {
        memcpy(&g_out[g_out_len], b, n);
        g_out_len += n;
    }
}

static void on_ptt(void *ctx, bool on)
{
    (void)ctx;
    g_ptt_events++;
    g_ptt_last = on;
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* "fe fe a4 e0 03 fd" -> bytes */
static size_t parse_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (*s) {
        int hi = hexval(s[0]);
        if (hi < 0) {
            s++;
            continue;
        }
        int lo = hexval(s[1]);
        if (lo < 0 || n >= cap) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

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

/* Send a frame, expect exactly this reply ("" for none). */
static void expect_at(civ_t *c, const char *send, const char *want, const char *file, int line)
{
    uint8_t in[128], exp[128];
    size_t in_len = parse_hex(send, in, sizeof in);
    size_t exp_len = parse_hex(want, exp, sizeof exp);
    g_out_len = 0;
    civ_feed(c, in, in_len);
    g_checks++;
    if (g_out_len != exp_len || memcmp(g_out, exp, exp_len) != 0) {
        fail(file, line, send);
        printf("     want: %s\n     got: ", want);
        for (size_t i = 0; i < g_out_len; i++) printf(" %02x", g_out[i]);
        printf("\n");
    }
}
#define EXPECT(c, send, want) expect_at((c), (send), (want), __FILE__, __LINE__)

static void fresh(civ_t *c)
{
    civ_init(c, &rig_ic705, tx, on_ptt, NULL);
    g_ptt_events = 0;
    g_ptt_last = false;
}

static void test_bcd(void)
{
    uint8_t b[5];
    uint64_t v;
    uint16_t w;
    civ_u64_to_bcd_le(14074000, b, 5);
    CHECK(memcmp(b, "\x00\x40\x07\x14\x00", 5) == 0);
    CHECK(civ_bcd_le_to_u64(b, 5, &v) && v == 14074000);
    civ_u64_to_bcd_le(433920000, b, 5);
    CHECK(memcmp(b, "\x00\x00\x92\x33\x04", 5) == 0);
    CHECK(!civ_bcd_le_to_u64((const uint8_t *)"\x0A\x00\x00\x00\x00", 5, &v)); /* not BCD */
    civ_u16_to_bcd_be(255, b, 2);
    CHECK(b[0] == 0x02 && b[1] == 0x55);
    CHECK(civ_bcd_be_to_u16(b, 2, &w) && w == 255);
}

/* The exchange Hamlib 4.7.1 performs on rig_open and for f / m / s / t. */
static void test_rigctl_open_and_read(void)
{
    civ_t c;
    fresh(&c);
    EXPECT(&c, "fe fe a4 e0 03 fd", "fe fe e0 a4 03 00 40 07 14 00 fd");             /* rigctl */
    EXPECT(&c, "fe fe a4 e0 25 00 fd", "fe fe e0 a4 25 00 00 40 07 14 00 fd");       /* rigctl */
    EXPECT(&c, "fe fe a4 e0 14 0c fd", "fe fe e0 a4 14 0c 01 30 fd");                /* rigctl */
    EXPECT(&c, "fe fe a4 e0 26 00 fd", "fe fe e0 a4 26 00 01 01 01 fd");             /* rigctl */
    EXPECT(&c, "fe fe a4 e0 1a 03 fd", "fe fe e0 a4 1a 03 34 fd");
    EXPECT(&c, "fe fe a4 e0 0f fd", "fe fe e0 a4 0f 00 fd");                         /* rigctl */
    EXPECT(&c, "fe fe a4 e0 1c 00 fd", "fe fe e0 a4 1c 00 00 fd");                   /* rigctl */
    CHECK(c.frames_ng == 0 && c.frames_dropped == 0);
}

/* F 7074000, M PKTUSB, M LSB, T 1, T 0, V VFOA, S 1 as rigctl sends them. */
static void test_rigctl_set(void)
{
    civ_t c;
    fresh(&c);
    EXPECT(&c, "fe fe a4 e0 25 00 00 40 07 07 00 fd", "fe fe e0 a4 fb fd"); /* rigctl */
    CHECK(civ_rx_freq(&c) == 7074000);
    EXPECT(&c, "fe fe a4 e0 26 00 01 01 01 fd", "fe fe e0 a4 fb fd");       /* rigctl */
    CHECK(civ_selected_vfo(&c)->mode == 0x01 && civ_selected_vfo(&c)->data);
    EXPECT(&c, "fe fe a4 e0 26 00 00 00 01 fd", "fe fe e0 a4 fb fd");       /* rigctl */
    CHECK(civ_selected_vfo(&c)->mode == 0x00 && !civ_selected_vfo(&c)->data);

    EXPECT(&c, "fe fe a4 e0 1c 00 01 fd", "fe fe e0 a4 fb fd");             /* rigctl */
    CHECK(civ_ptt(&c) && g_ptt_events == 1 && g_ptt_last);
    EXPECT(&c, "fe fe a4 e0 1c 00 fd", "fe fe e0 a4 1c 00 01 fd");
    EXPECT(&c, "fe fe a4 e0 1c 00 01 fd", "fe fe e0 a4 fb fd");             /* repeat: no second event */
    CHECK(g_ptt_events == 1);
    EXPECT(&c, "fe fe a4 e0 1c 00 00 fd", "fe fe e0 a4 fb fd");             /* rigctl */
    CHECK(!civ_ptt(&c) && g_ptt_events == 2 && !g_ptt_last);

    EXPECT(&c, "fe fe a4 e0 07 00 fd", "fe fe e0 a4 fb fd");                /* rigctl */
    EXPECT(&c, "fe fe a4 e0 0f 01 fd", "fe fe e0 a4 fb fd");                /* rigctl */
    EXPECT(&c, "fe fe a4 e0 0f fd", "fe fe e0 a4 0f 01 fd");
}

static void test_freq_and_mode(void)
{
    civ_t c;
    fresh(&c);
    EXPECT(&c, "fe fe a4 e0 05 00 00 35 14 00 fd", "fe fe e0 a4 fb fd");
    CHECK(civ_rx_freq(&c) == 14350000);
    EXPECT(&c, "fe fe a4 e0 05 00 00 00 00 03 fd", "fe fe e0 a4 fa fd");    /* 300 MHz: outside coverage */
    EXPECT(&c, "fe fe a4 e0 05 00 00 35 14 fd", "fe fe e0 a4 fa fd");       /* too short */
    EXPECT(&c, "fe fe a4 e0 05 0a 00 35 14 00 fd", "fe fe e0 a4 fa fd");    /* not BCD */
    CHECK(civ_rx_freq(&c) == 14350000);

    EXPECT(&c, "fe fe a4 e0 04 fd", "fe fe e0 a4 04 01 01 fd");
    EXPECT(&c, "fe fe a4 e0 06 03 02 fd", "fe fe e0 a4 fb fd");             /* CW, FIL2 */
    EXPECT(&c, "fe fe a4 e0 04 fd", "fe fe e0 a4 04 03 02 fd");
    EXPECT(&c, "fe fe a4 e0 06 00 fd", "fe fe e0 a4 fb fd");                /* LSB, filter kept */
    EXPECT(&c, "fe fe a4 e0 04 fd", "fe fe e0 a4 04 00 02 fd");
    EXPECT(&c, "fe fe a4 e0 06 09 fd", "fe fe e0 a4 fa fd");                /* no such mode */
    EXPECT(&c, "fe fe a4 e0 06 01 04 fd", "fe fe e0 a4 fa fd");             /* no FIL4 */

    /* data mode */
    EXPECT(&c, "fe fe a4 e0 1a 06 fd", "fe fe e0 a4 1a 06 01 02 fd");
    EXPECT(&c, "fe fe a4 e0 1a 06 00 00 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 1a 06 fd", "fe fe e0 a4 1a 06 00 00 fd");
    EXPECT(&c, "fe fe a4 e0 1a 06 01 03 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 26 00 fd", "fe fe e0 a4 26 00 00 01 03 fd");
}

static void test_vfo_and_split(void)
{
    civ_t c;
    fresh(&c);
    CHECK(civ_rx_freq(&c) == 14074000 && civ_tx_freq(&c) == 14074000);

    /* unselected VFO through 25 01 / 26 01 */
    EXPECT(&c, "fe fe a4 e0 25 01 fd", "fe fe e0 a4 25 01 00 40 07 07 00 fd");
    EXPECT(&c, "fe fe a4 e0 25 01 00 50 07 07 00 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 26 01 00 01 02 fd", "fe fe e0 a4 fb fd");
    CHECK(c.vfo[1].freq == 7075000 && c.vfo[1].mode == 0x00 && c.vfo[1].filter == 2);
    CHECK(civ_rx_freq(&c) == 14074000);

    EXPECT(&c, "fe fe a4 e0 0f 01 fd", "fe fe e0 a4 fb fd");
    CHECK(civ_tx_freq(&c) == 7075000 && civ_rx_freq(&c) == 14074000);
    EXPECT(&c, "fe fe a4 e0 0f 00 fd", "fe fe e0 a4 fb fd");

    EXPECT(&c, "fe fe a4 e0 07 01 fd", "fe fe e0 a4 fb fd"); /* select B */
    EXPECT(&c, "fe fe a4 e0 03 fd", "fe fe e0 a4 03 00 50 07 07 00 fd");
    EXPECT(&c, "fe fe a4 e0 07 b0 fd", "fe fe e0 a4 fb fd"); /* exchange */
    EXPECT(&c, "fe fe a4 e0 03 fd", "fe fe e0 a4 03 00 40 07 14 00 fd");
    EXPECT(&c, "fe fe a4 e0 07 a0 fd", "fe fe e0 a4 fb fd"); /* B -> A */
    CHECK(c.vfo[0].freq == 14074000 && c.vfo[1].freq == 14074000);
    EXPECT(&c, "fe fe a4 e0 07 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 07 d0 fd", "fe fe e0 a4 fa fd"); /* no main/sub on this radio */
    EXPECT(&c, "fe fe a4 e0 0f 12 fd", "fe fe e0 a4 fb fd"); /* DUP+ */
    EXPECT(&c, "fe fe a4 e0 0f 02 fd", "fe fe e0 a4 fa fd");
}

static void test_levels_meters_funcs(void)
{
    civ_t c;
    fresh(&c);
    EXPECT(&c, "fe fe a4 e0 14 0a fd", "fe fe e0 a4 14 0a 02 55 fd");
    EXPECT(&c, "fe fe a4 e0 14 0a 01 28 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 14 0a fd", "fe fe e0 a4 14 0a 01 28 fd");
    EXPECT(&c, "fe fe a4 e0 14 0a 02 56 fd", "fe fe e0 a4 fa fd"); /* above 255 */
    EXPECT(&c, "fe fe a4 e0 14 7f fd", "fe fe e0 a4 fa fd");       /* no such level */

    EXPECT(&c, "fe fe a4 e0 15 02 fd", "fe fe e0 a4 15 02 00 12 fd");
    CHECK(civ_set_meter(&c, 0x02, 120));
    EXPECT(&c, "fe fe a4 e0 15 02 fd", "fe fe e0 a4 15 02 01 20 fd");
    EXPECT(&c, "fe fe a4 e0 15 01 fd", "fe fe e0 a4 15 01 00 fd"); /* status: one byte */
    CHECK(!civ_set_meter(&c, 0x7F, 1));

    EXPECT(&c, "fe fe a4 e0 16 22 fd", "fe fe e0 a4 16 22 00 fd");
    EXPECT(&c, "fe fe a4 e0 16 22 01 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 16 22 fd", "fe fe e0 a4 16 22 01 fd");
}

static void test_menu_settings(void)
{
    civ_t c;
    fresh(&c);
    /* 1A 05 01 19, DATA MOD: FT8CN and FT8AF set it to USB at every PTT on. */
    EXPECT(&c, "fe fe a4 e0 1a 05 01 19 fd", "fe fe e0 a4 1a 05 01 19 01 fd");
    EXPECT(&c, "fe fe a4 e0 1a 05 01 19 01 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 1a 05 01 19 03 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 1a 05 01 19 fd", "fe fe e0 a4 1a 05 01 19 03 fd");
    EXPECT(&c, "fe fe a4 e0 1a 05 01 19 04 fd", "fe fe e0 a4 fa fd"); /* above WLAN */
    EXPECT(&c, "fe fe a4 e0 1a 05 01 19 0a fd", "fe fe e0 a4 fa fd"); /* not BCD */
    EXPECT(&c, "fe fe a4 e0 1a 05 01 19 01 00 fd", "fe fe e0 a4 fa fd"); /* too long */
    EXPECT(&c, "fe fe a4 e0 1a 05 01 19 fd", "fe fe e0 a4 1a 05 01 19 03 fd"); /* unchanged */
    EXPECT(&c, "fe fe a4 e0 1a 05 01 18 fd", "fe fe e0 a4 fa fd");    /* item not listed */
    EXPECT(&c, "fe fe a4 e0 1a 05 01 fd", "fe fe e0 a4 fa fd");       /* half an item number */
    EXPECT(&c, "fe fe a4 e0 1a 05 fd", "fe fe e0 a4 fa fd");
    CHECK(!civ_ptt(&c));

    fresh(&c); /* back to the power-up value */
    EXPECT(&c, "fe fe a4 e0 1a 05 01 19 fd", "fe fe e0 a4 1a 05 01 19 01 fd");
}

static void test_identity_and_addressing(void)
{
    civ_t c;
    fresh(&c);
    EXPECT(&c, "fe fe a4 e0 19 00 fd", "fe fe e0 a4 19 00 a4 fd");
    /* Any radio address is answered, from that same address. */
    EXPECT(&c, "fe fe 94 e0 03 fd", "fe fe e0 94 03 00 40 07 14 00 fd");
    EXPECT(&c, "fe fe 88 e1 03 fd", "fe fe e1 88 03 00 40 07 14 00 fd");
    EXPECT(&c, "fe fe a4 e0 7f fd", "fe fe e0 a4 fa fd"); /* unknown command */
    EXPECT(&c, "fe fe a4 e0 1a 05 00 01 fd", "fe fe e0 a4 fa fd");
    CHECK(civ_in_tx_range(&c, 14074000) && !civ_in_tx_range(&c, 14500000));
}

static void test_framing(void)
{
    civ_t c;
    fresh(&c);
    /* Byte at a time. */
    const uint8_t q[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x03, 0xFD};
    g_out_len = 0;
    for (size_t i = 0; i < sizeof q; i++) {
        CHECK(g_out_len == 0);
        civ_feed(&c, &q[i], 1);
    }
    CHECK(g_out_len == 11);

    /* Two frames in one read, extra preamble bytes, and leading noise. */
    EXPECT(&c, "00 12 fd fe fe fe fe a4 e0 0f fd fe fe a4 e0 1c 00 fd",
           "fe fe e0 a4 0f 00 fd fe fe e0 a4 1c 00 00 fd");
    /* A frame cut short by a new preamble: only the second is answered. */
    EXPECT(&c, "fe fe a4 e0 05 00 40 fe fe a4 e0 0f fd", "fe fe e0 a4 0f 00 fd");
    CHECK(civ_rx_freq(&c) == 14074000);
    /* Acknowledgements and transceive frames get no reply. */
    EXPECT(&c, "fe fe e0 a4 fb fd", "");
    EXPECT(&c, "fe fe 00 e0 00 00 40 07 07 00 fd", "");
    CHECK(civ_rx_freq(&c) == 7074000);
    /* Too short, and far too long. */
    EXPECT(&c, "fe fe a4 fd", "");
    uint8_t big[200];
    memset(big, 0x11, sizeof big);
    big[0] = big[1] = 0xFE;
    big[sizeof big - 1] = 0xFD;
    g_out_len = 0;
    civ_feed(&c, big, sizeof big);
    CHECK(g_out_len == 0);
    /* Still alive afterwards. */
    EXPECT(&c, "fe fe a4 e0 0f fd", "fe fe e0 a4 0f 00 fd");
}

static void test_external_ptt(void)
{
    civ_t c;
    fresh(&c);
    civ_set_ptt(&c, true); /* control line or VOX: visible to CAT, no callback loop */
    CHECK(g_ptt_events == 0);
    EXPECT(&c, "fe fe a4 e0 1c 00 fd", "fe fe e0 a4 1c 00 01 fd");
    civ_set_ptt(&c, false);
    EXPECT(&c, "fe fe a4 e0 1c 00 fd", "fe fe e0 a4 1c 00 00 fd");
}

int main(void)
{
    test_bcd();
    test_rigctl_open_and_read();
    test_rigctl_set();
    test_freq_and_mode();
    test_vfo_and_split();
    test_levels_meters_funcs();
    test_menu_settings();
    test_identity_and_addressing();
    test_framing();
    test_external_ptt();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
