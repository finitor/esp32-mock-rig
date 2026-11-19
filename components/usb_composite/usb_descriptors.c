/*
 * USB descriptors for the mock radio: one UAC2 audio function plus one
 * CDC-ACM port in a single composite device.
 *
 * host/tools/check_usb_descriptors.py reads desc_device and
 * desc_configuration out of the built firmware and checks them against the
 * constraints in DESIGN.md. Run it after any change here.
 */
#include <string.h>

#include "esp_mac.h"
#include "tusb.h"

#include "usb_descriptors.h"

/* Class 0xEF / 2 / 1 says "look at the interface association descriptors".
 * usb-serial-for-android needs exactly this to find the CDC port. */
const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,

    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,

    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,

    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&desc_device;
}

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN + TUD_CDC_DESC_LEN)

const uint8_t desc_configuration[] = {
    /* Bus powered, 200 mA: the board with Wi-Fi on draws more than the
     * 100 mA default budget. */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 200),

    /* Audio: control interface plus playback and capture streaming
     * interfaces, 48 kHz 16-bit mono each (set in sdkconfig.defaults). */
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STRID_AUDIO_CONTROL, EPNUM_AUDIO_OUT, EPNUM_AUDIO_IN, EPNUM_AUDIO_FB),

    /* CDC-ACM: its own association descriptor, a control interface whose
     * only endpoint is the interrupt IN notification endpoint, and a data
     * interface with bulk OUT and IN. */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_COMM, STRID_CDC, EPNUM_CDC_NOTIF, CDC_NOTIF_EP_SIZE, EPNUM_CDC_OUT, EPNUM_CDC_IN,
                       CDC_DATA_EP_SIZE),
};

_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "configuration descriptor length mismatch");

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static const char *const string_desc[STRID_COUNT] = {
    [STRID_MANUFACTURER] = "usb-mock-radio",
    [STRID_PRODUCT] = "Mock Radio",
    [STRID_AUDIO_CONTROL] = "Mock Radio Audio",
    [STRID_AUDIO_SPK] = "Mock Radio TX Audio",
    [STRID_AUDIO_MIC] = "Mock Radio RX Audio",
    [STRID_CDC] = "Mock Radio CAT",
};

/* The serial number is the chip's base MAC address, so two boards on one
 * host get distinct, stable identities. */
static const char *serial_string(void)
{
    static char serial[13];
    if (serial[0] == '\0') {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_BASE);
        static const char hex[] = "0123456789ABCDEF";
        for (int i = 0; i < 6; i++) {
            serial[2 * i] = hex[mac[i] >> 4];
            serial[2 * i + 1] = hex[mac[i] & 0x0F];
        }
        serial[12] = '\0';
    }
    return serial;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[32];
    size_t count;

    if (index == STRID_LANGID) {
        desc[1] = 0x0409; /* English (United States) */
        count = 1;
    } else {
        if (index >= STRID_COUNT) return NULL;
        const char *str = index == STRID_SERIAL ? serial_string() : string_desc[index];
        if (str == NULL) return NULL;
        count = strlen(str);
        if (count > 31) count = 31;
        for (size_t i = 0; i < count; i++) desc[1 + i] = (uint16_t)(unsigned char)str[i];
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return desc;
}
