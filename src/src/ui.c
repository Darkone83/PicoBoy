#include "ui.h"
#include "st7789.h"
#include "pins.h"
#include "theme.h"
#include "battery.h"
#include "pico/stdlib.h"
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

// Directional page wipe. A true horizontal slide would require either panel
// readback or a full-screen framebuffer; both are poor trades here. This keeps
// the spatial cue (forward comes from the right, back from the left) with only
// panel fills and ~70-100 ms of motion. Selection changes are never animated.
void ui_transition(ui_transition_t dir) {
    enum { STEPS = 8 };
    const int edge_w = 4;
    uint16_t veil = mix565(g_theme->bg, g_theme->accent, 10, 100);

    for (int i = 1; i <= STEPS; i++) {
        int covered = LCD_W * i / STEPS;
        if (dir == UI_TRANSITION_FORWARD) {
            int x = LCD_W - covered;
            st7789_fill_rect(x, 0, covered, LCD_H, veil);
            if (i < STEPS) st7789_fill_rect(x, 0, edge_w, LCD_H, g_theme->accent);
        } else {
            st7789_fill_rect(0, 0, covered, LCD_H, veil);
            if (i < STEPS) st7789_fill_rect(covered - edge_w, 0, edge_w, LCD_H, g_theme->accent);
        }
        sleep_ms(7);
    }
    st7789_fill(g_theme->bg);
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

// Small arrow glyph (^ v < >) drawn in an ~7px cell at (gx, gy).
static void footer_tri(int gx, int gy, char d, uint16_t c) {
    for (int i = 0; i < 4; i++) {
        if      (d == '^') st7789_fill_rect(gx + 3 - i, gy + i,     1 + 2 * i, 1, c);
        else if (d == 'v') st7789_fill_rect(gx + 3 - i, gy + 3 - i, 1 + 2 * i, 1, c);
        else if (d == '<') st7789_fill_rect(gx + i,     gy + 3 - i, 1, 1 + 2 * i, c);
        else if (d == '>') st7789_fill_rect(gx + 3 - i, gy + 3 - i, 1, 1 + 2 * i, c);
    }
}

// Accent keycap showing 1-2 glyphs (letters or arrows). Returns its pixel width.
static int footer_cap(int x, int cy, const char *label) {
    int n = (int)strlen(label);
    int w = n * 8 + 4, h = 13, y = cy - h / 2;
    pill_trim(x, y, w, h, g_theme->accent, g_theme->footer_bg);
    uint16_t fg = g_theme->footer_bg, bg = g_theme->accent;
    int cx = x + 2;
    for (int i = 0; i < n; i++) {
        char c = label[i];
        if (c == '^' || c == 'v' || c == '<' || c == '>')
            footer_tri(cx, y + (h - 7) / 2, c, fg);
        else {
            char s[2] = { c, 0 };
            st7789_draw_string(cx, y + (h - 8) / 2, s, fg, bg, 1);
        }
        cx += 8;
    }
    return w;
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
        int is_cap = (n >= 1 && n <= 2);                 // 1-2 glyphs, all in the set
        for (int i = 0; i < n && is_cap; i++)
            if (!strchr("AB^v<>", p[i])) is_cap = 0;
        if (is_cap) {
            char cap[3] = { p[0], n > 1 ? p[1] : (char)0, 0 };
            x += footer_cap(x, cy, cap) + 2;
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
// Tiny 12x12 pixel-art chomper, drawn 2x for a crisp 24x24 sprite.  Keeping
// this as row masks instead of a mathematically perfect circle makes it read as
// an arcade sprite at a glance.  Frames are wide-open, half-open and closed.
#define PAC_SRC_W 12
#define PAC_SRC_H 12
#define PAC_SCALE 2
#define PAC_W     (PAC_SRC_W * PAC_SCALE)
#define PAC_H     (PAC_SRC_H * PAC_SCALE)

static const uint16_t s_pac_frame[3][PAC_SRC_H] = {
    {   // wide-open
        0x1F8, 0x3FC, 0x7FE, 0xFE0, 0xFC0, 0xF80,
        0xF80, 0xFC0, 0xFE0, 0x7FE, 0x3FC, 0x1F8
    },
    {   // half-open
        0x1F8, 0x3FC, 0x7FE, 0xFFF, 0xFF8, 0xFC0,
        0xFC0, 0xFF8, 0xFFF, 0x7FE, 0x3FC, 0x1F8
    },
    {   // nearly closed -- a thin mouth line remains
        0x1F8, 0x3FC, 0x7FE, 0xFFF, 0xFFF, 0xFF0,
        0xFF0, 0xFFF, 0xFFF, 0x7FE, 0x3FC, 0x1F8
    }
};

static void pac_draw_sprite(int cx, int cy, int frame) {
    const uint16_t pac = COL_YELLOW;     // keep the character instantly recognisable
    const uint16_t eye = COL_BLACK;
    int x0 = cx - PAC_W / 2;
    int y0 = cy - PAC_H / 2;

    frame %= 3;
    for (int sy = 0; sy < PAC_SRC_H; sy++) {
        uint16_t row = s_pac_frame[frame][sy];
        int sx = 0;
        while (sx < PAC_SRC_W) {
            while (sx < PAC_SRC_W && !(row & (1u << (PAC_SRC_W - 1 - sx)))) sx++;
            if (sx >= PAC_SRC_W) break;
            int run = sx;
            while (sx < PAC_SRC_W && (row & (1u << (PAC_SRC_W - 1 - sx)))) sx++;
            st7789_fill_rect(x0 + run * PAC_SCALE, y0 + sy * PAC_SCALE,
                             (sx - run) * PAC_SCALE, PAC_SCALE, pac);
        }
    }

    // One chunky pixel is enough to give the silhouette a face instead of a
    // generic yellow disc.  It stays fixed across all mouth frames.
    st7789_fill_rect(x0 + 6 * PAC_SCALE, y0 + 3 * PAC_SCALE,
                     PAC_SCALE, PAC_SCALE, eye);

    // Tiny Ms. Pac-Man-style easter egg for the Trans theme. The bow uses the
    // theme accent, which is the flag pink in this theme.
    if (g_theme->led_mode == THEME_LED_TRANS_CYCLE) {
        uint16_t bow = g_theme->accent;
        st7789_fill_rect(x0 + 2,  y0 - 4, 6, 4, bow);  // left loop
        st7789_fill_rect(x0 + 10, y0 - 4, 6, 4, bow);  // right loop
        st7789_fill_rect(x0 + 7,  y0 - 3, 4, 4, bow);  // knot
    }
}

static void pac_draw_sparkle(int x, int y, uint16_t c) {
    st7789_fill_rect(x - 3, y,     7, 1, c);
    st7789_fill_rect(x,     y - 3, 1, 7, c);
}

// Arcade-style ROM loading indicator. Progress owns horizontal position while
// elapsed time owns the mouth frame, so a sector taking longer does not lock the
// character to one particular chomp pose. Pellets disappear as they are eaten;
// the centre pellet is deliberately larger as a tiny power-pellet nod.
void ui_progress_pacman(int done, int total) {
    if (total <= 0) total = 1;
    if (done < 0) done = 0;
    if (done > total) done = total;

    const uint16_t bg     = g_theme->bg;
    const uint16_t pellet = g_theme->footer_fg;
    const uint16_t txc    = g_theme->item_fg;
    const int ybar = 118;
    const int x0 = 28;
    const int x1 = 292;
    const int track_y = ybar;
    int px = x0 + (x1 - x0) * done / total;

    // Redraw only the progress strip. This keeps the header/footer intact and
    // makes eaten pellets deterministic even if progress advances in big jumps.
    st7789_fill_rect(0, ybar - 32, LCD_W, 62, bg);

    // Percentage stays visually separate from the sprite track.
    char s[8];
    snprintf(s, sizeof s, "%d%%", done * 100 / total);
    int tw = (int)strlen(s) * GLYPH_W;
    st7789_draw_string((LCD_W - tw) / 2, ybar - 30, s, txc, bg, 1);

    // Pellet lane. Anything at or behind the mouth is considered eaten. The
    // middle pellet is a 7x7 power pellet, the rest are crisp 3x3 pixels.
    const int first_pellet = x0 + 18;
    const int last_pellet  = x1 - 10;
    const int spacing      = 18;
    const int eat_x        = px + PAC_W / 3;
    int centre = (x0 + x1) / 2;
    for (int x = first_pellet; x <= last_pellet; x += spacing) {
        if (x <= eat_x) continue;
        if (x >= centre - spacing / 2 && x <= centre + spacing / 2)
            st7789_fill_rect(x - 3, track_y - 3, 7, 7, pellet);
        else
            st7789_fill_rect(x - 1, track_y - 1, 3, 3, pellet);
    }

    // Wide -> half -> closed -> half, roughly 11 chomps/sec. Position is tied
    // only to progress, so animation cadence and load progress are independent.
    static const uint8_t seq[4] = { 0, 1, 2, 1 };
    uint32_t tick = to_ms_since_boot(get_absolute_time()) / 90u;
    int frame = seq[tick & 3u];
    if (done >= total) frame = 2;                 // finish with the mouth closed
    pac_draw_sprite(px, ybar, frame);

    if (done >= total) {
        // Tiny completion sparkle -- visible immediately, but adds no artificial
        // loading delay before the emulator launches.
        uint16_t c = g_theme->accent;
        pac_draw_sparkle(px - 16, ybar - 15, c);
        pac_draw_sparkle(px - 9,  ybar + 16, c);
    }
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
        if (selected) {
            ui_fill_pill(8, y - 4, LCD_W - 16, ROW_H - 4, bg);
            st7789_fill_rect(11, y - 1, 3, ROW_H - 10, g_theme->sel_fg);  // left accent stripe
        }
        st7789_draw_string(22, y + 2, m->items[i], fg, bg, 2);
    }
    ui_footer("^v move   A select   B back");
}

bool ui_menu_move(menu_t *m, int delta) {
    if (m->count <= 0) return false;
    int old = m->sel;
    m->sel = (m->sel + delta + m->count) % m->count;
    return m->sel != old;
}