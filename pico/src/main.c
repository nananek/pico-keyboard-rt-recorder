#include "bsp/board.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "tusb.h"

#include "hid_keyboard_device.h"
#include "hid_keyboard_host.h"
#include "hid_boot_keyboard.h"
#include "hardware_config.h"
#include "keyboard_capture.h"
#include "mode_state.h"
#include "physical_report_dispatch.h"
#include "playback_queue.h"
#include "playback_scheduler.h"
#include "safety_release.h"
#include "uart_protocol.h"
#include "uart_transport.h"

#if PICO_PLAYBACK_SCHED_TEST
#include "playback_test_source.h"
#endif

static pico_keyboard_capture_t keyboard_capture;
static pico_uart_transport_t uart_transport;
static pico_mode_state_t mode_state;
static pico_safety_release_t safety_release;
static pico_playback_queue_t playback_queue;
static pico_playback_scheduler_t playback_scheduler;

static void mode_all_release(void *user) {
    (void)user;
    pico_safety_release_request(&safety_release);
}

static bool send_all_keys_release(void *user) {
    (void)user;
    return pico_hid_keyboard_send_all_keys_release();
}

static void mode_clear_physical(void *user) {
    (void)user;
    pico_keyboard_capture_clear(&keyboard_capture);
}

static uint64_t read_u64_le(const uint8_t *bytes) {
    uint64_t value = 0u;
    for (unsigned i = 0; i < 8u; ++i) {
        value |= (uint64_t)bytes[i] << (8u * i);
    }
    return value;
}

