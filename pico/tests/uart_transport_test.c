#include "uart_transport.h"

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

static void feed(
    pico_uart_transport_t *transport,
    const uint8_t *data,
    size_t length) {
    for (size_t i = 0; i < length; ++i) {
        pico_uart_transport_rx_push_isr(transport, data[i]);
    }
}

static size_t encode_mode(uint8_t mode, uint8_t *frame, size_t size) {
    return pico_uart_encode_frame(PICO_UART_MODE_SET, &mode, 1u, frame, size);
}

static void test_split_frame_and_ring_wraparound(void) {
    pico_uart_transport_t transport;
    pico_uart_frame_t command;
    uint8_t frame[8];
    const size_t length = encode_mode(PICO_UART_MODE_ARMED, frame, sizeof(frame));

    pico_uart_transport_init(&transport);
    feed(&transport, frame, 3u);
    pico_uart_transport_poll(&transport);
    CHECK(!pico_uart_transport_pop_command(&transport, &command));

    feed(&transport, frame + 3u, length - 3u);
    pico_uart_transport_poll(&transport);
    CHECK(pico_uart_transport_pop_command(&transport, &command));
    CHECK(command.type == PICO_UART_MODE_SET);
    CHECK(command.payload[0] == PICO_UART_MODE_ARMED);
    CHECK(!pico_uart_transport_take_fault(&transport));

    for (unsigned i = 0u; i < 40u; ++i) {
        feed(&transport, frame, length);
        pico_uart_transport_poll(&transport);
        CHECK(pico_uart_transport_pop_command(&transport, &command));
        CHECK(command.payload[0] == PICO_UART_MODE_ARMED);
    }
    CHECK(!pico_uart_transport_take_fault(&transport));
}

static void test_parser_rejects_and_resynchronizes(void) {
    pico_uart_transport_t transport;
    pico_uart_frame_t command;
    uint8_t frame[8];
    const size_t length = encode_mode(PICO_UART_MODE_ARMED, frame, sizeof(frame));
    const uint8_t bad_magic = 0u;

    pico_uart_transport_init(&transport);
    feed(&transport, &bad_magic, 1u);
    feed(&transport, frame, length);
    pico_uart_transport_poll(&transport);
    CHECK(pico_uart_transport_get_stats(&transport).bad_magic == 1u);
    CHECK(pico_uart_transport_get_stats(&transport).invalid_frames == 1u);
    CHECK(pico_uart_transport_take_fault(&transport));
    CHECK(pico_uart_transport_pop_command(&transport, &command));

    pico_uart_transport_init(&transport);
    uint8_t bad_version[8];
    for (size_t i = 0u; i < length; ++i) {
        bad_version[i] = frame[i];
    }
    bad_version[1] = 1u;
    feed(&transport, bad_version, length);
    feed(&transport, frame, length);
    pico_uart_transport_poll(&transport);
    CHECK(pico_uart_transport_take_fault(&transport));
    CHECK(pico_uart_transport_get_stats(&transport).bad_version == 1u);
    CHECK(pico_uart_transport_pop_command(&transport, &command));

    pico_uart_transport_init(&transport);
    const uint8_t bad_length[] = {
        PICO_UART_PROTOCOL_MAGIC, PICO_UART_PROTOCOL_VERSION,
        PICO_UART_MODE_SET, PICO_UART_PROTOCOL_MAX_PAYLOAD + 1u, 0u,
    };
    feed(&transport, bad_length, sizeof(bad_length));
    feed(&transport, frame, length);
    pico_uart_transport_poll(&transport);
    CHECK(pico_uart_transport_take_fault(&transport));
    CHECK(pico_uart_transport_get_stats(&transport).bad_length == 1u);
    CHECK(pico_uart_transport_pop_command(&transport, &command));

    pico_uart_transport_init(&transport);
    uint8_t bad_crc[8];
    for (size_t i = 0u; i < length; ++i) {
        bad_crc[i] = frame[i];
    }
    bad_crc[length - 1u] ^= 1u;
    feed(&transport, bad_crc, length);
    feed(&transport, frame, length);
    pico_uart_transport_poll(&transport);
    CHECK(pico_uart_transport_take_fault(&transport));
    CHECK(pico_uart_transport_get_stats(&transport).bad_crc == 1u);
    CHECK(pico_uart_transport_pop_command(&transport, &command));
}

