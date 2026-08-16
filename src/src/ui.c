#include "ui.h"
#include "st7789.h"
#include "pins.h"
#include "theme.h"
#include "battery.h"
#include <stdio.h>
#include <string.h>

#define HDR_H 30
#define FTR_H 18
#define ROW_H 26
#define LIST_Y (HDR_H + 10)
#define GLYPH_W 8           // font8x8 advance is 8*scale px/char

// Blend two RGB565 colours: a + (b-a)*num/den.
static uint16_t mix565(uint16_t a, uint16_t b, int num, int den) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + ((br - ar) * num) / den;
    int g = ag + ((bg - ag) * num) / den;
    int bl = ab + ((bb - ab) * num) / den;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Filled bar with the 4 corners trimmed back to a given bg -> a subtle rounded pill.
static void pill_trim(int x, int y, int w, int h, uint16_t c, uint16_t bg) {
    st7789_fill_rect(x, y, w, h, c);
    st7789_fill_rect(x,         y,         2, 2, bg);
    st7789_fill_rect(x + w - 2, y,         2, 2, bg);
    st7789_fill_rect(x,         y + h - 2, 2, 2, bg);
    st7789_fill_rect(x + w - 2, y + h - 2, 2, 2, bg);
}

// Selection pill: corners trim to theme bg. Call over theme bg.
void ui_fill_pill(int x, int y, int w, int h, uint16_t c) {
    pill_trim(x, y, w, h, c, g_theme->bg);
}

// Theme bg plus a faint accent dot-grid for subtle depth behind menu content.
void ui_fill_bg(int x, int y, int w, int h) {
    st7789_fill_rect(x, y, w, h, g_theme->bg);
    uint16_t dot = mix565(g_theme->bg, g_theme->accent, 13, 100);  // ~13% accent
    for (int yy = y + 6; yy < y + h; yy += 14)
        for (int xx = x + 8; xx < x + w - 1; xx += 14)
            st7789_fill_rect(xx, yy, 2, 2, dot);
}

void ui_header_right(const char *title, const char *right) {
    st7789_fill_rect(0, 0, LCD_W, HDR_H, g_theme->header_bg);
    st7789_draw_string(8, (HDR_H - 16) / 2, title, g_theme->header_fg, g_theme->header_bg, 2);
    int right_x = ui_battery_badge();             // badge owns the top-right corner
    if (right && right[0]) {
        int w = (int)strlen(right) * GLYPH_W;     // size 1
        st7789_draw_string(right_x - 6 - w, (HDR_H - 8) / 2, right,
                           g_theme->header_fg, g_theme->header_bg, 1);
    }
    st7789_fill_rect(0, HDR_H - 2, LCD_W, 2, g_theme->accent);   // accent underline
}

void ui_header(const char *title) {
    ui_header_right(title, 0);
}

// Small brand mark: accent rounded square with a knocked-out "play" triangle.
static void draw_logo(int x, int y) {
    pill_trim(x, y, 16, 16, g_theme->accent, g_theme->header_bg);
    for (int row = 0; row < 10; row++) {
        int dy = row < 5 ? row : 9 - row;          // 0..4..0
        int w  = 1 + dy * 7 / 4;                    // 1..8 -> right-pointing
        st7789_fill_rect(x + 4, y + 3 + row, w, 1, g_theme->header_bg);
    }
}

// Header with the brand mark left of the title (root menu).
static void ui_header_logo(const char *title) {
    st7789_fill_rect(0, 0, LCD_W, HDR_H, g_theme->header_bg);
    draw_logo(8, (HDR_H - 16) / 2);
    st7789_draw_string(8 + 16 + 6, (HDR_H - 16) / 2, title,
                       g_theme->header_fg, g_theme->header_bg, 2);
    ui_battery_badge();
    st7789_fill_rect(0, HDR_H - 2, LCD_W, 2, g_theme->accent);
}

// Small accent keycap with a button letter, corners trimmed to footer bg.
static void footer_keycap(int x, int cy, char letter) {
    int w = 12, h = 13, y = cy - h / 2;
    pill_trim(x, y, w, h, g_theme->accent, g_theme->footer_bg);
    char s[2] = { letter, 0 };
    st7789_draw_string(x + (w - 8) / 2, y + (h - 8) / 2, s,
                       g_theme->footer_bg, g_theme->accent, 1);
}

