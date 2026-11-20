/* health: see health.h. */
#include "health.h"

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#ifdef CONFIG_MOCK_WIFI
#include "wifi_net.h"
#endif

static const char *TAG = "health";

#define STACK_LOW 256 /* bytes: less than this never used and the log warns */

static console_cost_t s_cost; /* all zero is its starting state */

void health_block(uint32_t us, size_t n)
{
    console_cost_add(&s_cost, us, n);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "restarted by the firmware";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "woke from deep sleep";
    case ESP_RST_BROWNOUT: return "brownout: the supply voltage dropped";
    case ESP_RST_USB: return "reset over USB";
    case ESP_RST_JTAG: return "reset over JTAG";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "crash (processor lock-up)";
    default: return "unknown";
    }
}

#if (configGENERATE_RUN_TIME_STATS == 1)
/* A share of one core in thousandths: run microseconds out of elapsed. */
static uint16_t permille(uint32_t run_us, int64_t elapsed_us)
{
    uint64_t p = (uint64_t)run_us * 1000u / (uint64_t)elapsed_us;
    return p > 1000u ? 1000u : (uint16_t)p;
}

/* The run-time counters are 32 bits of microseconds: differences are right
 * as long as less than their 71 minutes went by, and need some time to mean
 * anything. */
static bool rate_valid(int64_t elapsed_us)
{
    return elapsed_us >= 100000 && elapsed_us < (int64_t)UINT32_MAX;
}
#endif

static void read_cores(health_reader_t *r, console_health_t *out, int64_t now)
{
#if (configGENERATE_RUN_TIME_STATS == 1)
    int64_t elapsed = now - r->at_us;
    bool valid = r->at_us != 0 && rate_valid(elapsed);
    unsigned cores = portNUM_PROCESSORS < CONSOLE_HEALTH_CORES ? portNUM_PROCESSORS : CONSOLE_HEALTH_CORES;

    for (unsigned i = 0; i < cores; i++) {
        uint32_t idle = (uint32_t)ulTaskGetIdleRunTimeCounterForCore((BaseType_t)i);
        if (valid) out->busy_permille[i] = (uint16_t)(1000u - permille(idle - r->idle[i], elapsed));
        r->idle[i] = idle;
    }
    if (valid) out->n_cores = cores;
#else
    (void)r;
    (void)out;
    (void)now;
#endif
}

/* Read the table of tasks into the reader. Everything else on the board
 * waits while FreeRTOS does it. */
static void scan_tasks(health_reader_t *r)
{
#if (configUSE_TRACE_FACILITY == 1)
    UBaseType_t exist = uxTaskGetNumberOfTasks();
    int64_t began = esp_timer_get_time();
    UBaseType_t got = uxTaskGetSystemState(r->scan, HEALTH_SCAN_TASKS, NULL); /* 0 if there are more than that */
    int64_t now = esp_timer_get_time();
#if (configGENERATE_RUN_TIME_STATS == 1)
    int64_t elapsed = now - r->tasks_at_us;
    bool rates = r->has_tasks && rate_valid(elapsed);
#endif

    r->n_tasks = 0;
    for (UBaseType_t i = 0; i < got && r->n_tasks < CONSOLE_HEALTH_TASKS; i++) {
        const TaskStatus_t *st = &r->scan[i];
        console_task_t *t = &r->tasks[r->n_tasks++];
        memset(t, 0, sizeof *t);
        strlcpy(t->name, st->pcTaskName ? st->pcTaskName : "?", sizeof t->name);
        t->stack_free = (uint32_t)st->usStackHighWaterMark * sizeof(StackType_t);
#if (configGENERATE_RUN_TIME_STATS == 1)
        for (size_t k = 0; rates && k < r->n_seen; k++) {
            if (r->seen[k].id == (uint32_t)st->xTaskNumber) {
                t->has_cpu = true;
                t->cpu_permille = permille((uint32_t)st->ulRunTimeCounter - r->seen[k].run, elapsed);
                break;
            }
        }
#endif
    }
    r->n_seen = got;
    for (UBaseType_t i = 0; i < got; i++) {
        r->seen[i].id = (uint32_t)r->scan[i].xTaskNumber;
        r->seen[i].run = (uint32_t)r->scan[i].ulRunTimeCounter;
    }
    r->tasks_missed = got ? (uint32_t)got - (uint32_t)r->n_tasks : (uint32_t)exist;
    r->tasks_took_us = (uint32_t)(now - began);
    r->tasks_at_us = now;
    r->has_tasks = true;
#else
    (void)r; /* built without FreeRTOS's trace facility: there is no table */
#endif
}

