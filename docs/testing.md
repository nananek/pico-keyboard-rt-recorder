# Testing and acceptance

## Automated checks

`sh pico/tests/run-host-tests.sh` builds with `-Wall -Wextra -Werror` and runs
the report layout, pin contract, capture FIFO, playback queue, playback
scheduler, UART protocol/CRC, UART ring and parser, mode transition, and host
adapter tests. These cover split frames, bad magic/version/length/CRC,
unknown direction/type, ring order and overflow, command/TX-ring saturation,
idempotent MODE_SET, invalid targets/transitions, all-release retry/queue
clearing, and physical input blocking. The playback queue test covers FIFO
order, capacity rejection, and clearing. The mode transition test also proves
that `PLAY_START` releases physical input like any other transition but does
not discard the playback queue it is about to consume, while `PLAY_ABORT` and
any fault entering ERROR do; it also covers `pico_mode_state_play_finish`
(PLAYING to ARMED with reason FINISHED, invalid from every other state). The
UART protocol test covers the `PLAY_STARTED`/`PLAY_FINISHED`/`PLAY_ABORTED`/
`PLAY_METRICS` payload length boundaries. The playback scheduler test fakes
the `pico/time.h` hardware alarm API (`add_alarm_at`, `cancel_alarm`,
`from_us_since_boot`) the same way the host adapter test fakes
`time_us_64()`, and covers: dispatch order and per-event deadlines against
both the alarm-fired flag and the `time_us_64()` polling fallback; that a
long stall before a `task()` call drains every already-due event without
shifting later deadlines; that a busy HID endpoint retries the same queued
head event via `pico_playback_queue_peek` without reordering or dropping it;
`stop()` reporting partial metrics with reason ABORTED mid-run and being a
no-op once not running; min/max/sum/p95/p99 lateness matched against a
hand-computed sample set; and `samples_truncated` once a run dispatches more
events than the fixed 2048-sample log holds. The main-output integration
test verifies that PASS keeps its FIFO through a failed release, the
release-sent iteration, and a normal HID-not-ready result; it also verifies
that RECORD drains to UART while HID output is blocked. Only a validated
command can request a mode change. In particular, a repeated PASS command
and a transport or malformed-frame fault received in PASS do not release a
held key or clear accepted physical input.

`git diff --check` is required before commit. Docker CI additionally performs
the normal and HID-demo Pico SDK builds with TinyUSB's pinned Pico-PIO-USB
dependency.

The Zero recorder host checks require only Python's standard-library unittest
runner (PySerial is needed only when opening a real serial device):

```sh
cd zero
python3 -m unittest discover -s tests -v
```

They cover UART v2 CRC/framing, byte-by-byte and multi-frame reads,
resynchronisation after invalid frames, mode handshakes, Pico timestamps to
JSON v1 deltas, duplicate/release persistence, schema/name validation, and
atomic-write failure cleanup. They also verify that rejected, timed-out, bad
frame, and Pico ERROR recordings are not published and that PASS is attempted.
They also cover `QUEUE_CLEAR`/`QUEUE_EVENT`/`QUEUE_END` encoding and
`BUFFER_STATUS` validation, and that `PicoTransport.queue_events()` paces
`QUEUE_EVENT` sends within the Pico-advertised `free_capacity` and surfaces a
protocol error rather than exceeding it.

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
6. While ARMED, send `QUEUE_CLEAR`, then several `QUEUE_EVENT` frames, then
   `QUEUE_END`. Confirm a `BUFFER_STATUS` reply after each queue command with
   `queued_count`/`free_capacity` tracking the pushes, and a single
   `PLAY_READY` after `QUEUE_END`. Send `QUEUE_EVENT` frames past the
   advertised `free_capacity` and confirm ERROR (all-release, blocked input,
   queue discarded) rather than a silently dropped event. Send a queue command
   while in PASS or RECORD and confirm the same wrong-state protocol-error
   behaviour as any other unexpected command. Confirm `PLAY_START` after a
   fresh `QUEUE_CLEAR`/`QUEUE_EVENT`/`QUEUE_END` load does not itself trigger
   another `BUFFER_STATUS` or clear the just-loaded queue.
