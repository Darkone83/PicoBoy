#include "flash.h"
#include "hardware/sync.h"

/* Supplied by the Pico SDK linker script. This is an XIP virtual address. */
extern uint8_t __flash_binary_end;

static inline uint32_t align_up_sector(uint32_t value) {
    return (value + FLASH_SECTOR_SIZE - 1u) &
           ~(uint32_t)(FLASH_SECTOR_SIZE - 1u);
}

uint32_t flash_firmware_end_offset(void) {
    uintptr_t end = (uintptr_t)&__flash_binary_end;
    uintptr_t flash_lo = (uintptr_t)XIP_BASE;
    uintptr_t flash_hi = flash_lo + (uintptr_t)PICO_FLASH_SIZE_BYTES;

    // A linker/layout mismatch must fail closed: report the entire flash used,
    // which makes the ROM capacity zero instead of risking firmware erasure.
    if (end < flash_lo || end > flash_hi)
        return (uint32_t)PICO_FLASH_SIZE_BYTES;

    return (uint32_t)(end - flash_lo);
}

uint32_t rom_flash_offset(void) {
    uint32_t end = flash_firmware_end_offset();

    if (end >= NVS_FLASH_OFFSET)
        return NVS_FLASH_OFFSET;

    uint32_t start = align_up_sector(end);

    /*
     * Keep one whole erase sector between the linked image and staged ROM.
     * Besides being cheap insurance, this guarantees ROM erases never share
     * the sector containing the final byte of firmware.
     */
    if (start <= NVS_FLASH_OFFSET - FLASH_SECTOR_SIZE)
        start += FLASH_SECTOR_SIZE;
    else
        start = NVS_FLASH_OFFSET;

    return start;
}

uint32_t rom_flash_capacity(void) {
    uint32_t start = rom_flash_offset();
    return (start < NVS_FLASH_OFFSET) ? (NVS_FLASH_OFFSET - start) : 0u;
}

bool rom_flash_range_valid(uint32_t offset, size_t length) {
    uint32_t start = rom_flash_offset();

    if (length == 0)
        return false;
    if (start >= NVS_FLASH_OFFSET)
        return false;
    if (offset < start || offset >= NVS_FLASH_OFFSET)
        return false;

    // Avoid offset + length overflow.
    size_t remaining = (size_t)NVS_FLASH_OFFSET - (size_t)offset;
    return length <= remaining;
}

void flash_erase_sector(uint32_t offset) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

void flash_program_page(uint32_t offset, const uint8_t *page256) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, page256, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

void flash_program_sector(uint32_t offset, const uint8_t *sector4096) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, sector4096, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}