#pragma once
#include <stdint.h>

#define NUM_BUTTONS 9

typedef enum {
    BTN_UP = 0, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_A, BTN_B, BTN_START, BTN_SELECT, BTN_MENU
} button_t;

extern const char *const button_names[NUM_BUTTONS];

void buttons_init(void);

// Call once per loop iteration: samples + debounces and latches edge events.
void buttons_update(void);

// Held state bitmask (1 = currently pressed). Valid after buttons_update().
uint16_t buttons_state(void);

// Edge bitmask: bits that transitioned released->pressed in the last update.
uint16_t buttons_pressed(void);
