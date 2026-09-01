# Testing and acceptance

## Automated tests

Maintain hardware-independent tests for:

- exact UART0 GP0/GP1, GP2 mode-input, and GP12 D+ / GP13 D- contracts,
  adjacency, range, and pairwise non-overlap;
- fixed-length capture, exact 64-bit timestamp preservation, duplicate/release
  retention, invalid-length rejection, FIFO order, and overflow accounting;
- HID host keyboard-only filtering, Boot protocol selection, receive re-arming,
  error counting, and unmount state clearing;
- UART framing, CRC, partial reads, resynchronization, and invalid frames;
- recording conversion from Pico absolute timestamps to stored deltas;
- playback conversion from deltas to absolute, speed-scaled offsets;
- queue accounting, `QUEUE_END`, and underrun state transitions;
- API validation and atomic recording persistence.

## Hardware tests by milestone

### USB HID keyboard device

Build `pico_keyboard_hid` for `PICO_BOARD=pico2`, flash the resulting UF2, and
connect its USB device port to a PC. The PC must enumerate one Boot Keyboard
interface named `Pico 2 Boot Keyboard`.

For a controlled report-path test, rebuild with `-DPICO_HID_DEMO_TEST=ON`. After
mount, observe exactly one `a` key press and an all-zero 8-byte release report
50 ms later in the OS input-event viewer. Confirm that no key remains held.
The default build sends neither report on its own.

### PIO-USB host and receive-boundary capture (Phase 2)

Build with `-DPICO_HID_HOST_CAPTURE_TEST=ON`, flash the UF2, and connect UART0
TX GP0 to a 3.3 V USB-UART receiver at 115200 baud with a common ground. Then:

1. Verify protected, current-limited 5 V keyboard VBUS and GP12 D+ / GP13 D-
   DPDM wiring before connecting a common Boot Keyboard/6KRO keyboard.
2. Confirm each diagnostic line is `CAPTURE <timestamp_us> 8` followed by the
   exact eight raw report bytes.
3. Exercise a key press, a held/duplicate report, modifiers, simultaneous keys,
   and an all-zero release. Every USB host transfer must be retained without
   deduplication, byte translation, or a missing release.
4. Confirm timestamps are nondecreasing and are preserved unchanged through
   the diagnostic consumer. They must be sampled by Pico at the TinyUSB host
   callback entry, not when UART output is written.
5. Send or stub a malformed-length report and confirm it is rejected while the
   receive endpoint is re-armed. A mouse or non-Boot HID interface must not be
   armed as the keyboard source.
6. Unplug and reconnect the keyboard. Confirm the active interface clears on
   unmount and captures resume after a new mount with no stale report.
7. In a normal build, confirm there is no text on UART. Phase 2 does not yet
   provide production pass-through or binary UART `RECORD_EVENT` framing.

Repeat the native USB device test above with both the normal and capture builds,
then repeat `PICO_HID_DEMO_TEST=ON` to ensure the Phase 1 one-press/one-release
device path still works while host support is linked.

On GP2 (Pico header pin 4), measure LOW while undriven due to the internal
pull-down and HIGH only while driven with 3.3 V logic. Confirm this does not
disturb UART0 GP0/GP1 traffic or PIO-USB traffic on GP12/GP13.

Future pass-through acceptance will connect the same host path to the native
USB HID device and verify typing. It is intentionally outside Phase 2.

### Scheduler jitter

Use Pico scheduler test mode to emit at least 1,000 reports at 10 ms intervals. Collect scheduled deadline and actual dispatch time. The initial acceptance target is maximum lateness below 1 ms; retain min, max, mean, p95, p99, and count.

### Linux and UART isolation

Run CPU load, web requests, disk I/O, and intentional 50–100 ms UART feeder stalls on Zero. With at least 500 ms already queued on Pico, dispatch measurements must not show timing impact attributable to those stalls.

### Underrun and failure safety

Stop the feeder for longer than prebuffer capacity and verify Pico sends all keys released, reports underrun, enters ERROR, and blocks physical keyboard reports until mode GPIO becomes LOW. Repeat for UART disconnect, abort, GPIO LOW, and Zero shutdown.

## Functional acceptance

- Record and reproduce `hello` with comparable event intervals.
- Reproduce Shift+A, Ctrl+C, Ctrl+V, and Alt-modified input.
- Reproduce simultaneous key state within Boot Keyboard/6KRO limits.
- Confirm physical keyboard input cannot reach the PC during ARMED or PLAYING.
- Stop playback at arbitrary points and confirm no key remains pressed.
