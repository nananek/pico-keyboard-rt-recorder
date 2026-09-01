# Real-time design

## Pico is the master clock

Pico's monotonic hardware timer (`time_us_64()` or an equivalent Pico SDK source) is the sole timing reference for capture and playback. Its clock is sampled when the USB-host path receives a physical keyboard report, and it establishes the playback epoch when Pico receives `PLAY_START`.

Pi Zero timestamps are unsuitable for these purposes. An event observed by Linux has already passed through UART buffering, kernel scheduling, and the userspace service. That arrival time measures transport latency as well as the key event and therefore adds non-deterministic jitter to a recording.

## Phase 2 capture boundary and USB ownership

`tuh_hid_report_received_cb()` samples `time_us_64()` as its first operation.
That exact value and the unmodified 8-byte Boot Keyboard report enter the
bounded capture handoff before diagnostic formatting or queue draining. Held or
duplicate reports and the all-zero release are separate timestamped events;
deduplication would destroy recording timing and is forbidden.

With Pico SDK 2.2.0's pinned TinyUSB/Pico-PIO-USB integration, core 0 runs both
`tuh_task()` for the PIO host and `tud_task()` for the native device in the main
loop. This follows TinyUSB's dual-stack model and avoids an unnecessary
cross-core queue in Phase 2. The host callback never invokes native-device APIs.
If later measurement requires moving host service to core 1, the capture
handoff must gain explicit bounded cross-core synchronization and this ownership
decision must be updated here.

## Zero is never the playback scheduler

Zero calculates data but does not execute timing. In particular, `time.sleep()`, `asyncio.sleep()`, `time.monotonic_ns()`, Linux timer wakeups, and UART write completion must not determine the instant a HID report reaches the Pico USB-device path.

Zero converts persisted `dt_us` values into absolute offsets, applies speed scaling, and queues future events. Pico calculates each deadline as:

```text
deadline_us = playback_start_us + event.offset_us
```

An alarm or equivalent hardware-timer mechanism wakes the Pico scheduler for the next deadline. Where TinyUSB requires task context rather than an interrupt callback, the alarm transfers ready work to that context; it does not convert the design into a relative-delay loop.

## Why absolute deadlines matter

With relative scheduling, a late event pushes all following events later. With absolute offsets, Pico dispatches a late event immediately, records its lateness, and still evaluates the next event against its original deadline. Jitter therefore does not accumulate into playback drift.

## Prebuffer and streaming

Before `PLAY_START`, Zero must queue at least 500 ms of future events, or all events for a shorter sequence. While playing, Zero maintains a 500 ms target buffer and treats 200 ms as the initial minimum operating reserve. These values are configuration defaults, not scheduling sources.

Prebuffering isolates Pico timing from short Zero CPU, disk, web, or UART delays. It does not guarantee uninterrupted playback indefinitely: Zero continuously replenishes the buffer using Pico-advertised capacity.

## Late reports, underrun, and failure

Pico records `lateness_us = dispatch_time_us - deadline_us`. Small lateness is dispatched immediately; the next deadline remains unchanged. The initial acceptance target is maximum scheduler jitter below 1 ms under the defined test load, with measurement retained for later optimization.

If the playback queue becomes empty before `QUEUE_END`, Pico must:

1. cancel pending playback scheduling;
2. send an all-keys-release HID report;
3. enter `ERROR`;
4. notify Zero with `PLAY_UNDERRUN`; and
5. keep physical keyboard input blocked until the mode GPIO is LOW.

UART loss during playback follows the same safe-stop policy. Pico must not continue queued playback after detecting link loss in version 1.
