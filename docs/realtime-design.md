# Real-time design

The Pico main loop is the only context allowed to parse UART frames or change
the mode state. UART0 RX uses a bounded single-producer/single-consumer byte
ring. The interrupt handler drains the FIFO, counts framing/parity/overrun
errors, and returns; it never blocks, allocates, invokes TinyUSB, or sends HID.
Ring overflow and hardware errors are latched for main-loop handling.

The main loop resynchronizes on `0xA5`, validates version 2, bounded length,
CRC-16/CCITT-FALSE, direction/type, and payload shape. Valid commands enter a
small command queue. Invalid input never changes mode directly and causes a
safe UART-fault transition when the current mode is not PASS. TX frames use a
bounded ring and are drained only while UART hardware is writable.

Physical reports are timestamped with `time_us_64()` at the TinyUSB host
callback. In PASS they are forwarded to the native HID device; in RECORD they
become `RECORD_EVENT` frames carrying that Pico timestamp; in ARMED/PLAYING or
ERROR they are discarded. Changing mode clears queued physical reports and
sends an all-zero release report. UART event arrival is never a timing source.

Playback remains an absolute-deadline feature: `PLAY_START` samples a Pico
epoch and future `QUEUE_EVENT` offsets are scheduled against that epoch. A
Zero sleep, UART write time, or chained delay must not determine HID output.
Playback alarm work must hand off USB calls to a safe Pico context when needed.
