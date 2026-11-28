/*
 * link_espnow: the link's packets over ESP-NOW.
 *
 * ESP-NOW sends on the channel Wi-Fi is on and through the interface Wi-Fi
 * is using: the station one when the board has joined a network, the access
 * point one when it hosts its own. Announcements go to the broadcast
 * address; audio goes to the peer's own address, which gets it acknowledged
 * and retried by the radio. Nothing is encrypted: it is test audio on a
 * bench.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"

#include "link_transport.h"
#include "wifi_net.h"

static const char *TAG = "link";

static const uint8_t BROADCAST[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static link_t *s_link;
static volatile bool s_up;

_Static_assert(LINK_MAX_PACKET <= ESP_NOW_MAX_DATA_LEN, "a link packet must fit one ESP-NOW frame");
_Static_assert(LINK_ADDR_LEN == ESP_NOW_ETH_ALEN, "the link's addresses are MAC addresses here");

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!s_link || !info || len <= 0) return;
    /* Runs in the Wi-Fi task: link_receive() only copies into the jitter buffer. */
    link_receive(s_link, info->src_addr, data, (size_t)len, info->rx_ctrl ? info->rx_ctrl->rssi : 0);
}

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    /* A broadcast always "succeeds"; this is audio the peer did not acknowledge. */
    if (status != ESP_NOW_SEND_SUCCESS && s_link) link_send_failed(s_link);
}

/* ESP-NOW only sends to addresses it has been told about, each with the
 * interface to use. */
static bool know_peer(const uint8_t *addr)
{
    wifi_interface_t want = wifi_net_hosting() ? WIFI_IF_AP : WIFI_IF_STA;
    esp_now_peer_info_t peer;

    if (esp_now_get_peer(addr, &peer) == ESP_OK) {
        if (peer.ifidx == want) return true;
        peer.ifidx = want; /* Wi-Fi has changed from joining to hosting since */
        return esp_now_mod_peer(&peer) == ESP_OK;
    }
    memset(&peer, 0, sizeof peer);
    memcpy(peer.peer_addr, addr, ESP_NOW_ETH_ALEN);
    peer.channel = 0; /* whatever channel Wi-Fi is on */
    peer.ifidx = want;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    return err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST; /* the other task got there first */
}

bool link_transport_send(void *ctx, const uint8_t *dst, const uint8_t *data, size_t len)
{
    (void)ctx;
    const uint8_t *to = dst ? dst : BROADCAST;
    if (!s_up) return false;
    /* A station looking for its network is off the network's channel, so
     * nothing sent now reaches the peer, and a hundred audio packets a
     * second would only crowd the frames the driver needs to rejoin. */
    if (!wifi_net_hosting() && !wifi_net_joined()) return false;
    if (!know_peer(to)) return false;
    return esp_now_send(to, data, len) == ESP_OK;
}

static esp_err_t espnow_up(void)
{
    esp_err_t err = esp_now_init();
    if (err == ESP_OK) err = esp_now_register_recv_cb(on_recv);
    if (err == ESP_OK) err = esp_now_register_send_cb(on_sent);
    s_up = err == ESP_OK;
    return err;
}

/* Wi-Fi was stopped and started again to host a network. ESP-NOW is set up
 * afresh, and the peers are told about again as they are next sent to. */
static void wifi_restarted(void)
{
    s_up = false;
    esp_now_deinit();
    esp_err_t err = espnow_up();
    if (err != ESP_OK) ESP_LOGE(TAG, "ESP-NOW did not come back after Wi-Fi restarted: %s", esp_err_to_name(err));
}

esp_err_t link_transport_start(link_t *l)
{
    s_link = l;
    esp_err_t err = wifi_net_start(); /* does nothing if the web console already has */
    if (err != ESP_OK) return err;
    wifi_net_on_restart(wifi_restarted);
    return espnow_up();
}

const char *link_transport_name(void)
{
    return "ESP-NOW";
}
