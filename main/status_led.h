/* The board's addressable RGB LED (WS2812 or SK6812), driven through RMT. */
#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdint.h>

#include "esp_err.h"

/* Set up the LED on this GPIO and turn it off. */
esp_err_t status_led_start(int gpio);

/* Show a colour. Does nothing if the LED was not started. */
void status_led_set(uint8_t r, uint8_t g, uint8_t b);

#endif /* STATUS_LED_H */
