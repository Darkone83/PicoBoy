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

// Simple About screen: name, version, credits, license. Reachable from the menu.
static void about_screen(void) {
    uint16_t ac = g_theme->accent, tx = g_theme->item_fg, bg = g_theme->bg, dim = g_theme->footer_fg;
    st7789_fill(bg);
    ui_header("About");
    int x = 14, y = 44;
    st7789_draw_string(x, y, "PicoBoy", ac, bg, 2);                 y += 24;
    st7789_draw_string(x, y, "Version " PICOBOY_VERSION, tx, bg, 1); y += 13;
    st7789_draw_string(x, y, "by Darkone83", tx, bg, 1);            y += 20;
    st7789_draw_string(x, y, "Emulator cores", dim, bg, 1);         y += 13;
    st7789_draw_string(x, y, "GB  Peanut-GB - deltabeard", tx, bg, 1); y += 12;
    st7789_draw_string(x, y, "NES InfoNES - kumogata", tx, bg, 1);     y += 12;
    st7789_draw_string(x, y, "SD  FatFs - ChaN", tx, bg, 1);           y += 20;
    st7789_draw_string(x, y, "License  GPL-3.0", tx, bg, 1);         y += 13;
    ui_footer("B back");
    while (true) {
        buttons_update();
        if (buttons_pressed() & (1u << BTN_B)) break;
        sleep_ms(15);
    }
}


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