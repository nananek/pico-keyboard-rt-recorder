# Architecture

## Responsibility boundary

```text
Pico 2 — Real-time domain             Pi Zero 2 W — Control/storage domain
--------------------------------      -------------------------------------
USB keyboard host                      Recording persistence
USB HID keyboard device                Sequence management
Pico capture timestamps                Future-event preparation
Pass-through                           UART transport and flow control
Playback ring buffer                   GPIO mode control
Hardware-timer deadline scheduler      HTTP/WebSocket API and web UI
Safety state machine                   Rename/delete/loop/speed features
```

The boundary is intentional: a Linux scheduling delay, Python pause, disk stall, or UART transmit delay must not affect a playback deadline already queued on Pico.

## Phase 2 USB host capture

The native RP2350 USB controller is TinyUSB root hub 0 in device mode. The
Pico-PIO-USB controller on GP12 D+ / GP13 D- is root hub 1 in host mode. Both
TinyUSB tasks currently run on Pico core 0; host callbacks only copy a raw
8-byte report plus the callback-entry `time_us_64()` value into a bounded local
handoff. The opt-in UART diagnostic drains that handoff later, so formatting and
transport time cannot replace the capture timestamp.

GP2 is initialized as the raw mode-gate input with a pull-down, but Phase 2
does not implement PASS/ARMED/PLAYING transitions. It is distinct from UART0
GP0/GP1 and PIO-USB GP12/GP13, preserving GPIO ownership and timing
responsibility without using the official Pico 2's reserved GP29 VSYS monitor.

## Data paths

### PASS

```text
Physical keyboard -> Pico USB host -> Pico timestamp -> UART -> Zero storage
                                      `-> USB HID device -> PC
```

Pico always captures the timestamp. Zero may decide whether to persist an event, but it does not reinterpret its time.

### ARMED and PLAYING

```text
Zero recording -> offsets from Pico epoch -> UART -> Pico ring buffer
                                                Pico hardware alarm -> USB HID -> PC
Physical keyboard ---------------------------------------------------X
```

Zero queues events in advance. Pico creates the epoch at `PLAY_START` and executes the queued reports against that epoch.

## State machine

```text
              GPIO HIGH
PASS ----------------------> ARMED
 ^                              |
 | GPIO LOW                     | PLAY_START
 +------------------------------+----> PLAYING
                                      |  ^
                         END/ABORT ---+  |
                                      |  |
                              underrun/error
                                      v
                                    ERROR
                                      |
                                   GPIO LOW
                                      v
                                    PASS
```

`GPIO LOW` has priority from every non-PASS state. It cancels playback, clears state, sends all keys released, and returns to `PASS`.

## Queue ownership

Pico owns a fixed-capacity ring buffer (initial target: 256 events). Zero sends only within advertised capacity or credits. Queue state and end-of-sequence are explicit protocol state; an empty queue during a non-ended playback is an underrun.
