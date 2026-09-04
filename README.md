# Pico Keyboard Real-time Recorder

Pico Keyboard Real-time Recorder uses a Raspberry Pi Pico 2 as the real-time
USB keyboard engine and a Pi Zero 2 W as the controller, storage service, and
web interface. The Pico owns capture timestamps, playback timing, USB HID, and
safety state. The Zero supplies framed future events and mode commands.

## Current status

The firmware has a Pico-PIO-USB Boot Keyboard host on **GP12 D+ / GP13 D-**,
the native HID device, and a binary UART0 transport on **GP0 TX / GP1 RX**.
UART RX interrupt work is bounded to FIFO draining into a byte ring. The main
loop validates version-2 frames and CRC, handles `MODE_SET (0x87)`, emits
`MODE_CHANGED (0x0B)`, and enforces PASS/RECORD/ARMED/PLAYING/ERROR safety
transitions. Mode is selected only through validated UART commands.

## Architecture

```text
Physical keyboard -> Pico PIO-USB host -> (PASS) Pico USB HID -> PC
                                      -> (RECORD) UART0 -> Pi Zero
Pi Zero -- UART0 MODE_SET/queue frames --> Pico main-loop state machine
```

The Pico timestamp is sampled at the TinyUSB host callback, never at UART
receive time. See [docs/architecture.md](docs/architecture.md),
[docs/realtime-design.md](docs/realtime-design.md), and
[docs/protocol.md](docs/protocol.md).

## Build and test

Use Pico SDK 2.2.0 (with TinyUSB and its pinned Pico-PIO-USB dependency):

```sh
git clone https://github.com/raspberrypi/pico-sdk.git
git -C pico-sdk submodule update --init lib/tinyusb
python3 pico-sdk/lib/tinyusb/tools/get_deps.py rp2040
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S pico -B pico/build -G Ninja -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
cmake --build pico/build --target pico_keyboard_hid
sh pico/tests/run-host-tests.sh
```

## Pi Zero recorder CLI

The Zero recorder stores only the timestamps supplied by Pico UART
`RECORD_EVENT` frames; it never substitutes serial receive time. It requires
Python 3.10+ and PySerial:

```sh
python3 -m pip install -r zero/requirements.txt
cd zero
python3 -m unittest discover -s tests -v

# The recorder remains in PASS unless `record` is explicitly requested.
PYTHONPATH=. python3 -m app --recordings-dir recordings record hello --device /dev/serial0
# Press Ctrl-C to request PASS, receive its acknowledgement, and publish hello.json.
PYTHONPATH=. python3 -m app --recordings-dir recordings list
PYTHONPATH=. python3 -m app --recordings-dir recordings dump hello
PYTHONPATH=. python3 -m app stop --device /dev/serial0
```

`record`, `stop`, `list`, and `dump` print JSON. `record` only atomically
publishes a file after a successful `MODE_SET(PASS)` acknowledgement. Invalid
UART frames, Pico errors, disconnection, rejected/timed-out transitions, and
unsafe recording names leave no partial recording. Saved names allow only
ASCII letters, digits, `.`, `_`, and `-`, so they cannot escape the recordings
directory.

The optional `PICO_HID_DEMO_TEST=ON` build sends one safe A press/release after
device enumeration. The normal firmware is UART-enabled and has no textual
capture diagnostic.

The optional `PICO_PLAYBACK_SCHED_TEST=ON` build is a self-contained playback
scheduler benchmark: with no UART traffic attached, it drives itself through
PASS -> ARMED -> `PLAY_START` and dispatches 1000 synthetic events at a 10 ms
nominal spacing, then reports the resulting `PLAY_FINISHED`/`PLAY_METRICS`
pair over UART0 for the real-time acceptance record described in
[docs/testing.md](docs/testing.md) (hardware acceptance step 11). Docker
reproduces host tests and normal/demo UF2 builds:

```sh
docker build --target verify --tag pico-keyboard-verify .
```

## Wiring and acceptance

Connect the physical keyboard to the protected 5 V/VBUS path described in
[docs/wiring.md](docs/wiring.md). Cross UART TX/RX between Pico and Zero and
share ground; do not connect a mode wire. Follow [docs/testing.md](docs/testing.md)
for protocol, pass-through, recording, and fault-injection checks.

## License

This project is licensed under the [MIT License](LICENSE).
