#include "playback_scheduler.h"

#include <string.h>

#include "uart_protocol.h"

// Interrupt context only: records that an armed deadline elapsed and returns
// immediately. Per AGENT.md, alarm callbacks must hand USB work off to the
// main loop rather than calling TinyUSB directly, so this does nothing else.
static int64_t alarm_isr(alarm_id_t id, void *user_data) {
    (void)id;
    pico_playback_scheduler_t *scheduler = (pico_playback_scheduler_t *)user_data;
    atomic_store_explicit(&scheduler->alarm_fired, true, memory_order_relaxed);
    // A nonzero return would ask the SDK to auto-reschedule this same alarm;
    // task() computes and arms each next deadline explicitly instead.
    return 0;
}

static void record_lateness(
    pico_playback_scheduler_t *scheduler,
    int32_t lateness_us) {
    if (scheduler->dispatched_count == 0u) {
        scheduler->min_lateness_us = lateness_us;
        scheduler->max_lateness_us = lateness_us;
    } else {
        if (lateness_us < scheduler->min_lateness_us) {
            scheduler->min_lateness_us = lateness_us;
        }
        if (lateness_us > scheduler->max_lateness_us) {
            scheduler->max_lateness_us = lateness_us;
        }
    }
    scheduler->sum_lateness_us += lateness_us;
    ++scheduler->dispatched_count;
    if (scheduler->sample_count < PICO_PLAYBACK_SCHEDULER_MAX_SAMPLES) {
        scheduler->lateness_samples[scheduler->sample_count] = lateness_us;
        ++scheduler->sample_count;
    } else {
        scheduler->samples_truncated = true;
    }
}

// Ascending insertion sort over at most PICO_PLAYBACK_SCHEDULER_MAX_SAMPLES
// entries. Runs once at the end of a playback run (never from task()'s hot
// path), so its worst-case O(n^2) cost is not a real-time concern.
static void sort_samples(int32_t *samples, uint32_t count) {
    for (uint32_t i = 1u; i < count; ++i) {
        const int32_t key = samples[i];
        uint32_t j = i;
        while (j > 0u && samples[j - 1u] > key) {
            samples[j] = samples[j - 1u];
            --j;
        }
        samples[j] = key;
    }
}

// Nearest-rank percentile: rank = ceil(p * count / 100), 1-based, into an
// ascending-sorted array. `sorted` must already be sorted; count == 0
// returns 0.
static int32_t percentile(const int32_t *sorted, uint32_t count, uint32_t p) {
    if (count == 0u) {
        return 0;
    }
    uint32_t rank = (p * count + 99u) / 100u;
    if (rank < 1u) {
        rank = 1u;
    }
    if (rank > count) {
        rank = count;
    }
    return sorted[rank - 1u];
}

static void finish(pico_playback_scheduler_t *scheduler, uint8_t reason) {
    scheduler->running = false;
    sort_samples(scheduler->lateness_samples, scheduler->sample_count);
    const pico_playback_scheduler_metrics_t metrics = {
        .dispatched_count = scheduler->dispatched_count,
        // Underruns are out of scope for Issue #7: under the current
        // QUEUE_END -> PLAY_READY -> PLAY_START contract the queue is always
        // a complete sequence by the time PLAY_START runs, so an empty queue
        // is always normal completion. This field is reserved for the
        // future Zero streaming feeder (Issue #9).
        .underrun_count = 0u,
        .min_lateness_us = scheduler->min_lateness_us,
        .max_lateness_us = scheduler->max_lateness_us,
        .sum_lateness_us = scheduler->sum_lateness_us,
        .p95_lateness_us =
            percentile(scheduler->lateness_samples, scheduler->sample_count, 95u),
        .p99_lateness_us =
            percentile(scheduler->lateness_samples, scheduler->sample_count, 99u),
        .samples_truncated = scheduler->samples_truncated,
    };
    if (scheduler->callbacks.on_complete != NULL) {
        scheduler->callbacks.on_complete(scheduler->callbacks.user, reason, &metrics);
    }
}

