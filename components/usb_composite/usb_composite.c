/* usb_composite: see usb_composite.h. */
#include "usb_composite.h"

#include <string.h>

#include "cat_port.h"
#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/usb_dwc_struct.h"
#include "tusb.h"
#include "usb_device_uac.h"

#include "usb_descriptors.h"

static const char *TAG = "usb";

static usb_composite_config_t s_cfg;
static volatile bool s_mounted;

/* ---- audio ---------------------------------------------------------------
 *
 * The audio component hands over raw bytes in buffers that are not
 * guaranteed to be aligned for int16_t, so samples go through a local
 * buffer rather than a pointer cast.
 */
#define CHUNK_SAMPLES 240

static esp_err_t audio_from_host_cb(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    if (!s_cfg.audio_from_host) return ESP_OK;

    int16_t samples[CHUNK_SAMPLES];
    size_t total = len / sizeof(int16_t);
    for (size_t done = 0; done < total;) {
        size_t n = total - done;
        if (n > CHUNK_SAMPLES) n = CHUNK_SAMPLES;
        memcpy(samples, buf + done * sizeof(int16_t), n * sizeof(int16_t));
        s_cfg.audio_from_host(s_cfg.ctx, samples, n);
        done += n;
    }
    return ESP_OK;
}

static esp_err_t audio_to_host_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx)
{
    (void)ctx;
    size_t total = len / sizeof(int16_t);
    if (!s_cfg.audio_to_host) {
        memset(buf, 0, total * sizeof(int16_t));
    } else {
        int16_t samples[CHUNK_SAMPLES];
        for (size_t done = 0; done < total;) {
            size_t n = total - done;
            if (n > CHUNK_SAMPLES) n = CHUNK_SAMPLES;
            s_cfg.audio_to_host(s_cfg.ctx, samples, n);
            memcpy(buf + done * sizeof(int16_t), samples, n * sizeof(int16_t));
            done += n;
        }
    }
    *bytes_read = total * sizeof(int16_t);
    return ESP_OK;
}

/* ---- serial ----------------------------------------------------------------
 *
 * Replies wait in cat_port, not in TinyUSB's buffer, and one packet's worth
 * goes out whenever the IN endpoint is free: from the writer's task when it
 * already is, and from the USB task when a transfer completes. Whatever
 * waits there can then be dropped when it goes stale or when the port is
 * opened afresh, which a byte already in TinyUSB's buffer cannot. s_send
 * keeps the two senders from sending out of order; neither waits for it.
 */
static cat_port_t s_port;
static portMUX_TYPE s_port_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_send;
static volatile bool s_send_again;
static cat_port_watch_t s_watch;

static void port_lock(void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&s_port_mux);
}

static void port_unlock(void *ctx)
{
    (void)ctx;
    taskEXIT_CRITICAL(&s_port_mux);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Whether the hardware still has a transfer armed on the CAT IN endpoint.
 * The core clears this bit itself when the transfer completes. */
static bool cat_in_armed(void)
{
    return USB_DWC.in_eps[(EPNUM_CDC_IN & 0x0F) - 1].diepctl_reg.epena;
}

/* Send what waits, a packet's worth, if the endpoint is free; else only drop
 * what has gone stale. From any task. */
static void send_waiting(void)
{
    if (!s_send || xSemaphoreTake(s_send, 0) != pdTRUE) {
        s_send_again = true; /* the other sender goes round once more */
        return;
    }
    do {
        s_send_again = false;
        uint8_t buf[CAT_PORT_FRAME_MAX];
        bool free = s_mounted && !usbd_edpt_busy(0, EPNUM_CDC_IN) &&
                    tud_cdc_write_available() >= sizeof buf;
        size_t n = cat_port_take(&s_port, free ? buf : NULL, free ? sizeof buf : 0, now_ms());
        if (n) {
            tud_cdc_write(buf, (uint32_t)n);
            tud_cdc_write_flush();
        }
        xSemaphoreGive(s_send);
    } while (s_send_again && xSemaphoreTake(s_send, 0) == pdTRUE);
}

void tud_cdc_rx_cb(uint8_t itf)
{
    uint8_t buf[CFG_TUD_CDC_EP_BUFSIZE];
    while (tud_cdc_n_available(itf)) {
        uint32_t n = tud_cdc_n_read(itf, buf, sizeof buf);
        if (n == 0) break;
        if (s_cfg.serial_rx) s_cfg.serial_rx(s_cfg.ctx, buf, n);
    }
}

/* In the USB task, as an IN transfer completes. */
void tud_cdc_tx_complete_cb(uint8_t itf)
{
    (void)itf;
    send_waiting();
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    if (s_cfg.line_state) s_cfg.line_state(s_cfg.ctx, dtr, rts);
}

size_t usb_composite_serial_write(const uint8_t *bytes, size_t len)
{
    if (!s_mounted || !cat_port_put(&s_port, bytes, len, now_ms())) return 0;
    send_waiting();
    return len;
}

void usb_composite_serial_discard(void)
{
    cat_port_discard(&s_port);
}

void usb_composite_serial_poll(void)
{
    if (!s_mounted) return;
    if (cat_port_watch(&s_port, &s_watch, usbd_edpt_busy(0, EPNUM_CDC_IN), cat_in_armed(), now_ms())) {
        /* The stack still waits for a completion the hardware gave long
         * ago: it was lost on the way to the USB task. Hand it over, as the
         * interrupt would have; TinyUSB then frees the endpoint and sends
         * what waits. Nothing else is touched, so the data toggle stays
         * right. */
        ESP_LOGW(TAG, "CAT port: a lost IN completion, restored");
        dcd_event_xfer_complete(0, EPNUM_CDC_IN, 0, XFER_RESULT_SUCCESS, false);
        return;
    }
    if (cat_port_waiting(&s_port)) send_waiting();
}

void usb_composite_serial_status(usb_composite_serial_status_t *st)
{
    cat_port_status_t ps;
    cat_port_get_status(&s_port, &ps);
    st->stale = ps.stale;
    st->full = ps.full;
    st->discarded = ps.discarded;
    st->recovered = ps.recovered;
}

/* ---- device state ---------------------------------------------------------- */

void tud_mount_cb(void)
{
    s_mounted = true;
    ESP_LOGI(TAG, "configured by host");
}

void tud_umount_cb(void)
{
    s_mounted = false;
    ESP_LOGI(TAG, "disconnected");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    ESP_LOGI(TAG, "suspended");
}

void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "resumed");
}

bool usb_composite_mounted(void)
{
    return s_mounted;
}

esp_err_t usb_composite_init(const usb_composite_config_t *config)
{
    if (config) s_cfg = *config;
    cat_port_init(&s_port, port_lock, port_unlock, NULL);
    s_send = xSemaphoreCreateMutex();
    if (!s_send) return ESP_ERR_NO_MEM;

    /* The audio component brings up the PHY and TinyUSB and runs the USB
     * task; the CDC class rides along in the same stack. */
    uac_device_config_t uac = {
        .output_cb = audio_from_host_cb,
        .input_cb = audio_to_host_cb,
        .spk_itf_num = ITF_NUM_AUDIO_SPK,
        .mic_itf_num = ITF_NUM_AUDIO_MIC,
    };
    return uac_device_init(&uac);
}
