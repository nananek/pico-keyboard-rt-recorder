#include "hardware_config.h"

#include <stdio.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

int main(void) {
    CHECK(PICO_KEYBOARD_PIO_USB_DP_GPIO == 12u);
    CHECK(PICO_KEYBOARD_PIO_USB_DM_GPIO == 13u);
    CHECK(PICO_KEYBOARD_UART_TX_GPIO == 0u);
    CHECK(PICO_KEYBOARD_UART_RX_GPIO == 1u);
    CHECK(PICO_KEYBOARD_PIO_USB_DM_GPIO ==
          PICO_KEYBOARD_PIO_USB_DP_GPIO + 1u);
    CHECK(PICO_KEYBOARD_PIO_USB_DP_GPIO != PICO_KEYBOARD_UART_TX_GPIO);
    CHECK(PICO_KEYBOARD_PIO_USB_DP_GPIO != PICO_KEYBOARD_UART_RX_GPIO);
    CHECK(PICO_KEYBOARD_PIO_USB_DM_GPIO != PICO_KEYBOARD_UART_TX_GPIO);
    CHECK(PICO_KEYBOARD_PIO_USB_DM_GPIO != PICO_KEYBOARD_UART_RX_GPIO);
    CHECK(PICO_KEYBOARD_PIO_USB_DP_GPIO <
          PICO_KEYBOARD_RP2350A_GPIO_COUNT);
    CHECK(PICO_KEYBOARD_PIO_USB_DM_GPIO <
          PICO_KEYBOARD_RP2350A_GPIO_COUNT);
    CHECK(PICO_KEYBOARD_UART_TX_GPIO < PICO_KEYBOARD_RP2350A_GPIO_COUNT);
    CHECK(PICO_KEYBOARD_UART_RX_GPIO < PICO_KEYBOARD_RP2350A_GPIO_COUNT);
    CHECK(PICO_KEYBOARD_NATIVE_DEVICE_RHPORT == 0u);
    CHECK(PICO_KEYBOARD_PIO_HOST_RHPORT == 1u);
    CHECK(PICO_KEYBOARD_UART_BAUD == 921600u);
    // Pinned so a future edit can't silently widen RECORD's loop interval
    // back past the 1ms sleep every other non-PLAYING mode still uses (Issue
    // #26; see docs/realtime-design.md's "Clock configuration and timing
    // accuracy" section).
    CHECK(PICO_RECORD_LOOP_INTERVAL_US == 200u);
    CHECK(PICO_RECORD_LOOP_INTERVAL_US < 1000u);
    return failures == 0 ? 0 : 1;
}
