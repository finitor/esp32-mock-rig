/* Unit tests for ui_led: which colour the status LED shows. */
#include <stdio.h>

#include "ui_led.h"

static int g_failed;
static int g_checks;

#define CHECK(cond)                                                \
    do {                                                           \
        g_checks++;                                                \
        if (!(cond)) {                                             \
            g_failed++;                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);  \
        }                                                          \
    } while (0)

static ui_led_state_t state(bool host, bool tx, bool waiting, bool replaying)
{
    const ui_led_input_t in = {host, tx, waiting, replaying};
    return ui_led_state(&in);
}

static bool is(ui_rgb_t c, int r, int g, int b)
{
    return c.r == r && c.g == g && c.b == b;
}

int main(void)
{
    /* One state at a time. */
    CHECK(state(true, false, false, false) == UI_LED_RECEIVE);
    CHECK(state(true, true, false, false) == UI_LED_TRANSMIT);
    CHECK(state(true, false, true, false) == UI_LED_REPLAY_WAIT);
    CHECK(state(true, false, false, true) == UI_LED_REPLAY);

    /* Transmitting wins over the parrot; replaying over waiting. */
    CHECK(state(true, true, true, false) == UI_LED_TRANSMIT);
    CHECK(state(true, true, false, true) == UI_LED_TRANSMIT);
    CHECK(state(true, false, true, true) == UI_LED_REPLAY);

    /* No host wins over everything. */
    CHECK(state(false, false, false, false) == UI_LED_NO_HOST);
    CHECK(state(false, true, true, true) == UI_LED_NO_HOST);

    /* Red recording, green playing, as the plan says; the rest tell apart. */
    CHECK(is(ui_led_color(UI_LED_TRANSMIT, 100), 255, 0, 0));
    CHECK(is(ui_led_color(UI_LED_REPLAY, 100), 0, 255, 0));
    CHECK(is(ui_led_color(UI_LED_REPLAY_WAIT, 100), 255, 96, 0));
    CHECK(is(ui_led_color(UI_LED_RECEIVE, 100), 0, 0, 96));
    CHECK(is(ui_led_color(UI_LED_NO_HOST, 100), 48, 48, 48));

    /* Brightness scales, clamps, and never puts a lit channel out. */
    CHECK(is(ui_led_color(UI_LED_TRANSMIT, 10), 26, 0, 0));
    CHECK(is(ui_led_color(UI_LED_REPLAY_WAIT, 50), 128, 48, 0));
    CHECK(is(ui_led_color(UI_LED_RECEIVE, 1), 0, 0, 1));
    CHECK(is(ui_led_color(UI_LED_NO_HOST, 1), 1, 1, 1));
    CHECK(is(ui_led_color(UI_LED_TRANSMIT, 0), 3, 0, 0));
    CHECK(is(ui_led_color(UI_LED_TRANSMIT, 500), 255, 0, 0));
    CHECK(is(ui_led_color((ui_led_state_t)99, 100), 48, 48, 48));

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
