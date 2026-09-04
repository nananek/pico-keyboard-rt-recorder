#include "uart_transport.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"

#include "hardware_config.h"

static pico_uart_transport_t *active_transport;

static void uart0_irq_handler(void) {
    if (active_transport == NULL) {
        return;
    }
    const uint32_t errors = uart_get_hw(uart0)->rsr;
    if (errors != 0u) {
        uart_get_hw(uart0)->rsr = 0u;
        pico_uart_transport_hardware_error_isr(active_transport);
    }
    while (uart_is_readable(uart0)) {
        pico_uart_transport_rx_push_isr(active_transport,
                                         (uint8_t)uart_getc(uart0));
    }
}

void pico_uart_transport_hw_init(pico_uart_transport_t *transport) {
    active_transport = transport;
    gpio_set_function(PICO_KEYBOARD_UART_TX_GPIO, GPIO_FUNC_UART);
    gpio_set_function(PICO_KEYBOARD_UART_RX_GPIO, GPIO_FUNC_UART);
    uart_init(uart0, PICO_KEYBOARD_UART_BAUD);
    uart_set_format(uart0, 8u, 1u, UART_PARITY_NONE);
    uart_set_hw_flow(uart0, false, false);
    irq_set_exclusive_handler(UART0_IRQ, uart0_irq_handler);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(uart0, true, false);
}

void pico_uart_transport_tx_task(pico_uart_transport_t *transport) {
    if (transport == NULL) {
        return;
    }
    uint8_t byte;
    while (uart_is_writable(uart0) &&
           pico_uart_transport_pop_tx_byte(transport, &byte)) {
        uart_putc_raw(uart0, byte);
    }
}
