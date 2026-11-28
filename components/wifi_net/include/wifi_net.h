/*
 * wifi_net: the board on the Wi-Fi network. ESP-IDF only.
 *
 * Shared by the web console and the ESP-NOW link: one radio, one channel.
 */
#ifndef WIFI_NET_H
#define WIFI_NET_H

#include <stdbool.h>
#include <stdint.h>

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
    uint32_t last_reason; /* why the last attempt or connection ended: ESP-IDF's wifi_err_reason_t, 0 for none */
    uint32_t fails;       /* attempts that failed since the last connection */
    uint32_t restarts;    /* times Wi-Fi was restarted because it could not rejoin */
    uint32_t drops;       /* as wifi_net_drops() */
    uint32_t down_s;      /* seconds without a connection, as a station; 0 when joined or hosting */
} wifi_net_status_t;

/* Join the configured network; if that has not worked after
 * CONFIG_MOCK_WIFI_JOIN_TIMEOUT_S, or there is none, host one. Returns once
 * Wi-Fi is started. Calling it again does nothing and returns the first
 * call's result. */
esp_err_t wifi_net_start(void);

/* Whether the board hosts its own network (an access point) and is not a
 * station on someone else's. ESP-NOW has to send on the matching interface. */
bool wifi_net_hosting(void);

/* fn is called, from a task of wifi_net's, after Wi-Fi has been stopped and
 * started again to host a network. ESP-NOW must be set up afresh then. */
void wifi_net_on_restart(void (*fn)(void));

/* mockradio-xxxx, from the MAC address. Valid after wifi_net_start(). */
const char *wifi_net_name(void);

void wifi_net_status(wifi_net_status_t *st);

/* Joined to a network as a station, with an address. While a station is
 * not joined its radio hops channels looking for the network, so nothing
 * sent through it can reach a peer on the network's channel. */
bool wifi_net_joined(void);

/* How many times a network the board had joined was lost since start. */
uint32_t wifi_net_drops(void);

/* Store the network to join from the next start. An empty ssid means host
 * one. Does not change the running connection. */
esp_err_t wifi_net_save(const char *ssid, const char *password);

#endif /* WIFI_NET_H */
