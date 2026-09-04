#ifndef PICO_KEYBOARD_RT_PLAYBACK_SCHEDULER_H
#define PICO_KEYBOARD_RT_PLAYBACK_SCHEDULER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "hid_boot_keyboard.h"
#include "pico/time.h"
#include "playback_queue.h"

enum {
    // Fixed-capacity raw lateness sample log for one playback run. 2048
    // int32_t entries is 8KB, comfortably covering the 1000-event acceptance
    // target (Issue #7) with headroom. Dispatches beyond this many still
    // update min/max/sum/count exactly in O(1); only the retained sample
    // prefix backs the end-of-run percentile calculation, and
    // samples_truncated records when that happened.
    PICO_PLAYBACK_SCHEDULER_MAX_SAMPLES = 2048u,
};

// Fixed-size RT metrics for one playback run, reported once in the
// PLAY_METRICS frame when the run ends (naturally or via abort). See
// docs/protocol.md for the wire payload this maps onto.
typedef struct {
    uint32_t dispatched_count;
    uint32_t underrun_count;
    int32_t min_lateness_us;
    int32_t max_lateness_us;
    int64_t sum_lateness_us;
    // Nearest-rank percentiles (rank = ceil(p * n / 100), 1-based, into the
    // ascending-sorted retained lateness samples) computed once at run end;
    // zero when dispatched_count is zero.
    int32_t p95_lateness_us;
    int32_t p99_lateness_us;
    bool samples_truncated;
} pico_playback_scheduler_metrics_t;

// Signature intentionally matches main.c's existing send_pass_report, so the
// same HID-send function can be reused verbatim as this callback.
typedef bool (*pico_playback_scheduler_send_t)(
    void *user,
    const pico_hid_boot_keyboard_report_t *report);

// Invoked exactly once per run, either when the queue drains naturally
// (reason == PICO_UART_REASON_FINISHED) or when the run is interrupted by
// pico_playback_scheduler_stop() before that (reason ==
// PICO_UART_REASON_ABORTED). metrics reflects only events dispatched before
// the run ended.
typedef void (*pico_playback_scheduler_complete_t)(
    void *user,
    uint8_t reason,
    const pico_playback_scheduler_metrics_t *metrics);

// Emitted once when a non-ended sequence first runs out of queued events.
// elapsed_offset_us is measured from the unchanged Pico playback epoch.
typedef void (*pico_playback_scheduler_underrun_t)(
    void *user,
    uint64_t elapsed_offset_us,
    uint16_t free_capacity);

// Emitted when a streaming queue transitions from full to having space, so
// Zero can obtain fresh flow-control credit without polling or scheduling.
typedef void (*pico_playback_scheduler_buffer_available_t)(void *user);

typedef struct {
    pico_playback_scheduler_send_t send_report;
    pico_playback_scheduler_complete_t on_complete;
    pico_playback_scheduler_underrun_t on_underrun;
    pico_playback_scheduler_buffer_available_t on_buffer_available;
    void *user;
} pico_playback_scheduler_callbacks_t;

typedef struct {
    pico_playback_queue_t *queue;
    pico_playback_scheduler_callbacks_t callbacks;

    bool running;
    bool sequence_ended;
    bool waiting_for_event;
    bool underrun_active;
    uint64_t playback_start_us;
    uint64_t next_deadline_us;

    // ISR<->main-loop handoff. The alarm callback runs in interrupt context
    // and must not call TinyUSB; it only records that a deadline elapsed.
    // The main loop's task() is the only place HID reports are sent from.
    atomic_bool alarm_fired;
    alarm_id_t alarm_id;
    bool alarm_armed;

    // O(1) running metrics, updated on every successful dispatch.
    uint32_t dispatched_count;
    uint32_t underrun_count;
    int32_t min_lateness_us;
    int32_t max_lateness_us;
    int64_t sum_lateness_us;

    // Raw sample log, sorted in place and reduced to percentiles once the
    // run ends. See PICO_PLAYBACK_SCHEDULER_MAX_SAMPLES above.
    int32_t lateness_samples[PICO_PLAYBACK_SCHEDULER_MAX_SAMPLES];
    uint32_t sample_count;
    bool samples_truncated;
} pico_playback_scheduler_t;

void pico_playback_scheduler_init(
    pico_playback_scheduler_t *scheduler,
    pico_playback_queue_t *queue,
    const pico_playback_scheduler_callbacks_t *callbacks);

// Starts draining `queue` against `playback_start_us` as the absolute
// playback epoch (every event's deadline is playback_start_us + its
// offset_us). Resets this run's metrics. No-op if already running.
void pico_playback_scheduler_start(
    pico_playback_scheduler_t *scheduler,
    uint64_t playback_start_us);

// Begins a newly loaded sequence. Call with the scheduler stopped when
// QUEUE_CLEAR (or a mode transition that discards the queue) is processed.
void pico_playback_scheduler_reset_sequence(
    pico_playback_scheduler_t *scheduler);

// Marks that no more QUEUE_EVENTs will arrive for the current sequence.
// May be called before PLAY_START or while the scheduler is running.
void pico_playback_scheduler_mark_sequence_ended(
    pico_playback_scheduler_t *scheduler);

// Cancels any armed hardware alarm and ends the run early, invoking
// on_complete with reason PICO_UART_REASON_ABORTED and the metrics collected
// so far. Does not touch the queue itself -- the caller is responsible for
// discarding it afterward. No-op if not running.
void pico_playback_scheduler_stop(pico_playback_scheduler_t *scheduler);

// Must be called unconditionally on every main-loop iteration, regardless of
// running state. Drains every event whose deadline is already due (via the
// alarm-fired flag or a direct time_us_64() comparison), retrying the same
// head event without popping it when send_report fails so order and content
// are preserved. Arms a single hardware alarm for the next deadline once one
// remains in the future. An empty ended sequence completes normally; an
// empty open sequence remains running and reports one underrun per empty
// interval while it waits for streaming refill.
void pico_playback_scheduler_task(pico_playback_scheduler_t *scheduler);

bool pico_playback_scheduler_is_running(
    const pico_playback_scheduler_t *scheduler);

#endif  // PICO_KEYBOARD_RT_PLAYBACK_SCHEDULER_H
