#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include <math.h>

#include "pins.h"
#include "st7789.h"
#include "ws2812.h"
#include "led.h"
#include "buttons.h"
#include "ui.h"
#include "gb_core.h"
#include "splash.h"
#include "settings.h"
#include "loader.h"
#include "arena.h"
#include "battery.h"
#include "theme.h"
#include "version.h"

// The one shared emulator RAM arena (see arena.h). Defined here so it needs no
// new source file in the build; GB and NES both carve their big buffers from it.
uint8_t g_emu_arena[ARENA_BYTES] __attribute__((aligned(4)));
uint8_t *arena_base(void) { return g_emu_arena; }

static const char *const main_items[] = { "Browse ROMs", "Load last game", "Settings", "About" };

// Runs the ROM staged in the flash window (set by Browse ROMs). Self-validates:
// gb_core shows an error if the window holds no/bad ROM.
static void load_last_game(void) {
    loader_launch_last();   // resolves the .srm from /lastrom.txt when the card is present
}

// Overclock for full-speed emulation. clk_peri follows clk_sys (250 MHz) so the
// LCD SPI can clock well above 24 MHz (the PL022 divides it down to LCD_SPI_HZ).
// PIO (audio + LED) runs off clk_sys too; its dividers are set at init, after
// this, so they stay correct. The PIO/SPI init below all runs after this.
#define PICOBOY_SYS_CLK_KHZ 285000
static void overclock(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_30);   // headroom for the overclock
    sleep_ms(10);                          // let the regulator settle
    set_sys_clock_khz(PICOBOY_SYS_CLK_KHZ, true);
    clock_configure(clk_peri, 1,
                CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                clock_get_hz(clk_sys), clock_get_hz(clk_sys));

    // clk_peri keeps following clk_sys (250 MHz) so the LCD SPI isn't capped at
    // 24 MHz -- the PL022 divides clk_sys down to LCD_SPI_HZ (pins.h). This runs
    // the SPI peripheral above its nominal rating; if the panel shows corrupt
    // pixels, lower LCD_SPI_HZ.
}

// Light, theme-aware plasma screensaver. Runs off the (idle) emulator arena and
// only fires when enabled in Settings. Any button wakes it.
#define SCREENSAVER_MS 30000
#define SS_PW 160
#define SS_PH 120

static uint16_t ss_mix(uint16_t a, uint16_t b, int num, int den) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + ((br - ar) * num) / den;
    int g = ag + ((bg - ag) * num) / den;
    int bl = ab + ((bb - ab) * num) / den;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void screensaver_run(void) {
    static int8_t sinlut[256];
    static bool   lut_ready = false;
    if (!lut_ready) {
        for (int i = 0; i < 256; i++)
            sinlut[i] = (int8_t)(sinf((float)i * 6.2831853f / 256.0f) * 127.0f);
        lut_ready = true;
    }

    // Theme-aware palette: loops bg -> accent -> light -> accent -> bg (no seam).
    uint16_t pal[256];
    uint16_t bg = g_theme->bg, ac = g_theme->accent, lite = ss_mix(ac, 0xFFFF, 45, 100);
    for (int i = 0; i < 256; i++) {
        int s = i & 63, seg = i >> 6;
        pal[i] = seg == 0 ? ss_mix(bg,   ac,   s, 64)
               : seg == 1 ? ss_mix(ac,   lite, s, 64)
               : seg == 2 ? ss_mix(lite, ac,   s, 64)
               :            ss_mix(ac,   bg,   s, 64);
    }

    uint16_t *buf = (uint16_t *)arena_base();   // menu is idle: the arena is free
    uint32_t t = 0;
    st7789_backlight_level(30);                 // dim the panel while idle
    while (true) {
        buttons_update();
        if (buttons_pressed()) break;           // any button wakes

        for (int y = 0; y < SS_PH; y++) {
            int sy = sinlut[(y * 2 - t) & 255];
            for (int x = 0; x < SS_PW; x++) {
                int v = sinlut[(x * 3 + t) & 255] + sy
                      + sinlut[((x + y) * 2 + (t >> 1)) & 255];
                int idx = (v + 384) * 255 / 768;   // -381..381 -> 0..254
                buf[y * SS_PW + x] = pal[idx & 255];
            }
        }
        st7789_blit_scaled(0, 0, LCD_W, LCD_H, buf, SS_PW, SS_PH);   // 2x nearest
        t += 3;
        sleep_ms(40);                            // ~15 fps, slow drift, low duty
    }
    st7789_backlight_level(g_settings.lcd_brightness);   // restore on wake
}

// About / credits -----------------------------------------------------------
// Keep these credits in the firmware as well as the repository documentation.
// PicoBoy's emulator cores are heavily adapted for this hardware, but the
// upstream projects and authors remain an important part of the lineage.
typedef struct {
    const char *text;
    uint8_t style;    // 0 = normal, 1 = section/accent, 2 = dim
} about_credit_t;

