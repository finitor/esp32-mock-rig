/*
 * web_console: the mock radio's control and telemetry console over Wi-Fi.
 *
 * ESP-IDF only. Brings up Wi-Fi (joins a network, or hosts one when there is
 * none to join), names the board on the network, and serves one page with a
 * JSON interface and a WebSocket that pushes telemetry:
 *
 *   GET  /                the page
 *   GET  /api/info        who this is and the list of settings
 *   GET  /api/settings    every setting
 *   POST /api/settings    change settings: a JSON object, any subset
 *   POST /api/rig         change the radio imitated: {"hamlib":3088}
 *   POST /api/reset       back to the power-up settings
 *   POST /api/keep        keep the settings as they are for power-up, in flash
 *   POST /api/forget      forget them: power up with the build's settings
 *   POST /api/clip        a clip operation: {"op":"save","clip":1}
 *   POST /api/replay      play the parrot's recording again
 *   GET  /api/telemetry   one snapshot of the telemetry
 *   GET  /ws              WebSocket: the same snapshot, pushed 5 times a second
 *   GET  /api/health      the board's health: memory, processor, audio timing
 *   POST /api/tasks       the same, with the table of tasks read afresh: {}
 *   GET  /api/wifi        how the board is connected
 *   POST /api/wifi        {"ssid":..., "password":...}: save, then restart
 *
 * The JSON is console_api's; this is the transport. No USB endpoint is
 * involved. There is no login: it is a bench device on a private network.
 */
#ifndef WEB_CONSOLE_H
#define WEB_CONSOLE_H

#include "esp_err.h"

#include "console_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mock_ctl_t *ctl; /* the settings and the choice of radio, shared with the CI-V controls */

    /* Held around every change of settings or of the radio imitated, so that
     * it cannot interleave with a change arriving over CAT. Must not block
     * for long. */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);

    /* Fill in everything but the Wi-Fi signal strength. Called from the
     * HTTP server's task, 5 times a second, without the lock. */
    void (*sample)(void *ctx, console_sample_t *out);

    /* The board's health, or NULL for a console that does not show it. With
     * scan_tasks the table of tasks is to be read afresh, which holds the
     * board up for a moment. Called from the HTTP server's task. */
    void (*health)(void *ctx, console_health_t *out, bool scan_tasks);

    void *ctx;
} web_console_config_t;

/* Start Wi-Fi and the server. config is copied; what it points to must stay.
 * Returns once both are started, which is before the network is joined. */
esp_err_t web_console_start(const web_console_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* WEB_CONSOLE_H */
