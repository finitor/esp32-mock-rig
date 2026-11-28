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

/* Send one CAT reply to the host, whole (at most 64 bytes). Never waits:
 * replies queue until the port's IN endpoint is free, and one that has
 * waited a second, or the oldest when 16 wait, is dropped (cat_port). Not
 * from the USB task's callbacks. Returns len, or 0 if it was not queued. */
size_t usb_composite_serial_write(const uint8_t *bytes, size_t len);

/* Drop the replies waiting: the host has opened the port afresh. */
void usb_composite_serial_discard(void);

/* Call every 20 ms or so from a task of its own, on either core: sends what
 * waits if the endpoint has come free, and notices an IN endpoint that would
 * otherwise leave the port receiving CAT and never answering (a completion
 * lost on the way to the USB task, or a transfer whose FIFO interrupt was
 * masked) and puts it right. */
void usb_composite_serial_poll(void);

typedef struct {
    uint32_t stale, full, discarded; /* replies dropped: too old, no room, port opened afresh */
    uint32_t recovered;              /* lost IN completions restored */
    uint32_t rearmed;                /* IN transfers starved of their FIFO interrupt, unmasked */
    /* the IN endpoint now: TinyUSB busy, hardware armed, bytes not yet in
     * the FIFO, FIFO-empty interrupt unmasked */
    bool in_busy, in_armed, in_fifo_irq;
    uint32_t in_unsent;
} usb_composite_serial_status_t;

void usb_composite_serial_status(usb_composite_serial_status_t *st);

/* Blocks of receive audio the host was sent as silence because the task that
 * makes them had fallen behind: late counts those asked for more than a
 * block's time late and dropped, early those asked for ahead of time while it
 * made up for that. A stream starting is not counted. Both stay 0 on a board
 * that keeps up. */
typedef struct {
    uint32_t late, early;
} usb_composite_audio_status_t;

void usb_composite_audio_status(usb_composite_audio_status_t *st);

/* Whether a host has configured the device. */
bool usb_composite_mounted(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_COMPOSITE_H */
