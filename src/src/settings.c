#include "settings.h"
#include "st7789.h"
#include "ws2812.h"
#include "led.h"
#include "flash.h"
#include "buttons.h"
#include "ui.h"
#include "diag.h"
#include "theme.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

settings_t g_settings;

// ---------- NVS (top 4 KB sector) ----------
#define NVS_MAGIC   0x59424F50u   // 'PBOY'
#define NVS_VERSION 7u

typedef struct {
    uint32_t   magic;
    uint16_t   version;
    uint16_t   size;       // sizeof(settings_t) — guards layout changes
    settings_t s;
} nvs_hdr_t;

// Previous v6 layout: both GG and NES display preferences, before USB SD Access.
typedef struct {
    uint8_t lcd_brightness;
    uint8_t led_brightness;
    uint8_t volume;
    uint8_t palette;
    int8_t  frameskip;
    uint8_t theme;
    uint8_t screensaver;
    uint8_t gg_scaled;
    uint8_t nes_scaled;
} settings_v6_t;

typedef struct {
    uint32_t      magic;
    uint16_t      version;
    uint16_t      size;
    settings_v6_t s;
} nvs_hdr_v6_t;

// Previous v5 layout. Preserve it so 1.0.4 development builds carrying the
// persistent Game Gear display preference migrate cleanly to the NES preference.
typedef struct {
    uint8_t lcd_brightness;
    uint8_t led_brightness;
    uint8_t volume;
    uint8_t palette;
    int8_t  frameskip;
    uint8_t theme;
    uint8_t screensaver;
    uint8_t gg_scaled;
} settings_v5_t;

typedef struct {
    uint32_t      magic;
    uint16_t      version;
    uint16_t      size;
    settings_v5_t s;
} nvs_hdr_v5_t;

// Pre-GG display layout, retained so users can also jump directly from v4.
typedef struct {
    uint8_t lcd_brightness;
    uint8_t led_brightness;
    uint8_t volume;
    uint8_t palette;
    int8_t  frameskip;
    uint8_t theme;
    uint8_t screensaver;
} settings_v4_t;

typedef struct {
    uint32_t      magic;
    uint16_t      version;
    uint16_t      size;
    settings_v4_t s;
} nvs_hdr_v4_t;

static bool s_dirty = false;

static void settings_defaults(void) {
    g_settings.lcd_brightness = 80;
    g_settings.led_brightness = 60;
    g_settings.volume         = 70;
    g_settings.palette        = 0;    // auto per-game color
    g_settings.frameskip      = -1;   // auto
    g_settings.theme          = 0;    // first built-in theme (Purple)
    g_settings.screensaver    = 1;    // idle animation on by default
    g_settings.gg_scaled      = 0;    // Game Gear defaults to Native
    g_settings.nes_scaled     = 0;    // NES defaults to Native
    g_settings.usb_sd_access  = 0;    // USB is charge-only unless explicitly enabled
}

