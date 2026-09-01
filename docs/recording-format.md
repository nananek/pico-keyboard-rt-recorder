# Recording format

## Version 1 JSON

Recordings persist Pico-captured timing as delta times. The first event's `dt_us` is zero; each following value is the elapsed Pico time since the preceding stored event.

```json
{
  "version": 1,
  "name": "recording-20260901-143512",
  "duration_us": 124202,
  "events": [
    {"dt_us": 0, "report": [0, 0, 11, 0, 0, 0, 0, 0]},
    {"dt_us": 82431, "report": [0, 0, 0, 0, 0, 0, 0, 0]},
    {"dt_us": 41771, "report": [0, 0, 12, 0, 0, 0, 0, 0]}
  ]
}
```

## Rules

- `duration_us` is the timestamp offset of the final event from the first event.
- A version-1 `report` is exactly an 8-byte Boot Keyboard report: modifier, reserved, then six keycodes.
- The recorder receives Pico `timestamp_us` values, chooses the first value as `recording_epoch`, and converts subsequent timestamps to relative offsets. It may then derive `dt_us` for persistence.
- The playback feeder accumulates `dt_us` into monotonic absolute offsets before applying speed scaling and sending `QUEUE_EVENT` messages.
- Names and IDs must be validated before use as filesystem paths. Store atomically so interrupted writes never replace a valid recording with partial JSON.

The persistence format may migrate to a binary representation later, but the playback semantics—Pico-derived capture time and absolute Pico-epoch playback offsets—must remain unchanged.
