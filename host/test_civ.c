/*
 * Unit tests for cat_civ with the IC-705 and G90 personalities.
 *
 * The request/reply pairs marked "rigctl" were recorded from Hamlib 4.7.1
 * (rigctl -m 3085) talking to this parser through host/civ_pty; Hamlib
 * accepted every reply. They are kept here as regression fixtures. The G90
 * personality's were recorded the same way with rigctl -m 3088.
 */
#include <stdio.h>
#include <string.h>

#include "cat_civ.h"
#include "rig_g90.h"
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

static int g_private_calls;

/* A stand-in for the mock's own controls: 01 is a set, 02 a read. */
static void private_fn(civ_t *c, const civ_frame_t *f, void *ctx)
{
    static const uint8_t data[] = {0x02, 0x12, 0x34};
    (*(int *)ctx)++;
    if (f->cmd == 0x01 && f->len == 1) civ_reply_ok(c, f);
    else if (f->cmd == 0x02 && f->len == 0) civ_reply(c, f, data, sizeof data);
    else civ_reply_ng(c, f);
}

static void test_private_address(void)
{
    civ_t c;
    fresh(&c);
    /* Until an address is taken, F0 is the radio like any other. */
    EXPECT(&c, "fe fe f0 e0 03 fd", "fe fe e0 f0 03 00 40 07 14 00 fd");

    g_private_calls = 0;
    civ_set_private(&c, 0xF0, private_fn, &g_private_calls);
    EXPECT(&c, "fe fe f0 e0 01 55 fd", "fe fe e0 f0 fb fd");
    EXPECT(&c, "fe fe f0 e1 02 fd", "fe fe e1 f0 02 12 34 fd");
    CHECK(g_private_calls == 2);

    /* Radio commands sent there do not reach the radio. */
    EXPECT(&c, "fe fe f0 e0 03 fd", "fe fe e0 f0 fa fd");
    EXPECT(&c, "fe fe f0 e0 05 00 00 10 07 00 fd", "fe fe e0 f0 fa fd");
    EXPECT(&c, "fe fe f0 e0 1c 00 01 fd", "fe fe e0 f0 fa fd");
    CHECK(g_ptt_events == 0 && !civ_ptt(&c));
    CHECK(g_private_calls == 5);

    /* Every other address is still the radio, unchanged by the above. */
    EXPECT(&c, "fe fe a4 e0 03 fd", "fe fe e0 a4 03 00 40 07 14 00 fd");
    EXPECT(&c, "fe fe 94 e0 03 fd", "fe fe e0 94 03 00 40 07 14 00 fd");
    CHECK(g_private_calls == 5);
    CHECK(c.frames_ok == 5 && c.frames_ng == 3); /* the handler's replies are counted too */

    /* Acknowledgements are dropped before the handler, as for the radio. */
    EXPECT(&c, "fe fe f0 e0 fb fd", "");
    CHECK(g_private_calls == 5);

    civ_set_private(&c, 0xF0, NULL, NULL);
    EXPECT(&c, "fe fe f0 e0 03 fd", "fe fe e0 f0 03 00 40 07 14 00 fd");

    civ_set_private(&c, 0xF0, private_fn, &g_private_calls);
    fresh(&c); /* civ_init clears it */
    EXPECT(&c, "fe fe f0 e0 03 fd", "fe fe e0 f0 03 00 40 07 14 00 fd");
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

    /* CAT's own PTT is kept apart: keying over CAT while another source
     * holds the radio in transmit still reaches the callback, and neither
     * source clears the other. */
    civ_set_ptt(&c, true);
    EXPECT(&c, "fe fe a4 e0 1c 00 01 fd", "fe fe e0 a4 fb fd");
    CHECK(g_ptt_events == 1 && g_ptt_last);
    civ_set_ptt(&c, false);
    CHECK(civ_ptt(&c));
    EXPECT(&c, "fe fe a4 e0 1c 00 fd", "fe fe e0 a4 1c 00 01 fd");
    civ_set_ptt(&c, true);
    EXPECT(&c, "fe fe a4 e0 1c 00 00 fd", "fe fe e0 a4 fb fd");
    CHECK(g_ptt_events == 2 && !g_ptt_last);
    EXPECT(&c, "fe fe a4 e0 1c 00 fd", "fe fe e0 a4 1c 00 01 fd");
    civ_set_ptt(&c, false);
    CHECK(!civ_ptt(&c));
    CHECK(g_ptt_events == 2); /* the other source never calls back */
}

