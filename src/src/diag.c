#include "diag.h"
#include "st7789.h"
#include "theme.h"
#include "ui.h"
#include "buttons.h"
#include "ws2812.h"
#include "led.h"
#include "audio.h"
#include "sd.h"
#include "battery.h"
#include "settings.h"
#include "version.h"
#include "pins.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include <stdio.h>
#include <string.h>

#define DIAG_TOP     36
#define DIAG_BOTTOM  (LCD_H - 20)

// Local rounded rectangle. Diagnostics use larger purpose-built visuals than the
// normal menu, but retain the same theme roles and 2 px trimmed corners.
static void diag_pill(int x, int y, int w, int h, uint16_t c, uint16_t bg) {
    st7789_fill_rect(x, y, w, h, c);
    st7789_fill_rect(x,         y,         2, 2, bg);
    st7789_fill_rect(x + w - 2, y,         2, 2, bg);
    st7789_fill_rect(x,         y + h - 2, 2, 2, bg);
    st7789_fill_rect(x + w - 2, y + h - 2, 2, 2, bg);
}

static void draw_centered(int y, const char *s, uint16_t fg, uint16_t bg, int scale) {
    int w = (int)strlen(s) * 8 * scale;
    st7789_draw_string((LCD_W - w) / 2, y, s, fg, bg, scale);
}

static int isqrt_i(int v) {
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static void draw_disc(int cx, int cy, int r, uint16_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int hw = isqrt_i(r * r - dy * dy);
        st7789_fill_rect(cx - hw, cy + dy, 2 * hw + 1, 1, c);
    }
}

// While the LED animator is paused, leave the physical LED sitting on the active
// theme rather than whatever diagnostic colour happened to run last.
static void diag_restore_theme_led(void) {
    uint16_t c = g_theme->accent;
    uint8_t r = (uint8_t)(((c >> 11) & 0x1F) << 3);
    uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) << 2);
    uint8_t b = (uint8_t)(( c        & 0x1F) << 3);
    ws2812_set((uint8_t)(r * 60u / 255u),
               (uint8_t)(g * 60u / 255u),
               (uint8_t)(b * 60u / 255u));
}

// ---------------------------------------------------------------------------
// System snapshot
// ---------------------------------------------------------------------------
static void system_draw_values(void) {
    char line[48];
    uint16_t bg = g_theme->bg, fg = g_theme->item_fg, dim = g_theme->footer_fg;
    const int x = 24;

    st7789_fill_rect(16, 68, LCD_W - 32, 132, bg);

    snprintf(line, sizeof line, "Firmware      %s", PICOBOY_VERSION);
    st7789_draw_string(x, 72, line, fg, bg, 1);

    snprintf(line, sizeof line, "CPU clock     %lu MHz",
             (unsigned long)(clock_get_hz(clk_sys) / 1000000u));
    st7789_draw_string(x, 92, line, fg, bg, 1);

    snprintf(line, sizeof line, "LCD SPI       %lu MHz",
             (unsigned long)(LCD_SPI_HZ / 1000000u));
    st7789_draw_string(x, 112, line, fg, bg, 1);

    uint32_t mv = battery_millivolts();
    if (mv) {
        const char *bs = battery_is_full() ? "Full" : battery_is_charging() ? "Charging" : "Battery";
        snprintf(line, sizeof line, "Power         %s %d%%", bs, battery_percent());
        st7789_draw_string(x, 132, line, fg, bg, 1);
        snprintf(line, sizeof line, "VBAT          %lu mV", (unsigned long)mv);
        st7789_draw_string(x, 152, line, dim, bg, 1);
    } else {
        st7789_draw_string(x, 132, "Power         No reading", dim, bg, 1);
    }

    st7789_draw_string(x, 176, sd_ready() ? "SD interface  Ready" : "SD interface  Idle",
                       sd_ready() ? g_theme->ok : dim, bg, 1);
}

