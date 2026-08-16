#pragma once

// Each diagnostic runs a blocking loop until its exit condition, then returns
// to the diagnostics menu.
void diag_display(void);   // colour fills / test, LEFT/RIGHT to cycle, B back
void diag_buttons(void);   // live button grid, START+SELECT to exit
void diag_rgb(void);       // SK6812 colour cycle, LEFT/RIGHT, B back
void diag_tone(void);      // STUB: I2S not wired
void diag_sd(void);        // STUB: FATFS not wired

// Diagnostics submenu (entered from Settings).
void diagnostics_menu(void);