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

# This firmware only needs TinyUSB. Initialise that required SDK submodule
# explicitly instead of downloading unrelated Bluetooth, Wi-Fi, TLS, and TCP/IP
# stacks during every clean CI build.
RUN git clone --branch "${PICO_SDK_VERSION}" --depth 1 \
        https://github.com/raspberrypi/pico-sdk.git /opt/pico-sdk \
    && git -C /opt/pico-sdk submodule update --init --depth 1 lib/tinyusb

ENV PICO_SDK_PATH=/opt/pico-sdk

WORKDIR /workspace
COPY pico ./pico

# First run the hardware-independent report tests, then build the complete
# Pico 2 firmware. Neither step needs a USB device attached to the container.
RUN sh pico/tests/run-host-tests.sh
RUN cmake -S pico -B /tmp/pico-build -G Ninja \
        -DPICO_BOARD=pico2 \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/pico-build --target pico_keyboard_hid

# A successful final stage exposes the UF2 as the build result. The CI job only
# needs `docker build --target verify`; a non-zero test or firmware build fails
# before this stage is produced.
FROM scratch AS verify
COPY --from=pico-builder /tmp/pico-build/pico_keyboard_hid.uf2 /pico_keyboard_hid.uf2
