#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "theme.h"

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

// Battery overlay: the battery poller drives this independently of the normal
// commanded state. Battery states may override both IDLE and RUNNING so charge
// and warnings stay visible during gameplay, but never mask boot/flash/SD/error.
typedef enum {
    LED_BATT_NONE = 0,   // on battery, healthy -> show the commanded state
    LED_BATT_CHARGING,   // orange  solid
    LED_BATT_FULL,       // green   solid (charger present + full/near-full)
    LED_BATT_LOW,        // amber   slow-blink (<=20% warning)
    LED_BATT_CRITICAL,   // red     fast pulse (<=10% warning)
} led_batt_t;

void led_init(void);                 // start the animator (call after ws2812_init)
void led_set_state(led_state_t st);  // source of truth for the LED
void led_set_count(uint8_t groups);  // blips per group for counted-blink states (a code); default 2
void led_set_idle_rgb565(uint16_t c); // LED_IDLE static base colour follows the UI theme accent
void led_set_idle_theme_mode(theme_led_mode_t mode); // optional theme-owned idle animation; status states still win
void led_set_battery(led_batt_t b);  // battery overlay, driven by the battery poller

// Hand the LED to direct ws2812_set() control (diagnostics that test the LED);
// led_resume() restarts the animator at the current state.
void led_pause(void);
void led_resume(void);