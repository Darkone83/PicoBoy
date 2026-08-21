#include "st7789.h"
#include "pins.h"
#include "font8x8.h"
#include "font16.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include <string.h>

/*
 * Shared LCD scratch.
 *
 * These buffers used to be separate function-local statics:
 *   fill row, glyph row/block, marquee row, blit row,
 *   three independent x-maps, and DMA ping-pong rows.
 * They are never needed by two of those helpers at once. The largest live set
 * is xmap[320] (640 B) + two 640-byte DMA rows = 1,920 bytes.
 *
 * Per-scanline emulator streaming uses caller-owned row buffers and does not
 * consume this scratch object.
 */
typedef struct {
    uint16_t xmap[LCD_W];
    uint8_t  lines[2][LCD_W * 2];
} st7789_scratch_t;

static st7789_scratch_t s_scratch;

static inline void cs(bool lo)  { gpio_put(LCD_CS_PIN, !lo); }
static inline void dc(bool data){ gpio_put(LCD_DC_PIN, data); }

static void wr_cmd(uint8_t c) {
    dc(false); cs(true);
    spi_write_blocking(LCD_SPI, &c, 1);
    cs(false);
}

static void wr_data(const uint8_t *d, size_t n) {
    dc(true); cs(true);
    spi_write_blocking(LCD_SPI, d, n);
    cs(false);
}

static void wr_data1(uint8_t b) { wr_data(&b, 1); }

static void set_window(int x0, int y0, int x1, int y1) {
    x0 += LCD_X_OFFSET; x1 += LCD_X_OFFSET;
    y0 += LCD_Y_OFFSET; y1 += LCD_Y_OFFSET;
    uint8_t c[4];
    wr_cmd(0x2A); // CASET
    c[0] = x0 >> 8; c[1] = x0 & 0xFF; c[2] = x1 >> 8; c[3] = x1 & 0xFF;
    wr_data(c, 4);
    wr_cmd(0x2B); // RASET
    c[0] = y0 >> 8; c[1] = y0 & 0xFF; c[2] = y1 >> 8; c[3] = y1 & 0xFF;
    wr_data(c, 4);
    wr_cmd(0x2C); // RAMWR
}

void st7789_init(void) {
    spi_init(LCD_SPI, LCD_SPI_HZ);
    gpio_set_function(LCD_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);

    gpio_init(LCD_CS_PIN);  gpio_set_dir(LCD_CS_PIN, GPIO_OUT);  gpio_put(LCD_CS_PIN, 1);
    gpio_init(LCD_DC_PIN);  gpio_set_dir(LCD_DC_PIN, GPIO_OUT);
    gpio_init(LCD_RST_PIN); gpio_set_dir(LCD_RST_PIN, GPIO_OUT);
    gpio_init(LCD_BL_PIN);  gpio_set_dir(LCD_BL_PIN, GPIO_OUT);  gpio_put(LCD_BL_PIN, 0);

    // Hardware reset.
    gpio_put(LCD_RST_PIN, 1); sleep_ms(10);
    gpio_put(LCD_RST_PIN, 0); sleep_ms(10);
    gpio_put(LCD_RST_PIN, 1); sleep_ms(120);

    wr_cmd(0x01); sleep_ms(150);          // SWRESET
    wr_cmd(0x11); sleep_ms(120);          // SLPOUT
    wr_cmd(0x3A); wr_data1(0x55);         // COLMOD: 16 bpp
    wr_cmd(0x36); wr_data1(LCD_MADCTL);   // MADCTL: orientation
    wr_cmd(0x21);                         // INVON (ST7789 needs inversion on)
    wr_cmd(0x13);                         // NORON
    wr_cmd(0x29); sleep_ms(20);           // DISPON

    st7789_fill(COL_BLACK);
    st7789_backlight(true);
}

void st7789_backlight(bool on) {
    gpio_put(LCD_BL_PIN, on);
}

