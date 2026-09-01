#include "hid_keyboard_host.h"

#include <stdio.h>
#include <string.h>

#include "tusb.h"

void tuh_hid_mount_cb(
    uint8_t dev_addr,
    uint8_t instance,
    const uint8_t *report_desc,
    uint16_t desc_len);
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance);
void tuh_hid_report_received_cb(
    uint8_t dev_addr,
    uint8_t instance,
    const uint8_t *report,
    uint16_t report_len);

static int failures;
static uint8_t fake_protocol;
static uint8_t selected_default_protocol;
static bool receive_result;
static uint32_t receive_calls;
static uint8_t last_receive_dev_addr;
static uint8_t last_receive_instance;
static uint64_t fake_timestamp_us;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

uint64_t time_us_64(void) {
    return fake_timestamp_us;
}

void tuh_hid_set_default_protocol(uint8_t protocol) {
    selected_default_protocol = protocol;
}

uint8_t tuh_hid_interface_protocol(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;
    (void)instance;
    return fake_protocol;
}

bool tuh_hid_receive_report(uint8_t dev_addr, uint8_t instance) {
    ++receive_calls;
    last_receive_dev_addr = dev_addr;
    last_receive_instance = instance;
    return receive_result;
}

static void reset_fakes(pico_keyboard_capture_t *capture) {
    fake_protocol = HID_ITF_PROTOCOL_NONE;
    selected_default_protocol = HID_PROTOCOL_REPORT;
    receive_result = true;
    receive_calls = 0u;
    last_receive_dev_addr = 0u;
    last_receive_instance = 0u;
    fake_timestamp_us = 0u;
    pico_keyboard_capture_init(capture);
    pico_hid_keyboard_host_init(capture);
}

static void test_only_keyboard_mount_is_armed_in_boot_protocol(void) {
    pico_keyboard_capture_t capture;

    reset_fakes(&capture);
    CHECK(selected_default_protocol == HID_PROTOCOL_BOOT);

    fake_protocol = HID_ITF_PROTOCOL_MOUSE;
    tuh_hid_mount_cb(2u, 3u, NULL, 0u);
    CHECK(!pico_hid_keyboard_host_is_mounted());
    CHECK(receive_calls == 0u);

    fake_protocol = HID_ITF_PROTOCOL_KEYBOARD;
    tuh_hid_mount_cb(4u, 5u, NULL, 0u);
    CHECK(pico_hid_keyboard_host_is_mounted());
    CHECK(receive_calls == 1u);
    CHECK(last_receive_dev_addr == 4u);
    CHECK(last_receive_instance == 5u);

    const pico_hid_keyboard_host_stats_t stats =
        pico_hid_keyboard_host_get_stats();
    CHECK(stats.keyboard_mounts == 1u);
    CHECK(stats.unsupported_mounts == 1u);
}

static void test_callback_captures_entry_timestamp_and_always_rearms(void) {
    pico_keyboard_capture_t capture;
    const uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {
        0x01u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    };
    pico_keyboard_capture_event_t event;

    reset_fakes(&capture);
    fake_protocol = HID_ITF_PROTOCOL_KEYBOARD;
    tuh_hid_mount_cb(1u, 0u, NULL, 0u);

    fake_timestamp_us = UINT64_C(0x1020304050607080);
    tuh_hid_report_received_cb(1u, 0u, report, sizeof(report));
    CHECK(receive_calls == 2u);
    CHECK(pico_keyboard_capture_pop(&capture, &event));
    CHECK(event.timestamp_us == UINT64_C(0x1020304050607080));
    CHECK(memcmp(&event.report, report, sizeof(report)) == 0);

    fake_timestamp_us = 99u;
    tuh_hid_report_received_cb(1u, 0u, report, sizeof(report) - 1u);
    CHECK(receive_calls == 3u);
    CHECK(!pico_keyboard_capture_pop(&capture, &event));

    const pico_keyboard_capture_stats_t capture_stats =
        pico_keyboard_capture_get_stats(&capture);
    CHECK(capture_stats.accepted == 1u);
    CHECK(capture_stats.invalid == 1u);

    const pico_hid_keyboard_host_stats_t host_stats =
        pico_hid_keyboard_host_get_stats();
    CHECK(host_stats.report_callbacks == 2u);
    CHECK(host_stats.receive_requests == 3u);
}

static void test_receive_errors_and_unmount_clear_state(void) {
    pico_keyboard_capture_t capture;
    const uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {0u};

    reset_fakes(&capture);
    fake_protocol = HID_ITF_PROTOCOL_KEYBOARD;
    receive_result = false;
    tuh_hid_mount_cb(7u, 1u, NULL, 0u);
    CHECK(pico_hid_keyboard_host_is_mounted());

    tuh_hid_report_received_cb(7u, 1u, report, sizeof(report));
    tuh_hid_umount_cb(7u, 1u);
    CHECK(!pico_hid_keyboard_host_is_mounted());

    const uint32_t calls_before_stale_report = receive_calls;
    tuh_hid_report_received_cb(7u, 1u, report, sizeof(report));
    CHECK(receive_calls == calls_before_stale_report);

    const pico_hid_keyboard_host_stats_t stats =
        pico_hid_keyboard_host_get_stats();
    CHECK(stats.receive_errors == 2u);
    CHECK(stats.keyboard_unmounts == 1u);
}

int main(void) {
    test_only_keyboard_mount_is_armed_in_boot_protocol();
    test_callback_captures_entry_timestamp_and_always_rearms();
    test_receive_errors_and_unmount_clear_state();
    return failures == 0 ? 0 : 1;
}
