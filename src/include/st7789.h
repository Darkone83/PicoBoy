#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COL_BLACK   0x0000
#define COL_WHITE   0xFFFF
#define COL_PURPLE  0xAABE   // ~(168,85,247), the accent
#define COL_GREEN   0x07E0
#define COL_RED     0xF800
#define COL_BLUE    0x001F
#define COL_YELLOW  0xFFE0
#define COL_GRAY    0x52AA

void st7789_init(void);
void st7789_backlight(bool on);
void st7789_backlight_level(uint8_t pct);   // PWM brightness, 0-100
void st7789_fill(uint16_t color);
void st7789_fill_rect(int x, int y, int w, int h, uint16_t color);
void st7789_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale);
void st7789_draw_string(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
// Horizontal marquee of one line within [x0,x1) at row y (8px tall); 'off' scrolls left.
void st7789_marquee(int x0, int x1, int y, const char *s, int off,
                    uint16_t fg, uint16_t bg, int gap_px);

// Push a w*h RGB565 buffer (host byte order) to the panel at (x,y).
void st7789_blit(int x, int y, int w, int h, const uint16_t *buf);

// Nearest-neighbor scale of a src_w*src_h RGB565 image into a dst_w*dst_h rect
// at (dx,dy). Streams per-row; no scaled framebuffer needed. dst_w <= LCD_W.
void st7789_blit_scaled(int dx, int dy, int dst_w, int dst_h,
                        const uint16_t *src, int src_w, int src_h);

// Throughput-optimised scaled blit: DMAs each row to SPI while packing the next.
// Byte-identical output to st7789_blit_scaled; overlaps pack with transfer.
void st7789_blit_scaled_dma(int dx, int dy, int dst_w, int dst_h,
                            const uint16_t *src, int src_w, int src_h);

// Per-scanline streaming: begin a RAMWR window, push rows (caller double-buffers),
// end to drain + release. Used by the NES full-res streaming path.
void st7789_stream_begin(int x, int y, int w, int h);
void st7789_stream_row(const uint8_t *row, int w);
void st7789_stream_end(void);
// Interlaced full-res path: stream one packed 565 row to an explicit panel row.
void st7789_stream_row_at(int x, int y, const uint8_t *row, int w);

// Interlaced scaled blit: draws only rows where (oy & 1) == field, leaving the
// rest as the panel last held them. Call with field alternating 0/1 each frame.
void st7789_blit_scaled_field(int dx, int dy, int dst_w, int dst_h,
                              const uint16_t *src, int src_w, int src_h, int field);