static const about_credit_t ABOUT_CREDITS[] = {
    { "GAME BOY",                         1 },
    { "Peanut-GB + minigb_apu",           0 },
    { "Mahyar Koshkouei / deltabeard",    0 },
    { "MIT licensed",                     2 },
    { "",                                 0 },

    { "NES",                              1 },
    { "InfoNES - Jay Kumogata",           0 },
    { "pico-infones - Shuichi Takano",    0 },
    { "pico-infonesPlus - fhoedemakers",  0 },
    { "",                                 0 },

    { "ATARI 2600",                       1 },
    { "pico-atari2600",                   0 },
    { "Ilya Maslennikov - MIT",           0 },
    { "Based on HiFive1-2600",            0 },
    { "David Grubb",                      0 },
    { "Accuracy reference: Stella project", 2 },
    { "",                                 0 },

    { "PLATFORM / LIBRARIES",             1 },
    { "FatFs - ChaN",                     0 },
    { "I2S - Vincent Mistler",            0 },
    { "Raspberry Pi Pico SDK",            0 },
    { "",                                 0 },
    { "PicoBoy firmware: GPL-3.0",        2 },
};

#define ABOUT_CREDIT_COUNT \
    ((int)(sizeof(ABOUT_CREDITS) / sizeof(ABOUT_CREDITS[0])))
#define ABOUT_VISIBLE_LINES 9
#define ABOUT_LIST_X        14
#define ABOUT_LIST_Y       105
#define ABOUT_LINE_H        11
#define ABOUT_LIST_H       (ABOUT_VISIBLE_LINES * ABOUT_LINE_H)

static void about_draw_credit_list(int top) {
    uint16_t bg  = g_theme->bg;
    uint16_t tx  = g_theme->item_fg;
    uint16_t dim = g_theme->footer_fg;
    uint16_t ac  = g_theme->accent;

    // Clear just the scrolling body; title/version above it stay fixed.
    st7789_fill_rect(8, ABOUT_LIST_Y - 3, LCD_W - 16, ABOUT_LIST_H + 6, bg);

    for (int row = 0; row < ABOUT_VISIBLE_LINES; row++) {
        int i = top + row;
        if (i >= ABOUT_CREDIT_COUNT) break;

        uint16_t col = ABOUT_CREDITS[i].style == 1 ? ac
                     : ABOUT_CREDITS[i].style == 2 ? dim
                     : tx;

        st7789_draw_string(ABOUT_LIST_X,
                           ABOUT_LIST_Y + row * ABOUT_LINE_H,
                           ABOUT_CREDITS[i].text,
                           col, bg, 1);
    }

    // Thin themed scrollbar. It also makes it obvious there is more text below.
    if (ABOUT_CREDIT_COUNT > ABOUT_VISIBLE_LINES) {
        const int track_x = LCD_W - 7;
        const int track_y = ABOUT_LIST_Y - 2;
        const int track_h = ABOUT_LIST_H + 2;
        const int max_top = ABOUT_CREDIT_COUNT - ABOUT_VISIBLE_LINES;

        int thumb_h = track_h * ABOUT_VISIBLE_LINES / ABOUT_CREDIT_COUNT;
        if (thumb_h < 12) thumb_h = 12;

        int thumb_y = track_y;
        if (max_top > 0)
            thumb_y += (track_h - thumb_h) * top / max_top;

        st7789_fill_rect(track_x, track_y, 3, track_h, g_theme->footer_bg);
        st7789_fill_rect(track_x, thumb_y, 3, thumb_h, ac);
    }
}

// Scrollable About screen. Up/Down moves one line; Left/Right pages.
// B returns to the main menu.
static void about_screen(void) {
    uint16_t ac  = g_theme->accent;
    uint16_t tx  = g_theme->item_fg;
    uint16_t bg  = g_theme->bg;
    uint16_t dim = g_theme->footer_fg;

    st7789_fill(bg);
    ui_header("About");

    st7789_draw_string(14, 40, "PicoBoy", ac, bg, 2);
    st7789_draw_string(146, 43, "Version " PICOBOY_VERSION, tx, bg, 1);
    st7789_draw_string(14, 69, "by Darkone83", tx, bg, 1);
    st7789_draw_string(14, 88, "Third-party credits", dim, bg, 1);

    int top = 0;
    about_draw_credit_list(top);
    ui_footer("Up/Down scroll   B back");

    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        bool redraw = false;

        if (ev & (1u << BTN_B))
            break;

        if ((ev & (1u << BTN_UP)) && top > 0) {
            top--;
            redraw = true;
        }

        if ((ev & (1u << BTN_DOWN)) &&
            top < ABOUT_CREDIT_COUNT - ABOUT_VISIBLE_LINES) {
            top++;
            redraw = true;
        }

        if (ev & (1u << BTN_LEFT)) {
            int next = top - ABOUT_VISIBLE_LINES;
            if (next < 0) next = 0;
            if (next != top) {
                top = next;
                redraw = true;
            }
        }

        if (ev & (1u << BTN_RIGHT)) {
            int max_top = ABOUT_CREDIT_COUNT - ABOUT_VISIBLE_LINES;
            int next = top + ABOUT_VISIBLE_LINES;
            if (next > max_top) next = max_top;
            if (next != top) {
                top = next;
                redraw = true;
            }
        }

        if (redraw)
            about_draw_credit_list(top);

        sleep_ms(15);
    }
}


