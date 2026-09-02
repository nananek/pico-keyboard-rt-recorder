#include "physical_report_dispatch.h"

void pico_physical_report_dispatch(
    pico_keyboard_capture_t *capture,
    const pico_mode_state_t *mode,
    bool pass_output_ready,
    pico_physical_report_send_t send_pass_report,
    pico_record_event_send_t send_record_event,
    void *user) {
    pico_keyboard_capture_event_t event;

    if (pico_mode_state_is_recording(mode)) {
        if (send_record_event == NULL) {
            return;
        }
        while (pico_keyboard_capture_pop(capture, &event)) {
            send_record_event(user, &event);
        }
        return;
    }

    if (!pass_output_ready || pico_mode_state_get(mode) != PICO_UART_MODE_PASS ||
        send_pass_report == NULL) {
        return;
    }
    while (pico_keyboard_capture_peek(capture, &event)) {
        if (!send_pass_report(user, &event.report)) {
            return;
        }
        // No producer runs concurrently with this main-loop consumer, so the
        // report just accepted by HID is still the head to consume.
        (void)pico_keyboard_capture_pop(capture, &event);
    }
}
