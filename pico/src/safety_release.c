#include "safety_release.h"

#include <stddef.h>

void pico_safety_release_init(pico_safety_release_t *release) {
    if (release != NULL) {
        release->pending = false;
    }
}

void pico_safety_release_request(pico_safety_release_t *release) {
    if (release != NULL) {
        release->pending = true;
    }
}

pico_safety_release_result_t pico_safety_release_service(
    pico_safety_release_t *release,
    pico_safety_release_send_t send,
    void *user) {
    if (release == NULL) {
        return PICO_SAFETY_RELEASE_BLOCKED;
    }
    if (!release->pending) {
        return PICO_SAFETY_RELEASE_READY;
    }
    if (send != NULL && send(user)) {
        release->pending = false;
        return PICO_SAFETY_RELEASE_SENT;
    }
    return PICO_SAFETY_RELEASE_BLOCKED;
}
