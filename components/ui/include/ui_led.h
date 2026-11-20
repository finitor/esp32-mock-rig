/*
 * ui_led: what the board's one RGB LED shows.
 *
 * Portable C with no ESP-IDF dependencies: this decides the colour, the
 * firmware drives the LED.
 *
 *   white, dim   no USB host
 *   blue, dim    receiving
 *   red          transmitting; in the parrot route that is recording
 *   amber        the parrot has a recording and is waiting to replay it
 *   green        the parrot is replaying
 */
#ifndef UI_LED_H
#define UI_LED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_LED_NO_HOST = 0,
    UI_LED_RECEIVE,
    UI_LED_TRANSMIT,
    UI_LED_REPLAY_WAIT,
    UI_LED_REPLAY,
} ui_led_state_t;

typedef struct {
    bool host;           /* a USB host has configured the device */
    bool transmitting;   /* keyed by any PTT source, or the parrot is still recording */
    bool replay_waiting; /* the parrot will replay when its delay is up */
    bool replaying;
} ui_led_input_t;

/* The one state to show. Transmitting wins over the parrot's states, and
 * having no host wins over everything: without one nothing else is current. */
ui_led_state_t ui_led_state(const ui_led_input_t *in);

typedef struct {
    uint8_t r, g, b;
} ui_rgb_t;

/* The colour for a state at a brightness of 1..100 percent (clamped). A
 * channel that is lit stays lit however low the brightness. */
ui_rgb_t ui_led_color(ui_led_state_t state, unsigned brightness_percent);

#ifdef __cplusplus
}
#endif

#endif /* UI_LED_H */