// Smooth boot LED ramp used by the current 1.0.x UI.
#define BOOT_LED_FADE_MS     1600u
#define BOOT_LED_FADE_STEPS   100u

// Fixed-rate smoothstep fade. The old loop made one brightness step per percent,
// so a low configured LED brightness could sit ~50-80 ms between visible steps.
// This keeps the timing cadence constant and eases both ends of the ramp.
static void boot_led_fade(uint8_t target) {
    const uint32_t frame_ms = BOOT_LED_FADE_MS / BOOT_LED_FADE_STEPS;

    if (target == 0) {
        ws2812_set_brightness(0);
        sleep_ms(BOOT_LED_FADE_MS);
        return;
    }

    for (uint32_t i = 0; i <= BOOT_LED_FADE_STEPS; i++) {
        // t is 0..1024. smoothstep(t) = 3t^2 - 2t^3.
        uint32_t t = (i * 1024u) / BOOT_LED_FADE_STEPS;
        uint32_t eased = (uint32_t)(
            ((uint64_t)t * t * (3072u - 2u * t) + 524288u) / 1048576u
        );                                              // 0..1024

        uint8_t b = (uint8_t)(((uint32_t)target * eased + 512u) / 1024u);
        ws2812_set_brightness(b);

        if (i != BOOT_LED_FADE_STEPS)
            sleep_ms(frame_ms);
    }
}

int main(void) {
    overclock();
    stdio_init_all();

    ws2812_init(RGB_PIN);
    led_init();
    ws2812_set_brightness(0);  // start dark; fade the LED in during the splash
    led_set_state(LED_BOOT);   // purple solid while we come up
    st7789_init();

    // Boot splash: centered 160x160 logo, held while the LED slowly ramps up.
    st7789_fill(COL_BLACK);
    st7789_blit((LCD_W - SPLASH_W) / 2, (LCD_H - SPLASH_H) / 2, SPLASH_W, SPLASH_H, picoboy_splash);

    settings_init();                             // need the LED-brightness target
    uint8_t led_t = g_settings.led_brightness;   // final level to fade up to
    boot_led_fade(led_t);                        // ~1.6 s eased, fixed-cadence ramp

    buttons_init();
    settings_apply();          // backlight + LED brightness (settles at the target)
    battery_init();            // battery/charge monitor -> LED overlay

    menu_t main_menu = { "PicoBoy", main_items, 4, 0, false };
    led_set_state(LED_IDLE);   // white heartbeat at rest
    ui_draw_menu(&main_menu);

    uint32_t last_input = to_ms_since_boot(get_absolute_time());
    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();
        if (ev) last_input = to_ms_since_boot(get_absolute_time());

        if ((ev & (1u << BTN_UP))   && ui_menu_move(&main_menu, -1)) ui_draw_menu(&main_menu);
        if ((ev & (1u << BTN_DOWN)) && ui_menu_move(&main_menu,  1)) ui_draw_menu(&main_menu);
        if (ev & (1u << BTN_A)) {
            switch (main_menu.sel) {
                case 0:
                    ui_transition(UI_TRANSITION_FORWARD);
                    loader_browse();
                    ui_transition(UI_TRANSITION_BACK);
                    break;
                case 1:
                    // Launching a game is not another menu level; don't make the
                    // player wait through UI motion on the hot path.
                    load_last_game();
                    break;
                case 2:
                    ui_transition(UI_TRANSITION_FORWARD);
                    settings_menu();
                    ui_transition(UI_TRANSITION_BACK);
                    break;
                case 3:
                    ui_transition(UI_TRANSITION_FORWARD);
                    about_screen();
                    ui_transition(UI_TRANSITION_BACK);
                    break;
            }
            led_set_state(LED_IDLE);
            ui_draw_menu(&main_menu);
            last_input = to_ms_since_boot(get_absolute_time());  // don't count submenu time
        }

        if (g_settings.screensaver &&
            to_ms_since_boot(get_absolute_time()) - last_input > SCREENSAVER_MS) {
            screensaver_run();                        // returns when a button wakes it
            last_input = to_ms_since_boot(get_absolute_time());
            ui_draw_menu(&main_menu);                 // repaint; the wake press is consumed
        }
        sleep_ms(15);
    }
}