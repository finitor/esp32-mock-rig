/* status_led: see status_led.h. */
#include "status_led.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

/* One tick is 0.1 us. A WS2812 bit is 1.2 us: a 0 is high for 0.3 us, a 1
 * for 0.9 us. The line idles low between updates, which is the reset. */
#define TICK_HZ 10000000

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_bytes;

esp_err_t status_led_start(int gpio)
{
    const rmt_tx_channel_config_t chan = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = TICK_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    const rmt_bytes_encoder_config_t bytes = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    rmt_channel_handle_t handle = NULL;

    esp_err_t err = rmt_new_tx_channel(&chan, &handle);
    if (err == ESP_OK) err = rmt_new_bytes_encoder(&bytes, &s_bytes);
    if (err == ESP_OK) err = rmt_enable(handle);
    if (err != ESP_OK) return err;

    s_chan = handle;
    status_led_set(0, 0, 0);
    return ESP_OK;
}

void status_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    static uint8_t grb[3]; /* the driver reads it after rmt_transmit() returns */
    const rmt_transmit_config_t once = {.loop_count = 0};

    if (!s_chan) return;
    grb[0] = g;
    grb[1] = r;
    grb[2] = b;
    if (rmt_transmit(s_chan, s_bytes, grb, sizeof grb, &once) == ESP_OK) {
        rmt_tx_wait_all_done(s_chan, 20);
    }
}
