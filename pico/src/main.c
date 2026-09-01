#include "bsp/board.h"
#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "hardware_config.h"
#include "hid_boot_keyboard.h"
#include "hid_keyboard_device.h"
#include "hid_keyboard_host.h"
#include "keyboard_capture.h"

static pico_keyboard_capture_t keyboard_capture;

static void mode_gate_init(void) {
    // GP2 is distinct from UART0 GP0/GP1 and PIO-USB GP12/GP13. The pull-down
    // makes loss of the Pi Zero drive resolve to LOW/PASS.
    gpio_init(PICO_KEYBOARD_MODE_GPIO);
    gpio_set_dir(PICO_KEYBOARD_MODE_GPIO, GPIO_IN);
    gpio_pull_down(PICO_KEYBOARD_MODE_GPIO);
}

#if PICO_HID_HOST_CAPTURE_TEST
static void capture_diagnostic_init(void) {
    uart_init(uart0, 115200u);
    gpio_set_function(PICO_KEYBOARD_UART_TX_GPIO, GPIO_FUNC_UART);
    gpio_set_function(PICO_KEYBOARD_UART_RX_GPIO, GPIO_FUNC_UART);
}

static void capture_diagnostic_task(void) {
    pico_keyboard_capture_event_t event;
    char line[128];

    while (pico_keyboard_capture_pop(&keyboard_capture, &event)) {
        const int count = snprintf(
            line,
            sizeof(line),
            "CAPTURE %llu %u %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
            (unsigned long long)event.timestamp_us,
            event.report_len,
            event.report.modifier,
            event.report.reserved,
            event.report.keycode[0],
            event.report.keycode[1],
            event.report.keycode[2],
            event.report.keycode[3],
            event.report.keycode[4],
            event.report.keycode[5]);
        if (count > 0 && (size_t)count < sizeof(line)) {
            uart_write_blocking(uart0, (const uint8_t *)line, (size_t)count);
        }
    }
}
#endif

#if PICO_HID_DEMO_TEST
static void hid_demo_test_task(void) {
    enum {
        HID_DEMO_WAIT_FOR_ENDPOINT,
        HID_DEMO_WAIT_TO_RELEASE,
        HID_DEMO_COMPLETE,
    };
    static uint8_t stage = HID_DEMO_WAIT_FOR_ENDPOINT;
    static uint32_t release_at_ms;

    if (stage == HID_DEMO_COMPLETE || !tud_mounted()) {
        return;
    }

    if (stage == HID_DEMO_WAIT_FOR_ENDPOINT) {
        pico_hid_boot_keyboard_report_t press_report;
        pico_hid_boot_keyboard_make_key_press(
            &press_report, 0u, PICO_HID_USAGE_KEY_A);

        if (pico_hid_keyboard_send_boot_report(&press_report)) {
            release_at_ms = to_ms_since_boot(get_absolute_time()) + 50u;
            stage = HID_DEMO_WAIT_TO_RELEASE;
        }
        return;
    }

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now_ms - release_at_ms) >= 0 &&
        pico_hid_keyboard_send_all_keys_release()) {
        stage = HID_DEMO_COMPLETE;
    }
}
#endif

int main(void) {
    board_init();
    mode_gate_init();
    pico_keyboard_capture_init(&keyboard_capture);
    pico_hid_keyboard_host_init(&keyboard_capture);

#if PICO_HID_HOST_CAPTURE_TEST
    capture_diagnostic_init();
#endif

    const tusb_rhport_init_t device_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    const tusb_rhport_init_t host_init = {
        .role = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_FULL,
    };

    if (!tusb_init(PICO_KEYBOARD_NATIVE_DEVICE_RHPORT, &device_init)) {
        panic("TinyUSB native device initialization failed");
    }
    if (!tusb_init(PICO_KEYBOARD_PIO_HOST_RHPORT, &host_init)) {
        panic("TinyUSB PIO host initialization failed");
    }

    while (true) {
        tud_task();
        tuh_task();

#if PICO_HID_DEMO_TEST
        hid_demo_test_task();
#endif

#if PICO_HID_HOST_CAPTURE_TEST
        capture_diagnostic_task();
#endif

        // USB work is poll-driven on one core in this phase. Playback
        // scheduling will add a Pico hardware alarm in its own phase.
        sleep_ms(1u);
    }
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t *buffer,
    uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    const uint8_t *buffer,
    uint16_t bufsize) {
    // Version 1 does not use host keyboard LED output reports yet. Consume the
    // callback so an OS may still issue its normal HID class requests.
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}