void settings_init(void) {
    const nvs_hdr_t *h = (const nvs_hdr_t *)flash_ptr(NVS_FLASH_OFFSET);

    if (h->magic == NVS_MAGIC &&
        h->version == NVS_VERSION &&
        h->size == sizeof(settings_t)) {
        g_settings = h->s;
    } else {
        const nvs_hdr_v6_t *v6 =
            (const nvs_hdr_v6_t *)flash_ptr(NVS_FLASH_OFFSET);
        const nvs_hdr_v5_t *v5 =
            (const nvs_hdr_v5_t *)flash_ptr(NVS_FLASH_OFFSET);
        const nvs_hdr_v4_t *v4 =
            (const nvs_hdr_v4_t *)flash_ptr(NVS_FLASH_OFFSET);

        if (v6->magic == NVS_MAGIC &&
            v6->version == 6u &&
            v6->size == sizeof(settings_v6_t)) {
            // Preserve the already-validated GG/NES presentation settings.
            g_settings.lcd_brightness = v6->s.lcd_brightness;
            g_settings.led_brightness = v6->s.led_brightness;
            g_settings.volume         = v6->s.volume;
            g_settings.palette        = v6->s.palette;
            g_settings.frameskip      = v6->s.frameskip;
            g_settings.theme          = v6->s.theme;
            g_settings.screensaver    = v6->s.screensaver;
            g_settings.gg_scaled      = v6->s.gg_scaled;
            g_settings.nes_scaled     = v6->s.nes_scaled;
            g_settings.usb_sd_access  = 0;

        } else if (v5->magic == NVS_MAGIC &&
                   v5->version == 5u &&
                   v5->size == sizeof(settings_v5_t)) {
            g_settings.lcd_brightness = v5->s.lcd_brightness;
            g_settings.led_brightness = v5->s.led_brightness;
            g_settings.volume         = v5->s.volume;
            g_settings.palette        = v5->s.palette;
            g_settings.frameskip      = v5->s.frameskip;
            g_settings.theme          = v5->s.theme;
            g_settings.screensaver    = v5->s.screensaver;
            g_settings.gg_scaled      = v5->s.gg_scaled;
            g_settings.nes_scaled     = 0;
            g_settings.usb_sd_access  = 0;

        } else if (v4->magic == NVS_MAGIC &&
                   v4->version == 4u &&
                   v4->size == sizeof(settings_v4_t)) {
            g_settings.lcd_brightness = v4->s.lcd_brightness;
            g_settings.led_brightness = v4->s.led_brightness;
            g_settings.volume         = v4->s.volume;
            g_settings.palette        = v4->s.palette;
            g_settings.frameskip      = v4->s.frameskip;
            g_settings.theme          = v4->s.theme;
            g_settings.screensaver    = v4->s.screensaver;
            g_settings.gg_scaled      = 0;
            g_settings.nes_scaled     = 0;
            g_settings.usb_sd_access  = 0;
        } else {
            settings_defaults();
        }
    }

    // Clamp in case of a corrupt/partial write.
    if (g_settings.lcd_brightness < 10)  g_settings.lcd_brightness = 10;
    if (g_settings.lcd_brightness > 100) g_settings.lcd_brightness = 100;
    if (g_settings.led_brightness > 100) g_settings.led_brightness = 100;
    if (g_settings.volume > 100)         g_settings.volume = 100;
    if (g_settings.palette > 13)         g_settings.palette = 0;
    if (g_settings.frameskip < -1)       g_settings.frameskip = -1;
    if (g_settings.frameskip > 5)        g_settings.frameskip = 5;
    if (g_settings.screensaver > 1)      g_settings.screensaver = 1;
    if (g_settings.gg_scaled > 1)        g_settings.gg_scaled = 0;
    if (g_settings.nes_scaled > 1)       g_settings.nes_scaled = 0;
    if (g_settings.usb_sd_access > 1)    g_settings.usb_sd_access = 0;

    if (g_settings.theme >= (uint8_t)theme_count()) g_settings.theme = 0;
    theme_select(g_settings.theme);
    led_set_idle_rgb565(g_theme->accent);
    led_set_idle_theme_mode(g_theme->led_mode);
}

void settings_save(void) {
    // 4-byte aligned so the nvs_hdr_t overlay's uint32 fields are aligned (M0+ faults on unaligned word access).
    static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(page, 0xFF, sizeof page);
    nvs_hdr_t *h = (nvs_hdr_t *)page;
    h->magic   = NVS_MAGIC;
    h->version = NVS_VERSION;
    h->size    = (uint16_t)sizeof(settings_t);
    h->s       = g_settings;
    flash_erase_sector(NVS_FLASH_OFFSET);
    flash_program_page(NVS_FLASH_OFFSET, page);
}

