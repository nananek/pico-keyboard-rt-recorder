#include "mode_state.h"

#include <string.h>

static void notify(pico_mode_state_t *mode, uint8_t reason) {
    if (mode->callbacks.mode_changed != NULL) {
        mode->callbacks.mode_changed(mode->callbacks.user, mode->state, reason);
    }
}

static void clear_and_release(pico_mode_state_t *mode) {
    if (mode->callbacks.clear_physical != NULL) {
        mode->callbacks.clear_physical(mode->callbacks.user);
    }
    if (mode->callbacks.all_release != NULL) {
        mode->callbacks.all_release(mode->callbacks.user);
    }
}

void pico_mode_state_init(
    pico_mode_state_t *mode,
    const pico_mode_state_callbacks_t *callbacks) {
    if (mode == NULL) {
        return;
    }
    memset(mode, 0, sizeof(*mode));
    mode->state = PICO_UART_MODE_PASS;
    if (callbacks != NULL) {
        mode->callbacks = *callbacks;
    }
}

uint8_t pico_mode_state_get(const pico_mode_state_t *mode) {
    return mode == NULL ? PICO_UART_MODE_ERROR : mode->state;
}

bool pico_mode_state_accepts_physical(const pico_mode_state_t *mode) {
    return mode != NULL &&
           (mode->state == PICO_UART_MODE_PASS ||
            mode->state == PICO_UART_MODE_RECORD);
}

bool pico_mode_state_is_recording(const pico_mode_state_t *mode) {
    return mode != NULL && mode->state == PICO_UART_MODE_RECORD;
}

bool pico_mode_state_handle_mode_set(pico_mode_state_t *mode, uint8_t target) {
    if (mode == NULL) {
        return false;
    }
    if (target > PICO_UART_MODE_ARMED) {
        notify(mode, PICO_UART_REASON_INVALID_TARGET);
        return false;
    }
    if (target == PICO_UART_MODE_PASS) {
        clear_and_release(mode);
        mode->state = PICO_UART_MODE_PASS;
        notify(mode, PICO_UART_REASON_OK);
        return true;
    }
    if (target == mode->state) {
        notify(mode, PICO_UART_REASON_OK);
        return true;
    }
    if ((target == PICO_UART_MODE_RECORD || target == PICO_UART_MODE_ARMED) &&
        mode->state == PICO_UART_MODE_PASS) {
        clear_and_release(mode);
        mode->state = target;
        notify(mode, PICO_UART_REASON_OK);
        return true;
    }
    notify(mode, PICO_UART_REASON_INVALID_TRANSITION);
    return false;
}

bool pico_mode_state_play_start(pico_mode_state_t *mode) {
    if (mode == NULL || mode->state != PICO_UART_MODE_ARMED) {
        if (mode != NULL) {
            notify(mode, PICO_UART_REASON_INVALID_TRANSITION);
        }
        return false;
    }
    clear_and_release(mode);
    mode->state = PICO_UART_MODE_PLAYING;
    notify(mode, PICO_UART_REASON_OK);
    return true;
}

bool pico_mode_state_play_abort(pico_mode_state_t *mode) {
    if (mode == NULL || (mode->state != PICO_UART_MODE_ARMED &&
                         mode->state != PICO_UART_MODE_PLAYING)) {
        if (mode != NULL) {
            notify(mode, PICO_UART_REASON_INVALID_TRANSITION);
        }
        return false;
    }
    clear_and_release(mode);
    mode->state = PICO_UART_MODE_ARMED;
    notify(mode, PICO_UART_REASON_ABORTED);
    return true;
}

static void enter_error(pico_mode_state_t *mode, uint8_t reason) {
    if (mode == NULL) {
        return;
    }
    clear_and_release(mode);
    if (mode->state != PICO_UART_MODE_PASS) {
        mode->state = PICO_UART_MODE_ERROR;
    }
    notify(mode, reason);
}

void pico_mode_state_protocol_error(pico_mode_state_t *mode) {
    enter_error(mode, PICO_UART_REASON_PROTOCOL_ERROR);
}

void pico_mode_state_uart_fault(pico_mode_state_t *mode) {
    enter_error(mode, PICO_UART_REASON_UART_FAULT);
}