/* ---- the G90 personality ------------------------------------------------------ */

static void fresh_g90(civ_t *c)
{
    civ_init(c, &rig_g90, tx, on_ptt, NULL);
    g_ptt_events = 0;
}

static void test_g90_rigctl(void)
{
    civ_t c;
    fresh_g90(&c);
    /* rigctl: what Hamlib 4.7.1 sends when it opens a G90, and reads back. */
    EXPECT(&c, "fe fe a4 e0 03 fd", "fe fe e0 a4 03 00 40 07 14 00 fd");
    EXPECT(&c, "fe fe a4 e0 25 00 fd", "fe fe e0 a4 25 00 00 40 07 14 00 fd");
    EXPECT(&c, "fe fe a4 e0 19 00 fd", "fe fe e0 a4 19 00 70 fd"); /* 70: Hamlib takes it for a G90 */
    EXPECT(&c, "fe fe a4 e0 26 00 fd", "fe fe e0 a4 26 00 01 01 01 fd"); /* USB, data, filter 1: U-D */
    EXPECT(&c, "fe fe a4 e0 1c 00 fd", "fe fe e0 a4 1c 00 00 fd");
    /* rigctl: F 7074000, M PKTLSB, T 1, T 0 */
    EXPECT(&c, "fe fe a4 e0 25 00 00 40 07 07 00 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 26 00 00 01 01 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 26 00 fd", "fe fe e0 a4 26 00 00 01 01 fd");
    EXPECT(&c, "fe fe a4 e0 1c 00 01 fd", "fe fe e0 a4 fb fd");
    CHECK(civ_ptt(&c) && g_ptt_events == 1 && g_ptt_last);
    EXPECT(&c, "fe fe a4 e0 1c 00 00 fd", "fe fe e0 a4 fb fd");
    CHECK(!civ_ptt(&c) && g_ptt_events == 2);
    CHECK(c.frames_ng == 0);

    /* The other addresses a G90 is found at: the manual's default, and the
     * one newer Hamlib uses. */
    EXPECT(&c, "fe fe 70 e0 03 fd", "fe fe e0 70 03 00 40 07 07 00 fd");
    EXPECT(&c, "fe fe 88 e0 03 fd", "fe fe e0 88 03 00 40 07 07 00 fd");
    /* The identity command of newer firmware, which newer Hamlib tries first. */
    EXPECT(&c, "fe fe 88 e0 1d 19 fd", "fe fe e0 88 1d 19 00 90 fd");
    EXPECT(&c, "fe fe 88 e0 1d 00 fd", "fe fe e0 88 fa fd");
    EXPECT(&c, "fe fe 88 e0 1d 19 00 fd", "fe fe e0 88 fa fd");
}

