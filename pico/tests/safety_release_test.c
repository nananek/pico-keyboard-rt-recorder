#include "safety_release.h"

#include <stdio.h>

static int failures;
static unsigned sends;
static bool send_succeeds;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static bool send_release(void *user) {
    (void)user;
    ++sends;
    return send_succeeds;
}

int main(void) {
    pico_safety_release_t release;
    pico_safety_release_init(&release);

    CHECK(pico_safety_release_service(&release, send_release, NULL) ==
          PICO_SAFETY_RELEASE_READY);
    CHECK(sends == 0u);

    pico_safety_release_request(&release);
    CHECK(release.pending);
    CHECK(pico_safety_release_service(&release, send_release, NULL) ==
          PICO_SAFETY_RELEASE_BLOCKED);
    CHECK(release.pending && sends == 1u);

    send_succeeds = true;
    CHECK(pico_safety_release_service(&release, send_release, NULL) ==
          PICO_SAFETY_RELEASE_SENT);
    CHECK(!release.pending && sends == 2u);

    // Multiple state changes before the endpoint becomes ready need one
    // eventual all-release report, not a queue of duplicate reports.
    pico_safety_release_request(&release);
    pico_safety_release_request(&release);
    CHECK(pico_safety_release_service(&release, send_release, NULL) ==
          PICO_SAFETY_RELEASE_SENT);
    CHECK(!release.pending && sends == 3u);

    return failures == 0 ? 0 : 1;
}
