cmake_minimum_required(VERSION 3.13)

# PICO_BOARD=pico2 also selects the RP2350 platform in the Pico SDK. Keep this
# as a cache value so another compatible board can be selected explicitly.
set(PICO_BOARD pico2 CACHE STRING "Pico SDK board")

include(pico_sdk_import.cmake)

project(pico_keyboard_rt_recorder C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

option(PICO_HID_DEMO_TEST
       "Send one A press/release after USB mount for hardware acceptance testing"
       OFF)
option(PICO_PLAYBACK_SCHED_TEST
       "Self-contained 1000-event/10ms playback scheduler benchmark driven without UART input, for Issue #7 acceptance testing"
       OFF)
option(PICO_CLOCK_DEBUG_PRINT
       "Print clk_sys/clk_peri/clk_ref/clk_usb once at boot over UART0-as-stdio, for Issue #26 clock-accuracy verification"
       OFF)
if(NOT TARGET tinyusb_pico_pio_usb)
    message(FATAL_ERROR
        "Pico-PIO-USB is unavailable. Initialize TinyUSB, then run "
        "Pico SDK's lib/tinyusb/tools/get_deps.py rp2040 before configuring.")
endif()

add_executable(pico_keyboard_hid
    src/main.c
    src/clock_debug_test.c
    src/hid_boot_keyboard.c
    src/hid_keyboard_device.c
    src/hid_keyboard_host.c
    src/keyboard_capture.c
    src/mode_state.c
    src/physical_report_dispatch.c
    src/playback_queue.c
    src/playback_scheduler.c
    src/playback_test_source.c
    src/safety_release.c
    src/uart_protocol.c
    src/uart_transport.c
    src/uart_transport_pico.c
    src/usb_descriptors.c
)

# tusb_config.h must be visible to TinyUSB while it compiles this target.
target_include_directories(pico_keyboard_hid PRIVATE include)

target_link_libraries(pico_keyboard_hid PRIVATE
    pico_stdlib
    pico_unique_id
    tinyusb_device
    tinyusb_host
    tinyusb_pico_pio_usb
    tinyusb_board
    hardware_uart
    hardware_irq
    hardware_timer
    hardware_clocks
)

target_compile_definitions(pico_keyboard_hid PRIVATE
    BOARD_TUD_RHPORT=0
    BOARD_TUH_RHPORT=1
    PICO_DEFAULT_PIO_USB_DP_PIN=12
    PICO_HID_DEMO_TEST=$<BOOL:${PICO_HID_DEMO_TEST}>
    PICO_PLAYBACK_SCHED_TEST=$<BOOL:${PICO_PLAYBACK_SCHED_TEST}>
    PICO_CLOCK_DEBUG_PRINT=$<BOOL:${PICO_CLOCK_DEBUG_PRINT}>
)

# USB is exclusively the TinyUSB HID device in this phase; do not add an SDK
# stdio USB/CDC interface alongside it. PICO_CLOCK_DEBUG_PRINT is the one
# exception to "stdio UART off": that standalone diagnostic build reuses
# UART0 as the SDK's stdio channel instead of the binary protocol, so it must
# never be flashed onto a Zero-integrated setup.
pico_enable_stdio_usb(pico_keyboard_hid 0)
if(PICO_CLOCK_DEBUG_PRINT)
    pico_enable_stdio_uart(pico_keyboard_hid 1)
else()
    pico_enable_stdio_uart(pico_keyboard_hid 0)
endif()
pico_add_extra_outputs(pico_keyboard_hid)
