#ifndef PICO_KEYBOARD_RT_HID_KEYBOARD_HOST_H
#define PICO_KEYBOARD_RT_HID_KEYBOARD_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "keyboard_capture.h"

typedef struct {
    uint32_t keyboard_mounts;
    uint32_t keyboard_unmounts;
    uint32_t unsupported_mounts;
    uint32_t report_callbacks;
    uint32_t receive_requests;
    uint32_t receive_errors;
} pico_hid_keyboard_host_stats_t;

// Must run before the TinyUSB host stack is initialized so Boot protocol is
// selected during HID enumeration.
void pico_hid_keyboard_host_init(pico_keyboard_capture_t *capture);

bool pico_hid_keyboard_host_is_mounted(void);

pico_hid_keyboard_host_stats_t pico_hid_keyboard_host_get_stats(void);

#endif  // PICO_KEYBOARD_RT_HID_KEYBOARD_HOST_H
