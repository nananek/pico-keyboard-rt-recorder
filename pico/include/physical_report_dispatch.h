#ifndef PICO_KEYBOARD_RT_PHYSICAL_REPORT_DISPATCH_H
#define PICO_KEYBOARD_RT_PHYSICAL_REPORT_DISPATCH_H

#include <stdbool.h>

#include "keyboard_capture.h"
#include "mode_state.h"

typedef bool (*pico_physical_report_send_t)(
    void *user,
    const pico_hid_boot_keyboard_report_t *report);
typedef void (*pico_record_event_send_t)(
    void *user,
    const pico_keyboard_capture_event_t *event);

// This is the physical-output portion of the main loop. Both PASS and RECORD
// send from the FIFO head to the native HID device and only consume (pop) a
// report after the HID endpoint accepted it, retrying the same head
// otherwise. RECORD additionally emits a RECORD_EVENT for each head exactly
// once -- the moment it is first peeked, before the HID outcome for that
// iteration is known -- so recording is unaffected by HID busy/retry state
// and a retried head never produces a duplicate RECORD_EVENT.
void pico_physical_report_dispatch(
    pico_keyboard_capture_t *capture,
    const pico_mode_state_t *mode,
    bool hid_output_ready,
    pico_physical_report_send_t send_pass_report,
    pico_record_event_send_t send_record_event,
    void *user);

#endif
