/* ui_led: see ui_led.h. */
#include "ui_led.h"

ui_led_state_t ui_led_state(const ui_led_input_t *in)
{
    if (!in->host) return UI_LED_NO_HOST;
    if (in->transmitting) return UI_LED_TRANSMIT;
    if (in->replaying) return UI_LED_REPLAY;
    if (in->replay_waiting) return UI_LED_REPLAY_WAIT;
    return UI_LED_RECEIVE;
}

/* At full brightness. The two idle states are kept low so that the active
 * ones stand out. */
static const ui_rgb_t colors[] = {
    [UI_LED_NO_HOST] = {48, 48, 48},
    [UI_LED_RECEIVE] = {0, 0, 96},
    [UI_LED_TRANSMIT] = {255, 0, 0},
    [UI_LED_REPLAY_WAIT] = {255, 96, 0},
    [UI_LED_REPLAY] = {0, 255, 0},
};

static uint8_t scale(uint8_t full, unsigned percent)
{
    if (!full) return 0;
    unsigned v = (full * percent + 50) / 100;
    return (uint8_t)(v ? v : 1);
}

ui_rgb_t ui_led_color(ui_led_state_t state, unsigned brightness_percent)
{
    if ((unsigned)state >= sizeof colors / sizeof colors[0]) state = UI_LED_NO_HOST;
    if (brightness_percent < 1) brightness_percent = 1;
    if (brightness_percent > 100) brightness_percent = 100;

    const ui_rgb_t full = colors[state];
    const ui_rgb_t out = {scale(full.r, brightness_percent), scale(full.g, brightness_percent),
                          scale(full.b, brightness_percent)};
    return out;
}
