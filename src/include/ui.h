#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *title;
    const char *const *items;
    int count;
    int sel;
    bool logo;   // draw the PicoBoy logo mark in the header (root menu); added last
} menu_t;

// Lightweight directional page transition. This is intentionally a themed wipe,
// not a framebuffer slide: it preserves the "deeper = left / back = right" spatial
// cue without spending the emulator arena or doing panel readback.
typedef enum {
    UI_TRANSITION_FORWARD = 0,   // enter/deeper: new page conceptually arrives from the right
    UI_TRANSITION_BACK,          // back/up: return toward the right
} ui_transition_t;

// Common chrome.
void ui_header(const char *title);
void ui_header_right(const char *title, const char *right);  // title + right-aligned text
void ui_footer(const char *hint);

// Battery badge (icon + %) at the header top-right; drawn by the header fns so
// it rides along on every screen. Returns the x of its left edge so a caller can
// place other right-aligned content to its left (LCD_W-8 when it drew nothing).
int ui_battery_badge(void);

// Fill a rect with theme bg plus a faint accent dot-grid for subtle depth.
void ui_fill_bg(int x, int y, int w, int h);

// Filled rounded-corner bar (selection pill). Corners are trimmed to the active
// theme background, so call it over theme bg.
void ui_fill_pill(int x, int y, int w, int h, uint16_t c);

// Fast themed directional wipe used between hierarchy levels. It deliberately
// does not animate selection changes; navigation stays immediate.
void ui_transition(ui_transition_t dir);

// Pac-Man-style progress bar (theme-aware). Call repeatedly with 0..100 during
// a long op; it draws a pellet track with Pac-Man chomping toward completion.
void ui_progress_pacman(int pct, int total);

// Full menu render (header + items + footer).
void ui_draw_menu(const menu_t *m);

// Move selection by delta with wrap; returns true if it changed.
bool ui_menu_move(menu_t *m, int delta);