// Peeks the new queue head and either arms a hardware alarm for its deadline
// (still in the future) or leaves the alarm unarmed so task()'s time_us_64()
// polling check picks it up on the very next call (already due). Returns
// false when the queue is empty, having already called finish() with
// PICO_UART_REASON_FINISHED in that case.
static bool arm_next(pico_playback_scheduler_t *scheduler) {
    // task() can dispatch the current head event because time_us_64()
    // polling already caught up to its deadline, before that event's own
    // alarm ISR has actually run (e.g. while an IRQ-masking critical section
    // elsewhere delays it by a few microseconds). When that happens this
    // alarm is still armed in the hardware pool; cancel it before arming the
    // next one so its eventual, now-stale firing can never set alarm_fired
    // for a deadline it does not belong to and pull in a later dispatch
    // early. Cancelling an alarm that already fired is a documented no-op.
    if (scheduler->alarm_armed) {
        (void)cancel_alarm(scheduler->alarm_id);
        scheduler->alarm_armed = false;
    }
    pico_playback_queue_event_t event;
    if (!pico_playback_queue_peek(scheduler->queue, &event)) {
        finish(scheduler, PICO_UART_REASON_FINISHED);
        return false;
    }
    scheduler->next_deadline_us = scheduler->playback_start_us + event.offset_us;
    if (time_us_64() < scheduler->next_deadline_us) {
        const alarm_id_t id = add_alarm_at(
            from_us_since_boot(scheduler->next_deadline_us), alarm_isr,
            scheduler, true);
        if (id > 0) {
            scheduler->alarm_id = id;
            scheduler->alarm_armed = true;
        }
        // id <= 0 means the SDK could not arm a slot (or the deadline has
        // since passed): task()'s time_us_64() comparison is the fallback
        // "due" signal either way, so correctness does not depend on this.
    }
    return true;
}

void pico_playback_scheduler_init(
    pico_playback_scheduler_t *scheduler,
    pico_playback_queue_t *queue,
    const pico_playback_scheduler_callbacks_t *callbacks) {
    if (scheduler == NULL) {
        return;
    }
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->queue = queue;
    if (callbacks != NULL) {
        scheduler->callbacks = *callbacks;
    }
    atomic_init(&scheduler->alarm_fired, false);
}

void pico_playback_scheduler_start(
    pico_playback_scheduler_t *scheduler,
    uint64_t playback_start_us) {
    if (scheduler == NULL || scheduler->queue == NULL || scheduler->running) {
        return;
    }
    scheduler->running = true;
    scheduler->playback_start_us = playback_start_us;
    scheduler->next_deadline_us = playback_start_us;
    scheduler->alarm_armed = false;
    atomic_store_explicit(&scheduler->alarm_fired, false, memory_order_relaxed);
    scheduler->dispatched_count = 0u;
    scheduler->min_lateness_us = 0;
    scheduler->max_lateness_us = 0;
    scheduler->sum_lateness_us = 0;
    scheduler->sample_count = 0u;
    scheduler->samples_truncated = false;
    (void)arm_next(scheduler);
}

void pico_playback_scheduler_stop(pico_playback_scheduler_t *scheduler) {
    if (scheduler == NULL || !scheduler->running) {
        return;
    }
    if (scheduler->alarm_armed) {
        (void)cancel_alarm(scheduler->alarm_id);
        scheduler->alarm_armed = false;
    }
    atomic_store_explicit(&scheduler->alarm_fired, false, memory_order_relaxed);
    finish(scheduler, PICO_UART_REASON_ABORTED);
}

void pico_playback_scheduler_task(pico_playback_scheduler_t *scheduler) {
    if (scheduler == NULL) {
        return;
    }
    while (scheduler->running) {
        const bool fired = atomic_exchange_explicit(
            &scheduler->alarm_fired, false, memory_order_relaxed);
        if (!fired && time_us_64() < scheduler->next_deadline_us) {
            return;
        }
        pico_playback_queue_event_t event;
        if (!pico_playback_queue_peek(scheduler->queue, &event)) {
            // Defensive: due but nothing queued. Treat like natural
            // completion rather than spinning.
            finish(scheduler, PICO_UART_REASON_FINISHED);
            return;
        }
        if (scheduler->callbacks.send_report == NULL ||
            !scheduler->callbacks.send_report(
                scheduler->callbacks.user, &event.report)) {
            // HID endpoint not ready: retry this same head event next call,
            // mirroring pico_physical_report_dispatch's PASS retry pattern.
            // Do not pop and do not recompute the deadline.
            return;
        }
        (void)pico_playback_queue_pop(scheduler->queue, &event);
        const uint64_t dispatch_time_us = time_us_64();
        const int64_t lateness_us =
            (int64_t)dispatch_time_us - (int64_t)scheduler->next_deadline_us;
        record_lateness(scheduler, (int32_t)lateness_us);
        if (!arm_next(scheduler)) {
            return;  // Queue drained: arm_next() already called finish().
        }
        // If the next deadline has already passed, loop immediately instead
        // of waiting for an alarm, so a late dispatch never shifts later
        // deadlines.
    }
}

bool pico_playback_scheduler_is_running(
    const pico_playback_scheduler_t *scheduler) {
    return scheduler != NULL && scheduler->running;
}
