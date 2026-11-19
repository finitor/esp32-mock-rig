# WHOLE_ARCHIVE: TinyUSB reaches this component only through callbacks it
# declares weak, so nothing would pull these objects into the link otherwise.
idf_component_register(SRCS "usb_composite.c" "usb_descriptors.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "tusb"
                       WHOLE_ARCHIVE)

# TinyUSB is compiled against the application's tusb_config.h. Put ours, and
# the audio component's helper headers it builds on, on TinyUSB's include
# path; everything that uses TinyUSB then sees the same configuration.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(uac_dir espressif__usb_device_uac COMPONENT_DIR)
target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb" "${uac_dir}/tusb_uac")
