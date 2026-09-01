#ifndef PICO_KEYBOARD_RT_UART_PROTOCOL_H
#define PICO_KEYBOARD_RT_UART_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    PICO_UART_PROTOCOL_MAGIC = 0xA5u,
    PICO_UART_PROTOCOL_VERSION = 2u,
    PICO_UART_PROTOCOL_MAX_PAYLOAD = 64u,
    PICO_UART_PROTOCOL_HEADER_LEN = 5u,
    PICO_UART_PROTOCOL_CRC_LEN = 2u,
};

/* Pico-to-Zero messages. */
enum {
    PICO_UART_RECORD_EVENT = 0x01u,
    PICO_UART_PICO_STATUS = 0x02u,
    PICO_UART_BUFFER_STATUS = 0x03u,
    PICO_UART_PLAY_READY = 0x04u,
    PICO_UART_PLAY_STARTED = 0x05u,
    PICO_UART_PLAY_FINISHED = 0x06u,
    PICO_UART_PLAY_ABORTED = 0x07u,
    PICO_UART_PLAY_UNDERRUN = 0x08u,
    PICO_UART_ERROR = 0x09u,
    PICO_UART_PONG = 0x0Au,
    PICO_UART_MODE_CHANGED = 0x0Bu,
};

/* Zero-to-Pico messages. */
enum {
    PICO_UART_QUEUE_CLEAR = 0x80u,
    PICO_UART_QUEUE_EVENT = 0x81u,
    PICO_UART_QUEUE_END = 0x82u,
    PICO_UART_PLAY_START = 0x83u,
    PICO_UART_PLAY_ABORT = 0x84u,
    PICO_UART_STATUS_REQUEST = 0x85u,
    PICO_UART_PING = 0x86u,
    PICO_UART_MODE_SET = 0x87u,
};

enum {
    PICO_UART_MODE_PASS = 0u,
    PICO_UART_MODE_RECORD = 1u,
    PICO_UART_MODE_ARMED = 2u,
    PICO_UART_MODE_PLAYING = 3u,
    PICO_UART_MODE_ERROR = 4u,
};

enum {
    PICO_UART_REASON_OK = 0u,
    PICO_UART_REASON_INVALID_TRANSITION = 1u,
    PICO_UART_REASON_INVALID_TARGET = 2u,
    PICO_UART_REASON_PROTOCOL_ERROR = 3u,
    PICO_UART_REASON_UART_FAULT = 4u,
    PICO_UART_REASON_ABORTED = 5u,
    PICO_UART_REASON_UNDERRUN = 6u,
};

typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t payload_len;
    uint8_t payload[PICO_UART_PROTOCOL_MAX_PAYLOAD];
} pico_uart_frame_t;

typedef enum {
    PICO_UART_DECODE_OK = 0,
    PICO_UART_DECODE_TOO_SHORT,
    PICO_UART_DECODE_BAD_MAGIC,
    PICO_UART_DECODE_BAD_VERSION,
    PICO_UART_DECODE_BAD_LENGTH,
    PICO_UART_DECODE_BAD_CRC,
} pico_uart_decode_result_t;

uint16_t pico_uart_crc16(const uint8_t *data, size_t length);

/* Returns the complete frame length, or zero when output space/input is bad. */
size_t pico_uart_encode_frame(
    uint8_t type,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out,
    size_t out_size);

pico_uart_decode_result_t pico_uart_decode_frame(
    const uint8_t *data,
    size_t data_len,
    pico_uart_frame_t *frame);

bool pico_uart_type_known(uint8_t type);
bool pico_uart_type_is_command(uint8_t type);
bool pico_uart_frame_payload_valid(const pico_uart_frame_t *frame);

#endif