// PWM brightness on the backlight pin. pct 0-100. Switches BL from plain GPIO
// to a PWM slice on first call; ~120 kHz so there's no visible flicker.
void st7789_backlight_level(uint8_t pct) {
    if (pct > 100) pct = 100;
    gpio_set_function(LCD_BL_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(LCD_BL_PIN);
    uint chan  = pwm_gpio_to_channel(LCD_BL_PIN);
    pwm_set_clkdiv(slice, 4.0f);
    pwm_set_wrap(slice, 255);
    pwm_set_chan_level(slice, chan, (uint16_t)((uint16_t)pct * 255u / 100u));
    pwm_set_enabled(slice, true);
}

void st7789_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;

    set_window(x, y, x + w - 1, y + h - 1);

    // Stream the fill one row at a time from a reusable line buffer.
    uint8_t *line = s_scratch.lines[0];
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (int i = 0; i < w; i++) { line[i * 2] = hi; line[i * 2 + 1] = lo; }

    dc(true); cs(true);
    for (int row = 0; row < h; row++) {
        spi_write_blocking(LCD_SPI, line, w * 2);
    }
    cs(false);
}

void st7789_fill(uint16_t color) {
    st7789_fill_rect(0, 0, LCD_W, LCD_H, color);
}

// Blend bg->fg by 8-bit coverage. Endpoints are fast-pathed (most glyph pixels
// are fully bg or fully fg); only edge pixels do the per-channel lerp.
static inline uint16_t font_blend(uint16_t bg, uint16_t fg, uint8_t a) {
    if (a == 0)   return bg;
    if (a == 255) return fg;
    int br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    int fr = (fg >> 11) & 0x1F, fgc = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    int r = br  + ((fr  - br)  * a) / 255;
    int g = bgc + ((fgc - bgc) * a) / 255;
    int b = bb  + ((fb  - bb)  * a) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Anti-aliased 16x16 glyph (font16). Used for scale-2 text (headers, menu rows).
static void st7789_draw_char_aa16(int x, int y, char c, uint16_t fg, uint16_t bg) {
    const uint8_t *cov = font16[c - 0x20];
    if (x < 0 || y < 0 || x + 16 > LCD_W || y + 16 > LCD_H) {     // clipped slow path
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 16; col++)
                st7789_fill_rect(x + col, y + row, 1, 1,
                                 font_blend(bg, fg, cov[row * 16 + col]));
        return;
    }
    set_window(x, y, x + 15, y + 15);
    uint8_t *buf = s_scratch.lines[0];
    dc(true); cs(true);
    for (int row = 0; row < 16; row++) {
        int idx = 0;
        for (int col = 0; col < 16; col++) {
            uint16_t color = font_blend(bg, fg, cov[row * 16 + col]);
            buf[idx++] = color >> 8; buf[idx++] = color & 0xFF;
        }
        spi_write_blocking(LCD_SPI, buf, 16 * 2);
    }
    cs(false);
}

void st7789_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale) {
    if (c < 0x20 || c > 0x7F) c = '?';
    if (scale < 1) scale = 1;
    if (scale == 2) { st7789_draw_char_aa16(x, y, c, fg, bg); return; }  // AA path

    const uint8_t *g = font8x8[c - 0x20];

    int cw = 8 * scale, ch = 8 * scale;
    if (x < 0 || y < 0 || x + cw > LCD_W || y + ch > LCD_H) {
        // Slow path / clipped: fall back to per-cell rects.
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++) {
                uint16_t color = (g[row] & (1 << col)) ? fg : bg;
                st7789_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        return;
    }

    set_window(x, y, x + cw - 1, y + ch - 1);
    uint8_t *buf = &s_scratch.lines[0][0]; // 1280 contiguous bytes across both rows
    // Build and push row-block by row-block to keep the buffer bounded.
    dc(true); cs(true);
    for (int row = 0; row < 8; row++) {
        int idx = 0;
        for (int col = 0; col < 8; col++) {
            uint16_t color = (g[row] & (1 << col)) ? fg : bg;
            uint8_t hi = color >> 8, lo = color & 0xFF;
            for (int s = 0; s < scale; s++) { buf[idx++] = hi; buf[idx++] = lo; }
        }
        for (int s = 0; s < scale; s++) spi_write_blocking(LCD_SPI, buf, cw * 2);
    }
    cs(false);
}

void st7789_draw_string(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale) {
    int cx = x;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') { cx = x; y += 8 * scale + scale; continue; }
        st7789_draw_char(cx, y, *p, fg, bg, scale);
        cx += 8 * scale;
    }
}

