#pragma once
#include <stdint.h>

#define NUM_BUTTONS 9

typedef enum {
    BTN_UP = 0, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_A, BTN_B, BTN_START, BTN_SELECT, BTN_MENU
} button_t;

extern const char *const button_names[NUM_BUTTONS];

void buttons_init(void);

// Call once per loop iteration. Held state follows the current GPIO sample;
// rising-edge events use a short bounce lockout without delaying the first press.
void buttons_update(void);

// Held state bitmask (1 = currently pressed). Valid after buttons_update().
uint16_t buttons_state(void);

// Edge bitmask: newly accepted released->pressed transitions from this update.
uint16_t buttons_pressed(void);