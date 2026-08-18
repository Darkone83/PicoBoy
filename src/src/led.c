#include "led.h"
#include "ws2812.h"
#include "pico/stdlib.h"

// One repeating timer animates every state. Heartbeat on steady states is the
// liveness tell (frozen-solid = the state machine hung). The global ws2812
// brightness (LED Bright setting) scales these base colours.

#define TICK_MS 25

typedef struct { uint8_t r, g, b; } rgb_t;

static volatile led_state_t s_state  = LED_BOOT;
static volatile bool        s_paused = false;
static volatile uint8_t     s_count  = 2;
static volatile uint32_t    s_phase  = 0;
static repeating_timer_t    s_timer;
static volatile led_batt_t  s_batt   = LED_BATT_NONE;

// LED_IDLE base colour, themeable: follows the UI accent (dim). Default white.
// Animated themes only affect LED_IDLE; battery/error/flash/SD states still win.
static volatile uint8_t          s_idle_r = 44, s_idle_g = 44, s_idle_b = 44;
static volatile theme_led_mode_t s_idle_mode = THEME_LED_STATIC;

static rgb_t mix_rgb(rgb_t a, rgb_t b, uint32_t n, uint32_t d) {
    rgb_t out;
    out.r = (uint8_t)((int)a.r + ((int)b.r - (int)a.r) * (int)n / (int)d);
    out.g = (uint8_t)((int)a.g + ((int)b.g - (int)a.g) * (int)n / (int)d);
    out.b = (uint8_t)((int)a.b + ((int)b.b - (int)a.b) * (int)n / (int)d);
    return out;
}

static rgb_t cycle_rgb(const rgb_t *key, uint32_t key_count,
                       uint32_t seg_ticks, uint32_t ph) {
    uint32_t seg_count = key_count - 1u;  // last key repeats the first
    uint32_t p = ph % (seg_ticks * seg_count);
    uint32_t seg = p / seg_ticks;
    uint32_t pos = p % seg_ticks;
    return mix_rgb(key[seg], key[seg + 1u], pos, seg_ticks);
}

static rgb_t idle_theme_color(uint32_t ph) {
    // Base values intentionally peak near 60, matching the existing status
    // palette. The user's LED Bright setting is applied afterward in ws2812.c.
    static const rgb_t trans[5] = {
        { 21, 48, 58 }, { 58, 40, 43 }, { 60, 60, 60 }, { 58, 40, 43 }, { 21, 48, 58 }
    };
    static const rgb_t dmg[5] = {
        {  8, 28,  8 }, { 36, 45,  5 }, { 50, 58, 42 }, { 38, 48,  5 }, {  8, 28,  8 }
    };
    static const rgb_t nes[5] = {
        { 60,  7,  9 }, { 30, 30, 32 }, { 60, 60, 60 }, { 30, 30, 32 }, { 60,  7,  9 }
    };
    static const rgb_t arcade[5] = {
        {  0, 55, 60 }, { 60,  0, 54 }, { 60, 50,  0 }, {  0, 60, 18 }, {  0, 55, 60 }
    };
    static const rgb_t pico[5] = {
        { 58,  8, 23 }, { 58, 58, 58 }, { 25, 50, 10 }, { 58, 58, 58 }, { 58,  8, 23 }
    };

    switch (s_idle_mode) {
        case THEME_LED_TRANS_CYCLE:  return cycle_rgb(trans,  5, 50u, ph); // ~5.0 s
        case THEME_LED_DMG_CYCLE:    return cycle_rgb(dmg,    5, 45u, ph); // ~4.5 s
        case THEME_LED_NES_CYCLE:    return cycle_rgb(nes,    5, 45u, ph); // ~4.5 s
        case THEME_LED_ARCADE_CYCLE: return cycle_rgb(arcade, 5, 35u, ph); // ~3.5 s
        case THEME_LED_PICO_CYCLE:   return cycle_rgb(pico,   5, 45u, ph); // ~4.5 s
        case THEME_LED_STATIC:
        default:
            return (rgb_t){ s_idle_r, s_idle_g, s_idle_b };
    }
}