// Single-line horizontal marquee inside [x0,x1) at row y (1-bit font, 8px tall).
// Scrolls s left by 'off' px, wrapping seamlessly with a gap_px gap between
// repeats. Renders the whole window strip row-buffered so it's one window write.
void st7789_marquee(int x0, int x1, int y, const char *s, int off,
                    uint16_t fg, uint16_t bg, int gap_px) {
    int ww = x1 - x0;
    if (ww <= 0) return;
    if (ww > LCD_W) ww = LCD_W;
    int len = 0; while (s[len]) len++;
    int period = len * 8 + gap_px;
    if (period < 8) period = 8;
    int start = off % period; if (start < 0) start += period;

    uint8_t *line = s_scratch.lines[0];
    set_window(x0, y, x0 + ww - 1, y + 7);
    dc(true); cs(true);
    for (int row = 0; row < 8; row++) {
        int tx = start;
        for (int i = 0; i < ww; i++) {
            uint16_t c = bg;
            int ch = tx >> 3;
            if (ch < len) {
                unsigned char ci = (unsigned char)s[ch];
                if (ci < 0x20 || ci > 0x7F) ci = '?';
                if (font8x8[ci - 0x20][row] & (1 << (tx & 7))) c = fg;
            }
            line[2 * i]     = (uint8_t)(c >> 8);
            line[2 * i + 1] = (uint8_t)(c & 0xFF);
            if (++tx >= period) tx = 0;
        }
        spi_write_blocking(LCD_SPI, line, ww * 2);
    }
    cs(false);
}

void st7789_blit(int x, int y, int w, int h, const uint16_t *buf) {
    if (w <= 0 || h <= 0) return;
    set_window(x, y, x + w - 1, y + h - 1);
    uint8_t *line = s_scratch.lines[0];
    dc(true); cs(true);
    for (int row = 0; row < h; row++) {
        const uint16_t *src = &buf[row * w];
        for (int i = 0; i < w; i++) {
            line[i * 2]     = src[i] >> 8;
            line[i * 2 + 1] = src[i] & 0xFF;
        }
        spi_write_blocking(LCD_SPI, line, w * 2);
    }
    cs(false);
}

// Nearest-neighbor scale of a src_w x src_h RGB565 image into a dst_w x dst_h
// rect at (dx,dy). Streams one destination row at a time from a single line
// buffer -- no full scaled framebuffer. dst_w must be <= LCD_W.
void st7789_blit_scaled(int dx, int dy, int dst_w, int dst_h,
                        const uint16_t *src, int src_w, int src_h) {
    if (dst_w <= 0 || dst_h <= 0 || dst_w > LCD_W) return;
    set_window(dx, dy, dx + dst_w - 1, dy + dst_h - 1);

    uint16_t *xmap = s_scratch.xmap;
    for (int ox = 0; ox < dst_w; ox++) xmap[ox] = (uint16_t)((ox * src_w) / dst_w);

    uint8_t *line = s_scratch.lines[0];
    dc(true); cs(true);
    for (int oy = 0; oy < dst_h; oy++) {
        const uint16_t *srow = &src[(size_t)((oy * src_h) / dst_h) * src_w];
        for (int ox = 0; ox < dst_w; ox++) {
            uint16_t px = srow[xmap[ox]];
            line[ox * 2]     = px >> 8;
            line[ox * 2 + 1] = px & 0xFF;
        }
        spi_write_blocking(LCD_SPI, line, dst_w * 2);
    }
    cs(false);
}

// --- DMA-overlapped blit (Step 1 toward full-res streaming) -----------------
// Lazily claim one DMA channel feeding the SPI TX FIFO, 8-bit, dreq-paced.
static int s_blit_dma = -1;
static void blit_dma_init(void) {
    if (s_blit_dma >= 0) return;
    s_blit_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_blit_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI, true));   // SPI TX
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(s_blit_dma, &c, &spi_get_hw(LCD_SPI)->dr, NULL, 0, false);
}

