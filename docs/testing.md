# Testing and acceptance

## Automated tests

Maintain hardware-independent tests for:

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

### Pass-through and capture

Connect a Boot Keyboard compatible keyboard to Pico's PIO USB host path and Pico to a PC as a USB HID keyboard. Verify normal typing works, modifiers and simultaneous keys are retained, and UART `RECORD_EVENT` timestamps originate on Pico.

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
