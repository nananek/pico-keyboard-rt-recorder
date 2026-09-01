#include "uart_protocol.h"

#include <string.h>

uint16_t pico_uart_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFFu;
    if (data == NULL) {
        return crc;
    }
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (crc & 0x8000u) != 0u
                ? (uint16_t)((crc << 1) ^ 0x1021u)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t pico_uart_encode_frame(
    uint8_t type,
    const uint8_t *payload,
    uint16_t payload_len,
    uint8_t *out,
    size_t out_size) {
    const size_t total = PICO_UART_PROTOCOL_HEADER_LEN + payload_len +
                         PICO_UART_PROTOCOL_CRC_LEN;
    if (out == NULL || payload_len > PICO_UART_PROTOCOL_MAX_PAYLOAD ||
        (payload_len != 0u && payload == NULL) || out_size < total ||
        !pico_uart_type_known(type)) {
        return 0u;
    }
    out[0] = PICO_UART_PROTOCOL_MAGIC;
    out[1] = PICO_UART_PROTOCOL_VERSION;
    out[2] = type;
    out[3] = (uint8_t)(payload_len & 0xFFu);
    out[4] = (uint8_t)(payload_len >> 8);
    if (payload_len != 0u) {
        memcpy(out + PICO_UART_PROTOCOL_HEADER_LEN, payload, payload_len);
    }
    const uint16_t crc = pico_uart_crc16(out + 1u, 4u + payload_len);
    out[5u + payload_len] = (uint8_t)(crc & 0xFFu);
    out[6u + payload_len] = (uint8_t)(crc >> 8);
    return total;
}

pico_uart_decode_result_t pico_uart_decode_frame(
    const uint8_t *data,
    size_t data_len,
    pico_uart_frame_t *frame) {
    if (data == NULL || frame == NULL || data_len < 7u) {
        return PICO_UART_DECODE_TOO_SHORT;
    }
    if (data[0] != PICO_UART_PROTOCOL_MAGIC) {
        return PICO_UART_DECODE_BAD_MAGIC;
    }
    if (data[1] != PICO_UART_PROTOCOL_VERSION) {
        return PICO_UART_DECODE_BAD_VERSION;
    }
    const uint16_t payload_len = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
    if (payload_len > PICO_UART_PROTOCOL_MAX_PAYLOAD ||
        data_len != (size_t)7u + payload_len) {
        return PICO_UART_DECODE_BAD_LENGTH;
    }
    const uint16_t expected = pico_uart_crc16(data + 1u, 4u + payload_len);
    const uint16_t actual = (uint16_t)data[5u + payload_len] |
                            ((uint16_t)data[6u + payload_len] << 8);
    if (expected != actual) {
        return PICO_UART_DECODE_BAD_CRC;
    }
    frame->version = data[1];
    frame->type = data[2];
    frame->payload_len = payload_len;
    if (payload_len != 0u) {
        memcpy(frame->payload, data + 5u, payload_len);
    }
    return PICO_UART_DECODE_OK;
}

bool pico_uart_type_known(uint8_t type) {
    switch (type) {
        case PICO_UART_RECORD_EVENT:
        case PICO_UART_PICO_STATUS:
        case PICO_UART_BUFFER_STATUS:
        case PICO_UART_PLAY_READY:
        case PICO_UART_PLAY_STARTED:
        case PICO_UART_PLAY_FINISHED:
        case PICO_UART_PLAY_ABORTED:
        case PICO_UART_PLAY_UNDERRUN:
        case PICO_UART_ERROR:
        case PICO_UART_PONG:
        case PICO_UART_MODE_CHANGED:
        case PICO_UART_QUEUE_CLEAR:
        case PICO_UART_QUEUE_EVENT:
        case PICO_UART_QUEUE_END:
        case PICO_UART_PLAY_START:
        case PICO_UART_PLAY_ABORT:
        case PICO_UART_STATUS_REQUEST:
        case PICO_UART_PING:
        case PICO_UART_MODE_SET:
            return true;
        default:
            return false;
    }
}

bool pico_uart_type_is_command(uint8_t type) {
    return type >= PICO_UART_QUEUE_CLEAR && type <= PICO_UART_MODE_SET;
}

bool pico_uart_frame_payload_valid(const pico_uart_frame_t *frame) {
    if (frame == NULL || !pico_uart_type_known(frame->type)) {
        return false;
    }
    switch (frame->type) {
        case PICO_UART_MODE_SET:
            return frame->payload_len == 1u && frame->payload[0] <= PICO_UART_MODE_ARMED;
        case PICO_UART_QUEUE_EVENT:
            return frame->payload_len == 17u && frame->payload[8] == 8u;
        case PICO_UART_PLAY_STARTED:
            return frame->payload_len == 8u;
        case PICO_UART_RECORD_EVENT:
            return frame->payload_len == 17u && frame->payload[8] == 8u;
        case PICO_UART_MODE_CHANGED:
            return frame->payload_len == 2u;
        case PICO_UART_BUFFER_STATUS:
            return frame->payload_len == 5u;
        case PICO_UART_PLAY_UNDERRUN:
            return frame->payload_len == 10u;
        default:
            return frame->payload_len == 0u;
    }
}
