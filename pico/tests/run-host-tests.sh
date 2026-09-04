#!/bin/sh
set -eu

compiler="${CC:-cc}"
test_directory="$(mktemp -d "${TMPDIR:-/tmp}/pico-hid-tests.XXXXXX")"
trap 'rm -rf "$test_directory"' EXIT HUP INT TERM

test_root="$(dirname "$0")"
include_root="$test_root/../include"

build_and_run() {
    name="$1"
    shift
    "$compiler" -std=c11 -Wall -Wextra -Werror \
        -I"$include_root" "$@" -o "$test_directory/$name"
    "$test_directory/$name"
}

build_and_run hid_boot_keyboard \
    "$test_root/../src/hid_boot_keyboard.c" \
    "$test_root/hid_boot_keyboard_test.c"

build_and_run hardware_config \
    -DBOARD_TUD_RHPORT=0 \
    -DBOARD_TUH_RHPORT=1 \
    -DPICO_DEFAULT_PIO_USB_DP_PIN=12 \
    "$test_root/hardware_config_test.c"

build_and_run keyboard_capture \
    "$test_root/../src/keyboard_capture.c" \
    "$test_root/keyboard_capture_test.c"

build_and_run playback_queue \
    "$test_root/../src/playback_queue.c" \
    "$test_root/playback_queue_test.c"

build_and_run uart_protocol \
    "$test_root/../src/uart_protocol.c" \
    "$test_root/uart_protocol_test.c"

build_and_run uart_transport \
    -I"$test_root/stubs" \
    "$test_root/../src/uart_protocol.c" \
    "$test_root/../src/uart_transport.c" \
    "$test_root/uart_transport_test.c"

build_and_run mode_state \
    "$test_root/../src/mode_state.c" \
    "$test_root/mode_state_test.c"

build_and_run safety_release \
    "$test_root/../src/safety_release.c" \
    "$test_root/safety_release_test.c"

build_and_run main_dispatch \
    "$test_root/../src/keyboard_capture.c" \
    "$test_root/../src/mode_state.c" \
    "$test_root/../src/physical_report_dispatch.c" \
    "$test_root/../src/safety_release.c" \
    "$test_root/main_dispatch_test.c"

"$compiler" -std=c11 -Wall -Wextra -Werror \
    -I"$test_root/stubs" -I"$include_root" \
    "$test_root/../src/keyboard_capture.c" \
    "$test_root/../src/mode_state.c" \
    "$test_root/../src/hid_keyboard_host.c" \
    "$test_root/hid_keyboard_host_test.c" \
    -o "$test_directory/hid_keyboard_host"
"$test_directory/hid_keyboard_host"

# Needs the pico/time.h alarm-API fakes under stubs/, like hid_keyboard_host
# above.
"$compiler" -std=c11 -Wall -Wextra -Werror \
    -I"$test_root/stubs" -I"$include_root" \
    "$test_root/../src/playback_queue.c" \
    "$test_root/../src/playback_scheduler.c" \
    "$test_root/playback_scheduler_test.c" \
    -o "$test_directory/playback_scheduler"
"$test_directory/playback_scheduler"