void diag_system(void) {
    st7789_fill(g_theme->bg);
    ui_fill_bg(0, DIAG_TOP, LCD_W, DIAG_BOTTOM - DIAG_TOP);
    ui_header("System");
    draw_centered(44, "PicoBoy", g_theme->accent, g_theme->bg, 2);
    system_draw_values();
    ui_footer("B back");

    uint32_t last = to_ms_since_boot(get_absolute_time());
    while (true) {
        buttons_update();
        if (buttons_pressed() & (1u << BTN_B)) return;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last >= 500) {
            system_draw_values();
            last = now;
        }
        sleep_ms(15);
    }
}

// ---------------------------------------------------------------------------
// Display diagnostic
// ---------------------------------------------------------------------------
static const char *const DISP_NAME[] = {
    "Color bars", "White", "Black", "Gradient", "Checker", "Grid"
};
#define N_DISP ((int)(sizeof(DISP_NAME) / sizeof(DISP_NAME[0])))

static void display_pattern(int idx) {
    const int y0 = 30, h = LCD_H - 30 - 18;
    switch (idx) {
        case 0: { // SMPTE-ish bright bars: useful for obvious channel/order faults.
            const uint16_t c[] = {
                COL_WHITE, COL_YELLOW, RGB565(0,255,255), COL_GREEN,
                RGB565(255,0,255), COL_RED, COL_BLUE, COL_BLACK
            };
            for (int i = 0; i < 8; i++)
                st7789_fill_rect(i * (LCD_W / 8), y0, LCD_W / 8, h, c[i]);
            break;
        }
        case 1: st7789_fill_rect(0, y0, LCD_W, h, COL_WHITE); break;
        case 2: st7789_fill_rect(0, y0, LCD_W, h, COL_BLACK); break;
        case 3: // grayscale ramp catches banding / missing data bits without a framebuffer.
            for (int x = 0; x < LCD_W; x += 2) {
                uint8_t v = (uint8_t)(x * 255 / (LCD_W - 1));
                st7789_fill_rect(x, y0, 2, h, RGB565(v, v, v));
            }
            break;
        case 4:
            for (int y = y0; y < y0 + h; y += 16)
                for (int x = 0; x < LCD_W; x += 16)
                    st7789_fill_rect(x, y, 16, 16,
                                     (((x / 16) + ((y - y0) / 16)) & 1) ? COL_WHITE : COL_BLACK);
            break;
        default: // geometry grid / edge visibility
            st7789_fill_rect(0, y0, LCD_W, h, COL_BLACK);
            for (int x = 0; x < LCD_W; x += 20) st7789_fill_rect(x, y0, 1, h, g_theme->accent);
            for (int y = y0; y < y0 + h; y += 20) st7789_fill_rect(0, y, LCD_W, 1, g_theme->accent);
            st7789_fill_rect(LCD_W / 2 - 1, y0, 3, h, g_theme->warn);
            st7789_fill_rect(0, y0 + h / 2 - 1, LCD_W, 3, g_theme->warn);
            break;
    }
    ui_header_right("Display", DISP_NAME[idx]);
    ui_footer("<> pattern   A next   B back");
}

void diag_display(void) {
    int idx = 0;
    display_pattern(idx);
    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) return;
        int old = idx;
        if (ev & (1u << BTN_LEFT))  idx = (idx + N_DISP - 1) % N_DISP;
        if (ev & (1u << BTN_RIGHT)) idx = (idx + 1) % N_DISP;
        if (ev & (1u << BTN_A))     idx = (idx + 1) % N_DISP;
        if (idx != old) display_pattern(idx);
        sleep_ms(15);
    }
}

// ---------------------------------------------------------------------------
// Controls diagnostic -- a small visual model of the handheld itself.
// ---------------------------------------------------------------------------
static void draw_control_rect(int x, int y, int w, int h, const char *label, bool pressed) {
    uint16_t bg = pressed ? g_theme->accent : g_theme->footer_bg;
    uint16_t fg = pressed ? g_theme->sel_fg : g_theme->item_fg;
    diag_pill(x, y, w, h, bg, g_theme->bg);
    int tw = (int)strlen(label) * 8;
    st7789_draw_string(x + (w - tw) / 2, y + (h - 8) / 2, label, fg, bg, 1);
}

