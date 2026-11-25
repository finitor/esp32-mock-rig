/*
 * web_console: see web_console.h.
 *
 * Every handler runs in the HTTP server's one task, and so does the
 * telemetry push (queued to it as work), so the buffers below need no lock.
 */
#include "web_console.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "wifi_net.h"

#ifndef CONFIG_HTTPD_WS_SUPPORT
#error "The web console needs CONFIG_HTTPD_WS_SUPPORT=y (it is in sdkconfig.defaults; delete sdkconfig and rebuild)"
#endif

static const char *TAG = "console";

#define PUSH_PERIOD_MS 200
#define SCOPE_PERIOD_MS (1000 / CONSOLE_SCOPE_ROWS_PER_S)
#define MAX_SOCKETS 7 /* LWIP allows 10 and the server keeps 3 for itself */
#define BODY_MAX 512

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static web_console_config_t s_cfg;
static httpd_handle_t s_server;
static char s_json[CONSOLE_JSON_MAX];
static char s_body[BODY_MAX];
static char s_telemetry[2048];
static console_health_t s_health;
static civ_t s_radio; /* a copy of the parser, so the lock is not held while it is written out */
static size_t s_telemetry_len;
static atomic_bool s_push_queued;

/* The waterfall. A connection to /ws/scope carries s_scope_mark as its
 * session's context, which is how a row finds the pages that want it and how
 * the telemetry leaves them alone. The count lets the push task ask for no
 * work at all while no page has the waterfall open. */
static console_scope_work_t s_scope_work;
static uint8_t s_scope_row[CONSOLE_SCOPE_ROW_MAX];
static const char s_scope_mark = 0;
static atomic_int s_scope_clients;
static atomic_bool s_scope_queued;

static bool is_scope_client(int fd)
{
    return httpd_sess_get_ctx(s_server, fd) == &s_scope_mark;
}

/* ---- replies ------------------------------------------------------------------ */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json, size_t len)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, (ssize_t)len);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    size_t len = console_error_json(message, s_json, sizeof s_json);
    return send_json(req, status, s_json, len);
}

static esp_err_t send_settings(httpd_req_t *req)
{
    size_t len = console_settings_json(s_cfg.ctl, s_json, sizeof s_json);
    return send_json(req, "200 OK", s_json, len);
}

/* The body of a POST as a zero-terminated string in s_body. On failure the
 * reply has been sent. A page on another site can make a browser POST here,
 * but not with this content type, so asking for it keeps such pages out. */
static bool read_json_body(httpd_req_t *req)
{
    char type[64] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof type);
    if (strncmp(type, "application/json", 16) != 0) {
        send_error(req, "415 Unsupported Media Type", "send the request as application/json");
        return false;
    }
    if (req->content_len >= sizeof s_body) {
        send_error(req, "413 Content Too Large", "the request is too long");
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, s_body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return false; /* the connection is gone; nothing to answer */
        got += (size_t)n;
    }
    s_body[got] = '\0';
    if (strlen(s_body) != got) { /* a zero byte inside the body */
        send_error(req, "400 Bad Request", "the request is not text");
        return false;
    }
    return true;
}

/* ---- the page and the settings --------------------------------------------------- */

static esp_err_t get_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, (ssize_t)(index_html_end - index_html_start - 1));
}

static esp_err_t get_info(httpd_req_t *req)
{
    const console_info_t info = {.name = wifi_net_name()}; /* the radio comes from the controls */
    size_t len = console_info_json(s_cfg.ctl, &info, s_json, sizeof s_json);
    return send_json(req, "200 OK", s_json, len);
}

static esp_err_t get_settings(httpd_req_t *req)
{
    return send_settings(req);
}

static esp_err_t post_settings(httpd_req_t *req)
{
    char err[CONSOLE_ERR_MAX];
    if (!read_json_body(req)) return ESP_OK;

    s_cfg.lock(s_cfg.ctx);
    console_result_t r = console_settings_apply(s_cfg.ctl, s_body, err, sizeof err);
    s_cfg.unlock(s_cfg.ctx);

    if (r == CONSOLE_OK) return send_settings(req);
    return send_error(req, r == CONSOLE_UNAVAILABLE ? "409 Conflict" : "400 Bad Request", err);
}

static esp_err_t post_rig(httpd_req_t *req)
{
    char err[CONSOLE_ERR_MAX];
    if (!read_json_body(req)) return ESP_OK;

    s_cfg.lock(s_cfg.ctx);
    console_result_t r = console_rig_apply(s_cfg.ctl, s_body, err, sizeof err);
    s_cfg.unlock(s_cfg.ctx);

    if (r == CONSOLE_OK) return send_json(req, "200 OK", "{}", 2);
    return send_error(req, r == CONSOLE_UNAVAILABLE ? "409 Conflict" : "400 Bad Request", err);
}

