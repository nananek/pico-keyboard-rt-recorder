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

"$compiler" -std=c11 -Wall -Wextra -Werror \
    -I"$test_root/stubs" -I"$include_root" \
    "$test_root/../src/keyboard_capture.c" \
    "$test_root/../src/hid_keyboard_host.c" \
    "$test_root/hid_keyboard_host_test.c" \
    -o "$test_directory/hid_keyboard_host"
"$test_directory/hid_keyboard_host"