static void draw_control_disc(int cx, int cy, int r, const char *label, bool pressed) {
    uint16_t bg = pressed ? g_theme->accent : g_theme->footer_bg;
    uint16_t fg = pressed ? g_theme->sel_fg : g_theme->item_fg;
    draw_disc(cx, cy, r, bg);
    int tw = (int)strlen(label) * 8;
    st7789_draw_string(cx - tw / 2, cy - 4, label, fg, bg, 1);
}

static void draw_button_visual(int i, bool pressed) {
    switch (i) {
        case BTN_UP:     draw_control_rect( 50,  62, 30, 30, "^", pressed); break;
        case BTN_DOWN:   draw_control_rect( 50, 122, 30, 30, "v", pressed); break;
        case BTN_LEFT:   draw_control_rect( 20,  92, 30, 30, "<", pressed); break;
        case BTN_RIGHT:  draw_control_rect( 80,  92, 30, 30, ">", pressed); break;
        case BTN_A:      draw_control_disc(270,  82, 18, "A", pressed); break;
        case BTN_B:      draw_control_disc(228, 116, 18, "B", pressed); break;
        case BTN_START:  draw_control_rect(170, 162, 62, 20, "START", pressed); break;
        case BTN_SELECT: draw_control_rect( 88, 162, 68, 20, "SELECT", pressed); break;
        case BTN_MENU:   draw_control_rect(126, 194, 68, 20, "MENU", pressed); break;
    }
}

void diag_buttons(void) {
    st7789_fill(g_theme->bg);
    ui_fill_bg(0, DIAG_TOP, LCD_W, DIAG_BOTTOM - DIAG_TOP);
    ui_header("Controls");
    st7789_draw_string(20, 42, "Press a control to light it", g_theme->footer_fg, g_theme->bg, 1);
    // D-pad centre makes the four independent contacts read visually as a cross.
    st7789_fill_rect(50, 92, 30, 30, g_theme->footer_bg);
    for (int i = 0; i < NUM_BUTTONS; i++) draw_button_visual(i, false);
    ui_footer("START+SELECT exit");

    uint16_t last = 0;
    while (true) {
        buttons_update();
        uint16_t st = buttons_state();
        if (st != last) {
            for (int i = 0; i < NUM_BUTTONS; i++) {
                bool now = (st & (1u << i)) != 0;
                bool was = (last & (1u << i)) != 0;
                if (now != was) draw_button_visual(i, now);
            }
            last = st;
        }
        if ((st & (1u << BTN_START)) && (st & (1u << BTN_SELECT))) {
            sleep_ms(120); // long enough to visibly confirm both contacts before leaving
            return;
        }
        sleep_ms(15);
    }
}

// ---------------------------------------------------------------------------
// Audio diagnostic
// ---------------------------------------------------------------------------
typedef struct { const char *name; uint32_t hz; } tone_t;
static const tone_t TONES[] = { {"LOW",220}, {"MID",440}, {"HIGH",880} };
#define N_TONES ((int)(sizeof(TONES) / sizeof(TONES[0])))

static void draw_speaker(bool active) {
    uint16_t c = active ? g_theme->warn : g_theme->accent;
    uint16_t bg = g_theme->bg;
    st7789_fill_rect(24, 70, 110, 76, bg);
    st7789_fill_rect(36, 92, 18, 28, c);                  // speaker body
    for (int i = 0; i < 20; i++)                         // cone wedge
        st7789_fill_rect(54 + i, 88 - i / 2, 1, 36 + i, c);
    // Three simple wave bars; active mode spreads them farther.
    for (int i = 0; i < 3; i++) {
        int x = 82 + i * 12;
        int hh = (active ? 18 : 10) + i * 8;
        st7789_fill_rect(x, 106 - hh / 2, 3, hh, c);
    }
}

