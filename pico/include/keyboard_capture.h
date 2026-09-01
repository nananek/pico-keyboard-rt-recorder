#ifndef PICO_KEYBOARD_RT_KEYBOARD_CAPTURE_H
#define PICO_KEYBOARD_RT_KEYBOARD_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_boot_keyboard.h"

enum {
    PICO_KEYBOARD_CAPTURE_CAPACITY = 16u,
};

typedef struct {
    uint64_t timestamp_us;
    uint8_t report_len;
    pico_hid_boot_keyboard_report_t report;
} pico_keyboard_capture_event_t;

typedef struct {
    uint32_t accepted;
    uint32_t invalid;
    uint32_t dropped;
} pico_keyboard_capture_stats_t;

// TinyUSB host callbacks and the consumer both run from the main loop in this
// phase, so this bounded FIFO deliberately has no cross-core synchronization.
typedef struct {
    pico_keyboard_capture_event_t events[PICO_KEYBOARD_CAPTURE_CAPACITY];
    size_t read_index;
    size_t write_index;
    size_t count;
    pico_keyboard_capture_stats_t stats;
} pico_keyboard_capture_t;

void pico_keyboard_capture_init(pico_keyboard_capture_t *capture);
void pico_keyboard_capture_clear(pico_keyboard_capture_t *capture);

// Records every valid report, including duplicates and all-zero releases.
// Returns false for invalid input or when the bounded handoff is full.
bool pico_keyboard_capture_push(
    pico_keyboard_capture_t *capture,
    uint64_t timestamp_us,
    const uint8_t *report,
    size_t report_len);

bool pico_keyboard_capture_pop(
    pico_keyboard_capture_t *capture,
    pico_keyboard_capture_event_t *event);

// Copies, but does not consume, the oldest accepted physical report. This lets
// PASS keep a report queued until the native HID endpoint accepts it.
bool pico_keyboard_capture_peek(
    const pico_keyboard_capture_t *capture,
    pico_keyboard_capture_event_t *event);

pico_keyboard_capture_stats_t pico_keyboard_capture_get_stats(
    const pico_keyboard_capture_t *capture);

#endif  // PICO_KEYBOARD_RT_KEYBOARD_CAPTURE_H
