# Pico Keyboard Real-time Recorder

Pico Keyboard Real-time Recorder records and plays USB keyboard HID reports with a Raspberry Pi Pico 2 as the real-time engine and a Raspberry Pi Zero 2 W as the controller, storage service, and web interface.

The project is designed for predictable playback: Pico timestamps physical input, owns the playback epoch, buffers future events, and schedules USB HID reports against absolute hardware-timer deadlines. The Zero never supplies the real-time clock.

## Status

Phase 1 provides the Pico 2 USB HID Boot Keyboard device foundation. It
enumerates as one keyboard interface, uses a fixed 8-byte 6KRO report, and has
an explicit all-keys-release send path. The complete implementation roadmap is
tracked as GitHub Issues and mirrored in [docs/roadmap.md](docs/roadmap.md).

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

## Pico 2 USB HID keyboard (Phase 1)

### Prerequisites

- Pico SDK checkout with submodules, including TinyUSB;
- CMake 3.13 or newer, Ninja (or another CMake generator), and the Pico 2
  ARM toolchain;
- `PICO_SDK_PATH` set to that SDK checkout.

For a standard SDK checkout:

```sh
git clone --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
export PICO_SDK_PATH=/path/to/pico-sdk
```

### Build and flash

The normal build is intentionally quiet: it enumerates as a keyboard but does
not synthesize a key. Configure and build the Pico 2 target with:

```sh
cmake -S pico -B pico/build -G Ninja -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
cmake --build pico/build --target pico_keyboard_hid
```

Put the Pico 2 into BOOTSEL mode, then copy
`pico/build/pico_keyboard_hid.uf2` to its mounted USB mass-storage volume (or
load that UF2 with `picotool`). Connect the firmware's USB device port directly
to the PC being tested.

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

### Reproducible Docker verification

The repository's CI uses Docker as its build environment. It pins Pico SDK
2.2.0, installs the Pico 2 toolchain in the container, runs the host report
test, and cross-builds the UF2. Run the same verification locally with Docker:

```sh
docker build --target verify --tag pico-keyboard-verify .
```

The final image contains `/pico_keyboard_hid.uf2`; it is only produced when
both verification steps pass. Hardware enumeration still requires flashing that
UF2 to a Pico 2 and following the on-device steps above.