7. Inject bad CRC/version/length bytes and UART framing errors. Confirm ERROR,
   all-release, blocked input, and recovery only after a valid
   `MODE_SET(PASS)` while ARMED or PLAYING. In PASS, confirm the same malformed
   input leaves a held key intact while reporting the fault. Disconnect/reconnect
   UART and repeat.
8. While ARMED, load a short recording (`QUEUE_CLEAR`/`QUEUE_EVENT`.../
   `QUEUE_END`) with a handful of events at known offsets, then send
   `PLAY_START`. Confirm `PLAY_STARTED` carries a `playback_start_us` sampled
   at that moment, and use a logic analyzer or scope on the native USB D+/D-
   lines (or the PC-side HID report timestamps) to confirm each report is
   emitted at `playback_start_us + offset_us`, not chained from the previous
   report or from UART receive time.
9. Let that recording drain to completion. Confirm exactly one
   `PLAY_FINISHED` (no payload), followed by one `PLAY_METRICS` frame with
   `dispatched_count` equal to the loaded event count, `underrun_count == 0`,
   and `max_lateness_us` under 1000 (sub-millisecond, the Issue #7 acceptance
   target), followed by `MODE_CHANGED(ARMED, FINISHED)`.
10. Repeat step 8 but send `PLAY_ABORT` partway through playback. Confirm an
    all-keys-release report and, on the wire in this order, exactly one
    `PLAY_ABORTED`, one `PLAY_METRICS` frame whose `dispatched_count` is less
    than the full load and reflects only the events that fired before the
    abort, and then `MODE_CHANGED(ARMED, ABORTED)` -- the same
    outcome-frame/metrics/MODE_CHANGED order as step 9. Confirm a following
    `PLAY_START` from a freshly reloaded queue behaves like step 8/9 again
    (the aborted run's
    state does not leak into the next one).
11. Flash the self-contained benchmark image
    (`cmake -DPICO_PLAYBACK_SCHED_TEST=ON ...`, see `pico/CMakeLists.txt`).
    With no UART traffic at all, it drives itself through PASS -> ARMED ->
    `PLAY_START` and keeps refilling the queue past its fixed 512-entry
    capacity until 1000 synthetic events at a 10 ms nominal spacing have been
    dispatched, then reports the same `PLAY_FINISHED` + `PLAY_METRICS` pair
    as any other run. Capture the raw `PLAY_METRICS` frame from UART0 and
    confirm `dispatched_count == 1000`, `samples_truncated == false` (1000 is
    under the 2048-sample cap), and record min/max/mean (`sum_lateness_us /
    dispatched_count`)/p95/p99 lateness for the acceptance record.

UART event time is not used as a HID deadline; playback deadlines are the
hardware-alarm-driven absolute `playback_start_us + offset_us` values
described in `docs/realtime-design.md`, and steps 8-11 above are how that is
checked against the Pico hardware timer.

## Zero recorder acceptance

1. On the Zero, install `zero/requirements.txt`; connect UART0 to Pico UART0
   (crossed TX/RX with common ground) and identify the serial device, normally
   `/dev/serial0`.
2. Run `PYTHONPATH=zero python3 -m app --recordings-dir zero/recordings record hello --device /dev/serial0`.
   Confirm Pico replies `MODE_CHANGED(RECORD, OK)` and physical keys no longer
   reach the PC.
3. Press/release a normal key, a modifier, and simultaneous keys; then press
   Ctrl-C. Confirm `MODE_CHANGED(PASS, OK)`, the CLI JSON success result, and
   the all-release behaviour at the PC.
4. Restart the CLI. Run `list` and `dump hello`; confirm the JSON v1 first
   `dt_us` is zero, following deltas and `duration_us` reflect Pico timestamps,
   and press/release/duplicate reports remain present.
5. While a second recording is active, test Ctrl-C and UART disconnect. In
   both cases confirm the recorder attempts PASS before serial close and that
   no new partial JSON file appears. Reconnect and explicitly run `stop` if
   the disconnect prevented acknowledgement.
