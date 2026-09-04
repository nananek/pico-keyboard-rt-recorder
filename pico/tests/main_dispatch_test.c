#include "keyboard_capture.h"
#include "mode_state.h"
#include "physical_report_dispatch.h"
#include "safety_release.h"

#include <stdio.h>

static int failures;
static bool release_ready;
static bool pass_ready;
static unsigned release_sends;
static unsigned pass_sends;
static unsigned record_sends;
static uint8_t last_pass_key;
static uint64_t last_record_timestamp;
// Snapshot of pass_sends taken inside send_record(), to verify RECORD_EVENT
// is emitted before that iteration's HID attempt (not just before HID
// success).
static unsigned pass_sends_at_record_time;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static bool send_release(void *user) {
    (void)user;
    ++release_sends;
    return release_ready;
}

static bool send_pass(
    void *user,
    const pico_hid_boot_keyboard_report_t *report) {
    (void)user;
    ++pass_sends;
    last_pass_key = report->keycode[0];
    return pass_ready;
}

static void send_record(
    void *user,
    const pico_keyboard_capture_event_t *event) {
    (void)user;
    ++record_sends;
    last_record_timestamp = event->timestamp_us;
    pass_sends_at_record_time = pass_sends;
}

static void push_report(
    pico_keyboard_capture_t *capture,
    uint64_t timestamp_us,
    uint8_t key) {
    uint8_t report[PICO_HID_BOOT_KEYBOARD_REPORT_LEN] = {0u};
    report[2] = key;
    CHECK(pico_keyboard_capture_push(capture, timestamp_us, report, sizeof(report)));
}

int main(void) {
    pico_keyboard_capture_t capture;
    pico_mode_state_t mode;
    pico_safety_release_t release;
    pico_keyboard_capture_event_t event;

    pico_keyboard_capture_init(&capture);
    pico_mode_state_init(&mode, NULL);
    pico_safety_release_init(&release);
    push_report(&capture, 10u, 0x04u);
    pico_safety_release_request(&release);

    // A release that cannot be submitted keeps PASS input queued.
    CHECK(pico_safety_release_service(&release, send_release, NULL) ==
          PICO_SAFETY_RELEASE_BLOCKED);
    pico_physical_report_dispatch(
        &capture, &mode, false, send_pass, send_record, NULL);
    CHECK(pass_sends == 0u);
    CHECK(pico_keyboard_capture_peek(&capture, &event));
    CHECK(event.report.keycode[0] == 0x04u);

    // The same iteration that sends the release must also defer PASS, because
    // the successful HID report makes the endpoint non-ready.
    release_ready = true;
    CHECK(pico_safety_release_service(&release, send_release, NULL) ==
          PICO_SAFETY_RELEASE_SENT);
    pico_physical_report_dispatch(
        &capture, &mode, false, send_pass, send_record, NULL);
    CHECK(release_sends == 2u && pass_sends == 0u);
    CHECK(pico_keyboard_capture_peek(&capture, &event));

    // Even after that deferral, a normal HID-ready failure leaves the head in
    // place for a later iteration rather than popping and losing it.
    CHECK(pico_safety_release_service(&release, send_release, NULL) ==
          PICO_SAFETY_RELEASE_READY);
    pico_physical_report_dispatch(
        &capture, &mode, true, send_pass, send_record, NULL);
    CHECK(pass_sends == 1u && pico_keyboard_capture_peek(&capture, &event));
    pass_ready = true;
    pico_physical_report_dispatch(
        &capture, &mode, true, send_pass, send_record, NULL);
    CHECK(pass_sends == 2u && last_pass_key == 0x04u);
    CHECK(!pico_keyboard_capture_pop(&capture, &event));

    // RECORD is a dual-sink dispatch (Issue #4): it forwards to the PC via
    // HID exactly like PASS (retrying a busy endpoint against the same FIFO
    // head), while also emitting exactly one RECORD_EVENT per head,
    // regardless of how many times that head is retried against HID.
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_RECORD));
    pass_sends = 0u;
    record_sends = 0u;

    // Case 1: HID accepts immediately -- exactly one send on each sink, and
    // the FIFO head is consumed.
    push_report(&capture, 20u, 0x05u);
    pass_ready = true;
    pico_physical_report_dispatch(
        &capture, &mode, true, send_pass, send_record, NULL);
    CHECK(record_sends == 1u && pass_sends == 1u);
    CHECK(last_record_timestamp == 20u && last_pass_key == 0x05u);
    CHECK(!pico_keyboard_capture_pop(&capture, &event));

    // Case 2 + 3: HID stays busy across several dispatch calls. The head is
    // retried against HID on every call (pass_sends keeps climbing) but the
    // RECORD_EVENT for that same head is sent exactly once -- and, per the
    // pass_sends_at_record_time snapshot taken inside send_record(), before
    // that first iteration's HID attempt is even made, so recording is
    // unaffected by the PC-side HID busy/retry state.
    pass_sends = 0u;
    record_sends = 0u;
    push_report(&capture, 21u, 0x06u);
    pass_ready = false;
    pico_physical_report_dispatch(
        &capture, &mode, true, send_pass, send_record, NULL);
    CHECK(record_sends == 1u && pass_sends_at_record_time == 0u);
    CHECK(pass_sends == 1u);
    CHECK(pico_keyboard_capture_peek(&capture, &event) &&
          event.report.keycode[0] == 0x06u);
    pico_physical_report_dispatch(
        &capture, &mode, true, send_pass, send_record, NULL);
    pico_physical_report_dispatch(
        &capture, &mode, true, send_pass, send_record, NULL);
    CHECK(record_sends == 1u && pass_sends == 3u);
    CHECK(pico_keyboard_capture_peek(&capture, &event));
    pass_ready = true;
    pico_physical_report_dispatch(
        &capture, &mode, true, send_pass, send_record, NULL);
    CHECK(record_sends == 1u && pass_sends == 4u);
    CHECK(!pico_keyboard_capture_pop(&capture, &event));

    // Case 4: head_recorded must not leak across a mode round-trip, so a
    // fresh RECORD session's first event is still recorded (not silently
    // skipped as if it were the previous session's already-recorded head).
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PASS));
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_RECORD));
    push_report(&capture, 22u, 0x07u);
    pico_physical_report_dispatch(
        &capture, &mode, true, send_pass, send_record, NULL);
    CHECK(record_sends == 2u && last_record_timestamp == 22u);

    return failures == 0 ? 0 : 1;
}
