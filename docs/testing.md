# Testing and acceptance

## Automated checks

`sh pico/tests/run-host-tests.sh` builds with `-Wall -Wextra -Werror` and runs
the report layout, pin contract, capture FIFO, UART protocol/CRC, UART ring and
parser, mode transition, and host adapter tests. These cover split frames,
bad magic/version/length/CRC, unknown direction/type, ring order and overflow,
command/TX-ring saturation, idempotent MODE_SET, invalid targets/transitions,
all-release retry/queue clearing, and physical input blocking. The main-output
integration test verifies that PASS keeps its FIFO through a failed release,
the release-sent iteration, and a normal HID-not-ready result; it also verifies
that RECORD drains to UART while HID output is blocked. Only a validated command
can request a mode change. In particular, a repeated PASS command and a
transport or malformed-frame fault received in PASS do not release a held key or
clear accepted physical input.

`git diff --check` is required before commit. Docker CI additionally performs
the normal and HID-demo Pico SDK builds with TinyUSB's pinned Pico-PIO-USB
dependency.

## Hardware acceptance

1. Verify the fixed wiring: UART0 GP0 TX/GP1 RX crossed to the Zero with common
   ground, PIO-USB GP12 D+/GP13 D-, and protected keyboard VBUS. No mode wire is
   present.
2. Flash the normal image and confirm native HID enumeration. In PASS, press and
   release a physical key and observe the same reports at the PC.
3. Send a version-2 `MODE_SET(RECORD)` frame from the Zero. Confirm physical
   reports stop reaching the PC and each report arrives as a CRC-valid
   `RECORD_EVENT` with a nondecreasing Pico timestamp.
4. Send `MODE_SET(PASS)` and verify `MODE_CHANGED(PASS, OK)`, an all-zero HID
   release, and no replay of a stale key; the next host report is the first
   forwarded report.
5. Verify `MODE_SET(ARMED)`, `PLAY_START`, and `PLAY_ABORT` transitions. Physical
   reports remain blocked in ARMED/PLAYING, and abort releases all keys.
6. Inject bad CRC/version/length bytes and UART framing errors. Confirm ERROR,
   all-release, blocked input, and recovery only after a valid
   `MODE_SET(PASS)` while ARMED or PLAYING. In PASS, confirm the same malformed
   input leaves a held key intact while reporting the fault. Disconnect/reconnect
   UART and repeat.

UART event time is not used as a HID deadline. Playback timing must be checked
against the Pico hardware timer in the later playback acceptance phase.