static rgb_t color_for(led_state_t st) {
    switch (st) {
        case LED_BOOT:        return (rgb_t){ 30, 14, 46 };  // purple
        case LED_IDLE:        return (rgb_t){ s_idle_r, s_idle_g, s_idle_b }; // themed
        case LED_RUNNING:     return (rgb_t){ 48,  0, 48 };  // magenta
        case LED_FLASH_BUSY:  return (rgb_t){  0,  0, 64 };  // blue
        case LED_FLASH_OK:    return (rgb_t){  0,  0, 64 };  // blue (solid)
        case LED_FLASH_MAINT: return (rgb_t){  0, 48, 48 };  // cyan
        case LED_SD_BUSY:     return (rgb_t){ 60, 18, 36 };  // pink
        case LED_ERROR:       return (rgb_t){ 64,  0,  0 };  // red
        default:              return (rgb_t){  0,  0,  0 };
    }
}

static void put(rgb_t c, uint8_t pct) {
    ws2812_set((uint8_t)(c.r * pct / 100u),
               (uint8_t)(c.g * pct / 100u),
               (uint8_t)(c.b * pct / 100u));
}

static bool tick(repeating_timer_t *t) {
    (void)t;
    if (s_paused) return true;
    led_state_t st = s_state;
    uint32_t ph = ++s_phase;
    rgb_t c = (st == LED_IDLE) ? idle_theme_color(ph) : color_for(st);

    // Battery status overlays both idle and gameplay so charge/full/low remains
    // visible while a game is running. Critical operation states still win:
    // boot, flash, SD activity, maintenance and errors are never masked.
    led_batt_t b = s_batt;
    if (b != LED_BATT_NONE && (st == LED_IDLE || st == LED_RUNNING)) {
        switch (b) {
            case LED_BATT_CHARGING:
                put((rgb_t){ 60, 20, 0 }, 100); return true;             // orange solid
            case LED_BATT_FULL:
                put((rgb_t){  0, 56, 0 }, 100); return true;             // green solid
            case LED_BATT_LOW:
                // <=20%: amber slow blink, 500 ms on / 2 s period.
                put((rgb_t){ 60, 34, 0 }, (ph % 80) < 20 ? 100 : 0); return true;
            case LED_BATT_CRITICAL: {
                // <=10%: fast red breathing pulse. 25 ms tick, 20 ticks = 500 ms.
                uint32_t p = ph % 20u;
                uint32_t tri = (p < 10u) ? p : (20u - p);                // 0..10..1
                uint8_t level = (uint8_t)(20u + tri * 8u);               // 20..100%
                put((rgb_t){ 64, 0, 0 }, level); return true;
            }
            default: break;
        }
    }

    switch (st) {
        case LED_BOOT:
        case LED_FLASH_OK:
        case LED_IDLE:
        case LED_RUNNING:
            put(c, 100);
            break;

        case LED_FLASH_BUSY:
            put(c, (ph % 20) < 10 ? 100 : 0);
            break;

        case LED_FLASH_MAINT:
        case LED_SD_BUSY:
        case LED_ERROR: {
            uint32_t group  = (uint32_t)s_count * 10;
            uint32_t period = group + 24;
            uint32_t p = ph % period;
            put(c, (p < group && (p % 10) < 5) ? 100 : 0);
            break;
        }

        default:
            put(c, 0);
            break;
    }
    return true;
}

void led_init(void) {
    add_repeating_timer_ms(TICK_MS, tick, NULL, &s_timer);
}

void led_set_state(led_state_t st) {
    s_phase = 0;
    s_state = st;
}

void led_set_battery(led_batt_t b) {
    if (b != s_batt) { s_batt = b; s_phase = 0; }
}

void led_set_count(uint8_t groups) {
    s_count = groups < 1 ? 1 : groups;
}

void led_set_idle_rgb565(uint16_t c) {
    uint8_t r = (uint8_t)(((c >> 11) & 0x1F) << 3);
    uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) << 2);
    uint8_t b = (uint8_t)(( c        & 0x1F) << 3);
    s_idle_r = (uint8_t)(r * 60u / 255u);
    s_idle_g = (uint8_t)(g * 60u / 255u);
    s_idle_b = (uint8_t)(b * 60u / 255u);
}

void led_set_idle_theme_mode(theme_led_mode_t mode) {
    if (mode != s_idle_mode) {
        s_idle_mode = mode;
        s_phase = 0;
    }
}

void led_pause(void)  { s_paused = true; }
void led_resume(void) { s_phase = 0; s_paused = false; }