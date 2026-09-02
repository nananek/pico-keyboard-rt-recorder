# Wiring and pin contract

The firmware pin assignments are deliberately narrow and non-overlapping:

| Function | Pico 2 pin |
| --- | ---: |
| UART0 TX to Zero RX | GP0 |
| UART0 RX from Zero TX | GP1 |
| PIO-USB D+ | GP12 |
| PIO-USB D- | GP13 |

Connect UART as a crossed 3.3 V TTL link (Pico GP0 → Zero RX, Zero TX → Pico
GP1) and connect grounds. UART is 460800 baud, 8-N-1. There is intentionally
no additional mode GPIO or Zero-side control wire.

The native Pico USB connector is the HID device presented to the PC. Attach the
physical keyboard to the PIO-USB host connector with a protected, current-
limited 5 V VBUS path. The firmware does not assign a VBUS-enable GPIO; verify
polarity and current limiting before plugging in a keyboard. Keep USB D+/D-
wiring short and follow the Pico-PIO-USB electrical guidance.

Before hardware power-up, verify that no external circuit drives GP0/GP1 at an
incompatible voltage and that the UART link is crossed. All mode changes must
be sent as CRC-checked version-2 `MODE_SET` frames; a disconnected or noisy
link cannot enable a playback mode silently.
