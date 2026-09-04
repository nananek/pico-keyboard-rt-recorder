#include "uart_transport.h"

#include <string.h>

static void latch_fault(pico_uart_transport_t *transport) {
    atomic_store_explicit(&transport->fault_pending, true, memory_order_relaxed);
}

static void note_decode_error(
    pico_uart_transport_t *transport,
    pico_uart_decode_result_t result) {
    ++transport->stats.invalid_frames;
    switch (result) {
        case PICO_UART_DECODE_BAD_MAGIC: ++transport->stats.bad_magic; break;
        case PICO_UART_DECODE_BAD_VERSION: ++transport->stats.bad_version; break;
        case PICO_UART_DECODE_BAD_LENGTH: ++transport->stats.bad_length; break;
        case PICO_UART_DECODE_BAD_CRC: ++transport->stats.bad_crc; break;
        default: break;
    }
    latch_fault(transport);
}

void pico_uart_transport_init(pico_uart_transport_t *transport) {
    if (transport != NULL) {
        memset(transport, 0, sizeof(*transport));
        atomic_init(&transport->fault_pending, false);
    }
}

void pico_uart_transport_rx_push_isr(
    pico_uart_transport_t *transport,
    uint8_t byte) {
    if (transport == NULL) {
        return;
    }
    const uint16_t head = transport->rx_head;
    if ((uint16_t)(head - transport->rx_tail) >= PICO_UART_RX_RING_CAPACITY) {
        ++transport->stats.rx_overflow;
        latch_fault(transport);
        return;
    }
    transport->rx_bytes[head % PICO_UART_RX_RING_CAPACITY] = byte;
    transport->rx_head = (uint16_t)(head + 1u);
}

void pico_uart_transport_hardware_error_isr(pico_uart_transport_t *transport) {
    if (transport != NULL) {
        ++transport->stats.hardware_errors;
        latch_fault(transport);
    }
}

static void enqueue_command(pico_uart_transport_t *transport,
                            const pico_uart_frame_t *frame) {
    if (!pico_uart_type_is_command(frame->type) ||
        !pico_uart_frame_payload_valid(frame)) {
        ++transport->stats.invalid_frames;
        if (!pico_uart_type_known(frame->type)) {
            ++transport->stats.unknown_type;
        }
        latch_fault(transport);
        return;
    }
    if ((uint8_t)(transport->command_head - transport->command_tail) >=
        PICO_UART_COMMAND_CAPACITY) {
        ++transport->stats.command_overflow;
        latch_fault(transport);
        return;
    }
    transport->commands[transport->command_head % PICO_UART_COMMAND_CAPACITY] = *frame;
    ++transport->command_head;
}

void pico_uart_transport_poll(pico_uart_transport_t *transport) {
    if (transport == NULL) {
        return;
    }
    while (transport->rx_tail != transport->rx_head) {
        const uint8_t byte = transport->rx_bytes[
            transport->rx_tail % PICO_UART_RX_RING_CAPACITY];
        transport->rx_tail = (uint16_t)(transport->rx_tail + 1u);

        if (transport->parser_len == 0u) {
            if (byte != PICO_UART_PROTOCOL_MAGIC) {
                note_decode_error(transport, PICO_UART_DECODE_BAD_MAGIC);
                continue;
            }
        }
        if (transport->parser_len >= PICO_UART_PARSER_CAPACITY) {
            ++transport->stats.invalid_frames;
            ++transport->stats.bad_length;
            latch_fault(transport);
            transport->parser_len = 0u;
        }
        transport->parser[transport->parser_len++] = byte;

        // Reject an unsupported version as soon as its complete fixed-size
        // prefix is present. Its declared length is untrusted, so waiting for
        // that many bytes could consume a following valid frame.
        if (transport->parser_len == 2u &&
            transport->parser[1] != PICO_UART_PROTOCOL_VERSION) {
            note_decode_error(transport, PICO_UART_DECODE_BAD_VERSION);
            transport->parser_len = 0u;
            continue;
        }
        if (transport->parser_len < PICO_UART_PROTOCOL_HEADER_LEN) {
            continue;
        }
        const uint16_t payload_len = (uint16_t)transport->parser[3] |
                                     ((uint16_t)transport->parser[4] << 8);
        if (payload_len > PICO_UART_PROTOCOL_MAX_PAYLOAD) {
            note_decode_error(transport, PICO_UART_DECODE_BAD_LENGTH);
            transport->parser_len = 0u;
            continue;
        }
        const size_t expected = 7u + payload_len;
        if (transport->parser_len < expected) {
            continue;
        }
        pico_uart_frame_t frame;
        const pico_uart_decode_result_t result =
            pico_uart_decode_frame(transport->parser, expected, &frame);
        if (result != PICO_UART_DECODE_OK) {
            note_decode_error(transport, result);
        } else {
            enqueue_command(transport, &frame);
        }
        transport->parser_len = 0u;
    }
}

bool pico_uart_transport_pop_command(
    pico_uart_transport_t *transport,
    pico_uart_frame_t *frame) {
    if (transport == NULL || frame == NULL ||
        transport->command_tail == transport->command_head) {
        return false;
    }
    *frame = transport->commands[
        transport->command_tail % PICO_UART_COMMAND_CAPACITY];
    ++transport->command_tail;
    return true;
}

bool pico_uart_transport_queue_frame(
    pico_uart_transport_t *transport,
    uint8_t type,
    const uint8_t *payload,
    uint16_t payload_len) {
    if (transport == NULL) {
        return false;
    }
    uint8_t encoded[PICO_UART_PARSER_CAPACITY];
    const size_t length = pico_uart_encode_frame(
        type, payload, payload_len, encoded, sizeof(encoded));
    if (length == 0u ||
        (uint16_t)(transport->tx_head - transport->tx_tail) + length >
            PICO_UART_TX_RING_CAPACITY) {
        ++transport->stats.tx_dropped;
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        transport->tx_bytes[transport->tx_head % PICO_UART_TX_RING_CAPACITY] = encoded[i];
        ++transport->tx_head;
    }
    return true;
}

bool pico_uart_transport_pop_tx_byte(
    pico_uart_transport_t *transport,
    uint8_t *byte) {
    if (transport == NULL || byte == NULL ||
        transport->tx_tail == transport->tx_head) {
        return false;
    }
    *byte = transport->tx_bytes[transport->tx_tail % PICO_UART_TX_RING_CAPACITY];
    ++transport->tx_tail;
    return true;
}

bool pico_uart_transport_take_fault(pico_uart_transport_t *transport) {
    return transport != NULL &&
           atomic_exchange_explicit(
               &transport->fault_pending, false, memory_order_relaxed);
}

pico_uart_transport_stats_t pico_uart_transport_get_stats(
    const pico_uart_transport_t *transport) {
    const pico_uart_transport_stats_t empty = {0};
    return transport == NULL ? empty : transport->stats;
}