static void test_g90_manual(void)
{
    civ_t c;
    fresh_g90(&c);

    /* Modes: LSB USB AM CW FM CW-R. No RTTY and no wide FM. */
    EXPECT(&c, "fe fe 70 e0 04 fd", "fe fe e0 70 04 01 01 fd");
    EXPECT(&c, "fe fe 70 e0 06 00 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 06 02 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 06 03 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 06 05 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 06 07 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 04 fd", "fe fe e0 70 04 07 01 fd");
    EXPECT(&c, "fe fe 70 e0 06 04 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 06 06 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 06 08 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 06 17 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 06 01 fd", "fe fe e0 70 fb fd");

    /* The data modes are USB and LSB with the data switch; it reads back as
     * two bytes, the second always 0. */
    EXPECT(&c, "fe fe 70 e0 1a 06 fd", "fe fe e0 70 1a 06 01 00 fd");
    EXPECT(&c, "fe fe 70 e0 1a 06 00 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 1a 06 fd", "fe fe e0 70 1a 06 00 00 fd");
    EXPECT(&c, "fe fe 70 e0 1a 06 01 02 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 1a 06 fd", "fe fe e0 70 1a 06 01 00 fd");
    EXPECT(&c, "fe fe 70 e0 04 fd", "fe fe e0 70 04 01 02 fd");

    /* 0.5 to 30 MHz, and the band edges say so. */
    EXPECT(&c, "fe fe 70 e0 02 fd", "fe fe e0 70 02 00 00 50 00 00 2d 00 00 00 30 00 fd");
    EXPECT(&c, "fe fe 70 e0 05 00 00 50 00 00 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 05 00 00 00 30 00 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 05 99 99 49 00 00 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 05 01 00 00 30 00 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 05 00 00 20 44 01 fd", "fe fe e0 70 fa fd"); /* 144.2 MHz: not this radio */
    EXPECT(&c, "fe fe 70 e0 05 00 40 07 14 00 fd", "fe fe e0 70 fb fd");
    CHECK(civ_in_tx_range(&c, 14074000) && civ_in_tx_range(&c, 5357000));
    CHECK(!civ_in_tx_range(&c, 13000000) && !civ_in_tx_range(&c, 50313000));

    /* VFOs: A, B and swap; no "A = B". Split, but no repeater duplex. */
    EXPECT(&c, "fe fe 70 e0 07 01 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 03 fd", "fe fe e0 70 03 00 40 07 07 00 fd");
    EXPECT(&c, "fe fe 70 e0 07 b0 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 03 fd", "fe fe e0 70 03 00 40 07 14 00 fd");
    EXPECT(&c, "fe fe 70 e0 07 00 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 07 a0 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 0f 01 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 0f fd", "fe fe e0 70 0f 01 fd");
    EXPECT(&c, "fe fe 70 e0 0f 00 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 0f 11 fd", "fe fe e0 70 fa fd");

    /* Levels: the five in the manual and RF gain. Any other reads as 255
     * and cannot be set. */
    EXPECT(&c, "fe fe 70 e0 14 0a fd", "fe fe e0 70 14 0a 02 55 fd");
    EXPECT(&c, "fe fe 70 e0 14 0a 01 28 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 14 0a fd", "fe fe e0 70 14 0a 01 28 fd");
    EXPECT(&c, "fe fe 70 e0 14 01 00 76 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 14 01 fd", "fe fe e0 70 14 01 00 76 fd");
    EXPECT(&c, "fe fe 70 e0 14 03 fd", "fe fe e0 70 14 03 00 00 fd");
    EXPECT(&c, "fe fe 70 e0 14 09 fd", "fe fe e0 70 14 09 01 28 fd");
    EXPECT(&c, "fe fe 70 e0 14 0c fd", "fe fe e0 70 14 0c 01 30 fd");
    EXPECT(&c, "fe fe 70 e0 14 02 fd", "fe fe e0 70 14 02 02 55 fd");
    EXPECT(&c, "fe fe 70 e0 14 0b fd", "fe fe e0 70 14 0b 02 55 fd"); /* mic gain: not there */
    EXPECT(&c, "fe fe 70 e0 14 06 fd", "fe fe e0 70 14 06 02 55 fd");
    EXPECT(&c, "fe fe 70 e0 14 7f fd", "fe fe e0 70 14 7f 02 55 fd");
    EXPECT(&c, "fe fe 70 e0 14 0b 01 28 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 14 0b fd", "fe fe e0 70 14 0b 02 55 fd");
    EXPECT(&c, "fe fe 70 e0 14 fd", "fe fe e0 70 fa fd");

    /* Meters and switches. */
    EXPECT(&c, "fe fe 70 e0 15 01 fd", "fe fe e0 70 15 01 00 fd");
    EXPECT(&c, "fe fe 70 e0 15 02 fd", "fe fe e0 70 15 02 00 12 fd");
    EXPECT(&c, "fe fe 70 e0 15 11 fd", "fe fe e0 70 15 11 00 00 fd");
    EXPECT(&c, "fe fe 70 e0 15 12 fd", "fe fe e0 70 15 12 00 00 fd");
    EXPECT(&c, "fe fe 70 e0 15 15 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 16 12 fd", "fe fe e0 70 16 12 01 fd"); /* AGC fast */
    EXPECT(&c, "fe fe 70 e0 16 12 03 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 16 12 fd", "fe fe e0 70 16 12 03 fd");
    EXPECT(&c, "fe fe 70 e0 16 02 02 fd", "fe fe e0 70 fb fd"); /* preamplifier: 01 or 02 is on */
    EXPECT(&c, "fe fe 70 e0 16 02 fd", "fe fe e0 70 16 02 02 fd");
    EXPECT(&c, "fe fe 70 e0 16 22 01 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 16 44 01 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 16 50 01 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 16 50 fd", "fe fe e0 70 16 50 01 fd");
    EXPECT(&c, "fe fe 70 e0 16 46 fd", "fe fe e0 70 fa fd"); /* VOX: not in the manual */
    EXPECT(&c, "fe fe 70 e0 16 40 fd", "fe fe e0 70 fa fd"); /* noise reduction: the radio has none */

    /* Attenuator, tuner, PTT. */
    EXPECT(&c, "fe fe 70 e0 11 fd", "fe fe e0 70 11 00 fd");
    EXPECT(&c, "fe fe 70 e0 11 12 fd", "fe fe e0 70 fb fd");
    EXPECT(&c, "fe fe 70 e0 11 fd", "fe fe e0 70 11 12 fd");
    EXPECT(&c, "fe fe 70 e0 1c 01 fd", "fe fe e0 70 1c 01 00 fd");
    EXPECT(&c, "fe fe 70 e0 1c 01 02 fd", "fe fe e0 70 fb fd"); /* start tuning */
    EXPECT(&c, "fe fe 70 e0 1c 01 fd", "fe fe e0 70 1c 01 01 fd");
    EXPECT(&c, "fe fe 70 e0 1c 00 fd", "fe fe e0 70 1c 00 00 fd");

    /* What the manual does not list and Hamlib does not send a G90. */
    EXPECT(&c, "fe fe 70 e0 18 01 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 10 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 1a 03 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 1a 05 01 19 fd", "fe fe e0 70 fa fd");
    EXPECT(&c, "fe fe 70 e0 02 00 fd", "fe fe e0 70 fa fd");
}

