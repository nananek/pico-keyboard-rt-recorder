# Architecture

## Ownership boundary

The Pico 2 is the real-time engine: TinyUSB device and PIO-USB host, physical
report timestamps, pass-through, playback queue, USB output, and safety state.
The Pi Zero 2 W stores recordings, prepares absolute future offsets, feeds the
queue, and presents the control UI. The Zero never supplies a playback clock.

The only control link is framed binary UART0 (Pico GP0 TX / GP1 RX, 460800
8-N-1). Mode selection is a `MODE_SET (0x87)` command; no GPIO gate is used.

## Data flow

```text
USB keyboard -> PIO-USB host -> host callback timestamp
                                  |-> bounded physical queue
                                  |       |-> PASS: native HID device -> PC
                                  |       `-> RECORD: RECORD_EVENT -> UART -> Zero
Zero UART MODE_SET/PLAY_* -> RX IRQ -> byte ring -> main parser -> state/queue
```

The UART IRQ drains hardware FIFO bytes and records hardware faults only. It
does not parse frames, allocate, call TinyUSB, or mutate mode. The main loop
performs magic/version/length/CRC/type checks, queues validated commands, and
executes state transitions. TX is also a non-blocking ring drained by the main
loop when UART space is available.

## Safety

PASS forwards physical reports. RECORD captures reports and does not forward
them. ARMED and PLAYING block physical input. PASS from any state cancels stale
physical data, sends all-keys-release, and waits for a fresh host report.
Malformed frames, ring overflow, and UART hardware errors enter ERROR outside
PASS; ERROR blocks input until a valid `MODE_SET(PASS)` recovers it. Every
transition is acknowledged with `MODE_CHANGED(state, reason)`.
