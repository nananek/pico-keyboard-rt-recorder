#include "mode_state.h"

#include <stdio.h>

static int failures;
static unsigned releases;
static unsigned clears;
static uint8_t last_state;
static uint8_t last_reason;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static void release(void *user) {
    (void)user;
    ++releases;
}

static void clear(void *user) {
    (void)user;
    ++clears;
}

static void changed(void *user, uint8_t state, uint8_t reason) {
    (void)user;
    last_state = state;
    last_reason = reason;
}

int main(void) {
    const pico_mode_state_callbacks_t callbacks = {release, clear, changed, NULL};
    pico_mode_state_t mode;
    pico_mode_state_init(&mode, &callbacks);

    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    // PASS is idempotent: retransmitting it must not invoke either safety
    // callback, so a held key and accepted physical reports are preserved.
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PASS));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    CHECK(last_state == PICO_UART_MODE_PASS && last_reason == PICO_UART_REASON_OK);
    CHECK(releases == 0u && clears == 0u);

    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_RECORD));
    CHECK(pico_mode_state_is_recording(&mode));
    CHECK(releases == 1u && clears == 1u && last_reason == PICO_UART_REASON_OK);

    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_RECORD));
    CHECK(releases == 1u && clears == 1u);
    CHECK(!pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PLAYING));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_RECORD);
    CHECK(last_reason == PICO_UART_REASON_INVALID_TARGET);
    CHECK(releases == 1u && clears == 1u);
    CHECK(!pico_mode_state_play_start(&mode));
    CHECK(last_reason == PICO_UART_REASON_INVALID_TRANSITION);
    CHECK(!pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_ARMED));

    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PASS));
    CHECK(pico_mode_state_accepts_physical(&mode));
    CHECK(releases == 2u && clears == 2u);
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_ARMED));
    CHECK(!pico_mode_state_accepts_physical(&mode));
    CHECK(releases == 3u && clears == 3u);

    CHECK(pico_mode_state_play_start(&mode));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PLAYING);
    CHECK(releases == 4u && clears == 4u);
    CHECK(pico_mode_state_play_abort(&mode));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_ARMED);
    CHECK(last_reason == PICO_UART_REASON_ABORTED);
    CHECK(releases == 5u && clears == 5u);

    pico_mode_state_uart_fault(&mode);
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_ERROR);
    CHECK(!pico_mode_state_accepts_physical(&mode));
    CHECK(last_reason == PICO_UART_REASON_UART_FAULT);
    CHECK(releases == 6u && clears == 6u);
    pico_mode_state_uart_fault(&mode);
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_ERROR);
    CHECK(releases == 6u && clears == 6u);
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PASS));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    CHECK(releases == 7u && clears == 7u);

    pico_mode_state_protocol_error(&mode);
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    CHECK(last_reason == PICO_UART_REASON_PROTOCOL_ERROR);
    CHECK(releases == 7u && clears == 7u);
    return failures == 0 ? 0 : 1;
}
