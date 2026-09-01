#include "mode_state.h"

#include <stdio.h>

static int failures;
static unsigned releases;
static unsigned clears;
static uint8_t last_state;
static uint8_t last_reason;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s\n", #c); ++failures; } } while (0)
static void release(void *u) { (void)u; ++releases; }
static void clear(void *u) { (void)u; ++clears; }
static void changed(void *u, uint8_t state, uint8_t reason) { (void)u; last_state = state; last_reason = reason; }

int main(void) {
    const pico_mode_state_callbacks_t callbacks = {release, clear, changed, NULL};
    pico_mode_state_t mode;
    pico_mode_state_init(&mode, &callbacks);
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_RECORD));
    CHECK(pico_mode_state_is_recording(&mode));
    CHECK(releases == 1u && clears == 1u && last_reason == PICO_UART_REASON_OK);
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_RECORD));
    CHECK(releases == 1u);
    CHECK(!pico_mode_state_play_start(&mode));
    CHECK(last_reason == PICO_UART_REASON_INVALID_TRANSITION);
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_ARMED) == false);
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PASS));
    CHECK(pico_mode_state_accepts_physical(&mode));
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_ARMED));
    CHECK(!pico_mode_state_accepts_physical(&mode));
    CHECK(pico_mode_state_play_start(&mode));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PLAYING);
    CHECK(pico_mode_state_play_abort(&mode));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_ARMED);
    pico_mode_state_uart_fault(&mode);
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_ERROR);
    CHECK(!pico_mode_state_accepts_physical(&mode));
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PASS));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    return failures == 0 ? 0 : 1;
}
