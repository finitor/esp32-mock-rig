/*
 * usb_composite: the mock radio's USB device.
 *
 * One composite device on the ESP32-S3's native USB port: a 48 kHz 16-bit
 * mono sound card (playback and capture) and a CDC-ACM serial port for CAT.
 * This component owns the descriptors and the TinyUSB callbacks; what the
 * bytes and samples mean is up to the caller.
 */
#ifndef USB_COMPOSITE_H
#define USB_COMPOSITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_COMPOSITE_SAMPLE_RATE 48000

typedef struct {
    /* Serial bytes from the host (CAT commands). Runs in the USB task. */
    void (*serial_rx)(void *ctx, const uint8_t *bytes, size_t len);

    /* The host changed DTR or RTS. Hosts raise both when they open the port,
     * so these are not PTT unless the user has chosen line keying. */
    void (*line_state)(void *ctx, bool dtr, bool rts);

    /* Audio the host is playing: the radio's transmit audio. */
    void (*audio_from_host)(void *ctx, const int16_t *samples, size_t n);

    /* Audio for the host to record: the radio's receive audio. Fill all n
     * samples (with silence if there is nothing to send). */
    void (*audio_to_host)(void *ctx, int16_t *samples, size_t n);

    void *ctx;
} usb_composite_config_t;

/* Start the USB device. Any callback may be NULL. */
esp_err_t usb_composite_init(const usb_composite_config_t *config);

/* Send serial bytes to the host (CAT replies). Returns how many were queued. */
size_t usb_composite_serial_write(const uint8_t *bytes, size_t len);

/* Whether a host has configured the device. */
bool usb_composite_mounted(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_COMPOSITE_H */
