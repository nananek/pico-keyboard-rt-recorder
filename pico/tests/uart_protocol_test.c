#include "uart_protocol.h"

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

static void test_encode_decode_and_crc(void) {
    const uint8_t payload[1] = {PICO_UART_MODE_RECORD};
    const uint8_t mode_changed[2] = {
        PICO_UART_MODE_RECORD,
        PICO_UART_REASON_OK,
    };
    uint8_t frame[32];
    const size_t length = pico_uart_encode_frame(
        PICO_UART_MODE_SET, payload, sizeof(payload), frame, sizeof(frame));
    pico_uart_frame_t decoded;

    CHECK(pico_uart_crc16((const uint8_t *)"123456789", 9u) == 0x29B1u);
    CHECK(length == 8u);
    CHECK(frame[0] == PICO_UART_PROTOCOL_MAGIC);
    CHECK(frame[1] == PICO_UART_PROTOCOL_VERSION);
    CHECK(pico_uart_decode_frame(frame, length, &decoded) == PICO_UART_DECODE_OK);
    CHECK(decoded.type == PICO_UART_MODE_SET);
    CHECK(decoded.payload[0] == PICO_UART_MODE_RECORD);
    CHECK(pico_uart_frame_payload_valid(&decoded));

    frame[length - 1u] ^= 0x01u;
    CHECK(pico_uart_decode_frame(frame, length, &decoded) == PICO_UART_DECODE_BAD_CRC);

    const size_t changed_length = pico_uart_encode_frame(
        PICO_UART_MODE_CHANGED, mode_changed, sizeof(mode_changed), frame,
        sizeof(frame));
    CHECK(changed_length == 9u);
    CHECK(pico_uart_decode_frame(frame, changed_length, &decoded) ==
          PICO_UART_DECODE_OK);
    CHECK(decoded.type == PICO_UART_MODE_CHANGED);
    CHECK(pico_uart_frame_payload_valid(&decoded));
}

static void test_decode_rejects_frame_errors(void) {
    const uint8_t payload[1] = {PICO_UART_MODE_ARMED};
    uint8_t frame[32];
    pico_uart_frame_t decoded;
    const size_t length = pico_uart_encode_frame(
        PICO_UART_MODE_SET, payload, sizeof(payload), frame, sizeof(frame));

    frame[0] = 0u;
    CHECK(pico_uart_decode_frame(frame, length, &decoded) == PICO_UART_DECODE_BAD_MAGIC);
    frame[0] = PICO_UART_PROTOCOL_MAGIC;

    frame[1] = 1u;
    CHECK(pico_uart_decode_frame(frame, length, &decoded) == PICO_UART_DECODE_BAD_VERSION);
    frame[1] = PICO_UART_PROTOCOL_VERSION;

    frame[3] = 2u;
    CHECK(pico_uart_decode_frame(frame, length, &decoded) == PICO_UART_DECODE_BAD_LENGTH);
}

static void test_command_payload_and_type_rules(void) {
    pico_uart_frame_t frame = {
        .version = PICO_UART_PROTOCOL_VERSION,
        .type = PICO_UART_MODE_SET,
        .payload_len = 1u,
        .payload = {PICO_UART_MODE_PLAYING},
    };
    const pico_uart_frame_t status = {
        .version = PICO_UART_PROTOCOL_VERSION,
        .type = PICO_UART_PICO_STATUS,
        .payload_len = 5u,
    };

    CHECK(pico_uart_frame_payload_valid(&frame));
    CHECK(pico_uart_frame_payload_valid(&status));
    CHECK(pico_uart_type_is_command(PICO_UART_MODE_SET));
    CHECK(!pico_uart_type_is_command(PICO_UART_MODE_CHANGED));
    CHECK(!pico_uart_type_known(0x0Cu));
}

int main(void) {
    test_encode_decode_and_crc();
    test_decode_rejects_frame_errors();
    test_command_payload_and_type_rules();
    return failures == 0 ? 0 : 1;
}
