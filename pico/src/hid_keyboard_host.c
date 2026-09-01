#include "hid_keyboard_host.h"

#include <string.h>

#include "pico/time.h"
#include "tusb.h"

typedef struct {
    pico_keyboard_capture_t *capture;
    bool mounted;
    uint8_t dev_addr;
    uint8_t instance;
    pico_hid_keyboard_host_stats_t stats;
} pico_hid_keyboard_host_state_t;

static pico_hid_keyboard_host_state_t host_state;

static void request_next_report(uint8_t dev_addr, uint8_t instance) {
    ++host_state.stats.receive_requests;
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        ++host_state.stats.receive_errors;
    }
}

void pico_hid_keyboard_host_init(pico_keyboard_capture_t *capture) {
    memset(&host_state, 0, sizeof(host_state));
    host_state.capture = capture;

    // TinyUSB applies this while enumerating every Boot-subclass interface.
    // Calling it before host initialization makes the wire report format an
    // explicit 8-byte Boot Keyboard contract rather than a device default.
    tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
}

bool pico_hid_keyboard_host_is_mounted(void) {
    return host_state.mounted;
}

pico_hid_keyboard_host_stats_t pico_hid_keyboard_host_get_stats(void) {
    return host_state.stats;
}

void tuh_hid_mount_cb(
    uint8_t dev_addr,
    uint8_t instance,
    const uint8_t *report_desc,
    uint16_t desc_len) {
    (void)report_desc;
    (void)desc_len;

    if (tuh_hid_interface_protocol(dev_addr, instance) !=
        HID_ITF_PROTOCOL_KEYBOARD) {
        ++host_state.stats.unsupported_mounts;
        return;
    }

    // Phase 2 intentionally supports one Boot Keyboard interface. A second
    // keyboard remains unarmed rather than silently sharing capture state.
    if (host_state.mounted &&
        (host_state.dev_addr != dev_addr || host_state.instance != instance)) {
        ++host_state.stats.unsupported_mounts;
        return;
    }

    host_state.mounted = true;
    host_state.dev_addr = dev_addr;
    host_state.instance = instance;
    ++host_state.stats.keyboard_mounts;
    request_next_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    if (!host_state.mounted || host_state.dev_addr != dev_addr ||
        host_state.instance != instance) {
        return;
    }

    host_state.mounted = false;
    ++host_state.stats.keyboard_unmounts;
}

void tuh_hid_report_received_cb(
    uint8_t dev_addr,
    uint8_t instance,
    const uint8_t *report,
    uint16_t report_len) {
    const uint64_t timestamp_us = time_us_64();

    if (!host_state.mounted || host_state.dev_addr != dev_addr ||
        host_state.instance != instance) {
        return;
    }

    ++host_state.stats.report_callbacks;
    pico_keyboard_capture_push(
        host_state.capture, timestamp_us, report, report_len);

    // Malformed reports are rejected by capture, but reception is still
    // re-armed so one bad transfer cannot permanently stop the keyboard.
    request_next_report(dev_addr, instance);
}
