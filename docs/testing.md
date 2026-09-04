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
`PLAY_UNDERRUN`/`PLAY_METRICS` payload length boundaries and the queue-command
state guard (`QUEUE_EVENT`/`QUEUE_END` in ARMED or PLAYING, `QUEUE_CLEAR` only
in ARMED). The playback scheduler test fakes
the `pico/time.h` hardware alarm API (`add_alarm_at`, `cancel_alarm`,
`from_us_since_boot`) the same way the host adapter test fakes
`time_us_64()`, and covers: dispatch order and per-event deadlines against
both the alarm-fired flag and the `time_us_64()` polling fallback; that a
long stall before a `task()` call drains every already-due event without
shifting later deadlines; that a busy HID endpoint retries the same queued
head event via `pico_playback_queue_peek` without reordering or dropping it;
`stop()` reporting partial metrics with reason ABORTED mid-run and being a
no-op once not running; min/max/sum/p95/p99 lateness matched against a
hand-computed sample set; `samples_truncated` once a run dispatches more
events than the fixed 2048-sample log holds; an open empty sequence remaining
PLAYING with one edge-triggered underrun notification; late refill retaining
the original absolute deadline; a full queue granting new flow-control
credit after its first pop; and the persistent-underrun watchdog query
(`pico_playback_scheduler_watchdog_expired`) staying false below the
2-second bound, becoming true once it elapses, and clearing again once a new
event arrives. The mode transition test additionally covers
`pico_mode_state_underrun_fault` entering ERROR with reason `UNDERRUN` the
same way the existing UART-fault and protocol-error entry points do. The
main-output integration
test verifies that PASS keeps its FIFO through a failed release, the
release-sent iteration, and a normal HID-not-ready result; it also verifies
RECORD's dual-sink dispatch: HID output retried through the same FIFO head
exactly like PASS, and exactly one `RECORD_EVENT` per head sent the moment
it is first peeked, unaffected by and never duplicated across that HID
retry, including across a mode round-trip. Only a validated
command can request a mode change. In particular, a repeated PASS command
and a transport or malformed-frame fault received in PASS do not release a
held key or clear accepted physical input.

`git diff --check` is required before commit. Docker CI additionally performs
the normal and HID-demo Pico SDK builds with TinyUSB's pinned Pico-PIO-USB
dependency.

Most Zero recorder host checks require only Python's standard-library
unittest runner (PySerial is needed only when opening a real serial device);
`test_web_api.py` additionally needs `fastapi`/`uvicorn` from
`zero/requirements.txt` importable, since it drives the web API through
FastAPI's `TestClient`:

