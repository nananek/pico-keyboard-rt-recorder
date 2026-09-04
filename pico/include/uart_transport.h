#ifndef PICO_KEYBOARD_RT_UART_TRANSPORT_H
#define PICO_KEYBOARD_RT_UART_TRANSPORT_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#include "uart_protocol.h"

enum {
    PICO_UART_RX_RING_CAPACITY = 256u,
    PICO_UART_COMMAND_CAPACITY = 8u,
    PICO_UART_TX_RING_CAPACITY = 1024u,
    PICO_UART_PARSER_CAPACITY = PICO_UART_PROTOCOL_HEADER_LEN +
                                PICO_UART_PROTOCOL_MAX_PAYLOAD +
                                PICO_UART_PROTOCOL_CRC_LEN,
};

typedef struct {
    uint32_t rx_overflow;
    uint32_t hardware_errors;
    uint32_t invalid_frames;
    uint32_t bad_magic;
    uint32_t bad_version;
    uint32_t bad_length;
    uint32_t bad_crc;
    uint32_t unknown_type;
    uint32_t command_overflow;
    uint32_t tx_dropped;
} pico_uart_transport_stats_t;

typedef struct {
    uint8_t rx_bytes[PICO_UART_RX_RING_CAPACITY];
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
    pico_uart_frame_t commands[PICO_UART_COMMAND_CAPACITY];
    uint8_t command_head;
    uint8_t command_tail;
    uint8_t tx_bytes[PICO_UART_TX_RING_CAPACITY];
    uint16_t tx_head;
    uint16_t tx_tail;
    uint8_t parser[PICO_UART_PARSER_CAPACITY];
    uint8_t parser_len;
    // Written from the UART IRQ and consumed in the main loop.  This must not
    // be a plain flag: clearing a plain flag can otherwise lose an IRQ fault
    // that arrives at the same instant.
    atomic_bool fault_pending;
    pico_uart_transport_stats_t stats;
    // time_us_64() at the most recent byte pushed by the UART IRQ. Written
    // only from pico_uart_transport_rx_push_isr; read from the main loop via
    // pico_uart_transport_rx_idle_us to detect a silent link during playback
    // (Issue #10). volatile for the same cross-context-visibility reason as
    // rx_head/rx_tail above.
    volatile uint64_t last_rx_us;
} pico_uart_transport_t;

void pico_uart_transport_init(pico_uart_transport_t *transport);

/* These two functions are the only operations permitted from UART0 IRQ. */
void pico_uart_transport_rx_push_isr(
    pico_uart_transport_t *transport,
    uint8_t byte);
void pico_uart_transport_hardware_error_isr(pico_uart_transport_t *transport);

/* Parse all bytes currently in the RX ring. Must run in the main context. */
void pico_uart_transport_poll(pico_uart_transport_t *transport);
bool pico_uart_transport_pop_command(
    pico_uart_transport_t *transport,
    pico_uart_frame_t *frame);

/* Enqueue one encoded Pico-to-Zero frame without blocking. */
bool pico_uart_transport_queue_frame(
    pico_uart_transport_t *transport,
    uint8_t type,
    const uint8_t *payload,
    uint16_t payload_len);
bool pico_uart_transport_pop_tx_byte(
    pico_uart_transport_t *transport,
    uint8_t *byte);

bool pico_uart_transport_take_fault(pico_uart_transport_t *transport);
pico_uart_transport_stats_t pico_uart_transport_get_stats(
    const pico_uart_transport_t *transport);

// Microseconds since the most recent byte was pushed by the UART IRQ, as of
// now_us. Returns 0 if now_us has not yet caught up to last_rx_us (e.g. a
// byte pushed between the caller's own now_us sample and this call), and 0
// if transport is NULL. Pure query: does not itself change transport state.
uint64_t pico_uart_transport_rx_idle_us(
    const pico_uart_transport_t *transport, uint64_t now_us);

void pico_uart_transport_hw_init(pico_uart_transport_t *transport);
void pico_uart_transport_tx_task(pico_uart_transport_t *transport);

#endif
