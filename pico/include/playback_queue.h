#ifndef PICO_KEYBOARD_RT_PLAYBACK_QUEUE_H
#define PICO_KEYBOARD_RT_PLAYBACK_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_boot_keyboard.h"

enum {
    // 512 entries is ~8.5KB of RP2350 SRAM (520KB total), well within budget
    // for a streaming flow-control window. This is not sized to hold whole
    // recordings; the Zero feeder (a future phase) streams events against
    // BUFFER_STATUS credits instead of requiring the full sequence to fit.
    PICO_PLAYBACK_QUEUE_CAPACITY = 512u,
};

typedef struct {
    uint64_t offset_us;
    uint8_t report_len;
    pico_hid_boot_keyboard_report_t report;
} pico_playback_queue_event_t;

// TinyUSB host/UART callbacks and the (future) scheduler consumer all run
// from the main loop in this phase, so this bounded FIFO deliberately has no
// cross-core synchronization.
typedef struct {
    pico_playback_queue_event_t events[PICO_PLAYBACK_QUEUE_CAPACITY];
    size_t read_index;
    size_t write_index;
    size_t count;
} pico_playback_queue_t;

void pico_playback_queue_init(pico_playback_queue_t *queue);
void pico_playback_queue_clear(pico_playback_queue_t *queue);

// Enqueues one future event at an absolute offset from the Pico playback
// epoch. Returns false for invalid input or when the queue is full.
bool pico_playback_queue_push(
    pico_playback_queue_t *queue,
    uint64_t offset_us,
    const uint8_t *report,
    size_t report_len);

bool pico_playback_queue_pop(
    pico_playback_queue_t *queue,
    pico_playback_queue_event_t *event);

bool pico_playback_queue_peek(
    const pico_playback_queue_t *queue,
    pico_playback_queue_event_t *event);

size_t pico_playback_queue_count(const pico_playback_queue_t *queue);
size_t pico_playback_queue_free_capacity(const pico_playback_queue_t *queue);

#endif  // PICO_KEYBOARD_RT_PLAYBACK_QUEUE_H
