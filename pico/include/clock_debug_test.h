#ifndef PICO_KEYBOARD_RT_CLOCK_DEBUG_TEST_H
#define PICO_KEYBOARD_RT_CLOCK_DEBUG_TEST_H

// Opt-in PICO_CLOCK_DEBUG_PRINT diagnostic (Issue #26): prints
// clk_sys/clk_peri/clk_ref/clk_usb once at boot over UART0-as-stdio, to
// verify the clock chain documented in docs/realtime-design.md ("Clock
// configuration and timing accuracy") against real hardware. Standalone
// bring-up build only, like PICO_HID_DEMO_TEST/PICO_PLAYBACK_SCHED_TEST --
// never combined with the production Zero-integrated UART0 protocol build,
// since it claims UART0 as SDK stdio instead.
void pico_clock_debug_print(void);

#endif  // PICO_KEYBOARD_RT_CLOCK_DEBUG_TEST_H
