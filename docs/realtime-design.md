# Real-time design

The Pico main loop is the only context allowed to parse UART frames or change
the mode state. UART0 RX uses a bounded single-producer/single-consumer byte
ring. The interrupt handler drains the FIFO, counts framing/parity/overrun
errors, and returns; it never blocks, allocates, invokes TinyUSB, or sends HID.
Ring overflow and hardware errors are latched for main-loop handling.
The ring holds 512 bytes: more than seven maximum-size 71-byte frames and
about 5.5 ms of continuous 921600-baud 8-N-1 input, while the main loop's
normal sleep interval is 1 ms.

Startup is safe by construction, with no separate hardware watchdog
involved: `pico_mode_state_init` always zero-initializes the mode state and
then explicitly sets `PICO_UART_MODE_PASS` before `main()` does anything
else with it, and both UART hardware init and the TinyUSB device/host stack
inits happen only after `pico_mode_state_init` has already run -- so no
physical or UART input can be processed before PASS is established. No code
in `pico/src` or `pico/include` uses a Pico-SDK reset-surviving RAM
annotation (`__uninitialized_ram` or similar), so every reset starts from a
clean, zero-initialized state with no stale mode, queue, or fault data
carried over. This satisfies Issue #10's "safe startup defaults" scope item
without any code change; the RP2350 hardware watchdog peripheral remains
explicitly out of scope, a candidate for a future issue.

The main loop resynchronizes on `0xA5`, validates version 2, bounded length,
CRC-16/CCITT-FALSE, direction/type, and payload shape. Valid commands enter a
small command queue. Invalid input never changes mode directly and causes a
safe UART-fault transition when the current mode is not PASS. TX frames use a
bounded ring and are drained only while UART hardware is writable. A full TX
ring drops an entire frame and increments `tx_dropped`; mode commands remain
safe because `MODE_SET` is idempotent and the Zero retries it if its
`MODE_CHANGED` acknowledgement is absent.

Physical reports are timestamped with `time_us_64()` at the TinyUSB host
callback. In both PASS and RECORD they are forwarded to the native HID device
from the same FIFO head, retried against a busy endpoint until it accepts, so
the device keeps working as a normal keyboard whether or not a recording is
running; in ARMED/PLAYING or ERROR they are discarded. RECORD additionally
emits one `RECORD_EVENT` frame carrying that Pico timestamp for each head --
sent the moment the head is first peeked, before that iteration's HID outcome
is known, so recording never waits on or is gated by HID readiness. A single
`read_index` cursor still drives both sinks: a `head_recorded` flag on the FIFO
tracks whether the current head's `RECORD_EVENT` has already gone out, so a
head retried multiple times against a busy HID endpoint still produces exactly
one `RECORD_EVENT`, and popping the head (only once HID accepts it) resets the
flag for the next one. A successful state change, abort, or a fault that
enters ERROR clears queued physical reports (and that flag) and sends an
all-zero release report; idempotent `MODE_SET(PASS)` retransmission and faults
received in PASS do neither. If the HID endpoint is temporarily busy, the
release is retried before any later physical report is forwarded. The loop
that successfully submits a release does not drain PASS/RECORD input, because
that submission consumes HID endpoint readiness; later reports remain queued
until a following loop accepts them. If submission fails they likewise remain
queued for retry, and in RECORD the already-sent `RECORD_EVENT` for that
queued head is never resent. UART event arrival is never a timing source.

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

The queue draining to empty ends the run with `PLAY_FINISHED` only after
`QUEUE_END` has closed the sequence. If an open sequence becomes empty, the
scheduler stays running, increments its underrun count, and emits one
`PLAY_UNDERRUN` for that contiguous empty interval. A later `QUEUE_EVENT`
re-arms directly against its original `playback_start_us + offset_us`; neither
the epoch nor later deadlines move, and late arrival is visible in the
ordinary lateness metrics. `PLAY_ABORT`, a direct `MODE_SET(PASS)`, or a fault
entering ERROR while PLAYING ends the run early with `PLAY_ABORTED` instead,
in both terminal cases immediately followed by one `PLAY_METRICS` frame. The
scheduler accumulates dispatched_count, underrun_count, and
min/max/sum lateness in O(1) on every successful dispatch, and separately
keeps up to 2048 raw per-event lateness samples; p95/p99 are computed once,
at run end, by sorting that retained sample set (never on the hot path), so
per-event dispatch cost stays constant regardless of run length. See
`docs/protocol.md` for the exact `PLAY_STARTED`/`PLAY_FINISHED`/
`PLAY_ABORTED`/`PLAY_METRICS` payloads and reasons.

The transient underrun above has no time bound of its own: as long as the
sequence stays open, the scheduler waits indefinitely for streamed refill.
The queue staying empty on an open sequence continuously past
`PICO_PLAYBACK_SCHEDULER_WATCHDOG_TIMEOUT_US` (2 seconds,
`pico/include/playback_scheduler.h`), checked once per main-loop iteration
while PLAYING (`pico/src/main.c`) via the pure query
`pico_playback_scheduler_watchdog_expired`, instead enters ERROR (reported
as `MODE_CHANGED(ERROR, UNDERRUN)`): releasing all keys, discarding the
queue, and blocking input until a CRC-checked `MODE_SET(PASS)` -- turning a
stall that never recovers into a safe stop instead of an indefinite wait.
See `docs/protocol.md` for the exact reason codes.

This is deliberately keyed off the scheduler's own queue state rather than
raw UART receive activity: Zero's credit-based feeder (`zero/app/playback.py`)
legitimately sends nothing for long stretches once it has queued everything
the Pico's buffer can currently hold (e.g. a whole short recording queued
before `PLAY_START`, or a streaming run waiting on the next real-time
dispatch to free capacity) -- silence on the wire is not by itself evidence
of a stalled or disconnected link. An actually-dead link only becomes an
operational problem once the queue would need refilling and does not get
it, which is exactly the condition above already detects; a genuine
transport-level fault (framing/parity/overrun, invalid frame) is separately
and unconditionally caught by the pre-existing
`pico_uart_transport_take_fault`/`pico_mode_state_uart_fault` path regardless
of mode.

The playback queue itself is a fixed-capacity ring buffer. `QUEUE_CLEAR` opens
a new sequence in ARMED; `QUEUE_EVENT` and `QUEUE_END` remain valid after
PLAY_START so Zero can refill and close a sequence while PLAYING. Each command
replies with `BUFFER_STATUS` (state plus queued/free slot counts), and dispatch
from a full open queue emits one additional status when the first slot becomes
free. Zero uses those credits with a bounded four-command pipeline: UART
acknowledgements and queue space control feeding, while Zero wall-clock sleeps
never schedule HID output. A `QUEUE_EVENT` beyond advertised capacity is a
protocol violation, not a silent drop. ARMED `QUEUE_END` also emits
`PLAY_READY`; PLAYING `QUEUE_END` does not. `PLAY_START` deliberately leaves
the queue intact — only `PLAY_ABORT`, a transition to PASS, or a fault entering
ERROR discards queued playback events, mirroring queued physical reports (and,
whenever the scheduler is running, first stopping it and reporting metrics so
far).
