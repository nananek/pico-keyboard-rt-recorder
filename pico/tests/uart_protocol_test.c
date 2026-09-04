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
    // 0x0D is the first Pico-to-Zero value still unassigned once PLAY_METRICS
    // (0x0C) is in use.
    CHECK(!pico_uart_type_known(0x0Du));
}

// PLAY_STARTED, PLAY_FINISHED, PLAY_ABORTED, and PLAY_METRICS previously had
// no test coverage: PLAY_STARTED/PLAY_FINISHED/PLAY_ABORTED payload shapes
// were reserved-but-unused, and PLAY_METRICS is new in Issue #7.
static void test_playback_scheduler_message_payloads(void) {
    pico_uart_frame_t frame = {
        .version = PICO_UART_PROTOCOL_VERSION,
        .type = PICO_UART_PLAY_STARTED,
        .payload_len = 8u,
    };
    CHECK(pico_uart_frame_payload_valid(&frame));
    frame.payload_len = 7u;
    CHECK(!pico_uart_frame_payload_valid(&frame));
    frame.payload_len = 9u;
    CHECK(!pico_uart_frame_payload_valid(&frame));

    // PLAY_FINISHED and PLAY_ABORTED carry no payload, like PLAY_READY.
    frame.type = PICO_UART_PLAY_FINISHED;
    frame.payload_len = 0u;
    CHECK(pico_uart_frame_payload_valid(&frame));
    frame.payload_len = 1u;
    CHECK(!pico_uart_frame_payload_valid(&frame));

    frame.type = PICO_UART_PLAY_ABORTED;
    frame.payload_len = 0u;
    CHECK(pico_uart_frame_payload_valid(&frame));
    frame.payload_len = 1u;
    CHECK(!pico_uart_frame_payload_valid(&frame));

    // PLAY_METRICS: dispatched_count u32, underrun_count u32,
    // min/max_lateness_us i32, sum_lateness_us i64, p95/p99_lateness_us i32,
    // samples_truncated u8 = 33 bytes.
    frame.type = PICO_UART_PLAY_METRICS;
    frame.payload_len = 33u;
    CHECK(pico_uart_frame_payload_valid(&frame));
    frame.payload_len = 32u;
    CHECK(!pico_uart_frame_payload_valid(&frame));
    frame.payload_len = 34u;
    CHECK(!pico_uart_frame_payload_valid(&frame));

    CHECK(pico_uart_type_known(PICO_UART_PLAY_METRICS));
    CHECK(!pico_uart_type_is_command(PICO_UART_PLAY_METRICS));
}

static void test_queue_command_state_guard(void) {
    CHECK(pico_uart_queue_command_allowed(
        PICO_UART_QUEUE_CLEAR, PICO_UART_MODE_ARMED));
    CHECK(!pico_uart_queue_command_allowed(
        PICO_UART_QUEUE_CLEAR, PICO_UART_MODE_PLAYING));

    for (uint8_t type = PICO_UART_QUEUE_EVENT;
         type <= PICO_UART_QUEUE_END;
         ++type) {
        CHECK(pico_uart_queue_command_allowed(type, PICO_UART_MODE_ARMED));
        CHECK(pico_uart_queue_command_allowed(type, PICO_UART_MODE_PLAYING));
        CHECK(!pico_uart_queue_command_allowed(type, PICO_UART_MODE_PASS));
        CHECK(!pico_uart_queue_command_allowed(type, PICO_UART_MODE_RECORD));
        CHECK(!pico_uart_queue_command_allowed(type, PICO_UART_MODE_ERROR));
    }
    CHECK(!pico_uart_queue_command_allowed(
        PICO_UART_PLAY_START, PICO_UART_MODE_ARMED));
}

int main(void) {
    test_encode_decode_and_crc();
    test_decode_rejects_frame_errors();
    test_command_payload_and_type_rules();
    test_playback_scheduler_message_payloads();
    test_queue_command_state_guard();
    return failures == 0 ? 0 : 1;
}
