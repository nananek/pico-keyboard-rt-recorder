#include "hid_keyboard_device.h"

#include "tusb.h"

bool pico_hid_keyboard_send_report(const uint8_t *report, size_t report_len) {
    if (report == NULL || report_len != PICO_HID_BOOT_KEYBOARD_REPORT_LEN) {
        return false;
    }

    if (!tud_hid_ready()) {
        return false;
    }

    // The descriptor intentionally has no report ID, so report ID zero carries
    // the fixed 8-byte v1 Boot Keyboard report without a prefix byte.
    return tud_hid_report(0u, report, (uint16_t)report_len);
}

bool pico_hid_keyboard_send_boot_report(
    const pico_hid_boot_keyboard_report_t *report) {
    return pico_hid_keyboard_send_report((const uint8_t *)report, sizeof(*report));
}

bool pico_hid_keyboard_send_all_keys_release(void) {
    pico_hid_boot_keyboard_report_t release_report;
    pico_hid_boot_keyboard_release_all(&release_report);
    return pico_hid_keyboard_send_boot_report(&release_report);
}
