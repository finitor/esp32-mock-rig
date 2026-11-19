/*
 * USB mock radio firmware entry point.
 *
 * So far this only proves that the portable modules run on the target: it
 * reports the hardware it found and runs a short self-check of the CAT
 * parser and the audio DSP. The USB device comes next (Milestone 1).
 */
#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_psram.h"

#include "audio_dsp.h"
#include "cat_civ.h"
#include "rig_ic705.h"

static const char *TAG = "mock-radio";

static uint8_t s_reply[CIV_MAX_FRAME];
static size_t s_reply_len;

static void capture_tx(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    if (len <= sizeof s_reply) {
        memcpy(s_reply, bytes, len);
        s_reply_len = len;
    }
}

/* Read the frequency over CI-V and compare with the power-up state. */
static bool check_cat(void)
{
    static const uint8_t read_freq[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x03, 0xFD};
    static const uint8_t want[] = {0xFE, 0xFE, 0xE0, 0xA4, 0x03, 0x00, 0x40, 0x07, 0x14, 0x00, 0xFD};
    static civ_t civ;

    civ_init(&civ, &rig_ic705, capture_tx, NULL, NULL);
    s_reply_len = 0;
    civ_feed(&civ, read_freq, sizeof read_freq);
    return s_reply_len == sizeof want && memcmp(s_reply, want, sizeof want) == 0;
}

/* A steady level must come through the 48 -> 12 -> 48 kHz path unchanged. */
static bool check_dsp(void)
{
    enum { N = 480, LEVEL = 10000 };
    static int16_t in[N], mid[N / 4], out[N];
    static dsp_decim4_t down;
    static dsp_interp4_t up;

    for (int i = 0; i < N; i++) in[i] = LEVEL;
    dsp_decim4_init(&down);
    dsp_interp4_init(&up);
    if (dsp_decim4(&down, in, N, mid) != N / 4) return false;
    dsp_interp4(&up, mid, N / 4, out);

    int error = out[N - 1] - LEVEL;
    return error >= -8 && error <= 8;
}

void app_main(void)
{
    ESP_LOGI(TAG, "USB mock radio: %s personality (Hamlib model %d)", rig_ic705.name, rig_ic705.hamlib_model);

    size_t psram = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    if (psram) {
        ESP_LOGI(TAG, "PSRAM: %u KB", (unsigned)(psram / 1024));
    } else {
        ESP_LOGW(TAG, "No PSRAM found: parrot mode will have nowhere to record");
    }

    bool cat_ok = check_cat();
    bool dsp_ok = check_dsp();
    ESP_LOGI(TAG, "self-check: CAT %s, DSP %s", cat_ok ? "ok" : "FAILED", dsp_ok ? "ok" : "FAILED");
}
