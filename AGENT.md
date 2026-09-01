# Pico Keyboard Real-time Recorder — Agent Instructions

## Project contract

This repository implements a keyboard recorder/player in two deliberately separate domains:

- **Raspberry Pi Pico 2 is the real-time engine.** It captures physical-keyboard timestamps, schedules playback, owns the playback queue, emits USB HID reports, and enforces safety transitions.
- **Raspberry Pi Zero 2 W is the control and storage service.** It persists recordings, supplies already-timestamped future playback events, manages the UI/API, and controls the mode GPIO.

Do not move responsibilities across this boundary without an explicit architecture decision recorded in `docs/architecture.md` and `docs/realtime-design.md`.

## Non-negotiable real-time rules

1. Capture a recording timestamp on Pico, at the USB-host receive boundary, with Pico's monotonic hardware timer. Never substitute Zero UART-receive time.
2. Start the playback epoch on Pico when `PLAY_START` is received. Zero must not define the execution epoch.
3. Pico schedules every playback report from `playback_start_us + offset_us`, where `offset_us` is absolute from the epoch. Never chain delays from the previous dispatch.
4. Zero is a future-event feeder, not a scheduler. `sleep`, `asyncio.sleep`, Linux timer timing, or UART write timing must never determine when a HID report is emitted.
5. Use a Pico hardware timer/alarm for playback deadlines. Do not busy-wait for normal operation. If USB APIs cannot run in the alarm callback, transfer work to a safe Pico execution context without changing the absolute deadline model.
6. Prebuffer before `PLAY_START`; stream replenishment only as queue capacity allows. Treat insufficient data during playback as an underrun, not as a reason to shift subsequent deadlines.

## Safety invariants

- Pico states are `PASS`, `ARMED`, `PLAYING`, and optionally `ERROR`.
- The mode GPIO is the hardware gate: LOW means `PASS`; HIGH enables `ARMED`/`PLAYING`. Configure Pico's input with a pull-down.
- On `PASS -> ARMED`, abort, finish, underrun, protocol error, or GPIO LOW: cancel scheduled playback as applicable and send an all-keys-release report.
- In `ARMED` and `PLAYING`, physical keyboard reports must not reach the PC.
- On an underrun or UART failure during playback: send all-keys-release, stop playback, enter `ERROR`, and keep physical input blocked until GPIO becomes LOW.
- On return to `PASS`, do not replay a stale physical-keyboard state. Wait for the next host report.

## Protocol and data rules

- UART is framed binary data with versioning, length, and CRC16. Update `docs/protocol.md` whenever it changes.
- Playback `QUEUE_EVENT` data contains a future absolute offset from the Pico epoch, not a command to act immediately.
- Persist recordings as relative delta times (`dt_us`); expand to absolute offsets on Zero before queueing.
- Version 1 targets 8-byte USB Boot Keyboard / 6KRO reports. Keep interfaces capable of carrying a report length for future HID expansion.

## Implementation and verification rules

- Keep Pico firmware in C/C++ with Pico SDK and TinyUSB; keep Zero services separate under `zero/`.
- Keep real-time metrics available: deadline, dispatch time, lateness, percentiles, event count, and underruns.
- Any phase that changes behavior must document practical verification in `README.md` and update the relevant document in `docs/`.
- Add automated tests where hardware-independent, and maintain the hardware acceptance tests in `docs/testing.md`.
- Prefer small, phase-scoped commits. Do not add unreviewed hardware pin assignments as facts; mark provisional wiring clearly.
