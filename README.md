# Pico Keyboard Real-time Recorder

Pico Keyboard Real-time Recorder records and plays USB keyboard HID reports with a Raspberry Pi Pico 2 as the real-time engine and a Raspberry Pi Zero 2 W as the controller, storage service, and web interface.

The project is designed for predictable playback: Pico timestamps physical input, owns the playback epoch, buffers future events, and schedules USB HID reports against absolute hardware-timer deadlines. The Zero never supplies the real-time clock.

## Status

Initial project scaffold. The complete implementation roadmap is tracked as GitHub Issues and mirrored in [docs/roadmap.md](docs/roadmap.md).

## Architecture at a glance

```text
Physical keyboard -> Pico 2 USB host -> PC USB HID keyboard
                           |                 ^
                           | timestamped     | absolute-deadline reports
                           v                 |
                       UART <-> Pi Zero 2 W -+
                         storage / UI / future-event feeder
```

- Pico 2: USB host/device, capture timestamps, pass-through, playback queue and hardware-timer scheduler, mode/safety handling.
- Pi Zero 2 W: recording persistence, playback data preparation and prebuffering, UART flow control, GPIO mode control, API, and UI.

## Repository layout

```text
docs/   Architecture, RT design, protocol, wiring, data format, and tests
pico/   Pico SDK firmware (real-time engine)
zero/   Pi Zero application (control and storage domain)
tools/  Development, protocol, and jitter-analysis tools
```

## Core constraints

1. Pico is the only real-time clock.
2. Zero must never timestamp recording events or schedule HID output with Python/Linux sleeps.
3. Playback events are absolute offsets from a Pico-owned epoch.
4. Playback requires prebuffering and must fail safely on an underrun.
5. Every mode change, abort, finish, and error sends all keys released.

Read [AGENT.md](AGENT.md) before implementation. The authoritative design documents are in [docs/](docs/).

## Initial hardware verification sequence

The first functional milestone is timestamped pass-through:

```text
Physical keyboard -> Pico PIO USB host -> Pico timestamp
                                      |-> PC HID device
                                      `-> UART -> Pi Zero
```

The intended verification steps for every later phase are maintained in [docs/testing.md](docs/testing.md).