static void tone_draw(int sel, const char *status, uint16_t status_col) {
    st7789_fill(g_theme->bg);
    ui_fill_bg(0, DIAG_TOP, LCD_W, DIAG_BOTTOM - DIAG_TOP);
    ui_header("Audio");
    draw_speaker(false);
    st7789_draw_string(156, 72, "I2S / MAX98357", g_theme->item_fg, g_theme->bg, 1);
    char line[32];
    snprintf(line, sizeof line, "Volume  %u%%", (unsigned)g_settings.volume);
    st7789_draw_string(156, 92, line, g_theme->footer_fg, g_theme->bg, 1);
    snprintf(line, sizeof line, "%lu Hz", (unsigned long)TONES[sel].hz);
    st7789_draw_string(156, 116, line, g_theme->accent, g_theme->bg, 2);

    for (int i = 0; i < N_TONES; i++) {
        int x = 42 + i * 84;
        bool on = i == sel;
        uint16_t cbg = on ? g_theme->sel_bg : g_theme->footer_bg;
        uint16_t cfg = on ? g_theme->sel_fg : g_theme->item_fg;
        diag_pill(x, 164, 68, 24, cbg, g_theme->bg);
        int tw = (int)strlen(TONES[i].name) * 8;
        st7789_draw_string(x + (68 - tw) / 2, 172, TONES[i].name, cfg, cbg, 1);
    }
    if (status && status[0]) {
        st7789_fill_rect(0, 196, LCD_W, 18, g_theme->bg);
        draw_centered(198, status, status_col, g_theme->bg, 1);
    }
    ui_footer("<> tone   A play   B back");
}

void diag_tone(void) {
    snd_init();
    int sel = 1;
    tone_draw(sel, "", g_theme->item_fg);
    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) return;
        int old = sel;
        if (ev & (1u << BTN_LEFT))  sel = (sel + N_TONES - 1) % N_TONES;
        if (ev & (1u << BTN_RIGHT)) sel = (sel + 1) % N_TONES;
        if (sel != old) tone_draw(sel, "", g_theme->item_fg);
        if (ev & (1u << BTN_A)) {
            char msg[32];
            snprintf(msg, sizeof msg, "Playing %lu Hz", (unsigned long)TONES[sel].hz);
            tone_draw(sel, msg, g_theme->warn);
            draw_speaker(true);
            bool ok = snd_tone(TONES[sel].hz, 750);
            tone_draw(sel, ok ? "Tone sent" : "Audio init failed", ok ? g_theme->ok : g_theme->err);
        }
        sleep_ms(15);
    }
}

// ---------------------------------------------------------------------------
// SD diagnostic -- deliberately read-only. Never writes user media.
// ---------------------------------------------------------------------------
static void draw_sd_icon(uint16_t c) {
    int x = 34, y = 70, w = 76, h = 98;
    st7789_fill_rect(x, y + 12, w, h - 12, c);
    st7789_fill_rect(x + 12, y, w - 12, 12, c);
    st7789_fill_rect(x + 8, y + 28, 12, 30, g_theme->bg);
    st7789_fill_rect(x + 26, y + 28, 12, 30, g_theme->bg);
    st7789_fill_rect(x + 44, y + 28, 12, 30, g_theme->bg);
}

static const char *sd_type_name(sd_type_t type) {
    switch (type) {
        case SD_V2_SDHC: return "SDHC/SDXC";
        case SD_V2_SDSC: return "SDSC v2";
        case SD_V1:      return "SDSC v1";
        default:         return "Unknown";
    }
}

static void sd_draw_base(void) {
    st7789_fill(g_theme->bg);
    ui_fill_bg(0, DIAG_TOP, LCD_W, DIAG_BOTTOM - DIAG_TOP);
    ui_header("SD Card");
    draw_sd_icon(g_theme->accent);
    st7789_draw_string(128, 72, "Read-only hardware test", g_theme->item_fg, g_theme->bg, 1);
    st7789_draw_string(128, 94, "Init card + read LBA 0", g_theme->footer_fg, g_theme->bg, 1);
    st7789_draw_string(128, 122, "Press A to probe", g_theme->accent, g_theme->bg, 1);
    ui_footer("A probe   B back");
}

