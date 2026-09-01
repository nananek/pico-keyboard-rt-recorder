# Real-time design

The Pico main loop is the only context allowed to parse UART frames or change
the mode state. UART0 RX uses a bounded single-producer/single-consumer byte
ring. The interrupt handler drains the FIFO, counts framing/parity/overrun
errors, and returns; it never blocks, allocates, invokes TinyUSB, or sends HID.
Ring overflow and hardware errors are latched for main-loop handling.
The ring holds 256 bytes: more than three maximum-size 71-byte frames and
about 5.5 ms of continuous 460800-baud 8-N-1 input, while the main loop's
normal sleep interval is 1 ms.

The main loop resynchronizes on `0xA5`, validates version 2, bounded length,
CRC-16/CCITT-FALSE, direction/type, and payload shape. Valid commands enter a
small command queue. Invalid input never changes mode directly and causes a
safe UART-fault transition when the current mode is not PASS. TX frames use a
bounded ring and are drained only while UART hardware is writable. A full TX
ring drops an entire frame and increments `tx_dropped`; mode commands remain
safe because `MODE_SET` is idempotent and the Zero retries it if its
`MODE_CHANGED` acknowledgement is absent.

Physical reports are timestamped with `time_us_64()` at the TinyUSB host
callback. In PASS they are forwarded to the native HID device; in RECORD they
become `RECORD_EVENT` frames carrying that Pico timestamp; in ARMED/PLAYING or
ERROR they are discarded. A successful state change, abort, or a fault that
enters ERROR clears queued physical reports and sends an all-zero release report;
idempotent `MODE_SET(PASS)` retransmission and faults received in PASS do
neither. If the HID endpoint is temporarily busy, the release is retried before
any later physical report is forwarded. UART event arrival is never a timing
source.

Playback remains an absolute-deadline feature: `PLAY_START` samples a Pico
epoch and future `QUEUE_EVENT` offsets are scheduled against that epoch. A
Zero sleep, UART write time, or chained delay must not determine HID output.
Playback alarm work must hand off USB calls to a safe Pico context when needed.
