#ifndef PICO_KEYBOARD_RT_HID_KEYBOARD_DEVICE_H
#define PICO_KEYBOARD_RT_HID_KEYBOARD_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_boot_keyboard.h"

// The transport boundary accepts a length even though version 1 validates the
// fixed 8-byte Boot Keyboard size. Future HID descriptor versions can extend
// the accepted length without changing callers or the scheduler boundary.
bool pico_hid_keyboard_send_report(const uint8_t *report, size_t report_len);

bool pico_hid_keyboard_send_boot_report(
    const pico_hid_boot_keyboard_report_t *report);

// Sends a real, all-zero Boot Keyboard report when the HID endpoint is ready.
// The caller can retry a false result after tud_task() has made progress.
bool pico_hid_keyboard_send_all_keys_release(void);

#endif  // PICO_KEYBOARD_RT_HID_KEYBOARD_DEVICE_H