/* One parser, two radios: switching is power-off and power-on. */
static void test_switch_personality(void)
{
    civ_t c;
    fresh(&c);
    civ_set_private(&c, 0xF0, private_fn, &g_private_calls);
    EXPECT(&c, "fe fe a4 e0 05 00 00 20 44 01 fd", "fe fe e0 a4 fb fd"); /* 144.2 MHz */
    EXPECT(&c, "fe fe a4 e0 0f 01 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 14 0b 00 10 fd", "fe fe e0 a4 fb fd");
    EXPECT(&c, "fe fe a4 e0 1c 00 01 fd", "fe fe e0 a4 fb fd");
    civ_set_ptt(&c, true); /* and the VOX is keying too */
    CHECK(g_ptt_events == 1 && g_ptt_last);
    uint32_t ok = c.frames_ok;

    civ_set_rig(&c, &rig_g90);
    CHECK(c.rig == &rig_g90);
    CHECK(g_ptt_events == 2 && !g_ptt_last); /* CAT's PTT is released, and said so */
    CHECK(civ_ptt(&c));                      /* the VOX's is not the parser's to release */
    civ_set_ptt(&c, false);
    CHECK(!civ_ptt(&c));
    CHECK(civ_rx_freq(&c) == 14074000 && !c.split && c.frames_ok == ok);
    EXPECT(&c, "fe fe a4 e0 19 00 fd", "fe fe e0 a4 19 00 70 fd");
    EXPECT(&c, "fe fe a4 e0 14 0b fd", "fe fe e0 a4 14 0b 02 55 fd");
    EXPECT(&c, "fe fe a4 e0 18 01 fd", "fe fe e0 a4 fa fd");
    EXPECT(&c, "fe fe f0 e0 02 fd", "fe fe e0 f0 02 12 34 fd"); /* the private address is kept */

    /* And back. Not keyed this time, so nothing is said about PTT. */
    EXPECT(&c, "fe fe a4 e0 16 12 03 fd", "fe fe e0 a4 fb fd");
    civ_set_rig(&c, &rig_ic705);
    CHECK(g_ptt_events == 2);
    EXPECT(&c, "fe fe a4 e0 19 00 fd", "fe fe e0 a4 19 00 a4 fd");
    EXPECT(&c, "fe fe a4 e0 16 12 fd", "fe fe e0 a4 16 12 02 fd");
    EXPECT(&c, "fe fe a4 e0 14 0b fd", "fe fe e0 a4 14 0b 01 28 fd");
    EXPECT(&c, "fe fe a4 e0 18 01 fd", "fe fe e0 a4 fb fd");

    /* A switch in the middle of a frame does not lose the frame. */
    static const uint8_t first[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x19};
    static const uint8_t rest[] = {0x00, 0xFD};
    g_out_len = 0;
    civ_feed(&c, first, sizeof first);
    civ_set_rig(&c, &rig_g90);
    civ_feed(&c, rest, sizeof rest);
    CHECK(g_out_len == 8 && g_out[6] == 0x70);
}

