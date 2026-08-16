#pragma once
#include <stdint.h>
#include <stdbool.h>

// SK6812 status scheme, driven by a single background animator. Colours are dim
// base values; the global LED-brightness setting scales them in ws2812_set().
typedef enum {
    LED_BOOT = 0,    // purple  solid              (booting)
    LED_IDLE,        // white   solid                (menus / idle)
    LED_RUNNING,     // magenta solid                (emulator running)
    LED_FLASH_BUSY,  // blue    even-blink          (loading / flashing)
    LED_FLASH_OK,    // blue    solid               (flash success; caller -> RUNNING)
    LED_FLASH_MAINT, // cyan    counted-blink       (flash maintenance: clear ROM / reset settings)
    LED_SD_BUSY,     // pink    counted-blink       (SD ops)
    LED_ERROR,       // red     counted-blink       (error)
} led_state_t;

// Battery overlay: an ambient layer the battery poller drives, independent of
// the commanded state above. CHARGING/FULL show only over LED_IDLE (resting);
// LOW also shows over LED_RUNNING (warn during play) but never interrupts a
// flash write, SD op, or error code.
typedef enum {
    LED_BATT_NONE = 0,   // on battery, healthy -> show the commanded state
    LED_BATT_CHARGING,   // orange  solid
    LED_BATT_FULL,       // green   solid
    LED_BATT_LOW,        // amber   slow-blink (warning)
} led_batt_t;

void led_init(void);                 // start the animator (call after ws2812_init)
void led_set_state(led_state_t st);  // source of truth for the LED
void led_set_count(uint8_t groups);  // blips per group for counted-blink states (a code); default 2
void led_set_idle_rgb565(uint16_t c); // LED_IDLE (menu) base colour follows the UI theme accent
void led_set_battery(led_batt_t b);  // battery overlay, driven by the battery poller

// Hand the LED to direct ws2812_set() control (diagnostics that test the LED);
// led_resume() restarts the animator at the current state.
void led_pause(void);
void led_resume(void);