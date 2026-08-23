#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU            OPT_MCU_RP2040
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_NONE
#endif
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG          0
#endif

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_HIGH_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             1
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

// Aggregate four complete SD sectors per MSC class-buffer transaction.
// USB Full Speed still uses 64-byte endpoint packets on the wire; this larger
// class buffer lets TinyUSB invoke READ10/WRITE10 with up to 2048 contiguous
// bytes so the raw SD layer can use CMD18/CMD25 multi-block transfers.
//
// Permanent SRAM cost versus the previous 512-byte setting: +1536 bytes.
#define CFG_TUD_MSC_EP_BUFSIZE  2048

#ifdef __cplusplus
}
#endif