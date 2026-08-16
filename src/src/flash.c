#include "flash.h"
#include "hardware/sync.h"

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