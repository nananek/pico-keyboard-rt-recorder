#include "playback_scheduler.h"

#include <stdio.h>
#include <string.h>

#include "playback_queue.h"
#include "uart_protocol.h"

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

/* ---- pico/time.h alarm API fake ----
 *
 * Mirrors the real pico-sdk hardware alarm pool closely enough to exercise
 * playback_scheduler.c's ISR<->main-loop handoff: add_alarm_at() records one
 * pending alarm (this project only ever has one playback alarm outstanding
 * at a time), and a test-only fire_fake_alarm_if_due() simulates the timer
 * IRQ invoking that alarm's callback once fake_timestamp_us reaches its
 * deadline, exactly as real hardware would.
 */
static uint64_t fake_timestamp_us;
static int32_t next_alarm_id;
static bool fake_alarm_pending;
static alarm_id_t fake_alarm_id;
static uint64_t fake_alarm_deadline_us;
static alarm_callback_t fake_alarm_callback;
static void *fake_alarm_user_data;
static uint32_t add_alarm_calls;
static uint32_t cancel_alarm_calls;

uint64_t time_us_64(void) {
    return fake_timestamp_us;
}

absolute_time_t from_us_since_boot(uint64_t us_since_boot) {
    absolute_time_t t;
    t._private_us_since_boot = us_since_boot;
    return t;
}

alarm_id_t add_alarm_at(
    absolute_time_t time,
    alarm_callback_t callback,
    void *user_data,
    bool fire_if_past) {
    ++add_alarm_calls;
    ++next_alarm_id;
    fake_alarm_pending = true;
    fake_alarm_id = next_alarm_id;
    fake_alarm_deadline_us = time._private_us_since_boot;
    fake_alarm_callback = callback;
    fake_alarm_user_data = user_data;
    if (fire_if_past && fake_timestamp_us >= fake_alarm_deadline_us) {
        fake_alarm_pending = false;
        (void)callback(fake_alarm_id, user_data);
    }
    return fake_alarm_id;
}

bool cancel_alarm(alarm_id_t id) {
    ++cancel_alarm_calls;
    if (fake_alarm_pending && fake_alarm_id == id) {
        fake_alarm_pending = false;
        return true;
    }
    return false;
}

// Test-only: simulate the hardware timer IRQ actually elapsing.
static void fire_fake_alarm_if_due(void) {
    if (fake_alarm_pending && fake_timestamp_us >= fake_alarm_deadline_us) {
        fake_alarm_pending = false;
        (void)fake_alarm_callback(fake_alarm_id, fake_alarm_user_data);
    }
}

/* ---- scheduler callback fakes ---- */

enum { MAX_SENT_REPORTS = 4096 };

static bool send_ready;
static uint32_t send_calls;
static pico_hid_boot_keyboard_report_t sent_reports[MAX_SENT_REPORTS];
static uint32_t sent_count;

// When enabled, fake_send_report tops up `refill_queue` from a synthetic
// index source as a side effect of every successful send -- used only by
// the sample-truncation test to drive the scheduler past its fixed-capacity
// queue (512) up to a total exceeding PICO_PLAYBACK_SCHEDULER_MAX_SAMPLES,
// the same technique pico/src/playback_test_source.c uses in firmware.
static bool auto_refill_enabled;
static pico_playback_queue_t *refill_queue;
static uint32_t refill_target;
static uint64_t refill_interval_us;
static uint32_t refill_pushed;

static void push_event(
    pico_playback_queue_t *queue,
    uint64_t offset_us,
    uint8_t marker) {
    uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {0u};
    report[2] = marker;
    CHECK(pico_playback_queue_push(queue, offset_us, report, sizeof(report)));
}

static bool fake_send_report(
    void *user,
    const pico_hid_boot_keyboard_report_t *report) {
    (void)user;
    ++send_calls;
    if (!send_ready) {
        return false;
    }
    if (sent_count < MAX_SENT_REPORTS) {
        sent_reports[sent_count] = *report;
    }
    ++sent_count;
    if (auto_refill_enabled) {
        while (refill_pushed < refill_target &&
               pico_playback_queue_free_capacity(refill_queue) > 0u) {
            push_event(refill_queue, (uint64_t)refill_pushed * refill_interval_us, 0u);
            ++refill_pushed;
        }
    }
    return true;
}

static uint32_t complete_calls;
static uint8_t last_reason;
static pico_playback_scheduler_metrics_t last_metrics;

