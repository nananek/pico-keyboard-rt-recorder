#include "hid_boot_keyboard.h"

#include <stdio.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static void test_release_report_is_eight_zero_bytes(void) {
    pico_hid_boot_keyboard_report_t report = {
        .modifier = 0xffu,
        .reserved = 0xffu,
        .keycode = {0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu},
    };

    pico_hid_boot_keyboard_release_all(&report);

    CHECK(sizeof(report) == PICO_HID_BOOT_KEYBOARD_REPORT_LEN);
    CHECK(pico_hid_boot_keyboard_is_release(&report));
    for (size_t index = 0u; index < sizeof(report); ++index) {
        CHECK(((const uint8_t *)&report)[index] == 0u);
    }
}

static void test_key_press_has_a_fixed_boot_keyboard_layout(void) {
    pico_hid_boot_keyboard_report_t report;

    pico_hid_boot_keyboard_make_key_press(&report, 0x02u, PICO_HID_USAGE_KEY_A);

    CHECK(sizeof(report) == PICO_HID_BOOT_KEYBOARD_REPORT_LEN);
    CHECK(!pico_hid_boot_keyboard_is_release(&report));
    CHECK(report.modifier == 0x02u);
    CHECK(report.reserved == 0u);
    CHECK(report.keycode[0] == PICO_HID_USAGE_KEY_A);
    for (size_t index = 1u; index < sizeof(report.keycode); ++index) {
        CHECK(report.keycode[index] == 0u);
    }
}

int main(void) {
    test_release_report_is_eight_zero_bytes();
    test_key_press_has_a_fixed_boot_keyboard_layout();
    return failures == 0 ? 0 : 1;
}