void health_read(health_reader_t *r, console_health_t *out, bool with_tasks)
{
    memset(out, 0, sizeof *out);
    if (with_tasks) scan_tasks(r);

    int64_t now = esp_timer_get_time();
    out->uptime_ms = (uint32_t)(now / 1000);
    out->reset = reset_reason();

    out->heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->heap_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->has_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    out->psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    console_cost_read(&s_cost, out);
#ifdef CONFIG_MOCK_WIFI
    out->has_wifi = true;
    out->wifi_drops = wifi_net_drops();
#endif

    read_cores(r, out, now);
    r->at_us = now;
    out->running = (uint32_t)uxTaskGetNumberOfTasks();

    if (r->has_tasks) {
        out->has_tasks = true;
        out->tasks_age_ms = (uint32_t)((now - r->tasks_at_us) / 1000);
        out->tasks_took_us = r->tasks_took_us;
        out->tasks_missed = r->tasks_missed;
        out->n_tasks = r->n_tasks;
        memcpy(out->tasks, r->tasks, r->n_tasks * sizeof r->tasks[0]);
    }
}

/* ---- the log ------------------------------------------------------------------ */

#if CONFIG_MOCK_HEALTH_LOG_S > 0
static void log_tasks(const console_health_t *h)
{
    const console_task_t *least = NULL;
    for (size_t i = 0; i < h->n_tasks; i++) {
        const console_task_t *t = &h->tasks[i];
        if (!least || t->stack_free < least->stack_free) least = t;
        if (t->stack_free < STACK_LOW) {
            ESP_LOGW(TAG, "task %.16s has only %" PRIu32 " bytes of stack it never used", t->name, t->stack_free);
        }
    }
    if (least) {
        ESP_LOGI(TAG, "%u tasks read in %" PRIu32 " us; least stack left: %.16s, %" PRIu32 " bytes", (unsigned)h->n_tasks,
                 h->tasks_took_us, least->name, least->stack_free);
    }
    for (size_t i = 0; i < h->n_tasks; i++) {
        const console_task_t *t = &h->tasks[i];
        if (t->has_cpu && t->cpu_permille >= 10) {
            ESP_LOGI(TAG, "  %-16.16s %3u.%u %% of a core, %" PRIu32 " bytes of stack left", t->name,
                     t->cpu_permille / 10u, t->cpu_permille % 10u, t->stack_free);
        }
    }
    if (h->tasks_missed) ESP_LOGW(TAG, "%" PRIu32 " tasks are not in the table: no room", h->tasks_missed);
}

static void health_task(void *arg)
{
    static health_reader_t reader;
    static console_health_t h;
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_MOCK_HEALTH_LOG_S * 1000));
#ifdef CONFIG_MOCK_HEALTH_LOG_TASKS
        health_read(&reader, &h, true);
#else
        health_read(&reader, &h, false);
#endif
        ESP_LOGI(TAG, "memory: %" PRIu32 " KB free, %" PRIu32 " KB at its lowest, largest piece %" PRIu32 " KB; PSRAM %" PRIu32 " KB free",
                 h.heap_free / 1024u, h.heap_min / 1024u, h.heap_largest / 1024u, h.psram_free / 1024u);
        if (h.n_cores == 2) {
            ESP_LOGI(TAG, "processor: core 0 %u.%u %% busy, core 1 %u.%u %%; %" PRIu32 " tasks", h.busy_permille[0] / 10u,
                     h.busy_permille[0] % 10u, h.busy_permille[1] / 10u, h.busy_permille[1] % 10u, h.running);
        } else if (h.n_cores == 1) {
            ESP_LOGI(TAG, "processor: %u.%u %% busy; %" PRIu32 " tasks", h.busy_permille[0] / 10u, h.busy_permille[0] % 10u,
                     h.running);
        }
        if (h.has_cost) {
            ESP_LOGI(TAG,
                     "audio: a block of %" PRIu32 " samples takes %" PRIu32 " us (longest in the last second %" PRIu32
                     ", ever %" PRIu32 "): %" PRIu32 ".%" PRIu32 " %% of its time",
                     h.cost_block_samples, h.cost_avg_us, h.cost_peak_us, h.cost_worst_us, h.cost_load_permille / 10u,
                     h.cost_load_permille % 10u);
        }
        if (h.has_wifi && h.wifi_drops) ESP_LOGI(TAG, "Wi-Fi: the network was lost %" PRIu32 " times", h.wifi_drops);
        if (h.has_tasks) log_tasks(&h);
    }
}
#endif

void health_start(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    if (why == ESP_RST_POWERON || why == ESP_RST_EXT || why == ESP_RST_SW || why == ESP_RST_USB || why == ESP_RST_JTAG) {
        ESP_LOGI(TAG, "last restart: %s", reset_reason());
    } else {
        ESP_LOGW(TAG, "last restart: %s", reset_reason());
    }
#if CONFIG_MOCK_HEALTH_LOG_S > 0
    if (xTaskCreate(health_task, "health", 4096, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no health task: nothing about the board's health in the log");
    }
#endif
}
