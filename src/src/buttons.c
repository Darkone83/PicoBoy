#include "buttons.h"
#include "pins.h"
#include "pico/stdlib.h"

static const uint8_t pins[NUM_BUTTONS] = {
    BTN_UP_PIN, BTN_DOWN_PIN, BTN_LEFT_PIN, BTN_RIGHT_PIN,
    BTN_A_PIN, BTN_B_PIN, BTN_START_PIN, BTN_SELECT_PIN, BTN_MENU_PIN
};

const char *const button_names[NUM_BUTTONS] = {
    "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "START", "SELECT", "MENU"
};

// PicoBoy polls input from frame-driven loops (roughly 60 Hz during gameplay).
// The old 4-sample integrator therefore added ~50-67 ms before a press became
// visible. Physical switch bounce is much shorter than a game frame, so held
// state now follows the GPIO sample immediately.
//
// Press edges still get a short per-button lockout. The FIRST rising edge is
// accepted immediately; any bounce-generated extra rising edges during the
// lockout window are ignored. This keeps menus/modal actions clean without
// adding multi-frame latency to gameplay.
#define BUTTON_EDGE_LOCKOUT_US 20000u

// D-pad auto-repeat (menus/lists/sliders only -- see buttons_pressed()). A held
// direction fires its first synthetic edge after DELAY, then every INTERVAL.
#define BUTTON_REPEAT_DELAY_US    400000u
#define BUTTON_REPEAT_INTERVAL_US  90000u

static uint16_t state;
static uint16_t raw_prev;
static uint16_t edges;
static uint32_t last_edge_us[NUM_BUTTONS];
static uint32_t repeat_at_us[NUM_BUTTONS];   // next synth-edge time for held D-pad; 0 = disarmed

static uint16_t read_raw_buttons(void) {
    uint16_t raw = 0;

    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (!gpio_get(pins[i]))                  // active-low
            raw |= (uint16_t)(1u << i);
    }

    return raw;
}

void buttons_init(void) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);                   // active-low
    }

    // Seed from the real GPIO state so a button held through init is represented
    // correctly without manufacturing a false "pressed" edge.
    state = read_raw_buttons();
    raw_prev = state;
    edges = 0;

    uint32_t now = time_us_32();
    for (int i = 0; i < NUM_BUTTONS; i++) {
        last_edge_us[i] = now - BUTTON_EDGE_LOCKOUT_US;
        repeat_at_us[i] = 0;
    }
}

void buttons_update(void) {
    uint16_t raw = read_raw_buttons();
    uint16_t rising = raw & (uint16_t)~raw_prev;
    uint32_t now = time_us_32();

    edges = 0;

    // Accept the first press immediately. Only suppress duplicate rising edges
    // from contact bounce/noise for a short period after that accepted press.
    for (int i = 0; i < NUM_BUTTONS; i++) {
        uint16_t bit = (uint16_t)(1u << i);
        if ((rising & bit) &&
            (uint32_t)(now - last_edge_us[i]) >= BUTTON_EDGE_LOCKOUT_US) {
            edges |= bit;
            last_edge_us[i] = now;
            repeat_at_us[i] = now + BUTTON_REPEAT_DELAY_US;   // arm auto-repeat on this press
        }
    }

    // Auto-repeat, D-pad only (indices BTN_UP..BTN_RIGHT). While a direction is
    // held past the initial delay, inject a synthetic edge every interval so lists
    // and value sliders advance on hold. Signed time compares tolerate the 32-bit
    // microsecond wrap. Only the edge mask is touched; held state is untouched, so
    // gameplay (which reads buttons_state()) is unaffected.
    for (int i = BTN_UP; i <= BTN_RIGHT; i++) {
        uint16_t bit = (uint16_t)(1u << i);
        if (raw & bit) {
            if (repeat_at_us[i] && (int32_t)(now - repeat_at_us[i]) >= 0) {
                edges |= bit;
                repeat_at_us[i] = now + BUTTON_REPEAT_INTERVAL_US;
            }
        } else {
            repeat_at_us[i] = 0;                               // released: disarm
        }
    }

    // Held/released state is deliberately immediate. At gameplay polling rates
    // this cuts the input-side delay from several frames to the next poll only.
    state = raw;
    raw_prev = raw;
}

uint16_t buttons_state(void)   { return state; }
uint16_t buttons_pressed(void) { return edges; }