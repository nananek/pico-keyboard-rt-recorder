#ifndef PICO_KEYBOARD_RT_MODE_STATE_H
#define PICO_KEYBOARD_RT_MODE_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "uart_protocol.h"

typedef void (*pico_mode_simple_callback_t)(void *user);
typedef void (*pico_mode_changed_callback_t)(
    void *user,
    uint8_t state,
    uint8_t reason);

typedef struct {
    pico_mode_simple_callback_t all_release;
    pico_mode_simple_callback_t clear_physical;
    // Invoked wherever a session boundary discards queued playback data:
    // entering PASS from a non-PASS state, entering RECORD/ARMED from PASS,
    // PLAY_ABORT, and any fault that enters ERROR. It is deliberately NOT
    // invoked by PLAY_START, which must leave the just-loaded queue intact
    // for the (future) scheduler to drain.
    pico_mode_simple_callback_t clear_queue;
    pico_mode_changed_callback_t mode_changed;
    void *user;
} pico_mode_state_callbacks_t;

typedef struct {
    uint8_t state;
    pico_mode_state_callbacks_t callbacks;
} pico_mode_state_t;

void pico_mode_state_init(
    pico_mode_state_t *mode,
    const pico_mode_state_callbacks_t *callbacks);
uint8_t pico_mode_state_get(const pico_mode_state_t *mode);
bool pico_mode_state_accepts_physical(const pico_mode_state_t *mode);
bool pico_mode_state_is_recording(const pico_mode_state_t *mode);

bool pico_mode_state_handle_mode_set(pico_mode_state_t *mode, uint8_t target);
bool pico_mode_state_play_start(pico_mode_state_t *mode);
bool pico_mode_state_play_abort(pico_mode_state_t *mode);
// PLAYING -> ARMED for a playback run that drained its queue naturally
// (as opposed to pico_mode_state_play_abort, which is for PLAY_ABORT).
// Reports PICO_UART_REASON_FINISHED. Fails outside PLAYING.
bool pico_mode_state_play_finish(pico_mode_state_t *mode);
void pico_mode_state_protocol_error(pico_mode_state_t *mode);
void pico_mode_state_uart_fault(pico_mode_state_t *mode);
// Persistent playback underrun or UART-unresponsive-during-PLAYING watchdog
// (Issue #10): same ERROR-entry behavior as pico_mode_state_uart_fault, but
// reports PICO_UART_REASON_UNDERRUN.
void pico_mode_state_underrun_fault(pico_mode_state_t *mode);

#endif
