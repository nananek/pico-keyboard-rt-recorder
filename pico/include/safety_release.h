#ifndef PICO_KEYBOARD_RT_SAFETY_RELEASE_H
#define PICO_KEYBOARD_RT_SAFETY_RELEASE_H

#include <stdbool.h>

typedef bool (*pico_safety_release_send_t)(void *user);

typedef struct {
    bool pending;
} pico_safety_release_t;

typedef enum {
    PICO_SAFETY_RELEASE_BLOCKED,
    PICO_SAFETY_RELEASE_SENT,
    PICO_SAFETY_RELEASE_READY,
} pico_safety_release_result_t;

void pico_safety_release_init(pico_safety_release_t *release);
void pico_safety_release_request(pico_safety_release_t *release);

// Retries a requested all-keys release. A report sent in this call is distinct
// from READY: PASS must wait one more main-loop iteration before sending a
// physical report because TinyUSB will normally make the endpoint non-ready.
pico_safety_release_result_t pico_safety_release_service(
    pico_safety_release_t *release,
    pico_safety_release_send_t send,
    void *user);

#endif
