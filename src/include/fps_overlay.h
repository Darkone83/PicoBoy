#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared in-game FPS diagnostic.
 *
 * The counter tracks EMULATED frames, not frames actually presented to the LCD.
 * That keeps the number useful when manual/automatic frame skip is active.
 *
 * fps_overlay_row_be() draws a tiny "FPSxx" badge into a packed big-endian
 * RGB565 scanline. y is relative to the top of the active game image.
 */
void fps_overlay_reset(void);
void fps_overlay_frame(void);
void fps_overlay_row_be(uint8_t *row, int width, int y);

#ifdef __cplusplus
}
#endif