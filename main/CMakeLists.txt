idf_component_register(SRCS "main.c" "status_led.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES cat_civ rig_ic705 audio_dsp audio_engine mock_ctl ui usb_composite
                                     web_console esp_psram esp_driver_rmt esp_timer)
