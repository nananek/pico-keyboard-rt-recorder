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

#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#endif

#define CFG_TUSB_DEBUG 0
#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 1

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

// Root hub 0 is the RP2350 native USB device controller. Root hub 1 is the
// Pico-PIO-USB host configured by board_init() from
// PICO_DEFAULT_PIO_USB_DP_PIN. Phase 2 supports one keyboard interface and no
// hub or additional host classes.
#define CFG_TUH_DEVICE_MAX 1
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUH_HID 1
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64
#define CFG_TUH_HUB 0
#define CFG_TUH_CDC 0
#define CFG_TUH_MSC 0
#define CFG_TUH_MIDI 0
#define CFG_TUH_VENDOR 0

#endif  // _TUSB_CONFIG_H_
