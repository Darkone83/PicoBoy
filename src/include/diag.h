#pragma once

// Each diagnostic runs a blocking UI loop until its exit condition, then returns
// to the themed diagnostics menu. These started as breadboard bring-up tests and
// intentionally remain useful hardware checks rather than becoming a benchmark.
void diag_system(void);    // firmware/clock/battery/storage snapshot
void diag_display(void);   // colour bars / solids / gradient / checker / grid
void diag_buttons(void);   // live handheld-style control map; START+SELECT exits
void diag_rgb(void);       // physical LED + on-screen colour preview / cycle
void diag_tone(void);      // selectable I2S tones through MAX98357
void diag_sd(void);        // card init + read-only sector-0 verification

// Diagnostics submenu (entered from Settings).
void diagnostics_menu(void);