void diag_sd(void) {
    sd_draw_base();
    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) { diag_restore_theme_led(); return; }
        if (ev & (1u << BTN_A)) {
            ws2812_set(60, 18, 36); // pink: probe in progress
            st7789_fill_rect(120, 118, 192, 78, g_theme->bg);
            st7789_draw_string(128, 122, "Probing...", g_theme->warn, g_theme->bg, 1);

            sd_info_t info;
            uint8_t sector[512];
            bool init_ok = sd_init(&info);
            bool read_ok = init_ok && sd_read_blocks(sector, 0, 1);
            bool ok = init_ok && read_ok;

            st7789_fill_rect(120, 118, 192, 82, g_theme->bg);
            if (ok) {
                ws2812_set(0, 56, 0);
                st7789_draw_string(128, 120, "PASS", g_theme->ok, g_theme->bg, 2);
                char line[40];
                snprintf(line, sizeof line, "%s", sd_type_name(info.type));
                st7789_draw_string(128, 148, line, g_theme->item_fg, g_theme->bg, 1);
                if (info.mb >= 1024)
                    snprintf(line, sizeof line, "%lu GB  read OK", (unsigned long)(info.mb / 1024));
                else
                    snprintf(line, sizeof line, "%lu MB  read OK", (unsigned long)info.mb);
                st7789_draw_string(128, 166, line, g_theme->footer_fg, g_theme->bg, 1);
            } else {
                ws2812_set(60, 0, 0);
                st7789_draw_string(128, 120, "FAIL", g_theme->err, g_theme->bg, 2);
                st7789_draw_string(128, 150, init_ok ? "Sector read failed" : "Card init failed",
                                   g_theme->item_fg, g_theme->bg, 1);
            }
        }
        sleep_ms(15);
    }
}

// ---------------------------------------------------------------------------
// RGB diagnostic -- screen swatch mirrors the physical LED. Final item is a
// smooth trans-flag colour cycle, useful for validating the themed idle effect.
// ---------------------------------------------------------------------------
typedef struct { const char *name; uint8_t r, g, b; } rgb_test_t;
static const rgb_test_t RGB_TEST[] = {
    {"Red",60,0,0}, {"Green",0,60,0}, {"Blue",0,0,60}, {"White",60,60,60},
    {"Pink",60,24,43}, {"Purple",38,16,58}, {"Cyan",0,54,58}, {"Cycle",0,0,0}, {"Off",0,0,0}
};
#define N_RGB ((int)(sizeof(RGB_TEST) / sizeof(RGB_TEST[0])))
#define RGB_CYCLE_INDEX (N_RGB - 2)

static void mix_rgb(uint8_t ar, uint8_t ag, uint8_t ab,
                    uint8_t br, uint8_t bg, uint8_t bb,
                    uint32_t n, uint32_t d, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = (uint8_t)((int)ar + ((int)br - (int)ar) * (int)n / (int)d);
    *g = (uint8_t)((int)ag + ((int)bg - (int)ag) * (int)n / (int)d);
    *b = (uint8_t)((int)ab + ((int)bb - (int)ab) * (int)n / (int)d);
}

static void rgb_cycle_at(uint32_t ms, uint8_t *r, uint8_t *g, uint8_t *b) {
    static const uint8_t key[5][3] = {
        {21,48,58}, {58,40,43}, {60,60,60}, {58,40,43}, {21,48,58}
    };
    const uint32_t seg_ms = 1250;
    uint32_t p = ms % (seg_ms * 4u);
    uint32_t seg = p / seg_ms, pos = p % seg_ms;
    mix_rgb(key[seg][0], key[seg][1], key[seg][2],
            key[seg + 1][0], key[seg + 1][1], key[seg + 1][2],
            pos, seg_ms, r, g, b);
}

