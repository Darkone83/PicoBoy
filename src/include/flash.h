#pragma once
#include <stdint.h>
#include <stddef.h>
#include "hardware/flash.h"   // FLASH_SECTOR_SIZE (4096), FLASH_PAGE_SIZE (256)
#include "pico/stdlib.h"      // XIP_BASE, PICO_FLASH_SIZE_BYTES

// Flash map (offsets from flash base; add XIP_BASE for a CPU-readable pointer):
//   program   : base .. (grows up; ~200 KB today, lots of headroom under 0x80000)
//   ROM window: 0x80000 .. NVS  (phase-2 staged GB ROM lives here)
//   NVS       : top 4 KB sector (persistent settings)
// Zones are fixed and non-overlapping, so each op erases only its own range.
#define ROM_FLASH_OFFSET  0x80000u
#define NVS_FLASH_OFFSET  ((uint32_t)(PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE))

// IRQ-safe single-sector erase / single-page program. Interrupts are disabled
// ONLY for the duration of each call, so loop over these for big regions and the
// LED timer / USB get serviced between sectors. The flash routines run from RAM;
// no handler may execute from XIP while a call is in flight, hence the IRQ mask.
// SINGLE-CORE ONLY: these do NOT lock out core1, which cannot execute-from or read
// flash mid-erase. The invariant is upheld by only calling them from the menu, where
// no core is launched (loader_browse / settings flash ops). If a future caller runs
// these while core1 is live, add a multicore lockout (flash_safe_execute) first.
void flash_erase_sector(uint32_t offset);              // offset must be 4 KB-aligned
void flash_program_page(uint32_t offset, const uint8_t *page256);  // offset 256-aligned

static inline const uint8_t *flash_ptr(uint32_t offset) {
    return (const uint8_t *)(XIP_BASE + offset);
}