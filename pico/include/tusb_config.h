#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

// The Pico SDK supplies CFG_TUSB_MCU for the selected board/platform. Keeping
// it SDK-provided lets PICO_BOARD=pico2 select RP2350 without an RP2040-only
// hard-coded MCU value here.
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined by the Pico SDK
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#define CFG_TUSB_DEBUG 0
#define CFG_TUD_ENABLED 1

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

#define CFG_TUD_HID 1
#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

// One no-report-ID Boot Keyboard input report is 8 bytes. Allow space for a
// control/LED output report and future small class transfers as well.
#define CFG_TUD_HID_EP_BUFSIZE 16

#endif  // _TUSB_CONFIG_H_
