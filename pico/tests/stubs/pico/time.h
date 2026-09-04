#ifndef PICO_KEYBOARD_TEST_PICO_TIME_H
#define PICO_KEYBOARD_TEST_PICO_TIME_H

#include <stdbool.h>
#include <stdint.h>

uint64_t time_us_64(void);

// Mirrors the real pico-sdk's opaque absolute_time_t: code must go through
// from_us_since_boot()/to_us_since_boot() rather than touching the field
// directly, so production code built against this fake behaves the same way
// it would against the real SDK type.
typedef struct {
    uint64_t _private_us_since_boot;
} absolute_time_t;

typedef int32_t alarm_id_t;

// Real signature returns int64_t: 0 means do not reschedule, a nonzero
// return asks the SDK to reschedule this same alarm. Production code in this
// project always returns 0 and re-arms explicitly from the main loop, but
// the fake keeps the real return type for signature compatibility.
typedef int64_t (*alarm_callback_t)(alarm_id_t id, void *user_data);

absolute_time_t from_us_since_boot(uint64_t us_since_boot);

alarm_id_t add_alarm_at(
    absolute_time_t time,
    alarm_callback_t callback,
    void *user_data,
    bool fire_if_past);
bool cancel_alarm(alarm_id_t id);

#endif  // PICO_KEYBOARD_TEST_PICO_TIME_H
