#include "hid_boot_keyboard.h"

#include <string.h>

void pico_hid_boot_keyboard_release_all(pico_hid_boot_keyboard_report_t *report) {
    if (report == NULL) {
        return;
    }

    memset(report, 0, sizeof(*report));
}

void pico_hid_boot_keyboard_make_key_press(
    pico_hid_boot_keyboard_report_t *report,
    uint8_t modifier,
    uint8_t key_usage) {
    if (report == NULL) {
        return;
    }

    pico_hid_boot_keyboard_release_all(report);
    report->modifier = modifier;
    report->keycode[0] = key_usage;
}

bool pico_hid_boot_keyboard_is_release(
    const pico_hid_boot_keyboard_report_t *report) {
    if (report == NULL) {
        return false;
    }

    const uint8_t *bytes = (const uint8_t *)report;
    for (size_t index = 0; index < sizeof(*report); ++index) {
        if (bytes[index] != 0u) {
            return false;
        }
    }
    return true;
}
