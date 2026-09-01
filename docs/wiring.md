# Wiring

## Electrical rules

- Raspberry Pi Pico 2 and Raspberry Pi Zero 2 W use 3.3 V UART/GPIO logic. Do not connect 5 V logic to either board's GPIO.
- Connect a common ground before connecting UART or mode-control signals.
- Pico's mode input uses an internal or external pull-down so loss of a driven Zero signal resolves to `LOW` (`PASS`).
- The physical keyboard and PC USB paths must be electrically distinct. Verify the selected Pico 2 board exposes the required device connection and the chosen PIO USB-host wiring.
- Supply keyboard VBUS from a separately protected 5 V path with current
  limiting or a suitable USB power switch. The firmware does not assign a
  VBUS-enable GPIO. Add ESD protection appropriate to the connector and layout.

## Firmware pin map

`pico/include/hardware_config.h` is the firmware source of truth for UART0,
USB, and mode pins.

| Signal | Pi Zero 2 W | Pico / RP2350 GPIO | Direction |
| --- | --- | --- | --- |
| UART TX | GPIO14 / pin 8 | **UART0 RX GP1 / Pico pin 2** | Zero → Pico |
| UART RX | GPIO15 / pin 10 | **UART0 TX GP0 / Pico pin 1** | Pico → Zero |
| Mode gate | GPIO17 / pin 11 | **GP2 / Pico pin 4** with pull-down | Zero → Pico |
| PIO USB D+ | — | **GP12 / Pico header pin 16** | keyboard ↔ Pico |
| PIO USB D- | — | **GP13 / Pico header pin 17** | keyboard ↔ Pico |
| Ground | any GND | any GND | shared |

Pico-PIO-USB is configured for `PIO_USB_PINOUT_DPDM`: GP12 is `pin_dp` and D-
is derived as the immediately following GP13. Route the pair together, preserve
that order, and place a recommended 22 Ω series resistor in each data line near
the RP2350. Do not infer or add a data-line swap in software.

GP2 is exposed on the official Pico 2 header and does not overlap UART0
GP0/GP1 or PIO-USB GP12/GP13. Firmware compile-time assertions and host tests
enforce that separation. GP29 remains the official board's internal VSYS/3
monitor and is not used by this project.

## Safe power-up and shutdown

1. Bring up shared ground and UART/GPIO connections.
2. Keep Zero mode GPIO LOW before starting the Pico/Zero services.
3. Verify Pico enters PASS and sends no spurious key reports.
4. On Zero shutdown, send `PLAY_ABORT` when possible, then drive mode GPIO LOW before closing UART.

Never rely on GPIO alone to clear a pressed key: Pico must also emit the all-keys-release report during every mode transition.
