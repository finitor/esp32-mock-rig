/*
 * health: what the board itself is doing, for a first bring-up. ESP-IDF only.
 *
 * Free memory, how busy each processor core is, how long the audio work
 * takes, how often Wi-Fi was lost, why the board last restarted and, when
 * asked for, every task's stack headroom and share of a core. The web
 * console shows a reading (GET /api/health); the log on the COM jack gets a
 * short one every CONFIG_MOCK_HEALTH_LOG_S seconds, so that a build without
 * Wi-Fi can be judged too.
 *
 * Reading the table of tasks holds the whole board up while FreeRTOS looks
 * through every stack: a millisecond or so, long enough to disturb USB audio.
 * So it is read only when asked for, and each reading says how long it took.
 * Everything else is a handful of counters.
 */
#ifndef HEALTH_H
#define HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "console_api.h"

#define HEALTH_SCAN_TASKS 40 /* more tasks than this and the table cannot be read at all */

/* What one reader remembers between its readings: each reader gets rates
 * "since I last asked" without disturbing another's. Zero to start with. */
typedef struct {
    int64_t at_us; /* the last reading; 0 for none */
    uint32_t idle[CONSOLE_HEALTH_CORES];

    bool has_tasks;
    int64_t tasks_at_us;
    uint32_t tasks_took_us, tasks_missed;
    size_t n_tasks;
    console_task_t tasks[CONSOLE_HEALTH_TASKS];

    size_t n_seen; /* the run-time counters at the last reading of the table */
    struct {
        uint32_t id, run;
    } seen[HEALTH_SCAN_TASKS];
#if (configUSE_TRACE_FACILITY == 1)
    TaskStatus_t scan[HEALTH_SCAN_TASKS];
#endif
} health_reader_t;

/* Say why the board last restarted and start the periodic log, if any. */
void health_start(void);

/* The audio task: one block of n samples took us microseconds to produce. */
void health_block(uint32_t us, size_t n);

/* A reading. With scan_tasks, the table of tasks is read afresh, which holds
 * the board up (see above); without, the reader's last table is given again
 * with its age. One task at a time per reader. */
void health_read(health_reader_t *r, console_health_t *out, bool scan_tasks);

#endif /* HEALTH_H */
