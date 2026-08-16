#include "diag.h"
#include "st7789.h"
#include "theme.h"
#include "ui.h"
#include "buttons.h"
#include "ws2812.h"
#include "led.h"
#include "audio.h"
#include "sd.h"
#include "pins.h"
#include "pico/stdlib.h"
#include <stdio.h>

#define CONTENT_Y 44

// ---- Display diagnostic ----
void diag_display(void) {
    const uint16_t cols[]  = {COL_RED, COL_GREEN, COL_BLUE, COL_WHITE, COL_BLACK, COL_PURPLE};
    const char *const nm[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK", "PURPLE"};
    const int n = 6;
    int idx = 0;
    bool redraw = true;

    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) return;
        if (ev & (1u << BTN_RIGHT)) { idx = (idx + 1) % n; redraw = true; }
        if (ev & (1u << BTN_LEFT))  { idx = (idx + n - 1) % n; redraw = true; }

        if (redraw) {
            st7789_fill(cols[idx]);
            uint16_t fg = (cols[idx] == COL_WHITE) ? COL_BLACK : COL_WHITE;
            ui_header("Display diag");
            st7789_draw_string(12, 90, nm[idx], fg, cols[idx], 3);
            ui_footer("LEFT/RIGHT color   B back");
            redraw = false;
        }
        sleep_ms(15);
    }
}

// ---- Button diagnostic ----
#define BW 74
#define BH 28
static const int box_x[NUM_BUTTONS] = {  8, 86,164,242,   8, 86,164,242,   8 };
static const int box_y[NUM_BUTTONS] = { 60, 60, 60, 60,  96, 96, 96, 96, 132 };

static void draw_button(int i, bool pressed) {
    uint16_t bg = pressed ? COL_GREEN : COL_GRAY;
    uint16_t fg = pressed ? COL_BLACK : COL_WHITE;
    st7789_fill_rect(box_x[i], box_y[i], BW, BH, bg);
    st7789_draw_string(box_x[i] + 5, box_y[i] + (BH - 8) / 2, button_names[i], fg, bg, 1);
}

void diag_buttons(void) {
    st7789_fill(g_theme->bg);
    ui_header("Button diag");
    for (int i = 0; i < NUM_BUTTONS; i++) draw_button(i, false);
    ui_footer("START+SELECT to exit");

    uint16_t last = 0;
    while (true) {
        buttons_update();
        uint16_t st = buttons_state();
        if ((st & (1u << BTN_START)) && (st & (1u << BTN_SELECT))) return;
        if (st != last) {
            for (int i = 0; i < NUM_BUTTONS; i++) {
                bool now = st & (1u << i), was = last & (1u << i);
                if (now != was) draw_button(i, now);
            }
            last = st;
        }
        sleep_ms(15);
    }
}

// ---- RGB diagnostic ----
void diag_rgb(void) {
    struct { const char *n; uint8_t r, g, b; } cols[] = {
        {"RED", 60, 0, 0}, {"GREEN", 0, 60, 0}, {"BLUE", 0, 0, 60},
        {"WHITE", 50, 50, 50}, {"PURPLE", 21, 11, 31}, {"OFF", 0, 0, 0}
    };
    const int n = 6;
    int idx = 0;
    bool redraw = true;
    st7789_fill(g_theme->bg);

    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) { ws2812_set(21, 11, 31); return; }
        if (ev & (1u << BTN_RIGHT)) { idx = (idx + 1) % n; redraw = true; }
        if (ev & (1u << BTN_LEFT))  { idx = (idx + n - 1) % n; redraw = true; }

        if (redraw) {
            ws2812_set(cols[idx].r, cols[idx].g, cols[idx].b);
            ui_header("RGB diag");
            st7789_fill_rect(0, CONTENT_Y, LCD_W, 60, g_theme->bg);
            st7789_draw_string(12, 90, "LED:", COL_WHITE, g_theme->bg, 2);
            st7789_draw_string(84, 90, cols[idx].n, COL_WHITE, g_theme->bg, 2);
            ui_footer("LEFT/RIGHT color   B back");
            redraw = false;
        }
        sleep_ms(15);
    }
}

