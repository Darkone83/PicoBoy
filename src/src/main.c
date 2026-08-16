#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

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

// The one shared emulator RAM arena (see arena.h). Defined here so it needs no
// new source file in the build; GB and NES both carve their big buffers from it.
uint8_t g_emu_arena[ARENA_BYTES] __attribute__((aligned(4)));
uint8_t *arena_base(void) { return g_emu_arena; }

static const char *const main_items[] = { "Browse ROMs", "Load last game", "Settings" };

// Runs the ROM staged in the flash window (set by Browse ROMs). Self-validates:
// gb_core shows an error if the window holds no/bad ROM.
static void load_last_game(void) {
    loader_launch_last();   // resolves the .srm from /lastrom.txt when the card is present
}

// Overclock for full-speed emulation. clk_peri follows clk_sys (250 MHz) so the
// LCD SPI can clock well above 24 MHz (the PL022 divides it down to LCD_SPI_HZ).
// PIO (audio + LED) runs off clk_sys too; its dividers are set at init, after
// this, so they stay correct. The PIO/SPI init below all runs after this.
#define PICOBOY_SYS_CLK_KHZ 272000
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

int main(void) {
    overclock();
    stdio_init_all();

    ws2812_init(RGB_PIN);
    led_init();
    led_set_state(LED_BOOT);   // purple solid while we come up
    st7789_init();

    // Boot splash: centered 160x160 logo, brief hold.
    st7789_fill(COL_BLACK);
    st7789_blit((LCD_W - SPLASH_W) / 2, (LCD_H - SPLASH_H) / 2, SPLASH_W, SPLASH_H, picoboy_splash);
    sleep_ms(1600);

    buttons_init();
    settings_init();
    settings_apply();          // backlight + LED brightness
    battery_init();            // battery/charge monitor -> LED overlay

    menu_t main_menu = { "PicoBoy", main_items, 3, 0 };
    led_set_state(LED_IDLE);   // white heartbeat at rest
    ui_draw_menu(&main_menu);

    while (true) {
        buttons_update();
        uint16_t ev = buttons_pressed();

        if ((ev & (1u << BTN_UP))   && ui_menu_move(&main_menu, -1)) ui_draw_menu(&main_menu);
        if ((ev & (1u << BTN_DOWN)) && ui_menu_move(&main_menu,  1)) ui_draw_menu(&main_menu);
        if (ev & (1u << BTN_A)) {
            switch (main_menu.sel) {
                case 0: loader_browse();  break;
                case 1: load_last_game(); break;
                case 2: settings_menu();  break;
            }
            led_set_state(LED_IDLE);
            ui_draw_menu(&main_menu);
        }
        sleep_ms(15);
    }
}