/* The front panel: commands run at the radio itself, and the number that
 * tells a display to read the radio again. */
static void test_front_panel(void)
{
    civ_t c;
    fresh(&c);
    uint32_t ok = c.frames_ok, ng = c.frames_ng;
    uint32_t h0 = civ_state_hash(&c);

    /* A command the radio takes: nothing goes out, nothing is counted, and
     * CAT reads back what was set. */
    static const uint8_t freq[] = {0x00, 0x40, 0x07, 0x07, 0x00}; /* 7.074 MHz */
    g_out_len = 0;
    CHECK(civ_local(&c, 0x05, freq, sizeof freq));
    CHECK(g_out_len == 0);
    CHECK(c.frames_ok == ok && c.frames_ng == ng);
    CHECK(civ_rx_freq(&c) == 7074000);
    EXPECT(&c, "fe fe a4 e0 03 fd", "fe fe e0 a4 03 00 40 07 07 00 fd");
    uint32_t h1 = civ_state_hash(&c);
    CHECK(h1 != h0);

    /* What the radio refuses over CAT it refuses here, and nothing changes. */
    static const uint8_t far[] = {0x00, 0x00, 0x00, 0x50, 0x02}; /* 250 MHz */
    static const uint8_t level_high[] = {0x01, 0x02, 0x56};      /* AF gain 256 */
    static const uint8_t no_mode[] = {0x09};
    g_out_len = 0;
    CHECK(!civ_local(&c, 0x05, far, sizeof far));
    CHECK(!civ_local(&c, 0x14, level_high, sizeof level_high));
    CHECK(!civ_local(&c, 0x06, no_mode, sizeof no_mode));
    CHECK(!civ_local(&c, 0x7E, NULL, 0)); /* no such command */
    CHECK(!civ_local(&c, 0x00, freq, sizeof freq)); /* transceive: taken, but never answered */
    CHECK(g_out_len == 0 && c.frames_ng == ng);
    CHECK(civ_rx_freq(&c) == 7074000);
    static const uint8_t too_long[CIV_MAX_FRAME] = {0};
    CHECK(!civ_local(&c, 0x05, too_long, sizeof too_long));

    /* A read is taken, and changes nothing. */
    CHECK(civ_local(&c, 0x03, NULL, 0));
    CHECK(civ_state_hash(&c) == h1);

    /* CAT goes on working after a local command: replies are sent again. */
    EXPECT(&c, "fe fe a4 e0 14 01 02 00 fd", "fe fe e0 a4 fb fd");
    uint32_t h2 = civ_state_hash(&c);
    CHECK(h2 != h1);
    EXPECT(&c, "fe fe a4 e0 14 01 02 00 fd", "fe fe e0 a4 fb fd"); /* the same again */
    EXPECT(&c, "fe fe a4 e0 14 01 fd", "fe fe e0 a4 14 01 02 00 fd");
    CHECK(civ_state_hash(&c) == h2);

    /* Everything CAT can read back is in the number. */
    uint32_t h = h2;
#define CHANGES(frame)                                             \
    do {                                                           \
        EXPECT(&c, frame, "fe fe e0 a4 fb fd");                    \
        CHECK(civ_state_hash(&c) != h);                            \
        h = civ_state_hash(&c);                                    \
    } while (0)
    CHANGES("fe fe a4 e0 06 03 02 fd");          /* mode and filter */
    CHANGES("fe fe a4 e0 1a 06 00 fd");          /* data mode */
    CHANGES("fe fe a4 e0 07 01 fd");             /* VFO B */
    CHANGES("fe fe a4 e0 0f 01 fd");             /* split */
    CHANGES("fe fe a4 e0 0f 11 fd");             /* duplex */
    CHANGES("fe fe a4 e0 16 22 01 fd");          /* a function */
    CHANGES("fe fe a4 e0 11 20 fd");             /* attenuator */
    CHANGES("fe fe a4 e0 10 03 fd");             /* tuning step */
    CHANGES("fe fe a4 e0 1a 03 10 fd");          /* filter width */
    CHANGES("fe fe a4 e0 1c 01 01 fd");          /* tuner */
    CHANGES("fe fe a4 e0 21 00 20 01 00 fd");    /* RIT frequency */
    CHANGES("fe fe a4 e0 21 01 01 fd");          /* RIT */
    CHANGES("fe fe a4 e0 1a 05 01 19 02 fd");    /* a menu setting */
    CHANGES("fe fe a4 e0 25 01 00 00 10 14 00 fd"); /* the other VFO's frequency */
#undef CHANGES
    CHECK(civ_set_meter(&c, 0x02, 120));
    CHECK(civ_state_hash(&c) != h);

    /* Another radio in the same state is not the same radio. */
    civ_t a, b;
    civ_init(&a, &rig_ic705, tx, on_ptt, NULL);
    civ_init(&b, &rig_g90, tx, on_ptt, NULL);
    CHECK(civ_state_hash(&a) != civ_state_hash(&b));
    civ_init(&b, &rig_ic705, tx, on_ptt, NULL);
    CHECK(civ_state_hash(&a) == civ_state_hash(&b));

    /* PTT is not in it: the display has that from elsewhere, five times a second. */
    h = civ_state_hash(&a);
    civ_set_ptt(&a, true);
    CHECK(civ_state_hash(&a) == h);

    CHECK(civ_meter_is_status(0x01) && civ_meter_is_status(0x05) && civ_meter_is_status(0x07));
    CHECK(!civ_meter_is_status(0x02) && !civ_meter_is_status(0x12));

    /* The G90's own commands run locally too, through its extra handler. */
    civ_init(&b, &rig_g90, tx, on_ptt, NULL);
    g_out_len = 0;
    CHECK(civ_local(&b, 0x02, NULL, 0)); /* the frequency edges */
    static const uint8_t equal[] = {0xA0};
    CHECK(!civ_local(&b, 0x07, equal, 1)); /* no A=B on a G90 */
    CHECK(g_out_len == 0);
}

