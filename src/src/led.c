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
static volatile uint8_t s_idle_r = 44, s_idle_g = 44, s_idle_b = 44;

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
    rgb_t c = color_for(st);
    uint32_t ph = ++s_phase;

    // Battery overlay sits on top of resting states. It never interrupts a
    // flash write, SD op, error code, or boot. LOW is a warning, so it also
    // shows while a game is RUNNING; CHARGING/FULL are ambient and only show
    // at rest (LED_IDLE).
    led_batt_t b = s_batt;
    if (b != LED_BATT_NONE) {
        bool show = (b == LED_BATT_LOW) ? (st == LED_IDLE || st == LED_RUNNING)
                                        : (st == LED_IDLE);
        if (show) {
            switch (b) {
                case LED_BATT_CHARGING: put((rgb_t){ 60, 20, 0 }, 100); return true; // orange solid
                case LED_BATT_FULL:     put((rgb_t){  0, 56, 0 }, 100); return true; // green  solid
                case LED_BATT_LOW:      // amber slow-blink: 500 ms on / 2 s period
                    put((rgb_t){ 60, 34, 0 }, (ph % 80) < 20 ? 100 : 0); return true;
                default: break;
            }
        }
    }

    switch (st) {
        case LED_BOOT:
        case LED_FLASH_OK:
        case LED_IDLE:
        case LED_RUNNING:
            put(c, 100);                                   // solid
            break;

        case LED_FLASH_BUSY: {                             // even-blink ~500 ms
            put(c, (ph % 20) < 10 ? 100 : 0);
            break;
        }

        case LED_FLASH_MAINT:
        case LED_SD_BUSY:
        case LED_ERROR: {                                  // N blips (125 ms each), then ~600 ms pause
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
    s_phase = 0;        // clean restart of the animation
    s_state = st;
}

void led_set_battery(led_batt_t b) {
    if (b != s_batt) { s_batt = b; s_phase = 0; }  // clean restart of the overlay blink
}

void led_set_count(uint8_t groups) {
    s_count = groups < 1 ? 1 : groups;
}

// Map the theme accent (RGB565) onto the dim LED_IDLE base. Peak ~60 keeps it in
// the same brightness family as the other states; global LED-bright scales it.
void led_set_idle_rgb565(uint16_t c) {
    uint8_t r = (uint8_t)(((c >> 11) & 0x1F) << 3);
    uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) << 2);
    uint8_t b = (uint8_t)(( c        & 0x1F) << 3);
    s_idle_r = (uint8_t)(r * 60u / 255u);
    s_idle_g = (uint8_t)(g * 60u / 255u);
    s_idle_b = (uint8_t)(b * 60u / 255u);
}

void led_pause(void)  { s_paused = true; }
void led_resume(void) { s_phase = 0; s_paused = false; }