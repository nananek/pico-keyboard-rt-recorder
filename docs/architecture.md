# Architecture

## Ownership boundary

The Pico 2 is the real-time engine: TinyUSB device and PIO-USB host, physical
report timestamps, pass-through, playback queue, USB output, and safety state.
The Pi Zero 2 W stores recordings, prepares absolute future offsets, feeds the
queue, and presents the control UI. The Zero never supplies a playback clock.

The control UI is a static single-page app (`zero/static/`) served by the
same FastAPI process (`zero/app/web.py`) that exposes the `/api/*` REST
surface, with a `/api/ws` WebSocket pushing periodic status/diagnostics
snapshots for live updates. Any diagnostic the UI shows for playback
position (`elapsed_us_estimate`) is a Zero-side wall-clock approximation
derived from when `PLAY_STARTED` was observed; it is never fed back into
scheduling, so it does not change the Pico being the sole HID scheduler
described below.

The only control link is framed binary UART0 (Pico GP0 TX / GP1 RX, 921600
8-N-1). Mode selection is a `MODE_SET (0x87)` command; no GPIO gate is used.
See `docs/realtime-design.md`'s "Clock configuration and timing accuracy"
section for the verified `clk_peri`/`clk_ref` chain and jitter bounds behind
these numbers.

## Data flow

```text
USB keyboard -> PIO-USB host -> host callback timestamp
                                  |-> bounded physical queue
                                  |       |-> PASS or RECORD: native HID device -> PC
                                  |       `-> RECORD only: RECORD_EVENT -> UART -> Zero
Zero UART MODE_SET/PLAY_* -> RX IRQ -> byte ring -> main parser -> state/queue
```

The UART IRQ drains hardware FIFO bytes and records hardware faults only. It
does not parse frames, allocate, call TinyUSB, or mutate mode. The main loop
performs magic/version/length/CRC/type checks, queues validated commands, and
executes state transitions. TX is also a non-blocking ring drained by the main
loop when UART space is available.

## Safety

PASS and RECORD both forward physical reports to the PC via HID, retrying
against a busy endpoint until it accepts. RECORD additionally captures each
report and sends it to Zero as `RECORD_EVENT`, sent exactly once per report
and unaffected by HID busy/retry state. ARMED and PLAYING block physical
input. Entering PASS from a non-PASS
state cancels stale physical data, sends all-keys-release, and waits for a
fresh host report; repeated PASS commands preserve current physical input.
Malformed frames, ring overflow, and UART hardware errors enter ERROR outside
PASS; ERROR blocks input until a valid `MODE_SET(PASS)` recovers it. Every
accepted or rejected mode request is acknowledged with `MODE_CHANGED(state,
reason)`. Each successful state change, abort, or fault that enters ERROR clears
physical data and sends all keys released.
