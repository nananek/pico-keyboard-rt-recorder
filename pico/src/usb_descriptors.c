#include "bsp/board.h"
#include "tusb.h"

#include <string.h>

enum {
    USB_VID = 0xCafe,
    USB_PID = 0x4014,
    USB_BCD = 0x0200,
};

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    // bDeviceClass 0 means each interface declares its own USB class.
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&device_descriptor;
}

// No HID report ID is present: the on-wire INPUT report is exactly the v1
// 8-byte Boot Keyboard layout (modifier, reserved, six key usages).
static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return hid_report_descriptor;
}

enum {
    ITF_NUM_HID = 0,
    ITF_NUM_TOTAL,
    EPNUM_HID = 0x81,
    CONFIG_TOTAL_LEN = TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN,
};

static const uint8_t configuration_descriptor[] = {
    // TUD_CONFIG_DESCRIPTOR supplies the mandatory bus-powered bit itself;
    // zero adds neither self-powered nor remote-wakeup capability bits.
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_HID_DESCRIPTOR(
        ITF_NUM_HID,
        0,
        HID_ITF_PROTOCOL_KEYBOARD,
        sizeof(hid_report_descriptor),
        EPNUM_HID,
        CFG_TUD_HID_EP_BUFSIZE,
        10),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return configuration_descriptor;
}

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
};

static const char *const string_descriptors[] = {
    (const char[]){0x09, 0x04},  // English (United States)
    "Pico Keyboard RT Recorder",
    "Pico 2 Boot Keyboard",
    NULL,  // supplied from the board's unique USB serial source
};

static uint16_t string_descriptor[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    size_t character_count;
    if (index == STRID_LANGID) {
        memcpy(&string_descriptor[1], string_descriptors[STRID_LANGID], 2u);
        character_count = 1u;
    } else if (index == STRID_SERIAL) {
        character_count = board_usb_get_serial(&string_descriptor[1], 32u);
    } else {
        if (index >= sizeof(string_descriptors) / sizeof(string_descriptors[0])) {
            return NULL;
        }

        const char *string = string_descriptors[index];
        character_count = strlen(string);
        if (character_count > 32u) {
            character_count = 32u;
        }

        for (size_t character = 0u; character < character_count; ++character) {
            string_descriptor[character + 1u] = (uint8_t)string[character];
        }
    }

    string_descriptor[0] =
        (uint16_t)((TUSB_DESC_STRING << 8u) | (2u * character_count + 2u));
    return string_descriptor;
}