// Same nearest-neighbour scale as st7789_blit_scaled, but DMAs each row to SPI
// while the CPU packs the NEXT row -- pack and transfer overlap instead of
// serialising, and SPI is fed back-to-back with no inter-row gap. Output is
// byte-identical to st7789_blit_scaled; this is purely a throughput change.
void st7789_blit_scaled_dma(int dx, int dy, int dst_w, int dst_h,
                            const uint16_t *src, int src_w, int src_h) {
    if (dst_w <= 0 || dst_h <= 0 || dst_w > LCD_W) return;
    blit_dma_init();
    set_window(dx, dy, dx + dst_w - 1, dy + dst_h - 1);

    uint16_t *xmap = s_scratch.xmap;
    for (int ox = 0; ox < dst_w; ox++) xmap[ox] = (uint16_t)((ox * src_w) / dst_w);

    uint8_t (*lines)[LCD_W * 2] = s_scratch.lines; // shared ping-pong rows
    int cur = 0;

    // pack row 0
    const uint16_t *srow = &src[0];
    for (int ox = 0; ox < dst_w; ox++) {
        uint16_t px = srow[xmap[ox]];
        lines[0][ox * 2] = px >> 8; lines[0][ox * 2 + 1] = px & 0xFF;
    }

    dc(true); cs(true);
    for (int oy = 0; oy < dst_h; oy++) {
        // stream this row over SPI via DMA
        dma_channel_transfer_from_buffer_now(s_blit_dma, lines[cur], (uint)(dst_w * 2));
        // pack the NEXT row into the other buffer while the DMA runs
        if (oy + 1 < dst_h) {
            const uint16_t *nrow = &src[(size_t)(((oy + 1) * src_h) / dst_h) * src_w];
            uint8_t *nb = lines[cur ^ 1];
            for (int ox = 0; ox < dst_w; ox++) {
                uint16_t px = nrow[xmap[ox]];
                nb[ox * 2] = px >> 8; nb[ox * 2 + 1] = px & 0xFF;
            }
        }
        dma_channel_wait_for_finish_blocking(s_blit_dma);   // row sent; buffer reusable
        cur ^= 1;
    }
    while (spi_is_busy(LCD_SPI)) tight_loop_contents();      // drain shift reg before CS
    cs(false);
}

// --- Per-scanline streaming (full-res path) --------------------------------
// Open one RAMWR window for the whole region, push rows into it one at a time
// (panel auto-advances), close at frame end. Reuses the proven blit DMA channel.
// Caller double-buffers the row data so it can pack row N+1 while row N streams.
void st7789_stream_begin(int x, int y, int w, int h) {
    blit_dma_init();
    set_window(x, y, x + w - 1, y + h - 1);
    dc(true); cs(true);                         // RAMWR open; CS held for the frame
}
void st7789_stream_row(const uint8_t *row, int w) {
    dma_channel_wait_for_finish_blocking(s_blit_dma);          // previous row sent
    dma_channel_transfer_from_buffer_now(s_blit_dma, row, (uint)(w * 2));
}
void st7789_stream_end(void) {
    dma_channel_wait_for_finish_blocking(s_blit_dma);
    while (spi_is_busy(LCD_SPI)) tight_loop_contents();
    cs(false);
}

// Stream one already-packed 565 row to a specific panel row via DMA. Used by the
// interlaced full-res path: rows aren't contiguous (every other line), so each
// gets its own 1-row window. Same set_window + dc/cs pattern as the field blit.
void st7789_stream_row_at(int x, int y, const uint8_t *row, int w) {
    blit_dma_init();
    set_window(x, y, x + w - 1, y);
    dc(true); cs(true);
    dma_channel_transfer_from_buffer_now(s_blit_dma, row, (uint)(w * 2));
    dma_channel_wait_for_finish_blocking(s_blit_dma);
    while (spi_is_busy(LCD_SPI)) tight_loop_contents();
    cs(false);
}

// Interlaced variant: draws only every other destination row -- the rows where
// (oy & 1) == field. Halves SPI traffic per call; the panel's GRAM retains the
// rows from the previous (opposite-field) frame, so the image stays complete and
// only the alternating field refreshes. Each drawn row uses a one-row window so
// the skipped rows are left untouched. field alternates 0/1 each frame.
void st7789_blit_scaled_field(int dx, int dy, int dst_w, int dst_h,
                              const uint16_t *src, int src_w, int src_h, int field) {
    if (dst_w <= 0 || dst_h <= 0 || dst_w > LCD_W) return;

    uint16_t *xmap = s_scratch.xmap;
    for (int ox = 0; ox < dst_w; ox++) xmap[ox] = (uint16_t)((ox * src_w) / dst_w);

    uint8_t *line = s_scratch.lines[0];
    for (int oy = (field & 1); oy < dst_h; oy += 2) {
        const uint16_t *srow = &src[(size_t)((oy * src_h) / dst_h) * src_w];
        for (int ox = 0; ox < dst_w; ox++) {
            uint16_t px = srow[xmap[ox]];
            line[ox * 2]     = px >> 8;
            line[ox * 2 + 1] = px & 0xFF;
        }
        set_window(dx, dy + oy, dx + dst_w - 1, dy + oy);  // one-row window at this dst row
        dc(true); cs(true);
        spi_write_blocking(LCD_SPI, line, dst_w * 2);
        cs(false);
    }
}