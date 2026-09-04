#include "mode_state.h"

#include <stdio.h>

static int failures;
static unsigned releases;
static unsigned clears;
static unsigned queue_clears;
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

static void clear_queue(void *user) {
    (void)user;
    ++queue_clears;
}

static void changed(void *user, uint8_t state, uint8_t reason) {
    (void)user;
    last_state = state;
    last_reason = reason;
}

int main(void) {
    const pico_mode_state_callbacks_t callbacks = {
        .all_release = release,
        .clear_physical = clear,
        .clear_queue = clear_queue,
        .mode_changed = changed,
        .user = NULL,
    };
    pico_mode_state_t mode;
    pico_mode_state_init(&mode, &callbacks);

    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    // PASS is idempotent: retransmitting it must not invoke either safety
    // callback, so a held key and accepted physical reports are preserved.
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PASS));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    CHECK(last_state == PICO_UART_MODE_PASS && last_reason == PICO_UART_REASON_OK);
    CHECK(releases == 0u && clears == 0u && queue_clears == 0u);

    // Transport and malformed-frame faults received in PASS only report their
    // reason. They must not release a key the PC is currently holding or clear
    // a physical report already accepted for PASS forwarding.
    pico_mode_state_uart_fault(&mode);
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    CHECK(pico_mode_state_accepts_physical(&mode));
    CHECK(last_reason == PICO_UART_REASON_UART_FAULT);
    CHECK(releases == 0u && clears == 0u && queue_clears == 0u);
    pico_mode_state_protocol_error(&mode);
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    CHECK(pico_mode_state_accepts_physical(&mode));
    CHECK(last_reason == PICO_UART_REASON_PROTOCOL_ERROR);
    CHECK(releases == 0u && clears == 0u && queue_clears == 0u);

    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_RECORD));
    CHECK(pico_mode_state_is_recording(&mode));
    CHECK(releases == 1u && clears == 1u && queue_clears == 1u &&
          last_reason == PICO_UART_REASON_OK);

    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_RECORD));
    CHECK(releases == 1u && clears == 1u && queue_clears == 1u);
    CHECK(!pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PLAYING));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_RECORD);
    CHECK(last_reason == PICO_UART_REASON_INVALID_TARGET);
    CHECK(releases == 1u && clears == 1u && queue_clears == 1u);
    CHECK(!pico_mode_state_play_start(&mode));
    CHECK(last_reason == PICO_UART_REASON_INVALID_TRANSITION);
    CHECK(!pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_ARMED));

    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PASS));
    CHECK(pico_mode_state_accepts_physical(&mode));
    CHECK(releases == 2u && clears == 2u && queue_clears == 2u);
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_ARMED));
    CHECK(!pico_mode_state_accepts_physical(&mode));
    CHECK(releases == 3u && clears == 3u && queue_clears == 3u);

    // PLAY_START must release physical input like any other transition, but
    // it must NOT discard the playback queue just loaded during ARMED: that
    // queue is exactly what the (future) scheduler needs to drain.
    CHECK(pico_mode_state_play_start(&mode));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PLAYING);
    CHECK(releases == 4u && clears == 4u && queue_clears == 3u);
    CHECK(pico_mode_state_play_abort(&mode));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_ARMED);
    CHECK(last_reason == PICO_UART_REASON_ABORTED);
    CHECK(releases == 5u && clears == 5u && queue_clears == 4u);

    pico_mode_state_uart_fault(&mode);
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_ERROR);
    CHECK(!pico_mode_state_accepts_physical(&mode));
    CHECK(last_reason == PICO_UART_REASON_UART_FAULT);
    CHECK(releases == 6u && clears == 6u && queue_clears == 5u);
    pico_mode_state_uart_fault(&mode);
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_ERROR);
    CHECK(releases == 6u && clears == 6u && queue_clears == 5u);
    CHECK(pico_mode_state_handle_mode_set(&mode, PICO_UART_MODE_PASS));
    CHECK(pico_mode_state_get(&mode) == PICO_UART_MODE_PASS);
    CHECK(releases == 7u && clears == 7u && queue_clears == 6u);

    return failures == 0 ? 0 : 1;
}