static uint16_t base60_to_565(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t rr = (uint8_t)((uint16_t)r * 255u / 60u);
    uint8_t gg = (uint8_t)((uint16_t)g * 255u / 60u);
    uint8_t bb = (uint8_t)((uint16_t)b * 255u / 60u);
    return RGB565(rr, gg, bb);
}

static void rgb_draw_shell(int idx) {
    st7789_fill(g_theme->bg);
    ui_fill_bg(0, DIAG_TOP, LCD_W, DIAG_BOTTOM - DIAG_TOP);
    ui_header("RGB LED");
    draw_centered(48, RGB_TEST[idx].name, g_theme->item_fg, g_theme->bg, 2);
    // swatch border
    st7789_fill_rect(79, 86, 162, 82, g_theme->item_fg);
    st7789_fill_rect(82, 89, 156, 76, g_theme->bg);
    st7789_draw_string(82, 188, idx == RGB_CYCLE_INDEX ? "Theme cycle preview" : "Physical LED mirrors swatch",
                       g_theme->footer_fg, g_theme->bg, 1);
    ui_footer("<> color   B back");
}

static void rgb_apply_swatch(uint8_t r, uint8_t g, uint8_t b) {
    ws2812_set(r, g, b);
    st7789_fill_rect(84, 91, 152, 72, base60_to_565(r, g, b));
}

void diag_rgb(void) {
    int idx = 0;
    rgb_draw_shell(idx);
    rgb_apply_swatch(RGB_TEST[idx].r, RGB_TEST[idx].g, RGB_TEST[idx].b);
    uint32_t last_cycle = 0;

    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) { diag_restore_theme_led(); return; }
        int old = idx;
        if (ev & (1u << BTN_LEFT))  idx = (idx + N_RGB - 1) % N_RGB;
        if (ev & (1u << BTN_RIGHT)) idx = (idx + 1) % N_RGB;
        if (idx != old) {
            rgb_draw_shell(idx);
            if (idx != RGB_CYCLE_INDEX)
                rgb_apply_swatch(RGB_TEST[idx].r, RGB_TEST[idx].g, RGB_TEST[idx].b);
            last_cycle = 0;
        }

        if (idx == RGB_CYCLE_INDEX) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - last_cycle >= 50) {
                uint8_t r, g, b;
                rgb_cycle_at(now, &r, &g, &b);
                rgb_apply_swatch(r, g, b);
                last_cycle = now;
            }
        }
        sleep_ms(15);
    }
}

// ---------------------------------------------------------------------------
// Diagnostics submenu. The LED animator is paused so RGB/SD tests can directly
// own the physical LED; the active theme colour is restored between tests.
// ---------------------------------------------------------------------------
void diagnostics_menu(void) {
    static const char *const items[] = { "System", "Controls", "Display", "Audio", "SD Card", "RGB LED" };
    menu_t m = { "Diagnostics", items, 6, 0, false };
    led_pause();
    diag_restore_theme_led();
    ui_draw_menu(&m);

    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev & (1u << BTN_B)) { led_resume(); return; }
        if ((ev & (1u << BTN_UP))   && ui_menu_move(&m, -1)) ui_draw_menu(&m);
        if ((ev & (1u << BTN_DOWN)) && ui_menu_move(&m,  1)) ui_draw_menu(&m);
        if (ev & (1u << BTN_A)) {
            ui_transition(UI_TRANSITION_FORWARD);
            switch (m.sel) {
                case 0: diag_system();  break;
                case 1: diag_buttons(); break;
                case 2: diag_display(); break;
                case 3: diag_tone();    break;
                case 4: diag_sd();      break;
                case 5: diag_rgb();     break;
            }
            diag_restore_theme_led();
            ui_transition(UI_TRANSITION_BACK);
            ui_draw_menu(&m);
        }
        sleep_ms(15);
    }
}