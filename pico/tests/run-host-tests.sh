#!/bin/sh
set -eu

compiler="${CC:-cc}"
test_binary="$(mktemp "${TMPDIR:-/tmp}/pico-hid-test.XXXXXX")"
trap 'rm -f "$test_binary"' EXIT HUP INT TERM

"$compiler" -std=c11 -Wall -Wextra -Werror \
    -I"$(dirname "$0")/../include" \
    "$(dirname "$0")/../src/hid_boot_keyboard.c" \
    "$(dirname "$0")/hid_boot_keyboard_test.c" \
    -o "$test_binary"

"$test_binary"
