#ifndef PICO_KEYBOARD_RT_HARDWARE_CONFIG_H
#define PICO_KEYBOARD_RT_HARDWARE_CONFIG_H

#include "uart_transport.h"

// Pico-PIO-USB uses DPDM ordering: D- is the GPIO immediately after D+.
enum {
    PICO_KEYBOARD_UART_TX_GPIO = 0u,
    PICO_KEYBOARD_UART_RX_GPIO = 1u,
    PICO_KEYBOARD_PIO_USB_DP_GPIO = 12u,
    PICO_KEYBOARD_PIO_USB_DM_GPIO = 13u,
    PICO_KEYBOARD_NATIVE_DEVICE_RHPORT = 0u,
    PICO_KEYBOARD_PIO_HOST_RHPORT = 1u,
    PICO_KEYBOARD_RP2350A_GPIO_COUNT = 30u,
    // Fixed, changed in lockstep with the Zero's --baud/ZERO_SERIAL_BAUD
    // defaults (see docs/protocol.md); not negotiated over the wire.
    PICO_KEYBOARD_UART_BAUD = 921600u,
    // RECORD-mode main-loop sleep (Issue #26), tightened from the default
    // 1ms to bound this codebase's own scheduling jitter contribution to
    // captured RECORD_EVENT timestamps; see docs/realtime-design.md's
    // "Clock configuration and timing accuracy" section for the trade-off
    // reasoning against a full busy-loop.
    PICO_RECORD_LOOP_INTERVAL_US = 200u,
};

#if defined(__cplusplus)
#define PICO_KEYBOARD_STATIC_ASSERT(condition, message) \
    static_assert((condition), message)
#else
#define PICO_KEYBOARD_STATIC_ASSERT(condition, message) \
    _Static_assert((condition), message)
#endif

PICO_KEYBOARD_STATIC_ASSERT(
    PICO_KEYBOARD_PIO_USB_DM_GPIO == PICO_KEYBOARD_PIO_USB_DP_GPIO + 1u,
    "Pico-PIO-USB DPDM wiring requires D- to follow D+");
PICO_KEYBOARD_STATIC_ASSERT(
    PICO_KEYBOARD_UART_TX_GPIO < PICO_KEYBOARD_RP2350A_GPIO_COUNT &&
        PICO_KEYBOARD_UART_RX_GPIO < PICO_KEYBOARD_RP2350A_GPIO_COUNT,
    "UART0 pins must be valid RP2350A GPIOs");
PICO_KEYBOARD_STATIC_ASSERT(
    PICO_KEYBOARD_PIO_USB_DP_GPIO < PICO_KEYBOARD_RP2350A_GPIO_COUNT,
    "PIO-USB D+ must be a valid RP2350A GPIO");
PICO_KEYBOARD_STATIC_ASSERT(
    PICO_KEYBOARD_PIO_USB_DM_GPIO < PICO_KEYBOARD_RP2350A_GPIO_COUNT,
    "PIO-USB D- must be a valid RP2350A GPIO");
PICO_KEYBOARD_STATIC_ASSERT(
    PICO_KEYBOARD_UART_TX_GPIO != PICO_KEYBOARD_UART_RX_GPIO,
    "UART TX and RX must use distinct GPIOs");
PICO_KEYBOARD_STATIC_ASSERT(
    PICO_UART_RX_RING_CAPACITY * 10000u >= 2u * PICO_KEYBOARD_UART_BAUD,
    "RX ring must hold at least ~2ms of continuous input at the configured baud");
PICO_KEYBOARD_STATIC_ASSERT(
    PICO_KEYBOARD_PIO_USB_DP_GPIO != PICO_KEYBOARD_UART_TX_GPIO &&
        PICO_KEYBOARD_PIO_USB_DP_GPIO != PICO_KEYBOARD_UART_RX_GPIO &&
        PICO_KEYBOARD_PIO_USB_DM_GPIO != PICO_KEYBOARD_UART_TX_GPIO &&
        PICO_KEYBOARD_PIO_USB_DM_GPIO != PICO_KEYBOARD_UART_RX_GPIO,
    "PIO-USB data pins must not overlap UART0 pins");

#if defined(PICO_DEFAULT_PIO_USB_DP_PIN)
PICO_KEYBOARD_STATIC_ASSERT(
    PICO_DEFAULT_PIO_USB_DP_PIN == PICO_KEYBOARD_PIO_USB_DP_GPIO,
    "TinyUSB PIO D+ definition must match the hardware contract");
#endif
#if defined(BOARD_TUD_RHPORT)
PICO_KEYBOARD_STATIC_ASSERT(
    BOARD_TUD_RHPORT == PICO_KEYBOARD_NATIVE_DEVICE_RHPORT,
    "TinyUSB device root hub must match the hardware contract");
#endif
#if defined(BOARD_TUH_RHPORT)
PICO_KEYBOARD_STATIC_ASSERT(
    BOARD_TUH_RHPORT == PICO_KEYBOARD_PIO_HOST_RHPORT,
    "TinyUSB host root hub must match the hardware contract");
#endif

#undef PICO_KEYBOARD_STATIC_ASSERT

#endif  // PICO_KEYBOARD_RT_HARDWARE_CONFIG_H
