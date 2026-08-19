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

// Edge bitmask: newly accepted released->pressed transitions from this update,
// PLUS synthetic repeats for a held D-pad direction (UP/DOWN/LEFT/RIGHT) once it
// has been held past an initial delay. Menus, the ROM browser, and value sliders
// read this, so holding a direction scrolls/adjusts. Gameplay reads buttons_state()
// (held), not this, so auto-repeat never affects in-game input. A/B/START/SELECT/
// MENU never auto-repeat.
uint16_t buttons_pressed(void);