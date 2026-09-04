#include "clock_debug_test.h"

#if PICO_CLOCK_DEBUG_PRINT

#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"

void pico_clock_debug_print(void) {
    stdio_init_all();
    printf(
        "clk_sys=%u clk_peri=%u clk_ref=%u clk_usb=%u\n",
        (unsigned)clock_get_hz(clk_sys), (unsigned)clock_get_hz(clk_peri),
        (unsigned)clock_get_hz(clk_ref), (unsigned)clock_get_hz(clk_usb));
}

#endif  // PICO_CLOCK_DEBUG_PRINT
