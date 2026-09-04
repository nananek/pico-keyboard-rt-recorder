#include "keyboard_capture.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static void test_preserves_report_and_timestamp(void) {
    pico_keyboard_capture_t capture;
    const uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {
        0x02u, 0x00u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x09u,
    };
    pico_keyboard_capture_event_t event;

    pico_keyboard_capture_init(&capture);
    CHECK(pico_keyboard_capture_push(
        &capture, UINT64_C(0x123456789abcdef0), report, sizeof(report)));
    CHECK(pico_keyboard_capture_peek(&capture, &event));
    CHECK(event.timestamp_us == UINT64_C(0x123456789abcdef0));
    CHECK(pico_keyboard_capture_pop(&capture, &event));
    CHECK(event.timestamp_us == UINT64_C(0x123456789abcdef0));
    CHECK(event.report_len == PICO_HID_BOOT_KEYBOARD_REPORT_LEN);
    CHECK(memcmp(&event.report, report, sizeof(report)) == 0);
    CHECK(!pico_keyboard_capture_pop(&capture, &event));
}

static void test_keeps_release_and_duplicate_reports(void) {
    pico_keyboard_capture_t capture;
    const uint8_t release[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {0u};
    pico_keyboard_capture_event_t first;
    pico_keyboard_capture_event_t second;

    pico_keyboard_capture_init(&capture);
    CHECK(pico_keyboard_capture_push(&capture, 100u, release, sizeof(release)));
    CHECK(pico_keyboard_capture_push(&capture, 101u, release, sizeof(release)));
    CHECK(pico_keyboard_capture_pop(&capture, &first));
    CHECK(pico_keyboard_capture_pop(&capture, &second));
    CHECK(first.timestamp_us == 100u);
    CHECK(second.timestamp_us == 101u);
    CHECK(memcmp(&first.report, &second.report, sizeof(first.report)) == 0);

    const pico_keyboard_capture_stats_t stats =
        pico_keyboard_capture_get_stats(&capture);
    CHECK(stats.accepted == 2u);
    CHECK(stats.invalid == 0u);
    CHECK(stats.dropped == 0u);
}

static void test_rejects_invalid_input_without_partial_event(void) {
    pico_keyboard_capture_t capture;
    const uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {0u};
    pico_keyboard_capture_event_t event;

    pico_keyboard_capture_init(&capture);
    CHECK(!pico_keyboard_capture_push(
        &capture, 1u, NULL, PICO_HID_BOOT_KEYBOARD_REPORT_LEN));
    CHECK(!pico_keyboard_capture_push(
        &capture, 2u, report, PICO_HID_BOOT_KEYBOARD_REPORT_LEN - 1u));
    CHECK(!pico_keyboard_capture_push(
        &capture, 3u, report, PICO_HID_BOOT_KEYBOARD_REPORT_LEN + 1u));
    CHECK(!pico_keyboard_capture_pop(&capture, &event));

    const pico_keyboard_capture_stats_t stats =
        pico_keyboard_capture_get_stats(&capture);
    CHECK(stats.accepted == 0u);
    CHECK(stats.invalid == 3u);
    CHECK(stats.dropped == 0u);
}

static void test_bounded_fifo_order_and_drop_accounting(void) {
    pico_keyboard_capture_t capture;
    uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {0u};
    pico_keyboard_capture_event_t event;

    pico_keyboard_capture_init(&capture);
    for (size_t index = 0u; index < PICO_KEYBOARD_CAPTURE_CAPACITY; ++index) {
        report[2] = (uint8_t)index;
        CHECK(pico_keyboard_capture_push(
            &capture, (uint64_t)index, report, sizeof(report)));
    }
    CHECK(!pico_keyboard_capture_push(&capture, 999u, report, sizeof(report)));

    for (size_t index = 0u; index < PICO_KEYBOARD_CAPTURE_CAPACITY; ++index) {
        CHECK(pico_keyboard_capture_pop(&capture, &event));
        CHECK(event.timestamp_us == (uint64_t)index);
        CHECK(event.report.keycode[0] == (uint8_t)index);
    }

    const pico_keyboard_capture_stats_t stats =
        pico_keyboard_capture_get_stats(&capture);
    CHECK(stats.accepted == PICO_KEYBOARD_CAPTURE_CAPACITY);
    CHECK(stats.invalid == 0u);
    CHECK(stats.dropped == 1u);
}

static void test_head_recorded_resets_on_pop_and_clear(void) {
    pico_keyboard_capture_t capture;
    const uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {0u};
    pico_keyboard_capture_event_t event;

    pico_keyboard_capture_init(&capture);
    CHECK(!pico_keyboard_capture_head_recorded(&capture));
    pico_keyboard_capture_mark_head_recorded(&capture);
    CHECK(!pico_keyboard_capture_head_recorded(&capture));

    CHECK(pico_keyboard_capture_push(&capture, 1u, report, sizeof(report)));
    CHECK(!pico_keyboard_capture_head_recorded(&capture));
    pico_keyboard_capture_mark_head_recorded(&capture);
    CHECK(pico_keyboard_capture_head_recorded(&capture));

    // A second push behind the still-unpopped head must not disturb the
    // head's recorded flag.
    CHECK(pico_keyboard_capture_push(&capture, 2u, report, sizeof(report)));
    CHECK(pico_keyboard_capture_head_recorded(&capture));

    // Popping the recorded head advances the FIFO to the next (unrecorded)
    // event, so the flag must clear even though the FIFO is not empty.
    CHECK(pico_keyboard_capture_pop(&capture, &event));
    CHECK(event.timestamp_us == 1u);
    CHECK(!pico_keyboard_capture_head_recorded(&capture));

    pico_keyboard_capture_mark_head_recorded(&capture);
    CHECK(pico_keyboard_capture_head_recorded(&capture));
    pico_keyboard_capture_clear(&capture);
    CHECK(!pico_keyboard_capture_head_recorded(&capture));
}

int main(void) {
    test_preserves_report_and_timestamp();
    test_keeps_release_and_duplicate_reports();
    test_rejects_invalid_input_without_partial_event();
    test_bounded_fifo_order_and_drop_accounting();
    test_head_recorded_resets_on_pop_and_clear();
    return failures == 0 ? 0 : 1;
}
