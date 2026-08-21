#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hardware/flash.h"   // FLASH_SECTOR_SIZE (4096), FLASH_PAGE_SIZE (256)
#include "pico/stdlib.h"      // XIP_BASE, PICO_FLASH_SIZE_BYTES

/*
 * PicoBoy flash map
 * -----------------
 *
 * Do NOT hard-code the ROM staging area at 0x80000.
 *
 * The firmware has grown as emulator cores were added, and a fixed 512 KiB
 * boundary can eventually overlap the linked image. Erasing/programming a ROM
 * into such an overlap destroys the currently running firmware.
 *
 * Instead:
 *
 *   firmware  : flash base .. __flash_binary_end
 *   guard     : one complete 4 KiB sector
 *   ROM       : sector-aligned dynamic base .. NVS
 *   NVS       : final 4 KiB sector
 *
 * The linker-provided __flash_binary_end includes the actual flash-resident
 * image, including load images for RAM sections. rom_flash_offset() rounds past
 * it and leaves one extra sector untouched.
 */
#define NVS_FLASH_OFFSET ((uint32_t)(PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE))

uint32_t flash_firmware_end_offset(void);
uint32_t rom_flash_offset(void);
uint32_t rom_flash_capacity(void);
bool     rom_flash_range_valid(uint32_t offset, size_t length);

/*
 * Backward-compatible name used throughout the emulator cores.
 * This is intentionally a runtime expression now, not a fixed constant.
 */
#define ROM_FLASH_OFFSET (rom_flash_offset())

// IRQ-safe erase/program helpers.
// These are generic because settings_save() legitimately writes the NVS sector.
// ROM callers must validate with rom_flash_range_valid() before modifying flash.
void flash_erase_sector(uint32_t offset);                       // 4 KiB aligned
void flash_program_page(uint32_t offset, const uint8_t *page256);   // 256 B aligned
void flash_program_sector(uint32_t offset, const uint8_t *sector4096); // 4 KiB aligned

static inline const uint8_t *flash_ptr(uint32_t offset) {
    return (const uint8_t *)(XIP_BASE + offset);
}