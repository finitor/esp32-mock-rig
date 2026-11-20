# console_api.c is portable and is also built by host/Makefile. The Wi-Fi and
# HTTP parts are ESP-IDF only and are left out when the console is switched
# off in menuconfig.
set(srcs "console_api.c")
set(embed "")
if(CONFIG_MOCK_WEB_CONSOLE)
    list(APPEND srcs "web_console.c" "wifi_net.c")
    set(embed "www/index.html")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES mock_ctl audio_engine audio_dsp cat_civ
                       PRIV_REQUIRES esp_http_server esp_wifi esp_netif esp_event nvs_flash
                       EMBED_TXTFILES ${embed})
