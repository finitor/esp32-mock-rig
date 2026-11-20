# Wi-Fi is brought up for the web console, for the ESP-NOW link, or for both.
# With neither in the build there is nothing to compile here.
set(srcs "")
if(CONFIG_MOCK_WIFI)
    list(APPEND srcs "wifi_net.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash)
