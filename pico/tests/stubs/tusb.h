#ifndef PICO_KEYBOARD_TEST_TUSB_H
#define PICO_KEYBOARD_TEST_TUSB_H

#include <stdbool.h>
#include <stdint.h>

enum {
    HID_ITF_PROTOCOL_NONE = 0u,
    HID_ITF_PROTOCOL_KEYBOARD = 1u,
    HID_ITF_PROTOCOL_MOUSE = 2u,
    HID_PROTOCOL_BOOT = 0u,
    HID_PROTOCOL_REPORT = 1u,
};

void tuh_hid_set_default_protocol(uint8_t protocol);
uint8_t tuh_hid_interface_protocol(uint8_t dev_addr, uint8_t instance);
bool tuh_hid_receive_report(uint8_t dev_addr, uint8_t instance);

#endif  // PICO_KEYBOARD_TEST_TUSB_H
