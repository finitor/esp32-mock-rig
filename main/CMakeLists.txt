idf_component_register(SRCS "main.c" "status_led.c" "storage.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES cat_civ rig_ic705 rig_g90 audio_dsp audio_engine mock_ctl ui usb_composite clips
                                     web_console link coord esp_psram esp_driver_rmt esp_timer nvs_flash esp_partition)
