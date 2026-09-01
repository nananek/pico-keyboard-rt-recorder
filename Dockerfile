# syntax=docker/dockerfile:1

# This is the authoritative build environment for local verification and CI.
# Pinning the SDK release keeps Pico 2 builds reproducible independently of the
# runner's installed SDK or ARM toolchain.
FROM debian:bookworm-slim AS pico-builder

ARG PICO_SDK_VERSION=2.2.0

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        gcc-arm-none-eabi \
        git \
        libnewlib-arm-none-eabi \
        libstdc++-arm-none-eabi-newlib \
        ninja-build \
        python3 \
    && rm -rf /var/lib/apt/lists/*

# This firmware needs TinyUSB and its pinned Pico-PIO-USB dependency. TinyUSB
# 0.18 obtains MCU dependencies with get_deps.py rather than Git submodules.
RUN git clone --branch "${PICO_SDK_VERSION}" --depth 1 \
        https://github.com/raspberrypi/pico-sdk.git /opt/pico-sdk \
    && git -C /opt/pico-sdk submodule update --init --recursive --depth 1 \
        lib/tinyusb \
    && python3 /opt/pico-sdk/lib/tinyusb/tools/get_deps.py rp2040 \
    && test -f \
        /opt/pico-sdk/lib/tinyusb/hw/mcu/raspberry_pi/Pico-PIO-USB/src/pio_usb.c

ENV PICO_SDK_PATH=/opt/pico-sdk

WORKDIR /workspace
COPY pico ./pico

# First run hardware-independent pin/report/UART/mode/host-adapter tests, then
# build the Pico 2 firmware configurations. No step needs attached hardware.
RUN sh pico/tests/run-host-tests.sh
RUN cmake -S pico -B /tmp/pico-build -G Ninja \
        -DPICO_BOARD=pico2 \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/pico-build --target pico_keyboard_hid

# Compile the opt-in hardware-acceptance path as well. Without this separate
# configuration, CI only sees the default build and cannot catch regressions in
# the press/release demo that is flashed during Phase 1 acceptance.
RUN cmake -S pico -B /tmp/pico-build-demo -G Ninja \
        -DPICO_BOARD=pico2 \
        -DPICO_HID_DEMO_TEST=ON \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/pico-build-demo --target pico_keyboard_hid

# A successful final stage exposes the UF2 as the build result. The CI job only
# needs `docker build --target verify`; a non-zero test or firmware build fails
# before this stage is produced.
FROM scratch AS verify
COPY --from=pico-builder /tmp/pico-build/pico_keyboard_hid.uf2 /pico_keyboard_hid.uf2
COPY --from=pico-builder /tmp/pico-build-demo/pico_keyboard_hid.uf2 /pico_keyboard_hid_demo.uf2