// ---- Test tone (stub) ----
void diag_tone(void) {
    snd_init();
    st7789_fill(g_theme->bg);
    ui_header("Test tone");
    st7789_draw_string(12, 60,  "I2S -> MAX98357", COL_WHITE, g_theme->bg, 1);
    st7789_draw_string(12, 80,  "A = play 440 Hz (1s)", COL_GRAY, g_theme->bg, 1);
    ui_footer("A play   B back");

    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) return;
        if (ev & (1u << BTN_A)) {
            st7789_fill_rect(12, 110, 300, 12, g_theme->bg);
            st7789_draw_string(12, 110, "playing...", COL_YELLOW, g_theme->bg, 1);
            bool ok = snd_tone(440, 1000);
            st7789_fill_rect(12, 110, 300, 12, g_theme->bg);
            st7789_draw_string(12, 110, ok ? "done" : "audio init failed",
                               COL_YELLOW, g_theme->bg, 1);
        }
        sleep_ms(15);
    }
}

// ---- SD read (stub) ----
void diag_sd(void) {
    st7789_fill(g_theme->bg);
    ui_header("SD probe");
    st7789_draw_string(12, 60, "Raw SPI card init", COL_WHITE, g_theme->bg, 1);
    st7789_draw_string(12, 80, "A = probe card",    COL_GRAY,  g_theme->bg, 1);
    ui_footer("A probe   B back");

    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) { ws2812_set(21, 11, 31); return; }
        if (ev & (1u << BTN_A)) {
            ws2812_set(40, 5, 25);  // pink: SD op in progress
            st7789_fill_rect(12, 110, 300, 12, g_theme->bg);
            st7789_draw_string(12, 110, "probing...", COL_YELLOW, g_theme->bg, 1);

            sd_info_t info;
            bool ok = sd_init(&info);

            st7789_fill_rect(12, 110, 300, 12, g_theme->bg);
            if (ok) {
                ws2812_set(0, 40, 0);  // green: pass
                const char *t = info.type == SD_V2_SDHC ? "SDHC/SDXC" :
                                info.type == SD_V2_SDSC ? "SDSC v2"   :
                                info.type == SD_V1      ? "SDSC v1"   : "?";
                char line[40];
                if (info.mb >= 1024)
                    snprintf(line, sizeof line, "OK: %s  %lu GB", t, (unsigned long)(info.mb / 1024));
                else
                    snprintf(line, sizeof line, "OK: %s  %lu MB", t, (unsigned long)info.mb);
                st7789_draw_string(12, 110, line, COL_GREEN, g_theme->bg, 1);
            } else {
                ws2812_set(40, 0, 0);  // red: fail
                st7789_draw_string(12, 110, "no card / init failed", COL_RED, g_theme->bg, 1);
            }
        }
        sleep_ms(15);
    }
}

// Diagnostics submenu (lives under Settings). The LED animator is paused here
// so the LED-driving tests (RGB LED, SD probe) can own the LED directly.
void diagnostics_menu(void) {
    static const char *const items[] = { "Display", "Buttons", "Test Tone", "SD Read", "RGB LED" };
    menu_t m = { "Diagnostics", items, 5, 0 };
    led_pause();
    ui_draw_menu(&m);
    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) { led_resume(); return; }
        if ((ev & (1u << BTN_UP))   && ui_menu_move(&m, -1)) ui_draw_menu(&m);
        if ((ev & (1u << BTN_DOWN)) && ui_menu_move(&m,  1)) ui_draw_menu(&m);
        if (ev & (1u << BTN_A)) {
            switch (m.sel) {
                case 0: diag_display(); break;
                case 1: diag_buttons(); break;
                case 2: diag_tone();    break;
                case 3: diag_sd();      break;
                case 4: diag_rgb();     break;
            }
            ui_draw_menu(&m);
        }
        sleep_ms(15);
    }
}