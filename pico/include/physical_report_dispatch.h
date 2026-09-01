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

// This is the physical-output portion of the main loop. RECORD drains to UART
// independently of HID state. PASS sends from the FIFO head and only consumes a
// report after the native HID endpoint accepted it.
void pico_physical_report_dispatch(
    pico_keyboard_capture_t *capture,
    const pico_mode_state_t *mode,
    bool pass_output_ready,
    pico_physical_report_send_t send_pass_report,
    pico_record_event_send_t send_record_event,
    void *user);

#endif
