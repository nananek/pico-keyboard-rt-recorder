#include "keyboard_capture.h"

#include <string.h>

void pico_keyboard_capture_init(pico_keyboard_capture_t *capture) {
    if (capture == NULL) {
        return;
    }

    memset(capture, 0, sizeof(*capture));
}

void pico_keyboard_capture_clear(pico_keyboard_capture_t *capture) {
    if (capture == NULL) {
        return;
    }
    capture->read_index = 0u;
    capture->write_index = 0u;
    capture->count = 0u;
}

bool pico_keyboard_capture_push(
    pico_keyboard_capture_t *capture,
    uint64_t timestamp_us,
    const uint8_t *report,
    size_t report_len) {
    if (capture == NULL) {
        return false;
    }

    if (report == NULL || report_len != PICO_HID_BOOT_KEYBOARD_REPORT_LEN) {
        ++capture->stats.invalid;
        return false;
    }

    if (capture->count == PICO_KEYBOARD_CAPTURE_CAPACITY) {
        ++capture->stats.dropped;
        return false;
    }

    pico_keyboard_capture_event_t *event =
        &capture->events[capture->write_index];
    event->timestamp_us = timestamp_us;
    event->report_len = (uint8_t)report_len;
    memcpy(&event->report, report, sizeof(event->report));

    capture->write_index =
        (capture->write_index + 1u) % PICO_KEYBOARD_CAPTURE_CAPACITY;
    ++capture->count;
    ++capture->stats.accepted;
    return true;
}

bool pico_keyboard_capture_pop(
    pico_keyboard_capture_t *capture,
    pico_keyboard_capture_event_t *event) {
    if (capture == NULL || event == NULL || capture->count == 0u) {
        return false;
    }

    *event = capture->events[capture->read_index];
    capture->read_index =
        (capture->read_index + 1u) % PICO_KEYBOARD_CAPTURE_CAPACITY;
    --capture->count;
    return true;
}

pico_keyboard_capture_stats_t pico_keyboard_capture_get_stats(
    const pico_keyboard_capture_t *capture) {
    if (capture == NULL) {
        const pico_keyboard_capture_stats_t empty = {0u, 0u, 0u};
        return empty;
    }

    return capture->stats;
}
