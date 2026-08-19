#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Run the NES ROM staged in the flash window (InfoNES). Returns when MENU is
// pressed. Shares the RAM arena with the GB core (only one runs at a time).
void nes_run(void);

// Set the .srm path for battery SRAM saves before nes_run(). Pass "" to disable.
void nes_set_save_path(const char *path);

// Set the .dat path for in-game save states (the overlay's Save/Load State) before
// nes_run(). Pass "" (or NULL) to disable. State captures the InfoNES machine RAMs,
// CPU/PPU registers, and current bank mapping (see nes_core.cpp for the caveat on
// scanline-IRQ mappers).
void nes_set_state_path(const char *path);

#ifdef __cplusplus
}
#endif