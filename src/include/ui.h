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

// Common chrome.
void ui_header(const char *title);
void ui_header_right(const char *title, const char *right);  // title + right-aligned text
void ui_footer(const char *hint);

// Small battery badge (icon + %) at the header top-right. Reads the battery
// module; draws nothing when there is no cell/reading (breadboard).
int ui_battery_badge(void);

// Fill a rect with theme bg plus a faint accent dot-grid (subtle depth).
void ui_fill_bg(int x, int y, int w, int h);

// Filled rounded-corner bar (selection pill). Corners are trimmed to the active
// theme background, so call it over theme bg.
void ui_fill_pill(int x, int y, int w, int h, uint16_t c);

// Full menu render (header + items + footer).
void ui_draw_menu(const menu_t *m);

// Move selection by delta with wrap; returns true if it changed.
bool ui_menu_move(menu_t *m, int delta);