/* Every level, meter and function has a name for the front panel, and a
 * function's named values are well formed. */
static void test_panel_names(void)
{
    static const civ_rig_t *const rigs[] = {&rig_ic705, &rig_g90};
    for (size_t r = 0; r < 2; r++) {
        const civ_rig_t *rig = rigs[r];
        const struct {
            const civ_item_t *items;
            size_t n;
            bool choices;
        } tables[] = {{rig->levels, rig->n_levels, false}, {rig->meters, rig->n_meters, false}, {rig->funcs, rig->n_funcs, true}};
        for (size_t t = 0; t < 3; t++) {
            for (size_t i = 0; i < tables[t].n; i++) {
                const civ_item_t *it = &tables[t].items[i];
                CHECK(it->name && it->name[0]);
                CHECK(tables[t].choices || !it->choices);
                if (it->choices) {
                    size_t len = strlen(it->choices);
                    CHECK(len > 0 && it->choices[len - 1] != '|' && strchr(it->choices, '|'));
                    CHECK(!strchr(it->choices, '"') && !strchr(it->choices, '\\'));
                }
            }
        }
        CHECK(rig->n_levels <= CIV_MAX_LEVELS && rig->n_meters <= CIV_MAX_METERS && rig->n_funcs <= CIV_MAX_FUNCS);
    }
    CHECK(rig_ic705.att_on == 0x20); /* what Hamlib 4.7.1 sends for L ATT 20 */
    CHECK(rig_g90.att_on == 0);      /* Hamlib versions disagree on the G90's: the panel only shows it */
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
    test_private_address();
    test_framing();
    test_external_ptt();
    test_g90_rigctl();
    test_g90_manual();
    test_switch_personality();
    test_front_panel();
    test_panel_names();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