static void fake_on_complete(
    void *user,
    uint8_t reason,
    const pico_playback_scheduler_metrics_t *metrics) {
    (void)user;
    ++complete_calls;
    last_reason = reason;
    last_metrics = *metrics;
}

static void reset_all(void) {
    fake_timestamp_us = 0u;
    next_alarm_id = 0;
    fake_alarm_pending = false;
    fake_alarm_id = 0;
    fake_alarm_deadline_us = 0u;
    fake_alarm_callback = NULL;
    fake_alarm_user_data = NULL;
    add_alarm_calls = 0u;
    cancel_alarm_calls = 0u;

    send_ready = false;
    send_calls = 0u;
    sent_count = 0u;
    memset(sent_reports, 0, sizeof(sent_reports));

    auto_refill_enabled = false;
    refill_queue = NULL;
    refill_target = 0u;
    refill_interval_us = 0u;
    refill_pushed = 0u;

    complete_calls = 0u;
    last_reason = 0u;
    memset(&last_metrics, 0, sizeof(last_metrics));
}

static void init_scheduler(
    pico_playback_scheduler_t *scheduler,
    pico_playback_queue_t *queue) {
    const pico_playback_scheduler_callbacks_t callbacks = {
        .send_report = fake_send_report,
        .on_complete = fake_on_complete,
        .user = NULL,
    };
    pico_playback_scheduler_init(scheduler, queue, &callbacks);
}

static void test_normal_order_and_deadlines(void) {
    pico_playback_queue_t queue;
    pico_playback_scheduler_t scheduler;

    reset_all();
    send_ready = true;
    pico_playback_queue_init(&queue);
    push_event(&queue, 0u, 1u);
    push_event(&queue, 10000u, 2u);
    push_event(&queue, 20000u, 3u);
    init_scheduler(&scheduler, &queue);

    fake_timestamp_us = 1000000u;
    pico_playback_scheduler_start(&scheduler, fake_timestamp_us);
    CHECK(pico_playback_scheduler_is_running(&scheduler));

    // offset 0 is due immediately at the epoch.
    pico_playback_scheduler_task(&scheduler);
    CHECK(sent_count == 1u && sent_reports[0].keycode[0] == 1u);
    CHECK(add_alarm_calls == 1u);

    // Not yet due: no additional send.
    pico_playback_scheduler_task(&scheduler);
    CHECK(sent_count == 1u);

    // Advance to the second deadline and let the fake ISR fire, exercising
    // the alarm-flag "due" path.
    fake_timestamp_us += 10000u;
    fire_fake_alarm_if_due();
    pico_playback_scheduler_task(&scheduler);
    CHECK(sent_count == 2u && sent_reports[1].keycode[0] == 2u);

    // Advance past the third deadline WITHOUT firing the fake alarm: task()
    // must still catch it via its own time_us_64() comparison.
    fake_timestamp_us += 10000u;
    pico_playback_scheduler_task(&scheduler);
    CHECK(sent_count == 3u && sent_reports[2].keycode[0] == 3u);

    CHECK(complete_calls == 1u && last_reason == PICO_UART_REASON_FINISHED);
    CHECK(last_metrics.dispatched_count == 3u);
    CHECK(!pico_playback_scheduler_is_running(&scheduler));
}

static void test_late_run_does_not_shift_later_deadlines(void) {
    pico_playback_queue_t queue;
    pico_playback_scheduler_t scheduler;

    reset_all();
    send_ready = true;
    pico_playback_queue_init(&queue);
    push_event(&queue, 0u, 1u);
    push_event(&queue, 10000u, 2u);
    push_event(&queue, 20000u, 3u);
    init_scheduler(&scheduler, &queue);

    const uint64_t epoch = 5000000u;
    fake_timestamp_us = epoch;
    pico_playback_scheduler_start(&scheduler, epoch);

    // Simulate a long stall (e.g. USB busy elsewhere) that leaves all three
    // deadlines already elapsed before task() ever runs again.
    fake_timestamp_us = epoch + 50000u;
    pico_playback_scheduler_task(&scheduler);

    CHECK(sent_count == 3u);
    CHECK(sent_reports[0].keycode[0] == 1u);
    CHECK(sent_reports[1].keycode[0] == 2u);
    CHECK(sent_reports[2].keycode[0] == 3u);
    CHECK(complete_calls == 1u && last_reason == PICO_UART_REASON_FINISHED);
    // Each event's lateness is measured against its own fixed absolute
    // deadline (epoch + offset), not chained from the previous dispatch, so
    // the three lateness values are 50000/40000/30000, not equal.
    CHECK(last_metrics.dispatched_count == 3u);
    CHECK(last_metrics.min_lateness_us == 30000);
    CHECK(last_metrics.max_lateness_us == 50000);
    CHECK(last_metrics.sum_lateness_us == 50000 + 40000 + 30000);
}

