#include "playback_queue.h"

#include <string.h>

void pico_playback_queue_init(pico_playback_queue_t *queue) {
    if (queue == NULL) {
        return;
    }

    memset(queue, 0, sizeof(*queue));
}

void pico_playback_queue_clear(pico_playback_queue_t *queue) {
    if (queue == NULL) {
        return;
    }
    queue->read_index = 0u;
    queue->write_index = 0u;
    queue->count = 0u;
}

bool pico_playback_queue_push(
    pico_playback_queue_t *queue,
    uint64_t offset_us,
    const uint8_t *report,
    size_t report_len) {
    if (queue == NULL || report == NULL ||
        report_len != PICO_HID_BOOT_KEYBOARD_REPORT_LEN) {
        return false;
    }

    if (queue->count == PICO_PLAYBACK_QUEUE_CAPACITY) {
        return false;
    }

    pico_playback_queue_event_t *event = &queue->events[queue->write_index];
    event->offset_us = offset_us;
    event->report_len = (uint8_t)report_len;
    memcpy(&event->report, report, sizeof(event->report));

    queue->write_index =
        (queue->write_index + 1u) % PICO_PLAYBACK_QUEUE_CAPACITY;
    ++queue->count;
    return true;
}

bool pico_playback_queue_pop(
    pico_playback_queue_t *queue,
    pico_playback_queue_event_t *event) {
    if (queue == NULL || event == NULL || queue->count == 0u) {
        return false;
    }

    *event = queue->events[queue->read_index];
    queue->read_index = (queue->read_index + 1u) % PICO_PLAYBACK_QUEUE_CAPACITY;
    --queue->count;
    return true;
}

bool pico_playback_queue_peek(
    const pico_playback_queue_t *queue,
    pico_playback_queue_event_t *event) {
    if (queue == NULL || event == NULL || queue->count == 0u) {
        return false;
    }

    *event = queue->events[queue->read_index];
    return true;
}

size_t pico_playback_queue_count(const pico_playback_queue_t *queue) {
    return queue == NULL ? 0u : queue->count;
}

size_t pico_playback_queue_free_capacity(const pico_playback_queue_t *queue) {
    return queue == NULL ? 0u : PICO_PLAYBACK_QUEUE_CAPACITY - queue->count;
}