void ui_footer(const char *hint) {
    st7789_fill_rect(0, LCD_H - FTR_H, LCD_W, FTR_H, g_theme->footer_bg);
    int cy = LCD_H - FTR_H + FTR_H / 2;
    int ty = cy - 4;                       // 8px text vertical centre
    int x = 8;
    const char *p = hint;
    char tok[40];
    while (*p) {
        while (*p == ' ') { x += GLYPH_W; p++; }       // spaces advance like text
        if (!*p) break;
        int n = 0;
        while (p[n] && p[n] != ' ' && n < (int)sizeof(tok) - 1) n++;
        if (n == 1 && (p[0] == 'A' || p[0] == 'B')) {   // lone A/B -> keycap
            footer_keycap(x, cy, p[0]);
            x += 12 + 2;
        } else {
            for (int i = 0; i < n; i++) tok[i] = p[i];
            tok[n] = 0;
            st7789_draw_string(x, ty, tok, g_theme->footer_fg, g_theme->footer_bg, 1);
            x += n * GLYPH_W;
        }
        p += n;
    }
}


// Battery icon in the header top-right: outline + proportional fill + NN%. Fill
// colour tracks state via theme roles (ok/warn/err/accent). Skipped when there
// is no reading and we are not on a charger (i.e. the breadboard build).
int ui_battery_badge(void) {
    if (battery_millivolts() == 0) return LCD_W - 8;  // nothing drawn; full right margin free
    int      pct      = battery_percent();
    bool     charging = battery_is_charging();
    bool     full     = battery_is_full();

    const int bw = 22, bh = 12, nub_w = 2, nub_h = 6;
    int x = LCD_W - 8 - (bw + nub_w);            // body left edge (8px right margin)
    int y = (HDR_H - 2 - bh) / 2;                // centred above the accent underline

    uint16_t hb = g_theme->header_bg, fg = g_theme->header_fg;
    uint16_t fill = charging || full ? g_theme->ok
                  : pct <= 15        ? g_theme->err
                  : pct <= 30        ? g_theme->warn
                  :                    g_theme->accent;

    // Percentage readout, right-aligned just left of the icon.
    char s[8];
    snprintf(s, sizeof s, "%d%%", pct);
    int tw = (int)strlen(s) * GLYPH_W;
    st7789_fill_rect(x - 6 - tw, 0, tw + 6, HDR_H - 2, hb);   // clear stale text
    st7789_draw_string(x - 6 - tw, (HDR_H - 2 - 8) / 2, s, fg, hb, 1);

    // Body outline (2px border) + terminal nub.
    st7789_fill_rect(x, y, bw, bh, fg);
    st7789_fill_rect(x + 2, y + 2, bw - 4, bh - 4, hb);       // hollow interior
    st7789_fill_rect(x + bw, y + (bh - nub_h) / 2, nub_w, nub_h, fg);

    // Proportional fill inside the interior (leaves a 1px gap to the border).
    int inner = bw - 6;                          // 2px border + 1px gap each side
    int w = inner * pct / 100;
    if (w > 0) st7789_fill_rect(x + 3, y + 3, w, bh - 6, fill);

    // Charging: a small accent tick on the nub end as a "plugged in" cue.
    if (charging) st7789_fill_rect(x + bw, y + (bh - nub_h) / 2, nub_w, nub_h, g_theme->ok);

    return x - 6 - tw;                           // left edge of everything the badge drew
}
void ui_draw_menu(const menu_t *m) {
    ui_fill_bg(0, HDR_H, LCD_W, LCD_H - HDR_H - FTR_H);
    if (m->logo) ui_header_logo(m->title);
    else         ui_header(m->title);
    for (int i = 0; i < m->count; i++) {
        int y = LIST_Y + i * ROW_H;
        bool selected = (i == m->sel);
        uint16_t bg = selected ? g_theme->sel_bg : g_theme->bg;
        uint16_t fg = selected ? g_theme->sel_fg : g_theme->item_fg;
        if (selected) ui_fill_pill(8, y - 4, LCD_W - 16, ROW_H - 4, bg);
        char line[96];
        snprintf(line, sizeof line, "%s %s", selected ? ">" : " ", m->items[i]);
        st7789_draw_string(16, y + 2, line, fg, bg, 2);
    }
    ui_footer("UP/DOWN move   A select   B back");
}

bool ui_menu_move(menu_t *m, int delta) {
    if (m->count <= 0) return false;
    int old = m->sel;
    m->sel = (m->sel + delta + m->count) % m->count;
    return m->sel != old;
}