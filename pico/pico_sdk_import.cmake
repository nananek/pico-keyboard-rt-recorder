# Import the Pico SDK from a caller-provided location. Keeping this small and
# local makes `cmake -S pico ...` work without copying the SDK into this repo.
if (NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif ()

if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR
        "PICO_SDK_PATH is not set. Set it to a Pico SDK checkout, or pass "
        "-DPICO_SDK_PATH=/path/to/pico-sdk when configuring.")
endif ()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH
    BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")

if (NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR
        "PICO_SDK_PATH (${PICO_SDK_PATH}) does not contain pico_sdk_init.cmake. "
        "Clone the Pico SDK including its submodules.")
endif ()

include("${PICO_SDK_PATH}/pico_sdk_init.cmake")
