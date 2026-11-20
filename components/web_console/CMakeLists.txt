# console_api.c is portable and is also built by host/Makefile. The HTTP
# server is ESP-IDF only and is left out when the console is switched off in
# menuconfig. Wi-Fi itself is the wifi_net component.
set(srcs "console_api.c")
set(embed "")
if(CONFIG_MOCK_WEB_CONSOLE)
    list(APPEND srcs "web_console.c")
    set(embed "www/index.html")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES mock_ctl audio_engine audio_dsp cat_civ link
                       PRIV_REQUIRES esp_http_server wifi_net
                       EMBED_TXTFILES ${embed})
