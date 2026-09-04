#include "playback_test_source.h"

#if PICO_PLAYBACK_SCHED_TEST

#include "pico/time.h"
#include "tusb.h"

#include "hid_boot_keyboard.h"
#include "uart_protocol.h"

enum {
    // Acceptance target from Issue #7: 1000 events, one every 10ms. This
    // exceeds PICO_PLAYBACK_QUEUE_CAPACITY (512), so this source must keep
    // refilling the queue as the scheduler drains it rather than loading
    // everything up front.
    PICO_PLAYBACK_TEST_TOTAL_EVENTS = 1000u,
    PICO_PLAYBACK_TEST_INTERVAL_US = 10000u,
};

enum {
    PICO_PLAYBACK_TEST_WAIT_FOR_MOUNT,
    PICO_PLAYBACK_TEST_RUNNING,
    PICO_PLAYBACK_TEST_DONE,
};

static pico_mode_state_t *test_mode;
static pico_playback_queue_t *test_queue;
static pico_playback_scheduler_t *test_scheduler;
static uint8_t test_stage;
static uint32_t test_pushed_count;

static void push_synthetic_event(uint32_t index) {
    pico_hid_boot_keyboard_report_t report;
    // Alternate press/release so the benchmark exercises a plausible report
    // sequence rather than repeating one static payload; scheduler timing,
    // not HID content, is what this benchmark is measuring.
    if ((index & 1u) == 0u) {
        pico_hid_boot_keyboard_make_key_press(&report, 0u, PICO_HID_USAGE_KEY_A);
    } else {
        pico_hid_boot_keyboard_release_all(&report);
    }
    (void)pico_playback_queue_push(
        test_queue, (uint64_t)index * PICO_PLAYBACK_TEST_INTERVAL_US,
        (const uint8_t *)&report, sizeof(report));
    ++test_pushed_count;
}

static void fill_available_slots(void) {
    while (test_pushed_count < PICO_PLAYBACK_TEST_TOTAL_EVENTS &&
           pico_playback_queue_free_capacity(test_queue) > 0u) {
        push_synthetic_event(test_pushed_count);
    }
}

void pico_playback_test_source_init(
    pico_mode_state_t *mode,
    pico_playback_queue_t *queue,
    pico_playback_scheduler_t *scheduler) {
    test_mode = mode;
    test_queue = queue;
    test_scheduler = scheduler;
    test_stage = PICO_PLAYBACK_TEST_WAIT_FOR_MOUNT;
    test_pushed_count = 0u;
}

void pico_playback_test_source_task(void) {
    if (test_stage == PICO_PLAYBACK_TEST_DONE || test_mode == NULL) {
        return;
    }
    if (test_stage == PICO_PLAYBACK_TEST_WAIT_FOR_MOUNT) {
        // Wait for native USB enumeration, matching hid_demo_test_task, so
        // the first synthetic dispatches are not spent retrying against a
        // not-yet-mounted HID endpoint before playback_start_us is sampled.
        if (!tud_mounted()) {
            return;
        }
        if (!pico_mode_state_handle_mode_set(test_mode, PICO_UART_MODE_ARMED)) {
            return;
        }
        fill_available_slots();
        // Mirrors dispatch_command's PLAY_START handling exactly: one
        // playback_start_us sample feeds both the mode transition and the
        // scheduler's epoch.
        const uint64_t playback_start_us = time_us_64();
        if (!pico_mode_state_play_start(test_mode)) {
            return;
        }
        pico_playback_scheduler_start(test_scheduler, playback_start_us);
        test_stage = PICO_PLAYBACK_TEST_RUNNING;
        return;
    }
    // PICO_PLAYBACK_TEST_RUNNING: keep the queue topped up from the tail as
    // the scheduler drains its head, until all synthetic events have been
    // queued. Once drained, the scheduler's own on_complete path reports
    // PLAY_FINISHED + PLAY_METRICS over UART0 exactly as a real PLAY_START
    // would; this module has no reporting logic of its own.
    if (test_pushed_count < PICO_PLAYBACK_TEST_TOTAL_EVENTS) {
        fill_available_slots();
        if (test_pushed_count >= PICO_PLAYBACK_TEST_TOTAL_EVENTS) {
            test_stage = PICO_PLAYBACK_TEST_DONE;
        }
    } else {
        test_stage = PICO_PLAYBACK_TEST_DONE;
    }
}

#endif  // PICO_PLAYBACK_SCHED_TEST
