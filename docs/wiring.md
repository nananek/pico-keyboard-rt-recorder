# Wiring

## Electrical rules

- Raspberry Pi Pico 2 and Raspberry Pi Zero 2 W use 3.3 V UART/GPIO logic. Do not connect 5 V logic to either board's GPIO.
- Connect a common ground before connecting UART or mode-control signals.
- Pico's mode input uses an internal or external pull-down so loss of a driven Zero signal resolves to `LOW` (`PASS`).
- The physical keyboard and PC USB paths must be electrically distinct. Verify the selected Pico 2 board exposes the required device connection and the chosen PIO USB-host wiring.

## Initial logical pin map

The following is the version-1 *provisional* mapping. Confirm it against the exact Pico 2 board, PIO USB implementation, and enclosure before soldering; firmware configuration becomes the source of truth once Phase 1 begins.

| Signal | Pi Zero 2 W | Pico 2 | Direction |
| --- | --- | --- | --- |
| UART TX | GPIO14 / pin 8 | UART RX (provisional GP1) | Zero → Pico |
| UART RX | GPIO15 / pin 10 | UART TX (provisional GP0) | Pico → Zero |
| Mode gate | GPIO17 / pin 11 | mode input (provisional GP2) | Zero → Pico |
| Ground | any GND | any GND | shared |

The physical-keyboard PIO USB D+/D− pins are intentionally not assigned in this initial document. Pin choice depends on the selected PIO USB host library and hardware layout and must be decided in the USB-host issue, together with VBUS power, current protection, and connector wiring.

## Safe power-up and shutdown

1. Bring up shared ground and UART/GPIO connections.
2. Keep Zero mode GPIO LOW before starting the Pico/Zero services.
3. Verify Pico enters PASS and sends no spurious key reports.
4. On Zero shutdown, send `PLAY_ABORT` when possible, then drive mode GPIO LOW before closing UART.

Never rely on GPIO alone to clear a pressed key: Pico must also emit the all-keys-release report during every mode transition.
