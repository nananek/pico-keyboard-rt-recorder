# Pico Keyboard Real-time Recorder — Agent Instructions

## Project contract

The Raspberry Pi Pico 2 is the real-time engine: it owns USB host/device,
receive-boundary timestamps, pass-through, playback scheduling, and safety
state. The Raspberry Pi Zero 2 W stores recordings, prepares future events,
feeds the queue, and controls the UI. The boundary is framed binary UART0
(Pico GP0 TX / GP1 RX, 460800 baud, 8-N-1); mode is never selected by GPIO.

## Non-negotiable real-time rules

1. Sample a recording timestamp at the Pico USB-host receive boundary with
   `time_us_64()`. Never substitute Zero UART-receive time.
2. Start playback on Pico when `PLAY_START` is accepted. Zero does not define
   the execution epoch.
3. Schedule every playback report from `playback_start_us + offset_us`; do not
   chain delays from a previous dispatch.
4. Zero is a future-event feeder, not a scheduler. Linux sleeps and UART write
   timing must never determine HID output.
5. Use a Pico hardware timer/alarm for playback deadlines and hand USB work to a
   safe Pico context when an alarm callback cannot call TinyUSB.
6. Prebuffer before playback and treat missing data as an underrun, not as a
   reason to shift later deadlines.

## UART and safety invariants

- UART RX IRQ only drains FIFO bytes into the bounded SPSC ring and counts
  hardware errors/overflow. It never parses, allocates, blocks, calls TinyUSB,
  or changes state.
- The main loop validates version 2, length, CRC, direction, and payload before
  queueing commands. Invalid input cannot directly change state.
- States are PASS, RECORD, ARMED, PLAYING, and ERROR. PASS and RECORD both
  forward physical reports to the PC via HID; RECORD additionally timestamps
  each report and sends it to Zero as `RECORD_EVENT`, sent exactly once per
  report regardless of HID busy/retry state. ARMED/PLAYING/ERROR block
  physical reports.
- Every successful state change, abort, and fault that enters ERROR clears stale
  physical input and sends an all-keys-release report. PASS waits for a fresh
  host report after release; an idempotent `MODE_SET(PASS)` leaves current input
  untouched.
- UART faults and malformed frames enter ERROR outside PASS. ERROR recovers only
  after a CRC-checked `MODE_SET(PASS)`.

## Protocol and data rules

- UART framing, message values, payloads, and CRC are authoritative in
  `docs/protocol.md`; update it with any protocol change.
- `QUEUE_EVENT` carries an absolute future offset from the Pico epoch.
- Persist recordings as relative `dt_us`; expand to absolute offsets before
  queueing.
- Version-2 messages carry an explicit report length while current Boot
  Keyboard reports remain 8-byte/6KRO.

## Implementation and verification

- Keep Pico firmware in C/C++ with Pico SDK/TinyUSB and Zero services under
  `zero/`.
- Keep real-time metrics available (deadline, dispatch time, lateness,
  percentiles, event count, underruns). The `PLAY_METRICS` UART frame
  (`pico/src/playback_scheduler.c`) is this project's concrete implementation
  of that requirement.
- Add hardware-independent tests for each behavior and document acceptance in
  `README.md` and `docs/testing.md`.
- Avoid inventing pin assignments. The fixed pins are UART0 GP0/GP1 and
  PIO-USB GP12/GP13 only.