static esp_err_t post_clip(httpd_req_t *req)
{
    char err[CONSOLE_ERR_MAX];
    if (!read_json_body(req)) return ESP_OK;

    /* Only begun here: the firmware's clip task does the work, and the
     * telemetry says when it is done. */
    s_cfg.lock(s_cfg.ctx);
    console_result_t r = console_clip_apply(s_cfg.ctl, s_body, err, sizeof err);
    s_cfg.unlock(s_cfg.ctx);

    if (r == CONSOLE_OK) return send_json(req, "200 OK", "{}", 2);
    return send_error(req, r == CONSOLE_UNAVAILABLE ? "409 Conflict" : "400 Bad Request", err);
}

/* ---- the front panel ------------------------------------------------------------------ */

static esp_err_t send_radio(httpd_req_t *req)
{
    if (!s_cfg.ctl->civ) return send_error(req, "404 Not Found", "this mock has no radio to show");
    s_cfg.lock(s_cfg.ctx);
    s_radio = *s_cfg.ctl->civ;
    s_cfg.unlock(s_cfg.ctx);
    size_t len = console_radio_json(&s_radio, s_json, sizeof s_json);
    return send_json(req, "200 OK", s_json, len);
}

static esp_err_t get_radio(httpd_req_t *req)
{
    return send_radio(req);
}

static esp_err_t post_radio(httpd_req_t *req)
{
    char err[CONSOLE_ERR_MAX];
    if (!read_json_body(req)) return ESP_OK;

    s_cfg.lock(s_cfg.ctx);
    console_result_t r = console_radio_apply(s_cfg.ctl, s_body, err, sizeof err);
    if (r == CONSOLE_OK && s_cfg.radio_changed) s_cfg.radio_changed(s_cfg.ctx);
    s_cfg.unlock(s_cfg.ctx);

    if (r == CONSOLE_OK) return send_radio(req);
    return send_error(req, r == CONSOLE_UNAVAILABLE ? "409 Conflict" : "400 Bad Request", err);
}

/* Keep the settings for power-up, or forget the kept ones. */
static esp_err_t post_keep(httpd_req_t *req)
{
    s_cfg.lock(s_cfg.ctx);
    bool ok = mock_ctl_keep_settings(s_cfg.ctl);
    s_cfg.unlock(s_cfg.ctx);
    if (ok) return send_json(req, "200 OK", "{}", 2);
    return send_error(req, "500 Internal Server Error", "the board could not write its flash");
}

static esp_err_t post_forget(httpd_req_t *req)
{
    s_cfg.lock(s_cfg.ctx);
    bool ok = mock_ctl_forget_settings(s_cfg.ctl);
    s_cfg.unlock(s_cfg.ctx);
    if (ok) return send_json(req, "200 OK", "{}", 2);
    return send_error(req, "500 Internal Server Error", "the board could not write its flash");
}

static esp_err_t post_reset(httpd_req_t *req)
{
    s_cfg.lock(s_cfg.ctx);
    mock_ctl_reset(s_cfg.ctl);
    s_cfg.unlock(s_cfg.ctx);
    return send_settings(req);
}

static esp_err_t post_replay(httpd_req_t *req)
{
    if (ae_parrot_replay(s_cfg.ctl->engine)) return send_json(req, "200 OK", "{}", 2);
    return send_error(req, "409 Conflict", "nothing to replay: it needs the parrot route, a recording and PTT off");
}

/* ---- telemetry ----------------------------------------------------------------------
 *
 * One snapshot is built every PUSH_PERIOD_MS, sent to every WebSocket client
 * and kept for GET /api/telemetry. Building it in one place matters: reading
 * a level meter tells it "seen", and two readers would each see the other's
 * levels as stale.
 */
static void build_telemetry(void)
{
    console_sample_t s;
    wifi_net_status_t net;

    memset(&s, 0, sizeof s);
    s_cfg.sample(s_cfg.ctx, &s);
    wifi_net_status(&net);
    s.has_rssi = net.has_rssi;
    s.rssi_dbm = net.rssi_dbm;
    s_telemetry_len = console_telemetry_json(s_cfg.ctl, &s, s_telemetry, sizeof s_telemetry);
}

static void push_work(void *arg)
{
    (void)arg;
    int fds[MAX_SOCKETS];
    size_t n = MAX_SOCKETS;

    atomic_store(&s_push_queued, false);
    build_telemetry();
    if (s_telemetry_len == 0 || httpd_get_client_list(s_server, &n, fds) != ESP_OK) return;

    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET || is_scope_client(fds[i])) continue;
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)s_telemetry,
            .len = s_telemetry_len,
        };
        if (httpd_ws_send_frame_async(s_server, fds[i], &frame) != ESP_OK) {
            httpd_sess_trigger_close(s_server, fds[i]); /* gone, or too slow to keep up */
        }
    }
}

