# UART protocol (version 1)

## Framing

All UART messages use little-endian multi-byte integer fields.

```text
+---------+---------+---------+----------+-------------+---------+
| MAGIC   | VERSION | TYPE    | LEN      | PAYLOAD     | CRC16   |
| u8 A5   | u8 01   | u8      | u16 LE   | LEN bytes   | u16 LE  |
+---------+---------+---------+----------+-------------+---------+
```

- `CRC16` covers `VERSION`, `TYPE`, `LEN`, and `PAYLOAD`; it excludes `MAGIC` and itself.
- Version 1 uses CRC-16/CCITT-FALSE (`poly=0x1021`, `init=0xFFFF`, `xorout=0x0000`, non-reflected).
- A receiver rejects unsupported versions and invalid lengths/CRCs, resynchronizing by searching for the next `MAGIC` byte.
- Default link speed is 460800 baud, 8-N-1. Baud rate remains configuration, not a protocol field.

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
| Zero → Pico | `0x80` | `QUEUE_CLEAR` |
| Zero → Pico | `0x81` | `QUEUE_EVENT` |
| Zero → Pico | `0x82` | `QUEUE_END` |
| Zero → Pico | `0x83` | `PLAY_START` |
| Zero → Pico | `0x84` | `PLAY_ABORT` |
| Zero → Pico | `0x85` | `STATUS_REQUEST` |
| Zero → Pico | `0x86` | `PING` |

## Payloads

### `RECORD_EVENT`

```text
timestamp_us  u64 LE  Pico hardware timestamp at host receive
report_len    u8      8 in version 1
report        bytes   HID Boot Keyboard input report
```

### `QUEUE_EVENT`

```text
offset_us     u64 LE  absolute offset from the future Pico playback epoch
report_len    u8      8 in version 1
report        bytes   HID Boot Keyboard output report
```

`offset_us` is never a Zero-local deadline and never a delta from the preceding queued event.

### Queue and playback control

- `QUEUE_CLEAR`: no payload; valid only while Pico is ARMED.
- `QUEUE_END`: no payload; declares that no more events belong to this sequence.
- `PLAY_START`: no payload; Pico samples its playback epoch immediately and responds with `PLAY_STARTED`.
- `PLAY_ABORT`: no payload; Pico cancels playback, clears its queue, releases all keys, and returns to ARMED while GPIO remains HIGH.
- `BUFFER_STATUS`: `used u16 LE`, `capacity u16 LE`, `state u8`. It is Pico's capacity authority.
- `PLAY_STARTED`: `playback_start_us u64 LE` for diagnostics.
- `PLAY_UNDERRUN`: `offset_us u64 LE`, `used u16 LE` at detection.

Any future change to the frame, message values, payload layouts, state validity, or CRC requires a versioned update to this document and protocol tests.
