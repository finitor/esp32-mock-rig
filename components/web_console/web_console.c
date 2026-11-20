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
#define MAX_SOCKETS 7 /* LWIP allows 10 and the server keeps 3 for itself */
#define BODY_MAX 512

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static web_console_config_t s_cfg;
static httpd_handle_t s_server;
static char s_json[CONSOLE_JSON_MAX];
static char s_body[BODY_MAX];
static char s_telemetry[2048];
static size_t s_telemetry_len;
static atomic_bool s_push_queued;

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
    const console_info_t info = {.name = wifi_net_name(), .rig = s_cfg.rig, .hamlib_model = s_cfg.hamlib_model};
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
        if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
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

/* Asks the server's task for a push, unless the last one is still waiting:
 * a client that stalls the server must not pile up work behind itself. */
static void push_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PUSH_PERIOD_MS));
        if (!atomic_exchange(&s_push_queued, true)) {
            if (httpd_queue_work(s_server, push_work, NULL) != ESP_OK) atomic_store(&s_push_queued, false);
        }
    }
}

static esp_err_t get_telemetry(httpd_req_t *req)
{
    if (s_telemetry_len == 0) build_telemetry();
    return send_json(req, "200 OK", s_telemetry, s_telemetry_len);
}

/* The page only listens. Whatever it sends is read and dropped. */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return ESP_OK; /* the handshake; the server has answered it */

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
        {.uri = "/api/reset", .method = HTTP_POST, .handler = post_reset},
        {.uri = "/api/replay", .method = HTTP_POST, .handler = post_replay},
        {.uri = "/api/telemetry", .method = HTTP_GET, .handler = get_telemetry},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = get_wifi},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = post_wifi},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
    };

    if (!config || !config->ctl || !config->sample) return ESP_ERR_INVALID_ARG;
    s_cfg = *config;
    if (!s_cfg.lock || !s_cfg.unlock) s_cfg.lock = s_cfg.unlock = no_lock;

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
