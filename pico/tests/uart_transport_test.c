#include "uart_transport.h"

#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s\n", #c); ++failures; } } while (0)

static void feed(pico_uart_transport_t *transport, const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        pico_uart_transport_rx_push_isr(transport, data[i]);
    }
}

int main(void) {
    pico_uart_transport_t transport;
    pico_uart_transport_init(&transport);
    uint8_t encoded[32];
    const uint8_t mode = PICO_UART_MODE_ARMED;
    const size_t length = pico_uart_encode_frame(
        PICO_UART_MODE_SET, &mode, 1u, encoded, sizeof(encoded));
    feed(&transport, encoded, 3u);
    pico_uart_transport_poll(&transport);
    pico_uart_frame_t command;
    CHECK(!pico_uart_transport_pop_command(&transport, &command));
    feed(&transport, encoded + 3u, length - 3u);
    pico_uart_transport_poll(&transport);
    CHECK(pico_uart_transport_pop_command(&transport, &command));
    CHECK(command.type == PICO_UART_MODE_SET && command.payload[0] == PICO_UART_MODE_ARMED);
    CHECK(!pico_uart_transport_take_fault(&transport));
    encoded[length - 1u] ^= 1u;
    feed(&transport, encoded, length);
    pico_uart_transport_poll(&transport);
    CHECK(pico_uart_transport_take_fault(&transport));
    CHECK(pico_uart_transport_get_stats(&transport).bad_crc == 1u);
    pico_uart_transport_init(&transport);
    encoded[2] = 0x0Cu;
    const uint16_t crc = pico_uart_crc16(encoded + 1u, 5u);
    encoded[6] = (uint8_t)(crc & 0xFFu);
    encoded[7] = (uint8_t)(crc >> 8);
    feed(&transport, encoded, 8u);
    pico_uart_transport_poll(&transport);
    CHECK(pico_uart_transport_get_stats(&transport).unknown_type == 1u);
    CHECK(pico_uart_transport_take_fault(&transport));
    pico_uart_transport_init(&transport);
    for (unsigned i = 0; i < PICO_UART_RX_RING_CAPACITY + 1u; ++i) {
        pico_uart_transport_rx_push_isr(&transport, (uint8_t)i);
    }
    CHECK(pico_uart_transport_get_stats(&transport).rx_overflow == 1u);
    CHECK(pico_uart_transport_take_fault(&transport));
    return failures == 0 ? 0 : 1;
}
