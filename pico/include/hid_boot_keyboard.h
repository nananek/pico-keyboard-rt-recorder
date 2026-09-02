#ifndef PICO_KEYBOARD_RT_HID_BOOT_KEYBOARD_H
#define PICO_KEYBOARD_RT_HID_BOOT_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

// The current USB HID Boot Keyboard report is modifier, reserved, and six
// key usages (6KRO). The report has no HID report ID byte.
enum {
    PICO_HID_BOOT_KEYBOARD_REPORT_LEN = 8u,
    PICO_HID_USAGE_KEY_A = 0x04u,
};

typedef struct {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} pico_hid_boot_keyboard_report_t;

_Static_assert(sizeof(pico_hid_boot_keyboard_report_t) ==
                   PICO_HID_BOOT_KEYBOARD_REPORT_LEN,
               "Boot Keyboard report must remain exactly 8 bytes");

// Constructs the explicit all-keys-release report required by the safety
// invariants. It is a complete zero-valued 8-byte report, not an omitted send.
void pico_hid_boot_keyboard_release_all(pico_hid_boot_keyboard_report_t *report);

// Constructs a single-key 6KRO report. The remaining key slots and the
// reserved byte are zeroed so this is always a valid fixed-length v1 report.
void pico_hid_boot_keyboard_make_key_press(
    pico_hid_boot_keyboard_report_t *report,
    uint8_t modifier,
    uint8_t key_usage);

bool pico_hid_boot_keyboard_is_release(
    const pico_hid_boot_keyboard_report_t *report);

#endif  // PICO_KEYBOARD_RT_HID_BOOT_KEYBOARD_H
