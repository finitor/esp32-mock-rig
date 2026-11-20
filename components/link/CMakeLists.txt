# link.c is portable and is also built by host/Makefile. The transport that
# carries its packets between two boards is ESP-IDF only and is chosen in
# menuconfig: ESP-NOW, a UART, or none.
set(srcs "link.c")
if(CONFIG_MOCK_LINK_ESPNOW)
    list(APPEND srcs "link_espnow.c")
elseif(CONFIG_MOCK_LINK_UART)
    list(APPEND srcs "link_uart.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES audio_dsp
                       PRIV_REQUIRES esp_wifi wifi_net esp_driver_uart)