static void write_u64_le(uint8_t *bytes, uint64_t value) {
    for (unsigned i = 0; i < 8u; ++i) {
        bytes[i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
}

static void write_u32_le(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) {
        bytes[i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
}

static void mode_clear_queue(void *user) {
    (void)user;
    // Must run before the queue is discarded: stop() reports whatever
    // partial metrics this run collected (a no-op if the scheduler is not
    // currently running, e.g. PASS/RECORD/ARMED-from-PASS entry).
    pico_playback_scheduler_stop(&playback_scheduler);
    pico_playback_queue_clear(&playback_queue);
}

static void mode_changed(void *user, uint8_t state, uint8_t reason) {
    pico_uart_transport_t *transport = (pico_uart_transport_t *)user;
    const uint8_t payload[2] = {state, reason};
    (void)pico_uart_transport_queue_frame(
        transport, PICO_UART_MODE_CHANGED, payload, sizeof(payload));
}

static void send_record_event(
    void *user,
    const pico_keyboard_capture_event_t *event) {
    (void)user;
    uint8_t payload[17];
    write_u64_le(payload, event->timestamp_us);
    payload[8] = event->report_len;
    memcpy(payload + 9u, &event->report, sizeof(event->report));
    (void)pico_uart_transport_queue_frame(
        &uart_transport, PICO_UART_RECORD_EVENT, payload, sizeof(payload));
}

static bool send_pass_report(
    void *user,
    const pico_hid_boot_keyboard_report_t *report) {
    (void)user;
    return pico_hid_keyboard_send_boot_report(report);
}

// The scheduler's on_complete callback. Fires exactly once per playback run,
// either from playback_scheduler's task() (queue drained naturally, reason
// PICO_UART_REASON_FINISHED) or from its stop() (interrupted, reason
// PICO_UART_REASON_ABORTED -- reached via mode_clear_queue whenever PLAYING
// ends through PLAY_ABORT, a direct MODE_SET(PASS), or a fault entering
// ERROR). PLAY_FINISHED additionally drives the PLAYING -> ARMED transition,
// but that transition (and the MODE_CHANGED it sends) is deliberately
// deferred to the end of this function: the ABORTED path's MODE_CHANGED
// naturally arrives after PLAY_ABORTED/PLAY_METRICS (playback_complete runs
// from inside mode_clear_queue, called before play_abort's own notify()), so
// FINISHED queues its own PLAY_FINISHED/PLAY_METRICS first too, keeping both
// end-of-playback paths in the same wire order for Zero.
static void playback_complete(
    void *user,
    uint8_t reason,
    const pico_playback_scheduler_metrics_t *metrics) {
    pico_uart_transport_t *transport = (pico_uart_transport_t *)user;
    if (reason == PICO_UART_REASON_FINISHED) {
        (void)pico_uart_transport_queue_frame(
            transport, PICO_UART_PLAY_FINISHED, NULL, 0u);
    } else {
        (void)pico_uart_transport_queue_frame(
            transport, PICO_UART_PLAY_ABORTED, NULL, 0u);
    }
    uint8_t payload[33];
    write_u32_le(payload, metrics->dispatched_count);
    write_u32_le(payload + 4u, metrics->underrun_count);
    write_u32_le(payload + 8u, (uint32_t)metrics->min_lateness_us);
    write_u32_le(payload + 12u, (uint32_t)metrics->max_lateness_us);
    write_u64_le(payload + 16u, (uint64_t)metrics->sum_lateness_us);
    write_u32_le(payload + 24u, (uint32_t)metrics->p95_lateness_us);
    write_u32_le(payload + 28u, (uint32_t)metrics->p99_lateness_us);
    payload[32] = (uint8_t)metrics->samples_truncated;
    (void)pico_uart_transport_queue_frame(
        transport, PICO_UART_PLAY_METRICS, payload, sizeof(payload));
    if (reason == PICO_UART_REASON_FINISHED) {
        (void)pico_mode_state_play_finish(&mode_state);
    }
}

static void send_status(void) {
    const pico_uart_transport_stats_t stats =
        pico_uart_transport_get_stats(&uart_transport);
    const uint8_t payload[5] = {
        pico_mode_state_get(&mode_state),
        (uint8_t)(stats.rx_overflow != 0u),
        (uint8_t)(stats.hardware_errors != 0u),
        (uint8_t)(stats.invalid_frames != 0u),
        (uint8_t)(stats.tx_dropped != 0u),
    };
    (void)pico_uart_transport_queue_frame(
        &uart_transport, PICO_UART_PICO_STATUS, payload, sizeof(payload));
}

static bool send_buffer_status(void) {
    const uint16_t queued_count =
        (uint16_t)pico_playback_queue_count(&playback_queue);
    const uint16_t free_capacity =
        (uint16_t)pico_playback_queue_free_capacity(&playback_queue);
    const uint8_t payload[5] = {
        pico_mode_state_get(&mode_state),
        (uint8_t)(queued_count & 0xFFu),
        (uint8_t)(queued_count >> 8),
        (uint8_t)(free_capacity & 0xFFu),
        (uint8_t)(free_capacity >> 8),
    };
    return pico_uart_transport_queue_frame(
        &uart_transport, PICO_UART_BUFFER_STATUS, payload, sizeof(payload));
}

/* QUEUE_CLEAR/QUEUE_EVENT/QUEUE_END load the fixed-capacity playback queue.
 * They are accepted only in ARMED, matching where PLAY_START is accepted; any
 * other state is a protocol error, consistent with how this dispatcher
 * already treats unexpected commands. */
static void dispatch_queue_command(const pico_uart_frame_t *frame) {
    if (pico_mode_state_get(&mode_state) != PICO_UART_MODE_ARMED) {
        pico_mode_state_protocol_error(&mode_state);
        return;
    }
    switch (frame->type) {
        case PICO_UART_QUEUE_CLEAR:
            pico_playback_queue_clear(&playback_queue);
            if (!send_buffer_status()) {
                /* The TX ring is saturated: Zero will never see this ack.
                 * Enter ERROR rather than leave Zero waiting on a reply that
                 * was silently dropped. */
                pico_mode_state_protocol_error(&mode_state);
            }
            break;
        case PICO_UART_QUEUE_EVENT: {
            /* offset_us u64 LE, report_len u8 (8), then the 8-byte report:
             * the same 17-byte shape as RECORD_EVENT, mirrored in direction.
             * The transport layer already guarantees this shape (17 bytes,
             * payload[8] == 8) before a command reaches dispatch. */
            const uint64_t offset_us = read_u64_le(frame->payload);
            if (!pico_playback_queue_push(
                    &playback_queue, offset_us, frame->payload + 9u, 8u)) {
                /* The queue is full: Zero sent beyond the capacity most
                 * recently advertised in BUFFER_STATUS. Treat this like any
                 * other protocol contract violation. */
                pico_mode_state_protocol_error(&mode_state);
                break;
            }
            if (!send_buffer_status()) {
                /* The event was already accepted onto the queue but its ack
                 * was dropped by a saturated TX ring; Zero cannot tell the
                 * two apart from a real timeout, so force ERROR (which also
                 * discards the now-unconfirmed queue) instead of leaving
                 * Zero and Pico state silently diverged. */
                pico_mode_state_protocol_error(&mode_state);
            }
            break;
        }
        case PICO_UART_QUEUE_END:
            (void)pico_uart_transport_queue_frame(
                &uart_transport, PICO_UART_PLAY_READY, NULL, 0u);
            if (!send_buffer_status()) {
                pico_mode_state_protocol_error(&mode_state);
            }
            break;
        default:
            /* Unreachable today (only the three QUEUE_* cases route here),
             * but keep the same "unknown command is a protocol error"
             * guarantee dispatch_command provides, in case this dispatcher
             * is ever reused for another command type. */
            pico_mode_state_protocol_error(&mode_state);
            break;
    }
}

static void dispatch_command(const pico_uart_frame_t *frame) {
    switch (frame->type) {
        case PICO_UART_MODE_SET:
            (void)pico_mode_state_handle_mode_set(&mode_state, frame->payload[0]);
            break;
        case PICO_UART_PLAY_START: {
            // Sample the epoch once and hand the identical value to the
            // scheduler and to Zero, so Zero's future QUEUE_EVENT offsets and
            // the scheduler's deadlines agree on the same origin.
            const uint64_t playback_start_us = time_us_64();
            if (pico_mode_state_play_start(&mode_state)) {
                // Queue PLAY_STARTED before starting the scheduler: an
                // empty queue makes pico_playback_scheduler_start() finish
                // the run synchronously (PLAY_FINISHED/PLAY_METRICS/
                // MODE_CHANGED), and that must not reach the wire ahead of
                // PLAY_STARTED for the same run.
                uint8_t payload[8];
                write_u64_le(payload, playback_start_us);
                (void)pico_uart_transport_queue_frame(
                    &uart_transport, PICO_UART_PLAY_STARTED, payload,
                    sizeof(payload));
                pico_playback_scheduler_start(&playback_scheduler, playback_start_us);
            }
            break;
        }
        case PICO_UART_PLAY_ABORT:
            (void)pico_mode_state_play_abort(&mode_state);
            break;
        case PICO_UART_PING:
            (void)pico_uart_transport_queue_frame(
                &uart_transport, PICO_UART_PONG, frame->payload,
                frame->payload_len);
            break;
        case PICO_UART_STATUS_REQUEST:
            send_status();
            break;
        case PICO_UART_QUEUE_CLEAR:
        case PICO_UART_QUEUE_EVENT:
        case PICO_UART_QUEUE_END:
            dispatch_queue_command(frame);
            break;
        default:
            pico_mode_state_protocol_error(&mode_state);
            break;
    }
}

#if PICO_HID_DEMO_TEST
static void hid_demo_test_task(void) {
    enum {
        HID_DEMO_WAIT_FOR_ENDPOINT,
        HID_DEMO_WAIT_TO_RELEASE,
        HID_DEMO_COMPLETE,
    };
    static uint8_t stage = HID_DEMO_WAIT_FOR_ENDPOINT;
    static uint32_t release_at_ms;

    if (stage == HID_DEMO_COMPLETE || !tud_mounted() ||
        pico_mode_state_get(&mode_state) != PICO_UART_MODE_PASS) {
        return;
    }
    if (stage == HID_DEMO_WAIT_FOR_ENDPOINT) {
        pico_hid_boot_keyboard_report_t press_report;
        pico_hid_boot_keyboard_make_key_press(&press_report, 0u, PICO_HID_USAGE_KEY_A);
        if (pico_hid_keyboard_send_boot_report(&press_report)) {
            release_at_ms = to_ms_since_boot(get_absolute_time()) + 50u;
            stage = HID_DEMO_WAIT_TO_RELEASE;
        }
        return;
    }
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now_ms - release_at_ms) >= 0 &&
        pico_hid_keyboard_send_all_keys_release()) {
        stage = HID_DEMO_COMPLETE;
    }
}
#endif

int main(void) {
    board_init();
    pico_keyboard_capture_init(&keyboard_capture);
    pico_uart_transport_init(&uart_transport);
    pico_safety_release_init(&safety_release);
    pico_playback_queue_init(&playback_queue);
    const pico_mode_state_callbacks_t callbacks = {
        .all_release = mode_all_release,
        .clear_physical = mode_clear_physical,
        .clear_queue = mode_clear_queue,
        .mode_changed = mode_changed,
        .user = &uart_transport,
    };
    pico_mode_state_init(&mode_state, &callbacks);
    const pico_playback_scheduler_callbacks_t scheduler_callbacks = {
        .send_report = send_pass_report,
        .on_complete = playback_complete,
        .user = &uart_transport,
    };
    pico_playback_scheduler_init(&playback_scheduler, &playback_queue, &scheduler_callbacks);
    pico_hid_keyboard_host_init(&keyboard_capture, &mode_state);
    pico_uart_transport_hw_init(&uart_transport);
#if PICO_PLAYBACK_SCHED_TEST
    pico_playback_test_source_init(&mode_state, &playback_queue, &playback_scheduler);
#endif

    const tusb_rhport_init_t device_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    const tusb_rhport_init_t host_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_FULL,
    };
    if (!tusb_init(PICO_KEYBOARD_NATIVE_DEVICE_RHPORT, &device_init)) {
        panic("TinyUSB native device initialization failed");
    }
    if (!tusb_init(PICO_KEYBOARD_PIO_HOST_RHPORT, &host_init)) {
        panic("TinyUSB PIO host initialization failed");
    }

    while (true) {
        tud_task();
        tuh_task();
        pico_uart_transport_poll(&uart_transport);
        if (pico_uart_transport_take_fault(&uart_transport)) {
            pico_mode_state_uart_fault(&mode_state);
        }
        pico_uart_frame_t command;
        while (pico_uart_transport_pop_command(&uart_transport, &command)) {
            dispatch_command(&command);
        }
        const pico_safety_release_result_t release_result =
            pico_safety_release_service(
            &safety_release, send_all_keys_release, NULL);
        pico_physical_report_dispatch(
            &keyboard_capture, &mode_state,
            release_result == PICO_SAFETY_RELEASE_READY, send_pass_report,
            send_record_event, NULL);
        pico_playback_scheduler_task(&playback_scheduler);
        pico_uart_transport_tx_task(&uart_transport);
#if PICO_HID_DEMO_TEST
        if (release_result == PICO_SAFETY_RELEASE_READY) {
            hid_demo_test_task();
        }
#endif
#if PICO_PLAYBACK_SCHED_TEST
        pico_playback_test_source_task();
#endif
        // PLAYING needs sub-millisecond scheduling precision, so it busy-polls
        // instead of sleeping; every other state keeps the previous 1 ms
        // sleep. This trades 100% CPU usage during playback for the <1 ms
        // lateness target (see docs/realtime-design.md).
        if (pico_mode_state_get(&mode_state) == PICO_UART_MODE_PLAYING) {
            tight_loop_contents();
        } else {
            sleep_ms(1u);
        }
    }
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t *buffer,
    uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    const uint8_t *buffer,
    uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}
