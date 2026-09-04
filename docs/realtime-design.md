# Real-time design

The Pico main loop is the only context allowed to parse UART frames or change
the mode state. UART0 RX uses a bounded single-producer/single-consumer byte
ring. The interrupt handler drains the FIFO, counts framing/parity/overrun
errors, and returns; it never blocks, allocates, invokes TinyUSB, or sends HID.
Ring overflow and hardware errors are latched for main-loop handling.
The ring holds 256 bytes: more than three maximum-size 71-byte frames and
about 5.5 ms of continuous 460800-baud 8-N-1 input, while the main loop's
normal sleep interval is 1 ms.

The main loop resynchronizes on `0xA5`, validates version 2, bounded length,
CRC-16/CCITT-FALSE, direction/type, and payload shape. Valid commands enter a
small command queue. Invalid input never changes mode directly and causes a
safe UART-fault transition when the current mode is not PASS. TX frames use a
bounded ring and are drained only while UART hardware is writable. A full TX
ring drops an entire frame and increments `tx_dropped`; mode commands remain
safe because `MODE_SET` is idempotent and the Zero retries it if its
`MODE_CHANGED` acknowledgement is absent.

Physical reports are timestamped with `time_us_64()` at the TinyUSB host
callback. In PASS they are forwarded to the native HID device; in RECORD they
become `RECORD_EVENT` frames carrying that Pico timestamp; in ARMED/PLAYING or
ERROR they are discarded. A successful state change, abort, or a fault that
enters ERROR clears queued physical reports and sends an all-zero release report;
idempotent `MODE_SET(PASS)` retransmission and faults received in PASS do
neither. If the HID endpoint is temporarily busy, the release is retried before
any later physical report is forwarded. The loop that successfully submits a
release does not drain PASS input, because that submission consumes HID endpoint
readiness; later PASS reports remain queued until a following loop accepts them.
If submission fails they likewise remain queued for retry. RECORD drains its
capture FIFO to UART independently of native HID readiness. UART event arrival
is never a timing source.

Playback is an absolute-deadline feature: `PLAY_START` samples a Pico epoch
with `time_us_64()` once and hands that same value to both the scheduler
(`pico_playback_scheduler_start`, `pico/src/playback_scheduler.c`) and the
`PLAY_STARTED` reply, so future `QUEUE_EVENT` offsets are scheduled against
that one epoch. Each queued event's deadline is `playback_start_us +
offset_us`, computed fresh from the fixed epoch rather than chained from the
previous dispatch, so a late or retried dispatch never shifts later
deadlines. The scheduler arms exactly one RP2350 hardware alarm
(`add_alarm_at`) for the next deadline at a time; its ISR callback only does
an atomic store of a "fired" flag and returns immediately, never calling
TinyUSB. The main loop's unconditional `pico_playback_scheduler_task()` call
is the only place that reads that flag (or independently notices
`time_us_64() >= next_deadline_us`, a fallback that does not depend on the
alarm having fired), sends the head event's HID report, and re-arms the next
alarm; a busy HID endpoint is retried from the same still-queued head event
next call, mirroring the existing PASS-output retry pattern. While the mode
is PLAYING the main loop calls `tight_loop_contents()` instead of its usual
1 ms `sleep_ms`, trading 100% CPU use during playback for sub-millisecond
scheduling precision; every other mode keeps the 1 ms sleep.

The queue draining to empty ends the run with `PLAY_FINISHED`; `PLAY_ABORT`,
a direct `MODE_SET(PASS)`, or a fault entering ERROR while PLAYING ends it
early with `PLAY_ABORTED` instead, in both cases immediately followed by one
`PLAY_METRICS` frame. The scheduler accumulates dispatched_count and
min/max/sum lateness in O(1) on every successful dispatch, and separately
keeps up to 2048 raw per-event lateness samples; p95/p99 are computed once,
at run end, by sorting that retained sample set (never on the hot path), so
per-event dispatch cost stays constant regardless of run length. See
`docs/protocol.md` for the exact `PLAY_STARTED`/`PLAY_FINISHED`/
`PLAY_ABORTED`/`PLAY_METRICS` payloads and reasons.

The playback queue itself is a fixed-capacity ring buffer, filled only while
ARMED. `QUEUE_CLEAR`/`QUEUE_EVENT`/`QUEUE_END` each reply with `BUFFER_STATUS`
(state plus queued/free slot counts) so Zero can pace `QUEUE_EVENT` sends
against Pico-advertised capacity instead of a fixed window; a `QUEUE_EVENT`
that arrives once the queue is full is treated as a protocol violation, not a
silent drop. `QUEUE_END` additionally emits `PLAY_READY` once, and `PLAY_START`
deliberately leaves the queue intact — only `PLAY_ABORT`, a transition to
PASS, or a fault entering ERROR discards queued playback events, mirroring how
those same transitions discard queued physical reports (and, whenever the
scheduler is currently running, first stops it and reports its metrics so
far -- see above). Draining the queue against the playback epoch is the
scheduler's job, described above.