void settings_apply(void) {
    // Keep the UI theme and its idle-LED behavior in one apply path. This also
    // means Reset Settings immediately restores the default theme/LED behavior.
    theme_select(g_settings.theme);
    led_set_idle_rgb565(g_theme->accent);
    led_set_idle_theme_mode(g_theme->led_mode);
    st7789_backlight_level(g_settings.lcd_brightness);
    ws2812_set_brightness(g_settings.led_brightness);  // animator re-applies colour at this level
}

// ---------- Flash Ops ----------
static bool confirm(const char *title, const char *prompt) {
    st7789_fill(COL_BLACK);
    ui_header(title);
    st7789_draw_string(12, 74, prompt, COL_WHITE, COL_BLACK, 1);
    ui_footer("A confirm   B cancel");
    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) return false;
        if (ev & (1u << BTN_A)) return true;
        sleep_ms(15);
    }
}

// Erase the whole ROM window, one sector at a time (IRQs serviced between them).
static void clear_rom(void) {
    led_set_state(LED_FLASH_MAINT);   // cyan
    st7789_fill(COL_BLACK);
    ui_header("Clear ROM");
    ui_footer("");

    const uint32_t start = ROM_FLASH_OFFSET, end = NVS_FLASH_OFFSET;
    const uint32_t total = (end - start) / FLASH_SECTOR_SIZE;
    uint32_t done = 0;
    for (uint32_t off = start; off < end; off += FLASH_SECTOR_SIZE) {
        flash_erase_sector(off);
        if ((++done & 0x0F) == 0 || done == total) {
            char line[24];
            snprintf(line, sizeof line, "Erasing... %lu%%", (unsigned long)(done * 100u / total));
            st7789_fill_rect(12, 90, 300, 14, COL_BLACK);
            st7789_draw_string(12, 90, line, COL_WHITE, COL_BLACK, 1);
        }
    }
    st7789_fill_rect(12, 90, 300, 14, COL_BLACK);
    st7789_draw_string(12, 90, "ROM window cleared", COL_GREEN, COL_BLACK, 1);
    led_set_state(LED_IDLE);
    sleep_ms(900);
}

static void reset_settings(void) {
    led_set_state(LED_FLASH_MAINT);   // cyan
    settings_defaults();
    settings_apply();
    settings_save();
    s_dirty = false;
    st7789_fill(COL_BLACK);
    ui_header("Reset Settings");
    st7789_draw_string(12, 90, "Defaults restored", COL_GREEN, COL_BLACK, 1);
    ui_footer("B back");
    led_set_state(LED_IDLE);
    sleep_ms(900);
}

static void flash_ops_menu(void) {
    static const char *const items[] = { "Clear ROM", "Reset Settings" };
    menu_t m = { "Flash Ops", items, 2, 0 };
    ui_draw_menu(&m);
    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) return;
        if ((ev & (1u << BTN_UP))   && ui_menu_move(&m, -1)) ui_draw_menu(&m);
        if ((ev & (1u << BTN_DOWN)) && ui_menu_move(&m,  1)) ui_draw_menu(&m);
        if (ev & (1u << BTN_A)) {
            if (m.sel == 0) { if (confirm("Clear ROM",      "Erase the ROM window?")) clear_rom(); }
            else            { if (confirm("Reset Settings", "Restore defaults?"))     reset_settings(); }
            ui_draw_menu(&m);
        }
        sleep_ms(15);
    }
}

// ---------- Settings menu (mixed value + action items) ----------
typedef enum { IT_VALUE, IT_ACTION, IT_CHOICE, IT_TOGGLE } itk_t;
typedef struct {
    const char *label;
    itk_t       kind;
    uint8_t    *val;            // IT_VALUE
    uint8_t     vmin;           // IT_VALUE
    void      (*action)(void);  // IT_ACTION
} item_t;

#define N_ITEMS 8
static item_t items[N_ITEMS];

