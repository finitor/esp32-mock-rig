/*
 * TinyUSB configuration for the mock radio's composite device:
 * one UAC2 audio function (from espressif/usb_device_uac, used in its
 * "as part of a composite device" mode) and one CDC-ACM serial port.
 */
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"

/* Audio class settings and descriptor macros from the audio component.
 * Channel counts, sample rate and resolution come from its Kconfig. */
#include "uac_config.h"
#include "uac_descriptors.h"
#include "tusb_config_uac.h"

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

/* The ESP32-S3's built-in PHY is full speed only. */
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUSB_OS          OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH freertos/

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUD_ENABLED 1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE 64

/* One CDC-ACM port for CAT. CI-V frames are a few bytes; these are ample. */
#define CFG_TUD_CDC            1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
#define CFG_TUD_CDC_EP_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
