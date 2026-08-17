#pragma once
#include <stdint.h>

typedef struct {
    uint8_t lcd_brightness;  // 10-100 (%)
    uint8_t led_brightness;  // 0-100 (%)
    uint8_t volume;          // 0-100 (%)
    uint8_t palette;         // 0 = auto (per-game color), 1..13 = manual preset
    int8_t  frameskip;       // -1 = auto (show when ready), 0..5 = fixed frames skipped
    uint8_t theme;           // index into the built-in theme table (theme.h)
    uint8_t screensaver;     // 0 = off, 1 = on (idle animation on the menus)
} settings_t;

extern settings_t g_settings;

void settings_init(void);    // load from NVS flash, or defaults on first boot / version mismatch
void settings_apply(void);   // push current settings to hardware (backlight, LED)
void settings_menu(void);    // interactive settings screen (saves to NVS on exit if changed)
void settings_save(void);    // persist current settings to NVS (used by the in-game overlay too)