#pragma once
#include <stdint.h>

// Size of the ROM currently staged in PicoBoy's flash ROM window.
void atari_core_set_rom_size(uint32_t size);

// PicoBoy-facing Atari 2600 entry point.
void atari_core_run(void);