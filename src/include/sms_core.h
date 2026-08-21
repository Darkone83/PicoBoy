#pragma once
#include <stdint.h>
#include <stdbool.h>

// Configure the SMSPlus core for the ROM already staged in PicoBoy's flash
// window. is_gamegear selects the Game Gear viewport/input personality.
void sms_core_set_rom(uint32_t size, bool is_gamegear);

// Per-game save paths. Empty/NULL disables the corresponding feature.
void sms_core_set_save_path(const char *path);
void sms_core_set_state_path(const char *path);

// Run the staged Master System / Game Gear ROM. Returns to the PicoBoy menu.
void sms_core_run(void);