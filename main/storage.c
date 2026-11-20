/* storage: see storage.h. */
#include "storage.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "clip_store.h"

static const char *TAG = "storage";

#define NAMESPACE "mockradio"

static bool s_nvs_ok;

bool storage_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    s_nvs_ok = err == ESP_OK;
    if (!s_nvs_ok) ESP_LOGW(TAG, "no flash storage (%s): nothing will be remembered", esp_err_to_name(err));
    return s_nvs_ok;
}

bool storage_get_u16(const char *key, uint16_t *value)
{
    nvs_handle_t nvs;
    if (!s_nvs_ok || nvs_open(NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false; /* nothing stored yet */
    esp_err_t err = nvs_get_u16(nvs, key, value);
    nvs_close(nvs);
    return err == ESP_OK;
}

bool storage_set_u16(const char *key, uint16_t value)
{
    nvs_handle_t nvs;
    if (!s_nvs_ok || nvs_open(NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_u16(nvs, key, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

size_t storage_get_blob(const char *key, uint8_t *buf, size_t cap)
{
    nvs_handle_t nvs;
    size_t len = cap;
    if (!s_nvs_ok || nvs_open(NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return 0;
    esp_err_t err = nvs_get_blob(nvs, key, buf, &len);
    nvs_close(nvs);
    return err == ESP_OK ? len : 0;
}

bool storage_set_blob(const char *key, const uint8_t *buf, size_t len)
{
    nvs_handle_t nvs;
    if (!s_nvs_ok || nvs_open(NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(nvs, key, buf, len);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

bool storage_erase(const char *key)
{
    nvs_handle_t nvs;
    if (!s_nvs_ok || nvs_open(NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(nvs, key);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

/* ---- clips -----------------------------------------------------------------
 *
 * One operation at a time, in this task. What the rest of the firmware sees
 * (busy, whether the last one failed, each clip's length) is kept in atomics,
 * so that asking never touches the flash.
 */
typedef struct {
    mock_ctl_clip_op_t op;
    unsigned slot;
} clip_job_t;

static const esp_partition_t *s_part;
static clip_store_t s_clips;
static ae_t *s_engine;
static QueueHandle_t s_jobs;
static atomic_bool s_busy;
static atomic_bool s_failed;
static _Atomic uint32_t s_length_ms[CLIP_SLOTS_MAX];

_Static_assert(CLIP_SLOTS_MAX <= MOCK_CTL_CLIPS_MAX, "the controls must have room for every clip");
_Static_assert((int)MOCK_CTL_CLIP_SAVE == (int)CLIP_SAVE && (int)MOCK_CTL_CLIP_PLAY == (int)CLIP_PLAY &&
                   (int)MOCK_CTL_CLIP_ERASE == (int)CLIP_ERASE,
               "the controls and the clip store must number the operations alike");

static bool part_read(void *ctx, uint32_t offset, void *buf, size_t len)
{
    (void)ctx;
    return esp_partition_read(s_part, offset, buf, len) == ESP_OK;
}

static bool part_write(void *ctx, uint32_t offset, const void *buf, size_t len)
{
    (void)ctx;
    return esp_partition_write(s_part, offset, buf, len) == ESP_OK;
}

static bool part_erase(void *ctx, uint32_t offset, uint32_t len)
{
    (void)ctx;
    return esp_partition_erase_range(s_part, offset, len) == ESP_OK;
}

static void measure(void)
{
    for (unsigned i = 0; i < clip_store_slots(&s_clips); i++) {
        atomic_store(&s_length_ms[i], clip_store_length(&s_clips, i + 1) / (CLIP_FS_HZ / 1000u));
    }
}

static void wait_a_little(void *ctx)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void clip_task(void *arg)
{
    static const char *const names[] = {"save", "play", "erase"};
    clip_job_t job;
    (void)arg;

    for (;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        uint32_t began = esp_log_timestamp();
        bool ok = clip_run(&s_clips, s_engine, (clip_op_t)job.op, job.slot, wait_a_little, NULL);
        measure();
        atomic_store(&s_failed, !ok);
        atomic_store(&s_busy, false);
        ESP_LOGI(TAG, "clip %u: %s %s after %d ms", job.slot, names[job.op], ok ? "done" : "FAILED",
                 (int)(esp_log_timestamp() - began));
    }
}

unsigned storage_clips_start(ae_t *engine)
{
    s_engine = engine;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "clips");
    if (!s_part) {
        ESP_LOGW(TAG, "no \"clips\" partition in this flash layout: clips are not available");
        return 0;
    }
    const clip_dev_t dev = {.read = part_read, .write = part_write, .erase = part_erase, .size = s_part->size};
    clip_store_init(&s_clips, &dev);
    measure();

    s_jobs = xQueueCreate(1, sizeof(clip_job_t));
    /* The lowest priority there is: everything else comes first. */
    if (!s_jobs || xTaskCreate(clip_task, "clips", 6144, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "clip task failed to start: clips are not available");
        memset(&s_clips, 0, sizeof s_clips);
        return 0;
    }
    return clip_store_slots(&s_clips);
}

bool storage_clip_begin(void *ctx, mock_ctl_clip_op_t op, unsigned slot)
{
    const clip_job_t job = {.op = op, .slot = slot};
    (void)ctx;
    if (!s_jobs || atomic_exchange(&s_busy, true)) return false; /* one at a time */
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        atomic_store(&s_busy, false);
        return false;
    }
    return true;
}

void storage_clip_status(void *ctx, mock_ctl_clips_status_t *st)
{
    (void)ctx;
    memset(st, 0, sizeof *st);
    st->slots = clip_store_slots(&s_clips);
    st->busy = atomic_load(&s_busy);
    st->failed = atomic_load(&s_failed);
    for (unsigned i = 0; i < st->slots; i++) st->length_ms[i] = atomic_load(&s_length_ms[i]);
}
