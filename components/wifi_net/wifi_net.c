/*
 * wifi_net: see wifi_net.h.
 *
 * Station first, access point as the fallback, never both at once: a station
 * that keeps scanning for a missing network drags an access point on the same
 * radio from channel to channel. Power save is off, because the ESP-NOW link
 * that will share this radio loses packets while a station sleeps, and the
 * board is USB powered.
 */
#include "wifi_net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define NVS_NAMESPACE "mockradio"
#define NVS_SSID "wifi_ssid"
#define NVS_PASSWORD "wifi_pass"
#define AP_ADDRESS "192.168.4.1" /* esp_netif's default for an access point */

#define GOT_IP BIT0

static char s_name[20] = "mockradio";
static char s_join[WIFI_NET_SSID_MAX + 1];
static char s_password[WIFI_NET_PASSWORD_MAX + 1];
static char s_ip[16];
static EventGroupHandle_t s_events;
static volatile bool s_hosting;
static volatile bool s_connected;
static void (*volatile s_on_restart)(void);

bool wifi_net_hosting(void)
{
    return s_hosting;
}

void wifi_net_on_restart(void (*fn)(void))
{
    s_on_restart = fn;
}

const char *wifi_net_name(void)
{
    return s_name;
}

/* The network to join: the saved one if there is one, else the build's. */
static void load_network(void)
{
    nvs_handle_t nvs;
    size_t len = sizeof s_join;
    bool saved = false;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, NVS_SSID, s_join, &len) == ESP_OK) {
            saved = true;
            len = sizeof s_password;
            if (nvs_get_str(nvs, NVS_PASSWORD, s_password, &len) != ESP_OK) s_password[0] = '\0';
        }
        nvs_close(nvs);
    }
    if (!saved) {
        strlcpy(s_join, CONFIG_MOCK_WIFI_SSID, sizeof s_join);
        strlcpy(s_password, CONFIG_MOCK_WIFI_PASSWORD, sizeof s_password);
    }
}

esp_err_t wifi_net_save(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    if (strlen(ssid) > WIFI_NET_SSID_MAX || strlen(password) > WIFI_NET_PASSWORD_MAX) return ESP_ERR_INVALID_ARG;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, NVS_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, NVS_PASSWORD, password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) ESP_LOGW(TAG, "lost \"%s\", joining again", s_join);
        s_connected = false;
        if (!s_hosting) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got = data;
        esp_ip4addr_ntoa(&got->ip_info.ip, s_ip, sizeof s_ip);
        s_connected = true;
        ESP_LOGI(TAG, "joined \"%s\": the console is at http://%s/ and http://%s.local/", s_join, s_ip, s_name);
        xEventGroupSetBits(s_events, GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "a device joined the board's network");
    }
}

static esp_err_t host_network(void)
{
    wifi_config_t cfg = {0};
    size_t pass_len = strlen(CONFIG_MOCK_WIFI_AP_PASSWORD);

    if (!esp_netif_create_default_wifi_ap()) return ESP_FAIL;
    strlcpy((char *)cfg.ap.ssid, s_name, sizeof cfg.ap.ssid);
    cfg.ap.ssid_len = (uint8_t)strlen(s_name);
    cfg.ap.channel = CONFIG_MOCK_WIFI_AP_CHANNEL;
    cfg.ap.max_connection = 4;
    if (pass_len >= 8 && pass_len <= WIFI_NET_PASSWORD_MAX) {
        strlcpy((char *)cfg.ap.password, CONFIG_MOCK_WIFI_AP_PASSWORD, sizeof cfg.ap.password);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    s_hosting = true;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) return err;
    strlcpy(s_ip, AP_ADDRESS, sizeof s_ip);
    ESP_LOGI(TAG, "hosting \"%s\" on channel %d (%s): the console is at http://%s/", s_name, CONFIG_MOCK_WIFI_AP_CHANNEL,
             cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2", s_ip);
    return ESP_OK;
}

/* Give the network its time, then host one if it never answered. */
static void fallback_task(void *arg)
{
    (void)arg;
    EventBits_t bits =
        xEventGroupWaitBits(s_events, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(CONFIG_MOCK_WIFI_JOIN_TIMEOUT_S * 1000));
    if (!(bits & GOT_IP)) {
        ESP_LOGW(TAG, "\"%s\" not joined after %d s", s_join, CONFIG_MOCK_WIFI_JOIN_TIMEOUT_S);
        s_hosting = true; /* stops the reconnecting */
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_err_t err = host_network();
        if (err != ESP_OK) ESP_LOGE(TAG, "could not host a network either: %s", esp_err_to_name(err));
        esp_wifi_set_ps(WIFI_PS_NONE);
        void (*fn)(void) = s_on_restart;
        if (err == ESP_OK && fn) fn();
    }
    vTaskDelete(NULL);
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err == ESP_OK) err = mdns_hostname_set(s_name);
    if (err == ESP_OK) err = mdns_instance_name_set("Mock radio console");
    if (err == ESP_OK) err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) ESP_LOGW(TAG, "no %s.local name: %s", s_name, esp_err_to_name(err));
}

static esp_err_t start(void);

esp_err_t wifi_net_start(void)
{
    static bool started;
    static esp_err_t result;
    if (!started) {
        started = true; /* both callers are on the main task, at start-up */
        result = start();
    }
    return result;
}

static esp_err_t start(void)
{
    uint8_t mac[6];
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_name, sizeof s_name, "mockradio-%02x%02x", mac[4], mac[5]);
    }
    load_network();

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err; /* the loop may exist already */

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM); /* the network is kept above, not by the driver */
    if (err == ESP_OK) err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    if (err != ESP_OK) return err;

    if (s_join[0]) {
        wifi_config_t cfg = {0};
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        if (!sta) return ESP_FAIL;
        esp_netif_set_hostname(sta, s_name);
        strlcpy((char *)cfg.sta.ssid, s_join, sizeof cfg.sta.ssid);
        strlcpy((char *)cfg.sta.password, s_password, sizeof cfg.sta.password);
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err == ESP_OK) err = esp_wifi_start();
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "%s: joining \"%s\"", s_name, s_join);
        if (xTaskCreate(fallback_task, "wifi_fallback", 4096, NULL, 2, NULL) != pdPASS) {
            ESP_LOGW(TAG, "no fallback task: the board will not host a network if \"%s\" is missing", s_join);
        }
    } else {
        err = host_network();
        if (err != ESP_OK) return err;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) ESP_LOGW(TAG, "power save stays on: %s", esp_err_to_name(err));
    start_mdns();
    return ESP_OK;
}

void wifi_net_status(wifi_net_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->hosting = s_hosting;
    st->connected = s_connected;
    strlcpy(st->join, s_join, sizeof st->join);
    strlcpy(st->ssid, s_hosting ? s_name : s_join, sizeof st->ssid);
    if (s_hosting || s_connected) strlcpy(st->ip, s_ip, sizeof st->ip);
    if (s_connected && !s_hosting) st->has_rssi = esp_wifi_sta_get_rssi(&st->rssi_dbm) == ESP_OK;
}
