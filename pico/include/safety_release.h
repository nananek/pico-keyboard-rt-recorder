#ifndef PICO_KEYBOARD_RT_SAFETY_RELEASE_H
#define PICO_KEYBOARD_RT_SAFETY_RELEASE_H

#include <stdbool.h>

typedef bool (*pico_safety_release_send_t)(void *user);

typedef struct {
    bool pending;
} pico_safety_release_t;

void pico_safety_release_init(pico_safety_release_t *release);
void pico_safety_release_request(pico_safety_release_t *release);

// Retries a requested all-keys release. It returns false while physical output
// must remain blocked until the HID endpoint has accepted that release.
bool pico_safety_release_service(
    pico_safety_release_t *release,
    pico_safety_release_send_t send,
    void *user);

#endif
