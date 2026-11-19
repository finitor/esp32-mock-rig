/* usb_composite: see usb_composite.h. */
#include "usb_composite.h"

#include <string.h>

#include "esp_log.h"
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

/* ---- serial ---------------------------------------------------------------- */

void tud_cdc_rx_cb(uint8_t itf)
{
    uint8_t buf[CFG_TUD_CDC_EP_BUFSIZE];
    while (tud_cdc_n_available(itf)) {
        uint32_t n = tud_cdc_n_read(itf, buf, sizeof buf);
        if (n == 0) break;
        if (s_cfg.serial_rx) s_cfg.serial_rx(s_cfg.ctx, buf, n);
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    if (s_cfg.line_state) s_cfg.line_state(s_cfg.ctx, dtr, rts);
}

size_t usb_composite_serial_write(const uint8_t *bytes, size_t len)
{
    if (!s_mounted) return 0;
    uint32_t n = tud_cdc_write(bytes, (uint32_t)len);
    tud_cdc_write_flush();
    return n;
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