/* One row of the waterfall to every page that has it open. The transforms
 * are done here, in the server's task, and only if some page wants them. */
static void scope_work(void *arg)
{
    (void)arg;
    int fds[MAX_SOCKETS];
    size_t n = MAX_SOCKETS;
    size_t len = 0;

    atomic_store(&s_scope_queued, false);
    if (httpd_get_client_list(s_server, &n, fds) != ESP_OK) return;

    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET || !is_scope_client(fds[i])) continue;
        if (len == 0) {
            ae_ptt_t ptt;
            ae_get_ptt(s_cfg.ctl->engine, &ptt);
            len = console_scope_row(&s_scope_work, s_cfg.rx_scope, s_cfg.tx_scope, ae_ptt_keyed(&ptt), s_scope_row,
                                    sizeof s_scope_row);
            if (len == 0) return;
        }
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = s_scope_row,
            .len = len,
        };
        if (httpd_ws_send_frame_async(s_server, fds[i], &frame) != ESP_OK) {
            httpd_sess_trigger_close(s_server, fds[i]); /* gone, or too slow to keep up */
        }
    }
}

/* Asks the server's task for a push, unless the last one is still waiting:
 * a client that stalls the server must not pile up work behind itself. The
 * waterfall's rows are asked for twice as often as the telemetry, and not at
 * all while no page shows it. */
static void push_task(void *arg)
{
    (void)arg;
    _Static_assert(PUSH_PERIOD_MS == 2 * SCOPE_PERIOD_MS, "the telemetry goes with every second row");
    for (bool telemetry = true;; telemetry = !telemetry) {
        vTaskDelay(pdMS_TO_TICKS(SCOPE_PERIOD_MS));
        if (atomic_load(&s_scope_clients) > 0 && !atomic_exchange(&s_scope_queued, true)) {
            if (httpd_queue_work(s_server, scope_work, NULL) != ESP_OK) atomic_store(&s_scope_queued, false);
        }
        if (telemetry && !atomic_exchange(&s_push_queued, true)) {
            if (httpd_queue_work(s_server, push_work, NULL) != ESP_OK) atomic_store(&s_push_queued, false);
        }
    }
}

static esp_err_t get_telemetry(httpd_req_t *req)
{
    if (s_telemetry_len == 0) build_telemetry();
    return send_json(req, "200 OK", s_telemetry, s_telemetry_len);
}

/* ---- the board's health -----------------------------------------------------------------
 *
 * Asked for by the page while its health section is open, not pushed. Reading
 * the table of tasks holds the board up for a moment, so it is its own
 * request, and one a page on another site cannot make a browser send.
 */
static esp_err_t send_health(httpd_req_t *req, bool scan_tasks)
{
    if (!s_cfg.health) return send_error(req, "404 Not Found", "this board does not report its health");
    s_cfg.health(s_cfg.ctx, &s_health, scan_tasks);
    size_t len = console_health_json(&s_health, s_json, sizeof s_json);
    return send_json(req, "200 OK", s_json, len);
}

static esp_err_t get_health(httpd_req_t *req)
{
    return send_health(req, false);
}

static esp_err_t post_tasks(httpd_req_t *req)
{
    if (!read_json_body(req)) return ESP_OK;
    return send_health(req, true);
}

/* The page only listens. Whatever it sends is read and dropped. The server
 * answers the handshake itself and does not call this for it, so every call
 * is a frame (and req->method is left over from some earlier request: it says
 * nothing). */
static esp_err_t ws_handler(httpd_req_t *req)
{
    uint8_t buf[64];
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0); /* the length only */
    if (err != ESP_OK) return err;
    if (frame.len > sizeof buf) return ESP_FAIL; /* closes the connection */
    if (frame.len) {
        frame.payload = buf;
        err = httpd_ws_recv_frame(req, &frame, sizeof buf);
    }
    return err;
}

/* Called by the server when a waterfall's connection closes. The mark is
 * not memory to free; without this the server would call free() on it. */
static void scope_closed(void *ctx)
{
    (void)ctx;
    atomic_fetch_sub(&s_scope_clients, 1);
}

/* The waterfall's WebSocket is marked as one as soon as its handshake is done,
 * which only the server's post-handshake callback hears of: the handler is
 * not called until the page sends something, and it never does. */
#if !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "the waterfall needs CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT (sdkconfig.defaults)"
#endif
static esp_err_t ws_scope_opened(httpd_req_t *req)
{
    if (!s_cfg.rx_scope && !s_cfg.tx_scope) return ESP_FAIL; /* this board has no waterfall: close */
    if (req->sess_ctx != &s_scope_mark) {
        req->sess_ctx = (void *)&s_scope_mark;
        req->free_ctx = scope_closed;
        atomic_fetch_add(&s_scope_clients, 1);
    }
    return ESP_OK;
}

