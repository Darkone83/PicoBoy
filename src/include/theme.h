#pragma once
#include <stdint.h>

// 8-bit-per-channel RGB -> RGB565 (the panel's native format). Lets themes be
// authored in plain RGB -- e.g. the brand purple RGB565(168,85,247) -- and stay
// readable and tweakable instead of opaque hex. (st7789.h defines an identical
// macro; guard so including both is warning-free regardless of order.)
#ifndef RGB565
#define RGB565(r, g, b) ((uint16_t)((((uint16_t)(r) & 0xF8) << 8) | \
                                    (((uint16_t)(g) & 0xFC) << 3) | \
                                    (((uint16_t)(b)) >> 3)))
#endif

// Optional status-LED behavior owned by a theme. Status/error/battery states
// still take priority; this only changes the normal LED_IDLE presentation.
typedef enum {
    THEME_LED_STATIC = 0,      // idle LED follows the theme accent
    THEME_LED_TRANS_CYCLE,     // blue -> pink -> white -> pink -> blue
    THEME_LED_DMG_CYCLE,       // four classic LCD-green shades
    THEME_LED_NES_CYCLE,       // red -> gray -> white -> gray -> red
    THEME_LED_ARCADE_CYCLE,    // cyan -> magenta -> yellow -> green -> cyan
    THEME_LED_PICO_CYCLE,      // raspberry -> white -> green -> white -> raspberry
} theme_led_mode_t;

// Semantic colour roles. The UI chrome asks the active theme for colours by
// ROLE, never by literal, so adding/altering a look is just a table entry.
typedef struct {
    const char *name;     // shown in Settings > Theme
    uint16_t bg;          // content background
    uint16_t header_bg;   // title bar fill
    uint16_t header_fg;   // title text
    uint16_t accent;      // brand highlight: header underline, selection, indicators
    uint16_t footer_bg;   // hint bar fill
    uint16_t footer_fg;   // hint text
    uint16_t item_fg;     // unselected row text (drawn on bg)
    uint16_t sel_bg;      // selected row fill
    uint16_t sel_fg;      // selected row text
    uint16_t ok;          // success / "good" status
    uint16_t warn;        // caution status
    uint16_t err;         // error status
    theme_led_mode_t led_mode; // optional idle-LED animation; omitted entries default static
} theme_t;

// Active theme: a pointer into the built-in table (no copy, ~0 RAM cost).
// Valid from program start (defaults to the first entry) so any UI drawn before
// settings load is still themed.
extern const theme_t *g_theme;

int         theme_count(void);
const char *theme_name(int i);
int         theme_index(void);      // currently-selected index
void        theme_select(int i);    // clamps to range; sets g_theme