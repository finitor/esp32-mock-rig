/* wifi_net: the board on the network. Private to web_console. */
#ifndef WIFI_NET_H
#define WIFI_NET_H

#include <stdbool.h>

#include "esp_err.h"

#define WIFI_NET_SSID_MAX 32
#define WIFI_NET_PASSWORD_MAX 63

typedef struct {
    bool hosting;                      /* the board hosts its own network */
    bool connected;                    /* joined, with an address */
    char ssid[WIFI_NET_SSID_MAX + 1];  /* the network joined or hosted */
    char join[WIFI_NET_SSID_MAX + 1];  /* the network it is set to join; empty for none */
    char ip[16];
    bool has_rssi;
    int rssi_dbm;
} wifi_net_status_t;

/* Join the configured network; if that has not worked after
 * CONFIG_MOCK_WIFI_JOIN_TIMEOUT_S, or there is none, host one. Returns once
 * Wi-Fi is started. */
esp_err_t wifi_net_start(void);

/* mockradio-xxxx, from the MAC address. Valid after wifi_net_start(). */
const char *wifi_net_name(void);

void wifi_net_status(wifi_net_status_t *st);

/* Store the network to join from the next start. An empty ssid means host
 * one. Does not change the running connection. */
esp_err_t wifi_net_save(const char *ssid, const char *password);

#endif /* WIFI_NET_H */