/* ---- Wi-Fi ----------------------------------------------------------------------------- */

static esp_err_t get_wifi(httpd_req_t *req)
{
    static const char *const keys[] = {"name", "mode", "ssid", "ip", "join"};
    wifi_net_status_t net;
    wifi_net_status(&net);
    const char *const values[] = {
        wifi_net_name(), net.hosting ? "ap" : net.connected ? "station" : "joining", net.ssid, net.ip, net.join,
    };
    size_t len = console_strings_json(keys, values, 5, s_json, sizeof s_json);
    return send_json(req, "200 OK", s_json, len);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000)); /* let the reply leave first */
    esp_restart();
}

static esp_err_t post_wifi(httpd_req_t *req)
{
    char ssid[WIFI_NET_SSID_MAX + 1];
    char password[WIFI_NET_PASSWORD_MAX + 1];
    if (!read_json_body(req)) return ESP_OK;

    if (!console_json_string(s_body, "ssid", ssid, sizeof ssid)) {
        return send_error(req, "400 Bad Request", "ssid: must be a network name of up to 32 characters, or empty");
    }
    if (!console_json_string(s_body, "password", password, sizeof password)) {
        return send_error(req, "400 Bad Request", "password: must be text of up to 63 characters, or empty");
    }
    if (password[0] && strlen(password) < 8) {
        return send_error(req, "400 Bad Request", "password: a Wi-Fi password has at least 8 characters");
    }
    esp_err_t err = wifi_net_save(ssid, password);
    memset(password, 0, sizeof password);
    memset(s_body, 0, sizeof s_body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save the network: %s", esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", "the board could not store the network");
    }
    ESP_LOGW(TAG, "Wi-Fi network changed from the console; restarting");
    if (xTaskCreate(restart_task, "restart", 2048, NULL, 1, NULL) != pdPASS) {
        return send_error(req, "500 Internal Server Error", "saved, but the board could not restart itself: reset it");
    }
    return send_json(req, "200 OK", "{}", 2);
}

/* ---- start ------------------------------------------------------------------------------- */

static void no_lock(void *ctx)
{
    (void)ctx;
}

esp_err_t web_console_start(const web_console_config_t *config)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_page},
        {.uri = "/api/info", .method = HTTP_GET, .handler = get_info},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = get_settings},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = post_settings},
        {.uri = "/api/rig", .method = HTTP_POST, .handler = post_rig},
        {.uri = "/api/radio", .method = HTTP_GET, .handler = get_radio},
        {.uri = "/api/radio", .method = HTTP_POST, .handler = post_radio},
        {.uri = "/api/clip", .method = HTTP_POST, .handler = post_clip},
        {.uri = "/api/keep", .method = HTTP_POST, .handler = post_keep},
        {.uri = "/api/forget", .method = HTTP_POST, .handler = post_forget},
        {.uri = "/api/reset", .method = HTTP_POST, .handler = post_reset},
        {.uri = "/api/replay", .method = HTTP_POST, .handler = post_replay},
        {.uri = "/api/telemetry", .method = HTTP_GET, .handler = get_telemetry},
        {.uri = "/api/health", .method = HTTP_GET, .handler = get_health},
        {.uri = "/api/tasks", .method = HTTP_POST, .handler = post_tasks},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = get_wifi},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = post_wifi},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
        {.uri = "/ws/scope",
         .method = HTTP_GET,
         .handler = ws_handler,
         .is_websocket = true,
         .ws_post_handshake_cb = ws_scope_opened},
    };

    if (!config || !config->ctl || !config->sample) return ESP_ERR_INVALID_ARG;
    s_cfg = *config;
    if (!s_cfg.lock || !s_cfg.unlock) s_cfg.lock = s_cfg.unlock = no_lock;
    console_scope_work_init(&s_scope_work);

    esp_err_t err = wifi_net_start();
    if (err != ESP_OK) return err;

    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.max_uri_handlers = sizeof routes / sizeof routes[0];
    http.max_open_sockets = MAX_SOCKETS;
    http.lru_purge_enable = true; /* a browser that vanished must not hold a socket for good */
    http.send_wait_timeout = 2;
    http.stack_size = 6144;
    http.task_priority = 3; /* below the USB and audio tasks, which run at 5 */
    err = httpd_start(&s_server, &http);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    if (xTaskCreate(push_task, "console_push", 2560, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no telemetry push: the page will show no live status");
    }
    ESP_LOGI(TAG, "web console started as %s", wifi_net_name());
    return ESP_OK;
}
