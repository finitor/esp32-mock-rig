/*
 * link_uart: the link's packets over a UART between two boards.
 *
 * Three wires: this board's TX to the other's RX, the other's TX to this
 * one's RX, and ground. A wire has no packets of its own, so each is framed
 * and checked (link_wire in link.c). There is one board at the other end
 * and no addresses; it is given a fixed one so the link has a peer.
 */
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "link_transport.h"

static const char *TAG = "link";

#define PORT UART_NUM_1

#if CONFIG_MOCK_LINK_UART_TX_GPIO == CONFIG_MOCK_LINK_UART_RX_GPIO
#error "MOCK_LINK_UART: TX and RX need different pins"
#endif
#define PIN_TAKEN(p) ((p) == 19 || (p) == 20 || ((p) >= 35 && (p) <= 37) || (p) == 43 || (p) == 44)
#if PIN_TAKEN(CONFIG_MOCK_LINK_UART_TX_GPIO) || PIN_TAKEN(CONFIG_MOCK_LINK_UART_RX_GPIO)
#error "MOCK_LINK_UART: GPIO19/20 are USB, 35-37 the octal PSRAM and 43/44 the console UART"
#endif

static const uint8_t OTHER_END[LINK_ADDR_LEN] = {0x02, 'w', 'i', 'r', 'e', 0x01};

static link_t *s_link;
static link_wire_t s_wire;
static QueueHandle_t s_events;
static SemaphoreHandle_t s_tx_lock;
static uint8_t s_frame[LINK_WIRE_MAX];
static volatile bool s_up;

static void on_packet(void *ctx, const uint8_t *pkt, size_t len)
{
    (void)ctx;
    link_receive(s_link, OTHER_END, pkt, len, 0);
}

static void rx_task(void *arg)
{
    static uint8_t buf[512];
    uart_event_t event;
    (void)arg;

    for (;;) {
        if (xQueueReceive(s_events, &event, portMAX_DELAY) != pdTRUE) continue;
        if (event.type == UART_DATA) {
            size_t left = event.size;
            while (left) {
                int n = uart_read_bytes(PORT, buf, left < sizeof buf ? left : sizeof buf, 0);
                if (n <= 0) break;
                link_wire_feed(&s_wire, buf, (size_t)n, on_packet, NULL);
                left -= (size_t)n;
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            /* Bytes were lost: what is buffered is not worth sorting out. */
            uart_flush_input(PORT);
            xQueueReset(s_events);
            link_wire_init(&s_wire);
        }
    }
}

bool link_transport_send(void *ctx, const uint8_t *dst, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)dst; /* one wire, one other end */
    if (!s_up) return false;

    bool ok = false;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY); /* the audio task and the status task both send */
    size_t n = link_wire_encode(data, len, s_frame, sizeof s_frame);
    size_t room = 0;
    /* Never wait for the wire: a packet that does not fit the buffer is a
     * packet lost, which the other end handles. */
    if (n && uart_get_tx_buffer_free_size(PORT, &room) == ESP_OK && room >= n) {
        ok = uart_write_bytes(PORT, s_frame, n) == (int)n;
    }
    xSemaphoreGive(s_tx_lock);
    return ok;
}

esp_err_t link_transport_start(link_t *l)
{
    const uart_config_t cfg = {
        .baud_rate = CONFIG_MOCK_LINK_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    s_link = l;
    link_wire_init(&s_wire);
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) return ESP_ERR_NO_MEM;

    esp_err_t err = uart_driver_install(PORT, 4096, 4096, 16, &s_events, 0);
    if (err == ESP_OK) err = uart_param_config(PORT, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(PORT, CONFIG_MOCK_LINK_UART_TX_GPIO, CONFIG_MOCK_LINK_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) return err;
    if (xTaskCreate(rx_task, "link_uart", 3072, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_up = true;
    ESP_LOGI(TAG, "UART link on GPIO%d (TX) and GPIO%d (RX) at %d bit/s", CONFIG_MOCK_LINK_UART_TX_GPIO,
             CONFIG_MOCK_LINK_UART_RX_GPIO, CONFIG_MOCK_LINK_UART_BAUD);
    return ESP_OK;
}

const char *link_transport_name(void)
{
    return "UART";
}