static void test_command_validation_and_overflow_faults(void) {
    pico_uart_transport_t transport;
    pico_uart_frame_t command;
    uint8_t frame[16];
    const size_t length = encode_mode(PICO_UART_MODE_PLAYING, frame, sizeof(frame));

    pico_uart_transport_init(&transport);
    feed(&transport, frame, length);
    pico_uart_transport_poll(&transport);
    CHECK(!pico_uart_transport_take_fault(&transport));
    CHECK(pico_uart_transport_pop_command(&transport, &command));
    CHECK(command.payload[0] == PICO_UART_MODE_PLAYING);

    pico_uart_transport_init(&transport);
    const uint8_t mode_changed[] = {PICO_UART_MODE_PASS, PICO_UART_REASON_OK};
    const size_t mode_changed_length = pico_uart_encode_frame(
        PICO_UART_MODE_CHANGED, mode_changed, sizeof(mode_changed), frame,
        sizeof(frame));
    feed(&transport, frame, mode_changed_length);
    pico_uart_transport_poll(&transport);
    CHECK(pico_uart_transport_take_fault(&transport));
    CHECK(pico_uart_transport_get_stats(&transport).invalid_frames == 1u);
    CHECK(!pico_uart_transport_pop_command(&transport, &command));

    pico_uart_transport_init(&transport);
    // 0x0D is the first Pico-to-Zero value still unassigned once
    // PLAY_METRICS (0x0C) is in use.
    const uint8_t unknown_type[] = {
        PICO_UART_PROTOCOL_MAGIC, PICO_UART_PROTOCOL_VERSION, 0x0Du, 1u, 0u,
        PICO_UART_MODE_ARMED, 0u, 0u,
    };
    uint8_t unknown_frame[sizeof(unknown_type)];
    for (size_t i = 0u; i < sizeof(unknown_type); ++i) {
        unknown_frame[i] = unknown_type[i];
    }
    const uint16_t crc = pico_uart_crc16(unknown_frame + 1u, 5u);
    unknown_frame[6] = (uint8_t)(crc & 0xFFu);
    unknown_frame[7] = (uint8_t)(crc >> 8);
    feed(&transport, unknown_frame, sizeof(unknown_frame));
    pico_uart_transport_poll(&transport);
    CHECK(pico_uart_transport_take_fault(&transport));
    CHECK(pico_uart_transport_get_stats(&transport).unknown_type == 1u);

    pico_uart_transport_init(&transport);
    const size_t valid_length = encode_mode(PICO_UART_MODE_ARMED, frame, sizeof(frame));
    for (unsigned i = 0u; i < PICO_UART_COMMAND_CAPACITY + 1u; ++i) {
        feed(&transport, frame, valid_length);
        pico_uart_transport_poll(&transport);
    }
    CHECK(pico_uart_transport_take_fault(&transport));
    CHECK(pico_uart_transport_get_stats(&transport).command_overflow == 1u);

    pico_uart_transport_init(&transport);
    const uint8_t status[5] = {0u};
    for (unsigned i = 0u; i < 85u; ++i) {
        CHECK(pico_uart_transport_queue_frame(
            &transport, PICO_UART_PICO_STATUS, status, sizeof(status)));
    }
    CHECK(!pico_uart_transport_queue_frame(
        &transport, PICO_UART_PICO_STATUS, status, sizeof(status)));
    CHECK(pico_uart_transport_get_stats(&transport).tx_dropped == 1u);
    uint8_t tx_byte;
    unsigned tx_bytes = 0u;
    while (pico_uart_transport_pop_tx_byte(&transport, &tx_byte)) {
        ++tx_bytes;
    }
    CHECK(tx_bytes == 1020u);

    pico_uart_transport_init(&transport);
    for (unsigned i = 0u; i < PICO_UART_RX_RING_CAPACITY + 1u; ++i) {
        pico_uart_transport_rx_push_isr(&transport, (uint8_t)i);
    }
    CHECK(pico_uart_transport_get_stats(&transport).rx_overflow == 1u);
    CHECK(pico_uart_transport_take_fault(&transport));

    pico_uart_transport_init(&transport);
    pico_uart_transport_hardware_error_isr(&transport);
    CHECK(pico_uart_transport_get_stats(&transport).hardware_errors == 1u);
    CHECK(pico_uart_transport_take_fault(&transport));
}

int main(void) {
    test_split_frame_and_ring_wraparound();
    test_parser_rejects_and_resynchronizes();
    test_command_validation_and_overflow_faults();
    return failures == 0 ? 0 : 1;
}
