#include "uart_protocol.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s\n", #c); ++failures; } } while (0)

int main(void) {
    uint8_t payload[1] = {PICO_UART_MODE_RECORD};
    uint8_t frame[32];
    const size_t length = pico_uart_encode_frame(
        PICO_UART_MODE_SET, payload, sizeof(payload), frame, sizeof(frame));
    CHECK(length == 8u);
    CHECK(frame[0] == PICO_UART_PROTOCOL_MAGIC);
    CHECK(frame[1] == PICO_UART_PROTOCOL_VERSION);
    pico_uart_frame_t decoded;
    CHECK(pico_uart_decode_frame(frame, length, &decoded) == PICO_UART_DECODE_OK);
    CHECK(decoded.type == PICO_UART_MODE_SET && decoded.payload[0] == PICO_UART_MODE_RECORD);
    CHECK(pico_uart_frame_payload_valid(&decoded));
    frame[length - 1u] ^= 0x01u;
    CHECK(pico_uart_decode_frame(frame, length, &decoded) == PICO_UART_DECODE_BAD_CRC);
    frame[length - 1u] ^= 0x01u;
    frame[1] = 1u;
    CHECK(pico_uart_decode_frame(frame, length, &decoded) == PICO_UART_DECODE_BAD_VERSION);
    CHECK(pico_uart_type_is_command(PICO_UART_MODE_SET));
    CHECK(!pico_uart_type_is_command(PICO_UART_MODE_CHANGED));
    CHECK(!pico_uart_type_known(0x0Cu));
    return failures == 0 ? 0 : 1;
}