static void test_hid_not_ready_retries_head_event_in_order(void) {
    pico_playback_queue_t queue;
    pico_playback_scheduler_t scheduler;

    reset_all();
    pico_playback_queue_init(&queue);
    push_event(&queue, 0u, 7u);
    push_event(&queue, 5000u, 8u);
    init_scheduler(&scheduler, &queue);

    fake_timestamp_us = 100u;
    send_ready = false;
    pico_playback_scheduler_start(&scheduler, fake_timestamp_us);

    // Due immediately, but HID is not ready: the head event must be retried
    // without being popped (playback_queue_peek's first real exercise).
    pico_playback_scheduler_task(&scheduler);
    CHECK(send_calls == 1u && sent_count == 0u);
    CHECK(pico_playback_queue_count(&queue) == 2u);

    pico_playback_scheduler_task(&scheduler);
    CHECK(send_calls == 2u && sent_count == 0u);
    CHECK(pico_playback_queue_count(&queue) == 2u);

    // HID becomes ready: the same head event is finally sent, still first.
    send_ready = true;
    pico_playback_scheduler_task(&scheduler);
    CHECK(send_calls == 3u && sent_count == 1u);
    CHECK(sent_reports[0].keycode[0] == 7u);
    CHECK(pico_playback_queue_count(&queue) == 1u);
    CHECK(complete_calls == 0u);

    // The second event is still scheduled at its own deadline.
    fake_timestamp_us = 5100u;
    pico_playback_scheduler_task(&scheduler);
    CHECK(sent_count == 2u && sent_reports[1].keycode[0] == 8u);
    CHECK(complete_calls == 1u && last_reason == PICO_UART_REASON_FINISHED);
}

static void test_arm_next_cancels_stale_pending_alarm(void) {
    pico_playback_queue_t queue;
    pico_playback_scheduler_t scheduler;

    reset_all();
    send_ready = true;
    pico_playback_queue_init(&queue);
    push_event(&queue, 0u, 1u);
    push_event(&queue, 1000u, 2u);
    push_event(&queue, 2000u, 3u);
    init_scheduler(&scheduler, &queue);

    fake_timestamp_us = 0u;
    pico_playback_scheduler_start(&scheduler, 0u);
    pico_playback_scheduler_task(&scheduler);
    CHECK(sent_count == 1u);
    CHECK(add_alarm_calls == 1u);

    // Advance straight to the second deadline without ever firing its fake
    // alarm (as test_normal_order_and_deadlines does for its last event):
    // task() dispatches it via the time_us_64() polling fallback while the
    // alarm armed for it is, per the fake's own bookkeeping, still pending.
    // Arming the third event's alarm right after must cancel that stale one
    // first, so its eventual (now irrelevant) firing can never set
    // alarm_fired for a deadline it does not belong to.
    fake_timestamp_us = 1000u;
    const uint32_t cancel_calls_before = cancel_alarm_calls;
    pico_playback_scheduler_task(&scheduler);
    CHECK(sent_count == 2u && sent_reports[1].keycode[0] == 2u);
    CHECK(cancel_alarm_calls == cancel_calls_before + 1u);

    fake_timestamp_us = 2000u;
    pico_playback_scheduler_task(&scheduler);
    CHECK(sent_count == 3u && sent_reports[2].keycode[0] == 3u);
    CHECK(complete_calls == 1u && last_reason == PICO_UART_REASON_FINISHED);
}

static void test_stop_reports_partial_metrics_as_aborted(void) {
    pico_playback_queue_t queue;
    pico_playback_scheduler_t scheduler;

    reset_all();
    send_ready = true;
    pico_playback_queue_init(&queue);
    push_event(&queue, 0u, 1u);
    push_event(&queue, 10000u, 2u);
    push_event(&queue, 20000u, 3u);
    init_scheduler(&scheduler, &queue);

    fake_timestamp_us = 0u;
    pico_playback_scheduler_start(&scheduler, 0u);
    pico_playback_scheduler_task(&scheduler);
    CHECK(sent_count == 1u);
    CHECK(add_alarm_calls == 1u);

    pico_playback_scheduler_stop(&scheduler);
    CHECK(!pico_playback_scheduler_is_running(&scheduler));
    CHECK(complete_calls == 1u && last_reason == PICO_UART_REASON_ABORTED);
    CHECK(last_metrics.dispatched_count == 1u);
    CHECK(cancel_alarm_calls == 1u);

    // stop() is a no-op once not running: on_complete must not fire again.
    pico_playback_scheduler_stop(&scheduler);
    CHECK(complete_calls == 1u);

    // task() after stop() must not dispatch the still-queued remainder.
    fake_timestamp_us = 1000000u;
    pico_playback_scheduler_task(&scheduler);
    CHECK(sent_count == 1u);
    CHECK(pico_playback_queue_count(&queue) == 2u);
}