static void build_items(void) {
    items[0] = (item_t){ "LCD Bright",  IT_VALUE,  &g_settings.lcd_brightness, 10, NULL };
    items[1] = (item_t){ "LED Bright",  IT_VALUE,  &g_settings.led_brightness, 0,  NULL };
    items[2] = (item_t){ "Volume",      IT_VALUE,  &g_settings.volume,         0,  NULL };
    items[3] = (item_t){ "Theme",       IT_CHOICE, &g_settings.theme,          0,  NULL };
    items[4] = (item_t){ "Screensaver",   IT_TOGGLE, &g_settings.screensaver,   0, NULL };
    items[5] = (item_t){ "USB SD Access", IT_TOGGLE, &g_settings.usb_sd_access, 0, NULL };
    items[6] = (item_t){ "Flash Ops",     IT_ACTION, NULL, 0, flash_ops_menu };
    items[7] = (item_t){ "Diagnostics",   IT_ACTION, NULL, 0, diagnostics_menu };
}

static void draw(int sel) {
    st7789_fill(g_theme->bg);
    ui_header("Settings");
    for (int i = 0; i < N_ITEMS; i++) {
        bool on = (i == sel);
        uint16_t fg = on ? g_theme->sel_fg : g_theme->item_fg;
        uint16_t bg = on ? g_theme->sel_bg : g_theme->bg;
        if (on) ui_fill_pill(8, 46 + i * 22, 304, 18, bg);
        char body[40], line[44];
        if (items[i].kind == IT_VALUE)
            snprintf(body, sizeof body, "%-13s %3u", items[i].label, (unsigned)*items[i].val);
        else if (items[i].kind == IT_CHOICE)
            snprintf(body, sizeof body, "%-13s %s", items[i].label, theme_name((int)*items[i].val));
        else if (items[i].kind == IT_TOGGLE)
            snprintf(body, sizeof body, "%-13s %s", items[i].label, *items[i].val ? "On" : "Off");
        else
            snprintf(body, sizeof body, "%-13s   >", items[i].label);
        snprintf(line, sizeof line, "%c %s", on ? '>' : ' ', body);
        st7789_draw_string(14, 46 + i * 22 + 6, line, fg, bg, 1);
    }
    ui_footer("U/D pick  L/R adj  A open  B back");
}

void settings_menu(void) {
    build_items();
    int sel = 0;
    draw(sel);
    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) { if (s_dirty) { settings_save(); s_dirty = false; } return; }

        bool moved = false, changed = false;
        if (ev & (1u << BTN_UP))   { sel = (sel + N_ITEMS - 1) % N_ITEMS; moved = true; }
        if (ev & (1u << BTN_DOWN)) { sel = (sel + 1) % N_ITEMS;           moved = true; }

        item_t *it = &items[sel];
        if (it->kind == IT_ACTION) {
            if (ev & (1u << BTN_A)) { it->action(); draw(sel); continue; }
        } else if (it->kind == IT_CHOICE) {
            int n = theme_count(), cur = (int)*it->val, nv = cur;
            if (ev & (1u << BTN_LEFT))  nv = (cur - 1 + n) % n;
            if (ev & (1u << BTN_RIGHT)) nv = (cur + 1) % n;
            if (nv != cur) { *it->val = (uint8_t)nv; changed = true; }
        } else if (it->kind == IT_TOGGLE) {
            if ((ev & (1u << BTN_LEFT))  &&  *it->val) { *it->val = 0; changed = true; }
            if ((ev & (1u << BTN_RIGHT)) && !*it->val) { *it->val = 1; changed = true; }
            if (ev & (1u << BTN_A))                    { *it->val = !*it->val; changed = true; }
        } else { // IT_VALUE
            if (ev & (1u << BTN_LEFT))  { int nv = (int)*it->val - 5; if (nv < it->vmin) nv = it->vmin; *it->val = (uint8_t)nv; changed = true; }
            if (ev & (1u << BTN_RIGHT)) { int nv = (int)*it->val + 5; if (nv > 100)      nv = 100;      *it->val = (uint8_t)nv; changed = true; }
        }

        if (changed) { s_dirty = true; settings_apply(); }
        if (moved || changed) draw(sel);
        sleep_ms(15);
    }
}