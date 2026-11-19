/* Interface, endpoint and string numbering shared by the descriptors and the
 * code that has to refer to them. */
#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

/* Interfaces. The audio function comes first, as in Espressif's reference;
 * the CDC function has its own interface association descriptor so hosts
 * (and usb-serial-for-android) can find it wherever it sits. */
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_SPK, /* host playback: transmit audio */
    ITF_NUM_AUDIO_MIC, /* host capture: receive audio */
    ITF_NUM_CDC_COMM,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

/*
 * Endpoints. The ESP32-S3 has endpoint numbers 1..6 and room for four IN
 * endpoints besides EP0. This layout uses all four:
 *
 *   0x01 OUT  isochronous  audio from host
 *   0x81 IN   isochronous  audio rate feedback
 *   0x82 IN   isochronous  audio to host
 *   0x83 IN   interrupt    CDC notifications
 *   0x04 OUT  bulk         CDC data from host
 *   0x84 IN   bulk         CDC data to host
 *
 * Do not add another IN endpoint.
 */
#define EPNUM_AUDIO_OUT 0x01
#define EPNUM_AUDIO_FB  0x81
#define EPNUM_AUDIO_IN  0x82
#define EPNUM_CDC_NOTIF 0x83
#define EPNUM_CDC_OUT   0x04
#define EPNUM_CDC_IN    0x84

#define CDC_NOTIF_EP_SIZE 8
#define CDC_DATA_EP_SIZE  64

/* String descriptor indices. */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_AUDIO_CONTROL, /* the audio macro uses this and the next two */
    STRID_AUDIO_SPK,
    STRID_AUDIO_MIC,
    STRID_CDC,
    STRID_COUNT
};

/* Development identity: the pid.codes test VID/PID, for private bench use
 * only. A device handed to anyone else needs an allocated PID. It must never
 * use a real radio manufacturer's IDs. */
#define USB_VID 0x1209
#define USB_PID 0x0001

#endif /* USB_DESCRIPTORS_H */
