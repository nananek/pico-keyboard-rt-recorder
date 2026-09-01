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
#include "safety_release.h"
#include "uart_protocol.h"
#include "uart_transport.h"

static pico_keyboard_capture_t keyboard_capture;
static pico_uart_transport_t uart_transport;
static pico_mode_state_t mode_state;
static pico_safety_release_t safety_release;

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

static void mode_changed(void *user, uint8_t state, uint8_t reason) {
    pico_uart_transport_t *transport = (pico_uart_transport_t *)user;
    const uint8_t payload[2] = {state, reason};
    (void)pico_uart_transport_queue_frame(
        transport, PICO_UART_MODE_CHANGED, payload, sizeof(payload));
}

static void send_record_event(const pico_keyboard_capture_event_t *event) {
    uint8_t payload[17];
    uint64_t timestamp = event->timestamp_us;
    for (unsigned i = 0; i < 8u; ++i) {
        payload[i] = (uint8_t)(timestamp & 0xFFu);
        timestamp >>= 8;
    }
    payload[8] = event->report_len;
    memcpy(payload + 9u, &event->report, sizeof(event->report));
    (void)pico_uart_transport_queue_frame(
        &uart_transport, PICO_UART_RECORD_EVENT, payload, sizeof(payload));
}

static void drain_physical_reports(void) {
    pico_keyboard_capture_event_t event;
    while (pico_keyboard_capture_pop(&keyboard_capture, &event)) {
        if (pico_mode_state_is_recording(&mode_state)) {
            send_record_event(&event);
        } else if (pico_mode_state_get(&mode_state) == PICO_UART_MODE_PASS) {
            (void)pico_hid_keyboard_send_boot_report(&event.report);
        }
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

static void dispatch_command(const pico_uart_frame_t *frame) {
    switch (frame->type) {
        case PICO_UART_MODE_SET:
            (void)pico_mode_state_handle_mode_set(&mode_state, frame->payload[0]);
            break;
        case PICO_UART_PLAY_START:
            (void)pico_mode_state_play_start(&mode_state);
            break;
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
        default:
            /* Playback queue support is intentionally not enabled yet. */
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
    const pico_mode_state_callbacks_t callbacks = {
        .all_release = mode_all_release,
        .clear_physical = mode_clear_physical,
        .mode_changed = mode_changed,
        .user = &uart_transport,
    };
    pico_mode_state_init(&mode_state, &callbacks);
    pico_hid_keyboard_host_init(&keyboard_capture, &mode_state);
    pico_uart_transport_hw_init(&uart_transport);

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
        const bool physical_output_safe = pico_safety_release_service(
            &safety_release, send_all_keys_release, NULL);
        if (physical_output_safe) {
            drain_physical_reports();
        }
        pico_uart_transport_tx_task(&uart_transport);
#if PICO_HID_DEMO_TEST
        if (physical_output_safe) {
            hid_demo_test_task();
        }
#endif
        sleep_ms(1u);
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
