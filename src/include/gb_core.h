#pragma once
// Game Boy core entry point (Peanut-GB). Runs the ROM staged in the flash
// window (flash.h ROM_FLASH_OFFSET). Returns when MENU is pressed.
void gb_core_run(void);

// Set the .srm path for cart-RAM battery saves before gb_core_run(). Pass "" (or
// NULL) to disable load/save (e.g. SD absent). cart RAM is loaded from this file
// at launch if it exists, and written back when the game exits.
void gb_set_save_path(const char *path);

// Set the .dat path for in-game save states (the overlay's Save/Load State). Pass
// "" (or NULL) to disable. State = full emulator snapshot + cart RAM.
void gb_set_state_path(const char *path);