static void test_metrics_statistics_match_hand_computed_values(void) {
    pico_playback_queue_t queue;
    pico_playback_scheduler_t scheduler;

    reset_all();
    send_ready = true;
    pico_playback_queue_init(&queue);
    const uint64_t interval = 100000u;
    const uint32_t count = 10u;
    for (uint32_t i = 0u; i < count; ++i) {
        push_event(&queue, (uint64_t)i * interval, (uint8_t)i);
    }
    init_scheduler(&scheduler, &queue);

    fake_timestamp_us = 0u;
    pico_playback_scheduler_start(&scheduler, 0u);

    // Hand-picked lateness values (us) dispatched one per task() call, each
    // well inside its own [i*interval, (i+1)*interval) window so no two
    // events are ever batched into the same call.
    const int32_t lateness[10] = {500, 100, 900, 300, 700, 200, 1000, 50, 400, 800};
    for (uint32_t i = 0u; i < count; ++i) {
        fake_timestamp_us = i * interval + (uint64_t)lateness[i];
        pico_playback_scheduler_task(&scheduler);
    }

    CHECK(sent_count == count);
    CHECK(complete_calls == 1u && last_reason == PICO_UART_REASON_FINISHED);
    CHECK(last_metrics.dispatched_count == count);
    // Sorted ascending: 50 100 200 300 400 500 700 800 900 1000.
    CHECK(last_metrics.min_lateness_us == 50);
    CHECK(last_metrics.max_lateness_us == 1000);
    CHECK(last_metrics.sum_lateness_us == 4950);
    // Nearest-rank, n=10: rank95 = ceil(9.5) = 10, rank99 = ceil(9.9) = 10 ->
    // both select the largest sample.
    CHECK(last_metrics.p95_lateness_us == 1000);
    CHECK(last_metrics.p99_lateness_us == 1000);
    CHECK(!last_metrics.samples_truncated);
}

static void test_samples_truncated_beyond_capacity(void) {
    pico_playback_queue_t queue;
    pico_playback_scheduler_t scheduler;

    reset_all();
    send_ready = true;
    pico_playback_queue_init(&queue);
    init_scheduler(&scheduler, &queue);

    refill_queue = &queue;
    refill_target = PICO_PLAYBACK_SCHEDULER_MAX_SAMPLES + 5u;
    refill_interval_us = 10u;
    refill_pushed = 0u;
    while (refill_pushed < refill_target &&
           pico_playback_queue_free_capacity(&queue) > 0u) {
        push_event(&queue, (uint64_t)refill_pushed * refill_interval_us, 0u);
        ++refill_pushed;
    }
    auto_refill_enabled = true;

    // Every deadline (at most refill_target * refill_interval_us) is already
    // in the past from this single fixed instant, so one task() call drains
    // the whole run: each successful send tops the queue back up (see
    // fake_send_report) until refill_target total events have been queued
    // and popped.
    fake_timestamp_us = (uint64_t)refill_target * refill_interval_us + 1000000u;
    pico_playback_scheduler_start(&scheduler, 0u);
    pico_playback_scheduler_task(&scheduler);
    auto_refill_enabled = false;

    CHECK(complete_calls == 1u && last_reason == PICO_UART_REASON_FINISHED);
    CHECK(last_metrics.dispatched_count == refill_target);
    CHECK(last_metrics.samples_truncated);
}

int main(void) {
    test_normal_order_and_deadlines();
    test_late_run_does_not_shift_later_deadlines();
    test_hid_not_ready_retries_head_event_in_order();
    test_arm_next_cancels_stale_pending_alarm();
    test_stop_reports_partial_metrics_as_aborted();
    test_metrics_statistics_match_hand_computed_values();
    test_samples_truncated_beyond_capacity();
    return failures == 0 ? 0 : 1;
}
