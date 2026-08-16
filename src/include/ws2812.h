#pragma once
#include <stdint.h>

// Initialise the SK6812 on the given pin (uses pio0, sm0 internally).
void ws2812_init(unsigned pin);

// Global brightness 0-100 (%), applied to every subsequent ws2812_set().
void ws2812_set_brightness(uint8_t pct);

// Set the single LED colour. Handles GRB ordering + global brightness.
void ws2812_set(uint8_t r, uint8_t g, uint8_t b);