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
static volatile bool    s_idle_trans_cycle = false;

// Trans-flag idle animation. Base values peak around 60, matching the rest of
// the status palette; the global LED Bright setting applies afterward. One full
// blue -> pink -> white -> pink -> blue loop takes ~5 seconds.
#define TRANS_SEG_TICKS 50u
static rgb_t mix_rgb(rgb_t a, rgb_t b, uint32_t n, uint32_t d) {
    rgb_t out;
    out.r = (uint8_t)((int)a.r + ((int)b.r - (int)a.r) * (int)n / (int)d);
    out.g = (uint8_t)((int)a.g + ((int)b.g - (int)a.g) * (int)n / (int)d);
    out.b = (uint8_t)((int)a.b + ((int)b.b - (int)a.b) * (int)n / (int)d);
    return out;
}

static rgb_t trans_cycle_color(uint32_t ph) {
    static const rgb_t key[5] = {
        { 21, 48, 58 }, // light blue  #5BCEFA, scaled to the status palette
        { 58, 40, 43 }, // pink        #F5A9B8
        { 60, 60, 60 }, // white
        { 58, 40, 43 }, // pink
        { 21, 48, 58 }, // light blue
    };
    uint32_t p = ph % (TRANS_SEG_TICKS * 4u);
    uint32_t seg = p / TRANS_SEG_TICKS;
    uint32_t pos = p % TRANS_SEG_TICKS;
    return mix_rgb(key[seg], key[seg + 1u], pos, TRANS_SEG_TICKS);
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
    rgb_t c = (st == LED_IDLE && s_idle_trans_cycle) ? trans_cycle_color(ph)
                                                      : color_for(st);

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

void led_set_idle_trans_cycle(bool enable) {
    if (enable != s_idle_trans_cycle) {
        s_idle_trans_cycle = enable;
        s_phase = 0;
    }
}

void led_pause(void)  { s_paused = true; }
void led_resume(void) { s_phase = 0; s_paused = false; }