# UART protocol (version 2)

The Pico 2 and Pi Zero 2 W communicate over UART0 at 460800 baud, 8-N-1.
Pico UART0 TX is GP0 and RX is GP1. The mode is controlled only by framed UART
commands; there is no separate mode-control wire.

## Framing

All multi-byte fields are little-endian.

```text
+---------+---------+---------+----------+-------------+---------+
| MAGIC   | VERSION | TYPE    | LEN      | PAYLOAD     | CRC16   |
| u8 A5   | u8 02   | u8      | u16 LE   | LEN bytes   | u16 LE  |
+---------+---------+---------+----------+-------------+---------+
```

CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`) covers VERSION, TYPE, LEN,
and PAYLOAD. It excludes MAGIC and the CRC itself. Receivers reject version 1,
bad lengths, unknown directions/types, and bad CRCs. The Pico IRQ only drains
UART FIFO bytes into a bounded ring; frame parsing and all state changes run in
the main loop.

## Message types

| Direction | Value | Type |
| --- | ---: | --- |
| Pico → Zero | `0x01` | `RECORD_EVENT` |
| Pico → Zero | `0x02` | `PICO_STATUS` |
| Pico → Zero | `0x03` | `BUFFER_STATUS` |
| Pico → Zero | `0x04` | `PLAY_READY` |
| Pico → Zero | `0x05` | `PLAY_STARTED` |
| Pico → Zero | `0x06` | `PLAY_FINISHED` |
| Pico → Zero | `0x07` | `PLAY_ABORTED` |
| Pico → Zero | `0x08` | `PLAY_UNDERRUN` |
| Pico → Zero | `0x09` | `ERROR` |
| Pico → Zero | `0x0A` | `PONG` |
| Pico → Zero | `0x0B` | `MODE_CHANGED` |
| Pico → Zero | `0x0C` | `PLAY_METRICS` |
| Zero → Pico | `0x80` | `QUEUE_CLEAR` |
| Zero → Pico | `0x81` | `QUEUE_EVENT` |
| Zero → Pico | `0x82` | `QUEUE_END` |
| Zero → Pico | `0x83` | `PLAY_START` |
| Zero → Pico | `0x84` | `PLAY_ABORT` |
| Zero → Pico | `0x85` | `STATUS_REQUEST` |
| Zero → Pico | `0x86` | `PING` |
| Zero → Pico | `0x87` | `MODE_SET` |

`MODE_SET` has one byte `target_mode`: PASS=0, RECORD=1, ARMED=2. Pico
responds to every accepted or rejected request with `MODE_CHANGED`, whose two
bytes are `state` and `reason`. States are PASS=0, RECORD=1, ARMED=2,
PLAYING=3, ERROR=4. Reasons are OK=0, INVALID_TRANSITION=1,
INVALID_TARGET=2, PROTOCOL_ERROR=3, UART_FAULT=4, ABORTED=5,
UNDERRUN=6, FINISHED=7. A CRC-valid `MODE_SET` with an unsupported target is a rejected
command, not a malformed frame: it leaves the current state unchanged and
returns `MODE_CHANGED(current_state, INVALID_TARGET)`.

## Mode semantics and payloads

- PASS forwards each physical Boot Keyboard report to the PC and emits no
  `RECORD_EVENT`.
- RECORD timestamps each valid 8-byte report at the Pico USB-host callback and
  emits `RECORD_EVENT` to Zero without forwarding it to the PC.
- ARMED and PLAYING block physical reports. PLAY_START is valid only in ARMED;
  it samples a Pico epoch, arms the absolute-deadline playback scheduler
  against it, and enters PLAYING. The queue draining to empty on its own
  returns to ARMED with reason FINISHED; PLAY_ABORT returns to ARMED with
  reason ABORTED instead, stopping the scheduler early. Every successful mode
  transition, abort, or fault that enters ERROR clears queued physical reports
  and sends an all-keys-release report.
- PASS entered from a non-PASS state cancels playback, clears stale physical
  data, and sends all keys released before waiting for a new host report. A
  repeated `MODE_SET(PASS)` while already in PASS only acknowledges the request;
  it preserves held keys and accepted physical reports.
- Invalid frames or UART errors enter ERROR (unless already PASS). ERROR blocks
  input and recovers only with a CRC-checked `MODE_SET(PASS)`. A fault in PASS
  leaves physical input intact and reports the fault reason without entering
  ERROR.
- `QUEUE_CLEAR`, `QUEUE_EVENT`, and `QUEUE_END` load the fixed-capacity Pico
  playback queue and are valid only in ARMED; receiving any of them outside
  ARMED is a protocol error, same as any other unexpected command. Each
  accepted queue command replies with `BUFFER_STATUS`. `QUEUE_CLEAR` discards
  every currently queued event. `QUEUE_EVENT` enqueues one event, or is a
  protocol error if the queue is already full: Zero must stay within the
  `free_capacity` last advertised by `BUFFER_STATUS`. `QUEUE_END` additionally
  replies `PLAY_READY` once, signalling Zero may send `PLAY_START`. Unlike
  every other state change, `PLAY_START` does not clear the queue it is about
  to consume; only `PLAY_ABORT`, a transition to PASS, or a fault entering
  ERROR clears it.

| Action | Valid current state | Result |
| --- | --- | --- |
| `MODE_SET(PASS)` | Any | PASS; only a non-PASS source state clears input and releases keys. |
| `MODE_SET(RECORD)` | PASS, RECORD | RECORD |
| `MODE_SET(ARMED)` | PASS, ARMED | ARMED |
| `QUEUE_CLEAR` / `QUEUE_EVENT` / `QUEUE_END` | ARMED | Modifies the playback queue; replies `BUFFER_STATUS` (`QUEUE_END` also replies `PLAY_READY`) |
| `PLAY_START` | ARMED | PLAYING |
| `PLAY_ABORT` | ARMED, PLAYING | ARMED with reason `ABORTED` |

`RECORD_EVENT` is `timestamp_us u64 LE`, `report_len u8` (8), then the report.
`QUEUE_EVENT` is `offset_us u64 LE`, `report_len u8` (8), then the report;
offsets are absolute from the Pico playback epoch. `MODE_CHANGED` is
`state u8, reason u8`. `PICO_STATUS` is `state u8` followed by four fault
flags (RX overflow, hardware error, invalid frame, TX drop). `BUFFER_STATUS`
is `state u8`, `queued_count u16 LE`, `free_capacity u16 LE`; both counts are
queue slots, not bytes. `PLAY_READY` has no payload. PING/PONG have no
payload.

`PLAY_STARTED` carries the single `playback_start_us u64 LE` sampled when
`PLAY_START` was accepted: this is the same value handed to the scheduler as
its epoch, so Zero's `QUEUE_EVENT` offsets and Pico's playback deadlines
agree on one origin. `PLAY_FINISHED` and `PLAY_ABORTED` both have no payload;
Pico sends exactly one of the two once a playback run ends -- `PLAY_FINISHED`
when the queue drains naturally, `PLAY_ABORTED` when `PLAY_ABORT`, a direct
`MODE_SET(PASS)`, or a fault interrupts PLAYING first -- immediately followed
by one `PLAY_METRICS` frame covering only the events actually dispatched
before the run ended, and then the `MODE_CHANGED(ARMED, FINISHED)` or
`MODE_CHANGED(ARMED, ABORTED)` for the same transition: both end-of-playback
paths queue their outcome frame and `PLAY_METRICS` before `MODE_CHANGED`, so
a client can rely on that order regardless of which way the run ended.

`PLAY_METRICS` is 33 bytes: `dispatched_count u32 LE`, `underrun_count u32 LE`
(always 0; reserved for Issue #9's Zero streaming feeder, see below),
`min_lateness_us i32 LE`, `max_lateness_us i32 LE`, `sum_lateness_us i64 LE`,
`p95_lateness_us i32 LE`, `p99_lateness_us i32 LE`, and `samples_truncated
u8`. Lateness is `actual HID dispatch time - (playback_start_us + offset_us)`
in microseconds, always >= 0 by construction. Percentiles use the
nearest-rank method (`rank = ceil(p * n / 100)`, 1-based, into the
ascending-sorted retained samples) computed once at run end over up to 2048
raw lateness samples; `min`/`max`/`sum`/`dispatched_count` cover every
dispatched event regardless of that cap, and `samples_truncated` is nonzero
only when a run dispatches more than 2048 events.

`PLAY_UNDERRUN`'s existing 10-byte payload constraint is unchanged but
remains reserved and unsent: under the current `QUEUE_END` -> `PLAY_READY`
-> `PLAY_START` contract the queue is always a complete pre-buffered sequence
by the time `PLAY_START` runs, so an empty queue is always normal completion,
never an underrun. It is expected to gain meaning once a future Zero
streaming feeder (Issue #9) can leave the queue empty mid-playback.

If the bounded TX ring is full, Pico drops the whole outgoing frame and latches
the TX-drop status flag; it never blocks the host callback or a state
transition. `RECORD_EVENT` loss is observable through that flag. A Zero that
misses a `MODE_CHANGED` acknowledgement retries its idempotent `MODE_SET`.

Any frame or payload change requires a versioned update and matching tests.
