#ifndef PICO_KEYBOARD_RT_PLAYBACK_TEST_SOURCE_H
#define PICO_KEYBOARD_RT_PLAYBACK_TEST_SOURCE_H

#include "mode_state.h"
#include "playback_queue.h"
#include "playback_scheduler.h"

// Self-contained PICO_PLAYBACK_SCHED_TEST acceptance benchmark. Drives the
// production PASS -> ARMED -> PLAY_START -> scheduler-drain -> PLAY_FINISHED
// code path end-to-end through the same internal C APIs QUEUE_EVENT and
// PLAY_START would otherwise reach over UART (pico_playback_queue_push,
// pico_mode_state_handle_mode_set/play_start, pico_playback_scheduler_start),
// without any UART input. This exists only because the 1000-event/10ms
// acceptance target exceeds the fixed 512-entry playback queue capacity and
// real UART-fed streaming refill is Issue #9's scope, not this one's: this
// module keeps the queue topped up from its own synthetic event source as
// the scheduler drains it, so the production dispatch, mode-state, and
// metrics code is exercised at the full 1000-event scale. No test-only UART
// command is introduced anywhere in this path.
void pico_playback_test_source_init(
    pico_mode_state_t *mode,
    pico_playback_queue_t *queue,
    pico_playback_scheduler_t *scheduler);

// Must be called unconditionally on every main-loop iteration once
// initialized (mirroring hid_demo_test_task's usage). No-op once the
// benchmark's full synthetic event count has been queued.
void pico_playback_test_source_task(void);

#endif  // PICO_KEYBOARD_RT_PLAYBACK_TEST_SOURCE_H
