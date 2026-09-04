#include "playback_queue.h"

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

static void test_preserves_offset_and_report(void) {
    pico_playback_queue_t queue;
    const uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {
        0x02u, 0x00u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x09u,
    };
    pico_playback_queue_event_t event;

    pico_playback_queue_init(&queue);
    CHECK(pico_playback_queue_push(
        &queue, UINT64_C(0x123456789abcdef0), report, sizeof(report)));
    CHECK(pico_playback_queue_peek(&queue, &event));
    CHECK(event.offset_us == UINT64_C(0x123456789abcdef0));
    CHECK(pico_playback_queue_pop(&queue, &event));
    CHECK(event.offset_us == UINT64_C(0x123456789abcdef0));
    CHECK(event.report_len == PICO_HID_BOOT_KEYBOARD_REPORT_LEN);
    CHECK(memcmp(&event.report, report, sizeof(report)) == 0);
    CHECK(!pico_playback_queue_pop(&queue, &event));
}

static void test_rejects_invalid_input_without_partial_event(void) {
    pico_playback_queue_t queue;
    const uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {0u};
    pico_playback_queue_event_t event;

    pico_playback_queue_init(&queue);
    CHECK(!pico_playback_queue_push(
        &queue, 1u, NULL, PICO_HID_BOOT_KEYBOARD_REPORT_LEN));
    CHECK(!pico_playback_queue_push(
        &queue, 2u, report, PICO_HID_BOOT_KEYBOARD_REPORT_LEN - 1u));
    CHECK(!pico_playback_queue_push(
        &queue, 3u, report, PICO_HID_BOOT_KEYBOARD_REPORT_LEN + 1u));
    CHECK(!pico_playback_queue_pop(&queue, &event));
    CHECK(pico_playback_queue_count(&queue) == 0u);
    CHECK(pico_playback_queue_free_capacity(&queue) ==
          PICO_PLAYBACK_QUEUE_CAPACITY);
}

static void test_bounded_fifo_order_and_capacity_accounting(void) {
    pico_playback_queue_t queue;
    uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {0u};
    pico_playback_queue_event_t event;

    pico_playback_queue_init(&queue);
    for (size_t index = 0u; index < PICO_PLAYBACK_QUEUE_CAPACITY; ++index) {
        report[2] = (uint8_t)index;
        CHECK(pico_playback_queue_push(
            &queue, (uint64_t)index, report, sizeof(report)));
        CHECK(pico_playback_queue_count(&queue) == index + 1u);
        CHECK(pico_playback_queue_free_capacity(&queue) ==
              PICO_PLAYBACK_QUEUE_CAPACITY - (index + 1u));
    }
    CHECK(!pico_playback_queue_push(&queue, 999u, report, sizeof(report)));
    CHECK(pico_playback_queue_count(&queue) == PICO_PLAYBACK_QUEUE_CAPACITY);
    CHECK(pico_playback_queue_free_capacity(&queue) == 0u);

    for (size_t index = 0u; index < PICO_PLAYBACK_QUEUE_CAPACITY; ++index) {
        CHECK(pico_playback_queue_pop(&queue, &event));
        CHECK(event.offset_us == (uint64_t)index);
        CHECK(event.report.keycode[0] == (uint8_t)index);
    }
    CHECK(pico_playback_queue_count(&queue) == 0u);
    CHECK(pico_playback_queue_free_capacity(&queue) ==
          PICO_PLAYBACK_QUEUE_CAPACITY);
}

static void test_clear_discards_queued_events(void) {
    pico_playback_queue_t queue;
    const uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {0u};
    pico_playback_queue_event_t event;

    pico_playback_queue_init(&queue);
    CHECK(pico_playback_queue_push(&queue, 1u, report, sizeof(report)));
    CHECK(pico_playback_queue_push(&queue, 2u, report, sizeof(report)));
    pico_playback_queue_clear(&queue);
    CHECK(pico_playback_queue_count(&queue) == 0u);
    CHECK(!pico_playback_queue_peek(&queue, &event));
    CHECK(!pico_playback_queue_pop(&queue, &event));

    // The ring must be reusable from a clean slate after clearing.
    CHECK(pico_playback_queue_push(&queue, 3u, report, sizeof(report)));
    CHECK(pico_playback_queue_pop(&queue, &event));
    CHECK(event.offset_us == 3u);
}

int main(void) {
    test_preserves_offset_and_report();
    test_rejects_invalid_input_without_partial_event();
    test_bounded_fifo_order_and_capacity_accounting();
    test_clear_discards_queued_events();
    return failures == 0 ? 0 : 1;
}
