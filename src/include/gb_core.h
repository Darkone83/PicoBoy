#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configure the currently staged GB/GBC image.
//
// total_size:
//   Raw cartridge image length.
//
// flash_prefix:
//   Number of bytes available from ROM_FLASH_OFFSET in XIP flash.
//   For normal GB/GBC this equals total_size.
//   For a split large GBC image this is a complete 16-KiB-bank-aligned prefix.
//
// sd_path:
//   Original .gbc path used only when flash_prefix < total_size.
//   Pass "" / NULL for an all-flash image.
void gb_set_rom_source(uint32_t total_size,
                       uint32_t flash_prefix,
                       const char *sd_path);

void gb_set_save_path(const char *path);
void gb_set_state_path(const char *path);
void gb_core_run(void);

#ifdef __cplusplus
}
#endif