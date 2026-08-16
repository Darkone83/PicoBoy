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

static uint8_t  integ[NUM_BUTTONS];
static uint16_t stable;
static uint16_t edges;

void buttons_init(void) {
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);   // active-low
        integ[i] = 0;
    }
    stable = 0;
    edges = 0;
}

void buttons_update(void) {
    uint16_t prev = stable;
    for (int i = 0; i < NUM_BUTTONS; i++) {
        bool pressed = !gpio_get(pins[i]);
        if (pressed) { if (integ[i] < 4) integ[i]++; }
        else         { if (integ[i] > 0) integ[i]--; }
        if (integ[i] == 0)      stable &= ~(1u << i);
        else if (integ[i] >= 4) stable |=  (1u << i);
    }
    edges = stable & ~prev;
}

uint16_t buttons_state(void)   { return stable; }
uint16_t buttons_pressed(void) { return edges; }
