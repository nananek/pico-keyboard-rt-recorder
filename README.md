# Pico Keyboard Real-time Recorder

Pico Keyboard Real-time Recorder records and plays USB keyboard HID reports with a Raspberry Pi Pico 2 as the real-time engine and a Raspberry Pi Zero 2 W as the controller, storage service, and web interface.

The project is designed for predictable playback: Pico timestamps physical input, owns the playback epoch, buffers future events, and schedules USB HID reports against absolute hardware-timer deadlines. The Zero never supplies the real-time clock.

## Status

Phase 2 adds a Pico-PIO-USB Boot Keyboard host and Pico-owned receive-boundary
capture timestamps to the Phase 1 USB HID device foundation. The fixed Pico pin
contract is **GP12 D+**, **GP13 D-**, and **GP2 mode input with pull-down**.
Capture currently retains raw 8-byte 6KRO reports in a bounded handoff and an
opt-in UART diagnostic; production pass-through, UART `RECORD_EVENT` framing,
and the mode state machine remain later phases. The complete implementation
roadmap is tracked as GitHub Issues and mirrored in
[docs/roadmap.md](docs/roadmap.md). UART0 uses GP0 TX / GP1 RX; compile-time
checks keep GP2 and GP12/GP13 distinct from that pair.

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

## Pico 2 dual USB host/device capture (Phase 2)

### Prerequisites

- Pico SDK checkout with TinyUSB and TinyUSB's pinned Pico-PIO-USB dependency;
- CMake 3.13 or newer, Ninja (or another CMake generator), and the Pico 2
  ARM toolchain;
- `PICO_SDK_PATH` set to that SDK checkout.

For a focused SDK checkout, initialize TinyUSB and then use TinyUSB's dependency
tool to obtain the pinned Pico-PIO-USB revision:

```sh
git clone https://github.com/raspberrypi/pico-sdk.git
git -C pico-sdk submodule update --init lib/tinyusb
python3 pico-sdk/lib/tinyusb/tools/get_deps.py rp2040
export PICO_SDK_PATH=/path/to/pico-sdk
```

### Build and flash

The normal build is intentionally quiet: it services the native USB HID device
on root hub 0 and the GP12/GP13 PIO-USB host on root hub 1, captures reports,
but emits no textual diagnostics and synthesizes no key. Configure and build:

```sh
cmake -S pico -B pico/build -G Ninja -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
cmake --build pico/build --target pico_keyboard_hid
```

Put the Pico 2 into BOOTSEL mode, then copy
`pico/build/pico_keyboard_hid.uf2` to its mounted USB mass-storage volume (or
load that UF2 with `picotool`). Connect the firmware's USB device port directly
to the PC being tested. Supply the physical keyboard's VBUS through a protected,
current-limited 5 V path; firmware does not assign a VBUS-enable GPIO. Follow
[docs/wiring.md](docs/wiring.md) before attaching a keyboard.

### Verify enumeration, press, and release

Run the hardware acceptance build below only on a PC where one deliberate `a`
keystroke is safe. After USB enumeration and HID endpoint readiness, it sends
one `a` key press, waits 50 ms, and sends a full all-zero 8-byte release report.

```sh
cmake -S pico -B pico/build-demo -G Ninja -DPICO_BOARD=pico2 -DPICO_HID_DEMO_TEST=ON
cmake --build pico/build-demo --target pico_keyboard_hid
```

Flash the demo UF2, reset the Pico 2, and use the operating system's input-event
viewer to confirm all of the following:

1. The PC enumerates **Pico 2 Boot Keyboard** as a keyboard device.
2. The event stream contains one `a` press followed by its release.
3. No key remains pressed after the release event.

The pure report-layout test can run without the Pico SDK or attached hardware:

```sh
sh pico/tests/run-host-tests.sh
```

### Verify physical keyboard capture and Pico timestamps

The capture diagnostic is acceptance-only. It drains captured events outside
the USB receive callback and writes lines over UART0 TX on GP0 at 115200 baud:

```sh
cmake -S pico -B pico/build-capture -G Ninja \
  -DPICO_BOARD=pico2 -DPICO_HID_HOST_CAPTURE_TEST=ON
cmake --build pico/build-capture --target pico_keyboard_hid
```

Each valid host transfer produces a record in this form:

```text
CAPTURE <timestamp_us> 8 <modifier> <reserved> <key0> ... <key5>
```

After validating GP12/GP13 wiring and protected VBUS, connect a Boot/6KRO
keyboard. Confirm exact press, held/duplicate, modifier, simultaneous-key, and
all-zero release reports; timestamps must be nondecreasing. Unplug/replug and
confirm capture resumes. The timestamp printed is the exact value sampled at
the TinyUSB host callback entry, not UART output time. Full details are in
[docs/testing.md](docs/testing.md).

### Reproducible Docker verification

The repository's CI uses Docker as its build environment. It pins Pico SDK
2.2.0, obtains TinyUSB's pinned Pico-PIO-USB dependency with `get_deps.py`, runs
the pin/report/capture/host-adapter tests, and cross-builds the normal, HID-demo,
and capture-diagnostic UF2 configurations. Run the same verification locally
with Docker:

```sh
docker build --target verify --tag pico-keyboard-verify .
```

The final image contains `/pico_keyboard_hid.uf2`,
`/pico_keyboard_hid_demo.uf2`, and `/pico_keyboard_hid_capture.uf2`. Hardware
enumeration, electrical safety, GP2 mode-gate behavior, and timestamp acceptance
still require the on-device procedures above.

## License

This project is licensed under the [MIT License](LICENSE).