```sh
cd zero
python3 -m pip install -r requirements.txt
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

Playback feeder tests cover speed 1.0/2.0/0.5 offset expansion, invalid speed,
500 ms prebuffer selection, short recordings closed before PLAY_START, long
recordings streamed beyond a deliberately tiny fake Pico capacity, four-event
pipeline writes, asynchronous credit renewal, underrun credit/diagnostics,
explicit PLAY_ABORT outcome/metrics consumption, and rejection when 500 ms of
events cannot fit the advertised Pico capacity. No test advances a Linux timer
to schedule an HID report; all reports carry future Pico-relative offsets.

`RecordingStore.rename`/`delete` tests cover renaming rewriting the recording's
embedded name and rejecting a missing source, a collision, or a path-traversal
name without moving or removing anything, and deleting rejecting a missing
name.

Web API tests drive `app.web`'s FastAPI app through `TestClient` against a
fake Pico byte stream, covering: status/list/download and their 404/400/409
error-code mapping; a full record start/stop lifecycle whose persisted
`dt_us` deltas match what was received; a second recording or playback while
one is active being rejected with 409; a rejected `MODE_SET` recovering to
PASS; rename/delete refusing the currently-active recording; a normal
playback completing and returning to PASS; and playback `stop` aborting
mid-stream (before `PLAY_FINISHED` arrives) and still returning to PASS. The
FastAPI lifespan's startup/shutdown `Controller.safe_stop()` reconciliation is
exercised by every test implicitly, since each one opens and closes an app.

`test_web_ui.py` covers the static frontend and the `/api/ws` WebSocket added
for issue #12: `GET /` serving `index.html`, `/app.js`/`/style.css` serving
correctly, and that the explicit `/api/*` routes still take priority over the
catch-all static mount. It drives the same `TestClient.websocket_connect`
against a fake Pico byte stream to confirm a freshly connected socket
receives a status snapshot with `diagnostics: {buffer: null, playback: null,
recording: null}` while idle, that `diagnostics.recording.event_count` ticks
up live during an active RECORD session and clears back to null once it
stops, and (alongside `GET /api/status` directly) that `diagnostics.buffer`/
`diagnostics.playback` report real `queued_count`/`free_capacity`/
`queued_events`/`total_events` while a playback is mid-stream.
`PlaybackSession.progress()`/`RecordingSession.progress()` are also tested
directly in `test_playback.py`/`test_service_cli.py`, including that the
diagnostic-only `elapsed_us_estimate` clamps to the (speed-scaled) recording
duration rather than overshooting it.

## Hardware acceptance

1. Verify the fixed wiring: UART0 GP0 TX/GP1 RX crossed to the Zero with common
   ground, PIO-USB GP12 D+/GP13 D-, and protected keyboard VBUS. No mode wire is
   present.
2. Flash the normal image and confirm native HID enumeration. In PASS, press and
   release a physical key and observe the same reports at the PC.
3. Send a version-2 `MODE_SET(RECORD)` frame from the Zero. Confirm physical
   reports still reach the PC exactly as in PASS, AND each also arrives as a
   CRC-valid `RECORD_EVENT` with a nondecreasing Pico timestamp.
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
   queue discarded) rather than a silently dropped event. While PLAYING,
   confirm `QUEUE_EVENT` and `QUEUE_END` remain accepted but `QUEUE_CLEAR`
   enters ERROR. Send any queue command while in PASS or RECORD and confirm
   the same wrong-state protocol-error behaviour as any other unexpected
   command. Confirm `PLAY_START` after a
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
12. Stream more than 512 events from Zero. When the queue becomes full, confirm
    the first subsequent dispatch emits an asynchronous `BUFFER_STATUS` with
    positive free capacity, feeding resumes without exceeding advertised
    credit, and PLAYING `QUEUE_END` emits no `PLAY_READY`. Confirm the run ends
    only after all queued events following that marker dispatch.
13. During an open streaming sequence, deliberately stop Zero feeding until
    the Pico queue empties. Confirm exactly one `PLAY_UNDERRUN` for the empty
    interval and no `PLAY_FINISHED`; resume with an event whose absolute
    deadline is already past, then send `QUEUE_END`. Confirm it dispatches
    immediately, lateness reflects the whole stall (the deadline did not
    shift), `underrun_count` increments, and normal completion follows.
14. Repeat step 13, but this time do not resume feeding: leave the open
    sequence's queue empty for longer than the 2-second watchdog bound
    (`PICO_PLAYBACK_SCHEDULER_WATCHDOG_TIMEOUT_US`,
    `pico/include/playback_scheduler.h`). Confirm the run does not stay
    PLAYING indefinitely: it enters ERROR, releases all keys, and sends
    `MODE_CHANGED(ERROR, UNDERRUN)`. Confirm ERROR blocks input and recovers
    only via a CRC-checked `MODE_SET(PASS)` -- no other command is accepted.
    Note this watchdog is deliberately keyed off the queue staying empty, not
    off raw UART silence: a genuinely dead link is caught by this same path
    once the queue would need refilling and does not get it (see
    `docs/realtime-design.md`), while step 7 above already covers UART
    hardware/framing faults (including physical disconnection) independently
    of playback state.
15. In RECORD (Issue #4 capture/pass-through latency diagnostics), hold the
    native HID endpoint busy (e.g. block the PC-side driver) while sending
    more physical key events than the 16-entry capture FIFO can hold; both
    PC forwarding and `RECORD_EVENT` emission share this one FIFO, so an
    overflow here means an event reaches neither sink. Confirm the events
    that do fit still forward to the PC and record via `RECORD_EVENT` once
    HID frees up, and that the device resumes normal PASS-equivalent
    forwarding and recording for new key events afterward (no crash, no
    stuck state). `pico_keyboard_capture_stats_t.dropped` is not currently
    exposed over UART, so its accounting is instead exercised on the host by
    `test_bounded_fifo_order_and_drop_accounting` in
    `pico/tests/keyboard_capture_test.c`.

UART event time is not used as a HID deadline; playback deadlines are the
hardware-alarm-driven absolute `playback_start_us + offset_us` values
described in `docs/realtime-design.md`, and steps 8-11 above are how that is
checked against the Pico hardware timer.

## Zero recorder acceptance

1. On the Zero, install `zero/requirements.txt`; connect UART0 to Pico UART0
   (crossed TX/RX with common ground) and identify the serial device, normally
   `/dev/serial0`.
2. Run `PYTHONPATH=zero python3 -m app --recordings-dir zero/recordings record hello --device /dev/serial0`.
   Confirm Pico replies `MODE_CHANGED(RECORD, OK)` and physical keys continue
   reaching the PC (unlike before this issue).
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
6. Run `PYTHONPATH=zero python3 -m app --recordings-dir zero/recordings play hello --device /dev/serial0`.
   Confirm at least 500 ms (or the entire shorter recording) is queued before
   PLAY_START, long recordings continue sending in PLAYING, the final
   QUEUE_END is acknowledged without PLAY_READY, and the JSON result contains
   PLAY_METRICS. Confirm the CLI returns the Pico to PASS afterward. Repeat
   with `--speed 2.0` and verify the absolute offsets are halved while their
   common Pico epoch and relative timing remain intact.
7. Interrupt a long `play` command with Ctrl-C. Confirm `PLAY_ABORTED`, then
   `PLAY_METRICS`, then `MODE_CHANGED(ARMED, ABORTED)` are consumed in order,
   followed by a successful transition to PASS before the serial port closes.

## Zero web API acceptance

1. Install `zero/systemd/pico-keyboard-recorder.service` (adjust
   `WorkingDirectory`/`User` for the target host), `systemctl enable --now`
   it, and confirm `GET /api/status` reports `"state": "PASS"` -- this also
   exercises startup reconciliation (a CRC-checked `MODE_SET(PASS)`, not
   GPIO) when the Pico is freshly booted and already in PASS.
2. Manually put the Pico in a non-PASS state (e.g. `zero-recorder stop` after
   first sending `MODE_SET(RECORD)` out of band, or restart the service while
   a prior run left it ARMED), then start/restart the service. Confirm status
   settles on PASS without needing a client request.
3. `POST /api/recordings/<name>/record`, confirm status reports `"state":
   "RECORD"`, press/release physical keys, then `POST /api/record/stop` and
   confirm the response's recording JSON matches a `GET
   .../download` of the same name, and that `MODE_CHANGED(PASS, OK)` was
   observed on the wire.
4. While that recording is active, `POST` a second `record` or `play` request
   and confirm 409 rather than a second Pico session. Confirm `POST
   /api/recordings/<name>/rename` and `DELETE /api/recordings/<name>` both
   reject the currently-active recording with a non-2xx status.
5. `POST /api/recordings/<name>/play`, confirm `"state": "PLAYING"`, then
   `POST /api/playback/stop` mid-playback. Confirm an all-keys-release
   report at the PC, `PLAY_ABORTED`/`PLAY_METRICS`/`MODE_CHANGED(ARMED,
   ABORTED)` on the wire in order, and status returning to PASS.
6. Let a playback run to completion untouched; confirm `PLAY_FINISHED`/
   `PLAY_METRICS`/`MODE_CHANGED(ARMED, FINISHED)` and a return to PASS,
   matching hardware-acceptance step 9 above but driven over HTTP.
7. `POST /api/recordings/<name>/rename` and `DELETE /api/recordings/<name>`
   on an inactive recording; confirm `GET /api/recordings` reflects the
   change and a repeat `DELETE` returns 404.
8. While a recording or playback is active, `systemctl stop
   pico-keyboard-recorder` (or send SIGTERM directly). Confirm the all-release
   report and a final `MODE_CHANGED(PASS, OK)` are observed before the
   process exits, and that `systemctl restart` afterward again settles on
   PASS per step 1/2.

## Web UI acceptance

The static browser UI (`zero/static/`) is served by the same FastAPI app as
the web API above and adds no new backend dependency; only the manual,
browser-driven flow below is not covered by `test_web_ui.py`. Like every
other hardware-touching issue in this project (#4/#6/#7/#9/#10/#11), this
pass requires a physical Pico 2 + Zero 2 W and a browser and is not
achievable in a sandboxed dev environment -- mark it pending rather than
claiming false completion when only the automated `TestClient`/WebSocket
checks above have run.

1. With the service running (see "Zero web API acceptance" step 1), load
   `http://<zero-host>:8000/` in a browser. Confirm the status panel shows
   `PASS` and the Pico/mode state, with no stale "disconnected" banner.
2. Start a recording from the UI, press/release physical keys, then stop it
   from the UI. Confirm the new recording appears in the list with the
   expected duration/event count, matching a `GET /api/recordings` call.
3. Play that recording from the UI. Confirm the buffer and position
   diagnostics update live (queued/free counts, elapsed/duration, event
   progress) without a page reload, then Abort mid-playback and confirm an
   all-keys-release at the PC and the UI settling back on PASS.
4. Play a recording to completion untouched. Confirm the position indicator
   reaches the end, the underrun count (0 for a clean run) stays visible,
   and the state returns to PASS.
5. Rename and delete an inactive recording from the UI; confirm the list
   updates without a manual refresh. Confirm the UI disables rename/delete
   for the recording currently being recorded or played, and that attempting
   either via a stale/second tab still surfaces the backend's 409 inline
   rather than silently failing.
6. Force an ERROR state (e.g. disconnect UART briefly, or inject a bad frame
   as in hardware-acceptance step 7). Confirm the UI visually distinguishes
   ERROR from every other state and offers a "Return to PASS" action;
   trigger it and confirm the Pico and UI both recover to PASS.
7. Open the UI in two browser tabs at once. Confirm both reflect the same
   live state, and that starting a session in one tab is visible in the
   other without a manual refresh.
8. Briefly stop the service (or block the network path) while the UI is
   open. Confirm the UI shows a disconnected/retrying indicator and recovers
   automatically (status updates resume) once the service is reachable
   again, with no page reload required.

## UART baud rate (921600) acceptance

Raising UART0 above 460800 (issue #23) is only validated by the host-side
checks above (parser/framing/CRC logic, ring-capacity math) plus the
compile-time static assert in `pico/include/hardware_config.h` tying the RX
ring to the configured baud. None of that confirms the physical link holds
up at the new rate; do this manually before treating the change as done:

1. Confirm which physical UART backs `/dev/serial0` on the target Zero 2 W
   image (full PL011 vs. the BCM's mini-UART) -- e.g. via
   `raspi-gpio`/`dtoverlay` config or the kernel's UART aliasing. If it's the
   mini-UART, confirm `core_freq`/`core_freq_min` is pinned in `config.txt`
   so its baud divisor (which is derived from the VPU clock, not a fixed
   UART clock) is accurate at a non-standard rate. This is a prerequisite
   for reliable timing at any rate above the defaults, not specific to
   921600.
2. Flash the updated firmware and re-run the pass-through, RECORD, and
   playback hardware-acceptance steps above end to end at 921600. Confirm no
   framing errors, CRC failures, or unexpected `ERROR` transitions appear
   that did not also appear at 460800.
3. Run a sustained/high-rate stress case -- a long recording played back at
   an elevated `--speed`, or a dense rapid-keypress burst during RECORD --
   and confirm the `PICO_STATUS` fault counters (`rx_overflow`,
   `hardware_errors`, `tx_dropped`; see `docs/protocol.md`) stay at zero
   throughout. This is the hardware-side confirmation that the doubled
   RX/TX ring capacities actually cover the new byte rate.
4. If practical, measure the Pico's actual `clk_peri` (e.g. a debug print of
   `clock_get_hz(clk_peri)` during bring-up) and record the value in the
   issue/PR thread. This closes the "which clock, and how much divisor
   error" unknown this plan's software-side analysis had to assume across a
   range of plausible values, and gives a concrete baseline for any future
   baud increase.
