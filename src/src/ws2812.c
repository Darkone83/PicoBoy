#include "ws2812.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"   // generated from src/ws2812.pio

#define RGB_PIO pio0
#define RGB_SM  0

static uint8_t s_brightness = 100;  // percent, applied to every ws2812_set()

void ws2812_init(unsigned pin) {
    uint offset = pio_add_program(RGB_PIO, &ws2812_program);
    ws2812_program_init(RGB_PIO, RGB_SM, offset, pin, 800000.0f); // 800 kHz
}

void ws2812_set_brightness(uint8_t pct) {
    s_brightness = pct > 100 ? 100 : pct;
}

void ws2812_set(uint8_t r, uint8_t g, uint8_t b) {
    r = (uint8_t)((uint16_t)r * s_brightness / 100u);
    g = (uint8_t)((uint16_t)g * s_brightness / 100u);
    b = (uint8_t)((uint16_t)b * s_brightness / 100u);
    // GRB, left-justified into the top 24 bits (PIO shifts MSB first).
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
    pio_sm_put_blocking(RGB_PIO, RGB_SM, grb << 8u);
}