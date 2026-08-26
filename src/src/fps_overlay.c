#include "fps_overlay.h"
#include "settings.h"
#include "pico/stdlib.h"

/* One shared counter for every emulator core. Only one core is active at once. */
static uint32_t s_window_start_us;
static uint16_t s_window_frames;
static volatile uint16_t s_fps;

void fps_overlay_reset(void) {
    s_window_start_us = time_us_32();
    s_window_frames = 0;
    s_fps = 0;
}

void fps_overlay_frame(void) {
    if (!g_settings.show_fps)
        return;

    s_window_frames++;
    uint32_t now = time_us_32();
    uint32_t elapsed = now - s_window_start_us;

    /* Half-second window: responsive enough for stress testing without flicker. */
    if (elapsed >= 500000u) {
        uint32_t fps = ((uint32_t)s_window_frames * 1000000u + (elapsed >> 1)) / elapsed;
        if (fps > 999u) fps = 999u;
        s_fps = (uint16_t)fps;
        s_window_frames = 0;
        s_window_start_us = now;
    }
}

/* 3x5 glyph rows, MSB-left in bits 2..0. */
static uint8_t glyph_row(char c, int row) {
    static const uint8_t digits[10][5] = {
        {7,5,5,5,7}, /* 0 */
        {2,6,2,2,7}, /* 1 */
        {7,1,7,4,7}, /* 2 */
        {7,1,7,1,7}, /* 3 */
        {5,5,7,1,1}, /* 4 */
        {7,4,7,1,7}, /* 5 */
        {7,4,7,5,7}, /* 6 */
        {7,1,1,1,1}, /* 7 */
        {7,5,7,5,7}, /* 8 */
        {7,5,7,1,7}, /* 9 */
    };
    static const uint8_t f[5] = {7,4,6,4,4};
    static const uint8_t p[5] = {6,5,6,4,4};
    static const uint8_t s[5] = {7,4,7,1,7};

    if ((unsigned)row >= 5u) return 0;
    if (c >= '0' && c <= '9') return digits[c - '0'][row];
    if (c == 'F') return f[row];
    if (c == 'P') return p[row];
    if (c == 'S') return s[row];
    return 0;
}

static inline void put565_be(uint8_t *row, int x, uint16_t color) {
    row[x * 2]     = (uint8_t)(color >> 8);
    row[x * 2 + 1] = (uint8_t)color;
}

void fps_overlay_row_be(uint8_t *row, int width, int y) {
    if (!g_settings.show_fps || !row || width < 32 || y < 0 || y >= 7)
        return;

    uint16_t fps = s_fps;
    if (fps == 0)
        return; /* avoid showing a misleading 0 during the first sample window */

    char text[7];
    int n = 0;
    text[n++] = 'F';
    text[n++] = 'P';
    text[n++] = 'S';

    if (fps >= 100u) {
        text[n++] = (char)('0' + (fps / 100u));
        text[n++] = (char)('0' + ((fps / 10u) % 10u));
        text[n++] = (char)('0' + (fps % 10u));
    } else {
        text[n++] = (char)('0' + (fps / 10u));
        text[n++] = (char)('0' + (fps % 10u));
    }

    const int glyph_w = 3;
    const int advance = 4;
    const int text_w = n * advance - 1;
    const int box_w = text_w + 4;
    const int x0 = width - box_w - 2;

    /* Solid black backing keeps the tiny text readable over any game scene. */
    for (int x = x0; x < x0 + box_w; x++)
        put565_be(row, x, 0x0000u);

    if (y == 0 || y == 6)
        return;

    const int gy = y - 1;
    for (int i = 0; i < n; i++) {
        uint8_t bits = glyph_row(text[i], gy);
        int gx = x0 + 2 + i * advance;
        for (int px = 0; px < glyph_w; px++) {
            if (bits & (uint8_t)(1u << (2 - px)))
                put565_be(row, gx + px, 0xFFFFu);
        }
    }
}