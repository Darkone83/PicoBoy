// PicoBoy Sega Master System / Game Gear front-end.
//
// Core: fhoedemakers/pico-smsplus (SMS Plus lineage), pinned in CMake.
// PicoBoy supplies the flash-backed ROM, shared-arena allocator, ST7789
// scanline streamer, controls, I2S audio, battery SRAM and .dat save-state UI.
// Only one emulator core is active at a time, matching the existing GB/NES/
// Atari architecture.

#include "sms_core.h"
#include "arena.h"
#include "adaptive_frameskip.h"
#include "flash.h"
#include "st7789.h"
#include "pins.h"
#include "buttons.h"
#include "ui.h"
#include "theme.h"
#include "settings.h"
#include "fps_overlay.h"
#include "led.h"
#include "audio.h"
#include "ff.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"

// Upstream SMSPlus core API. CMake adds the pinned smsplus directory to the
// include path and applies MB_SMS / LSB_FIRST only to these source files.
#include "shared.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

// One-line renderer/dummy buffer lives in the shared arena while this core is hot.
static uint8_t *s_linebuf = NULL;

// ---------------------------------------------------------------------------
// Launch configuration supplied by loader.c
// ---------------------------------------------------------------------------

static uint32_t s_rom_size = 0;
static bool     s_is_gg = false;
static char     s_save_path[96] = {0};
static char     s_state_path[96] = {0};

void sms_core_set_rom(uint32_t size, bool is_gamegear) {
    s_rom_size = size;
    s_is_gg = is_gamegear;
}

static void set_path(char dst[96], const char *src) {
    if (!src) src = "";
    snprintf(dst, 96, "%s", src);
}

void sms_core_set_save_path(const char *path)  { set_path(s_save_path, path); }
void sms_core_set_state_path(const char *path) { set_path(s_state_path, path); }

// ---------------------------------------------------------------------------
// Shared arena allocator expected by pico-smsplus
// ---------------------------------------------------------------------------

static uint8_t *s_apos = NULL;
static uint8_t *s_aend = NULL;

static void sms_arena_reset(void) {
    s_apos = arena_base();
    s_aend = arena_base() + ARENA_WORK_BYTES;
}

void *frens_f_malloc(size_t size) {
    if (!s_apos || !s_aend)
        return NULL;

    size = (size + 3u) & ~3u;
    if ((size_t)(s_aend - s_apos) < size)
        return NULL;

    void *p = s_apos;
    s_apos += size;
    return p;
}

// The arena is discarded as one unit when the core exits. Upstream frees its
// allocations in system_shutdown(), but individual frees intentionally do
// nothing here, like the NES bump allocator.
void frens_f_free(void *ptr) { (void)ptr; }

char unalChar(const char *adr) { return *adr; }

// vdp.h is mechanically patched at configure time so this pointer replaces the
// upstream permanent 16-KiB VDP .bss object.
extern t_vdp *pb_vdp_ptr;

// ---------------------------------------------------------------------------
// Palette callbacks used by render.c
// ---------------------------------------------------------------------------

static uint16_t s_pal565[PALETTE_SIZE];

static inline uint16_t rgb565_u8(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

void sms_palette_sync(int index) {
    if ((unsigned)index >= PALETTE_SIZE || !pb_vdp_ptr) return;
    uint8_t c = vdp.cram[index & 0x1F];
    unsigned r = (c & 0x03u) * 85u;
    unsigned g = ((c >> 2) & 0x03u) * 85u;
    unsigned b = ((c >> 4) & 0x03u) * 85u;
    s_pal565[index & 0x1F] = rgb565_u8(r, g, b);
}

void sms_palette_syncGG(int index) {
    if ((unsigned)index >= PALETTE_SIZE || !pb_vdp_ptr) return;
    int i = (index & 0x1F) << 1;
    uint8_t lo = vdp.cram[i];
    uint8_t hi = vdp.cram[i + 1];
    unsigned r = (lo & 0x0Fu) * 17u;
    unsigned g = ((lo >> 4) & 0x0Fu) * 17u;
    unsigned b = (hi & 0x0Fu) * 17u;
    s_pal565[index & 0x1F] = rgb565_u8(r, g, b);
}

// ---------------------------------------------------------------------------
// NES-style scanline ring -> Core1 ST7789 DMA
// ---------------------------------------------------------------------------

#define SMS_NATIVE_W       256
#define SMS_NATIVE_H       192
#define SMS_DST_X          32
#define SMS_DST_Y          24

#define GG_NATIVE_W        160
#define GG_NATIVE_H        144
#define GG_SRC_X           48
#define GG_SRC_Y           24

// Game Gear display modes.
//
// Native:
//   160x144 -> 160x144, centered at 80,48.
//
// Scaled:
//   160x144 -> 240x216, centered at 40,12.
//   Every two source pixels/rows become three using A, average(A,B), B.
//   The selection is persistent in g_settings.gg_scaled.
#define GG_NATIVE_DST_W    160
#define GG_NATIVE_DST_H    144
#define GG_NATIVE_DST_X    80
#define GG_NATIVE_DST_Y    48

#define GG_SCALED_DST_W    240
#define GG_SCALED_DST_H    216
#define GG_SCALED_DST_X    40
#define GG_SCALED_DST_Y    12
#define GG_SCALED_ROW_BYTES (GG_SCALED_DST_W * 2u)

#define SMS_RING           8u
#define SMS_ROW_MAX_W      256u
#define SMS_ROW_BYTES      (SMS_ROW_MAX_W * 2u)

static uint8_t *s_ring = NULL; // SMS_RING * SMS_ROW_BYTES, allocated from arena
static uint8_t *s_gg_scaled_prev = NULL; // 240px packed RGB565 scratch
static uint8_t *s_gg_scaled_cur  = NULL; // 240px packed RGB565 scratch
static volatile uint32_t s_wr = 0;
static volatile uint32_t s_rd = 0;
static volatile bool s_frame_done = false;
static volatile bool s_core1_run = false;
static volatile bool s_core1_idle = true;
static volatile bool s_core1_started = false;
static bool s_stream_this = false;

static inline uint8_t *ring_row(uint32_t index) {
    return s_ring + (index % SMS_RING) * SMS_ROW_BYTES;
}

static void __not_in_flash_func(sms_core1_stream)(void) {
    bool win = false;
    int width = SMS_NATIVE_W;
    int dx = SMS_DST_X;
    int dy = SMS_DST_Y;
    int dh = SMS_NATIVE_H;
    bool frame_gg_scaled = false;
    int out_y = 0;

    /*
     * s_send is the next ring slot Core1 intends to submit.
     * s_rd is the first slot Core0 may safely reuse.
     *
     * st7789_stream_row() waits for the PREVIOUS DMA and then starts the new
     * one. Therefore the just-submitted row is still owned by DMA when the
     * function returns. The old SMS/GG path advanced s_rd immediately and could
     * let Core0 overwrite that slot while SPI was still reading it.
     *
     * Keep one source row explicitly in-flight. A slot is released only after
     * the next DMA launch proves the previous row completed, or stream_end()
     * drains the final DMA at the end of the frame.
     */
    uint32_t s_send = s_rd;
    bool have_inflight = false;

    s_core1_idle = false;
    __sync_synchronize();
    s_core1_started = true;

    while (s_core1_run) {
        if (s_send != s_wr) {
            if (!win) {
                frame_gg_scaled = s_is_gg && (g_settings.gg_scaled != 0);

                if (!s_is_gg) {
                    width = SMS_NATIVE_W;
                    dx = SMS_DST_X;
                    dy = SMS_DST_Y;
                    dh = SMS_NATIVE_H;
                } else if (frame_gg_scaled) {
                    width = GG_SCALED_DST_W;
                    dx = GG_SCALED_DST_X;
                    dy = GG_SCALED_DST_Y;
                    dh = GG_SCALED_DST_H;
                } else {
                    width = GG_NATIVE_DST_W;
                    dx = GG_NATIVE_DST_X;
                    dy = GG_NATIVE_DST_Y;
                    dh = GG_NATIVE_DST_H;
                }

                st7789_stream_begin(dx, dy, width, dh);
                win = true;
                have_inflight = false;
                out_y = 0;
            }

            uint8_t *row = ring_row(s_send);
            if (g_settings.show_fps && out_y < 7)
                fps_overlay_row_be(row, width, out_y);
            out_y++;

            st7789_stream_row(row, width);

            // At this point the PREVIOUS source row is guaranteed complete.
            // Keep the current source row reserved until the next launch.
            if (have_inflight) {
                __sync_synchronize();
                s_rd = s_send;
            }

            s_send++;
            have_inflight = true;

        } else if (s_frame_done) {
            if (win) {
                // Drains the final DMA, so the last submitted source row is now
                // safe for Core0 to reuse as well.
                st7789_stream_end();
                win = false;
            }

            __sync_synchronize();
            s_rd = s_send;
            have_inflight = false;
            s_frame_done = false; // acknowledge complete frame to core0
            out_y = 0;

        } else {
            tight_loop_contents();
        }
    }

    if (win) {
        st7789_stream_end();
        s_rd = s_send;
    }
    s_core1_idle = true;
}

static inline uint16_t gg_avg565(uint16_t a, uint16_t b) {
    // Exact channel-wise rounded average, packed:
    //   floor((A+B)/2) per RGB565 field, plus one when that field's LSB differs.
    // This is pixel-identical to the previous extract/add/repack implementation.
    uint16_t x = (uint16_t)(a ^ b);
    return (uint16_t)((a & b) +
                      ((x & 0xF7DEu) >> 1) +
                      (x & 0x0821u));
}

static inline void gg_put565(uint8_t *dst, int x, uint16_t c) {
    dst[x * 2]     = (uint8_t)(c >> 8);
    dst[x * 2 + 1] = (uint8_t)c;
}

static void __not_in_flash_func(gg_build_scaled_row)(uint8_t *dst,
                                                      const uint8_t *src) {
    // 160 -> 240. Each non-overlapping pair becomes:
    //   A, average(A,B), B
    // so the scale has a stable 3-pixels-for-2-source-pixels cadence without
    // whole-pixel interpolation across every output sample.
    for (int sx = 0, ox = 0; sx < GG_NATIVE_W; sx += 2, ox += 3) {
        uint16_t a = s_pal565[src[sx] & 0x1Fu];
        uint16_t b = s_pal565[src[sx + 1] & 0x1Fu];
        gg_put565(dst, ox + 0, a);
        gg_put565(dst, ox + 1, gg_avg565(a, b));
        gg_put565(dst, ox + 2, b);
    }
}

static void __not_in_flash_func(gg_mix_scaled_rows)(uint8_t *dst,
                                                     const uint8_t *a,
                                                     const uint8_t *b) {
    for (int x = 0; x < GG_SCALED_DST_W; x++) {
        uint16_t ca = (uint16_t)((a[x * 2] << 8) | a[x * 2 + 1]);
        uint16_t cb = (uint16_t)((b[x * 2] << 8) | b[x * 2 + 1]);
        gg_put565(dst, x, gg_avg565(ca, cb));
    }
}

static void __not_in_flash_func(queue_packed_row)(const uint8_t *src,
                                                   unsigned bytes) {
    while ((uint32_t)(s_wr - s_rd) >= SMS_RING)
        tight_loop_contents();

    uint8_t *dst = ring_row(s_wr);
    memcpy(dst, src, bytes);
    __sync_synchronize();
    s_wr++;
}

// Called by upstream render.c after composing one palette-indexed VDP line.
// Like the NES full-resolution path, core0 only converts the line to panel 565;
// Core1 owns the SPI transaction and DMA waits.
void __not_in_flash_func(sms_render_line)(int line, const uint8_t *buffer) {
    if (!s_stream_this || !buffer || !s_ring) return;


    if (!s_is_gg) {
        if (line < 0 || line >= SMS_NATIVE_H) return;

        while ((uint32_t)(s_wr - s_rd) >= SMS_RING)
            tight_loop_contents();

        uint8_t *dst = ring_row(s_wr);
        for (int x = 0; x < SMS_NATIVE_W; x++) {
            uint16_t c = s_pal565[buffer[x] & 0x1Fu];
            dst[x * 2]     = (uint8_t)(c >> 8);
            dst[x * 2 + 1] = (uint8_t)c;
        }
    } else {
        const uint8_t *src = buffer + GG_SRC_X;

        if (line < GG_SRC_Y || line >= GG_SRC_Y + GG_NATIVE_H) return;

        if (g_settings.gg_scaled) {
            if (!s_gg_scaled_prev || !s_gg_scaled_cur) return;

            int sy = line - GG_SRC_Y;

            if ((sy & 1) == 0) {
                // First row of a pair: output A and retain it.
                gg_build_scaled_row(s_gg_scaled_prev, src);
                queue_packed_row(s_gg_scaled_prev, GG_SCALED_ROW_BYTES);
            } else {
                // Second row B completes the 3-for-2 cadence:
                // A was already queued, now queue average(A,B), then B.
                gg_build_scaled_row(s_gg_scaled_cur, src);

                while ((uint32_t)(s_wr - s_rd) >= SMS_RING)
                    tight_loop_contents();
                uint8_t *mid = ring_row(s_wr);
                gg_mix_scaled_rows(mid, s_gg_scaled_prev, s_gg_scaled_cur);
                __sync_synchronize();
                s_wr++;

                queue_packed_row(s_gg_scaled_cur, GG_SCALED_ROW_BYTES);
            }
            return;
        }

        while ((uint32_t)(s_wr - s_rd) >= SMS_RING)
            tight_loop_contents();

        uint8_t *dst = ring_row(s_wr);
        for (int x = 0; x < GG_NATIVE_W; x++) {
            uint16_t c = s_pal565[src[x] & 0x1Fu];
            dst[x * 2]     = (uint8_t)(c >> 8);
            dst[x * 2 + 1] = (uint8_t)c;
        }

        __sync_synchronize();
        s_wr++;
        return;
    }
    __sync_synchronize();
    s_wr++;

}

static void display_drain(void) {
    while (s_frame_done || s_rd != s_wr)
        tight_loop_contents();
}

// ---------------------------------------------------------------------------
// Battery SRAM hook used by upstream system_reset()
// ---------------------------------------------------------------------------

void system_load_sram(void) {
    if (!s_save_path[0] || !sms.sram) return;

    FIL f;
    if (f_open(&f, s_save_path, FA_READ) == FR_OK) {
        UINT br = 0;
        f_read(&f, sms.sram, SRAMSIZEBYTES, &br);
        f_close(&f);
        // sms_reset() already zeroed the full SRAM, so a shorter legacy save is
        // safely zero-filled in the unread tail.
    }
}

static void save_battery_sram(void) {
    if (!s_save_path[0] || !sms.sram || !sms.save) return;

    led_set_state(LED_SD_BUSY);
    FIL f;
    if (f_open(&f, s_save_path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        UINT bw = 0;
        f_write(&f, sms.sram, SRAMSIZEBYTES, &bw);
        f_close(&f);
    }
    led_set_state(LED_RUNNING);
}

// ---------------------------------------------------------------------------
// PicoBoy .dat save-state wrapper around upstream SMSPlus state serializer
// ---------------------------------------------------------------------------

#define SMS_STATE_MAGIC   0x534D4250u // 'PBMS' on disk
#define SMS_STATE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t  game_gear;
    uint8_t  reserved;
    uint32_t rom_size;
} sms_state_header_t;

static bool sms_state_exists(void) {
    if (!s_state_path[0]) return false;
    FILINFO fno;
    return f_stat(s_state_path, &fno) == FR_OK;
}

static bool save_sms_state(void) {
    if (!s_state_path[0]) return false;

    FIL f;
    if (f_open(&f, s_state_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;

    sms_state_header_t h = {
        .magic = SMS_STATE_MAGIC,
        .version = SMS_STATE_VERSION,
        .game_gear = s_is_gg ? 1u : 0u,
        .reserved = 0,
        .rom_size = s_rom_size,
    };

    UINT bw = 0;
    bool ok = (f_write(&f, &h, sizeof h, &bw) == FR_OK && bw == sizeof h);
    if (ok) ok = system_save_state(&f);
    f_close(&f);
    return ok;
}

static bool load_sms_state(void) {
    if (!s_state_path[0]) return false;

    FIL f;
    if (f_open(&f, s_state_path, FA_READ) != FR_OK)
        return false;

    sms_state_header_t h;
    UINT br = 0;
    bool ok = (f_read(&f, &h, sizeof h, &br) == FR_OK && br == sizeof h);
    ok = ok && h.magic == SMS_STATE_MAGIC && h.version == SMS_STATE_VERSION;
    ok = ok && h.game_gear == (uint8_t)(s_is_gg ? 1 : 0) && h.rom_size == s_rom_size;
    if (ok) ok = system_load_state(&f);
    f_close(&f);
    return ok;
}

// ---------------------------------------------------------------------------
// In-game overlay -- same PicoBoy controls/semantics as NES/GB
// ---------------------------------------------------------------------------

static int sms_overlay(void) {
    static const char *const labels_sms[] = {
        "Resume", "Brightness", "Volume", "Frame Skip",
        "Save State", "Load State", "Quit"
    };
    static const char *const labels_gg[] = {
        "Resume", "Brightness", "Volume", "Frame Skip", "GG Display",
        "Save State", "Load State", "Quit"
    };

    const char *const *labels = s_is_gg ? labels_gg : labels_sms;
    const int N = s_is_gg
        ? (int)(sizeof labels_gg / sizeof labels_gg[0])
        : (int)(sizeof labels_sms / sizeof labels_sms[0]);

    const int display_idx = s_is_gg ? 4 : -1;
    const int save_idx    = s_is_gg ? 5 : 4;
    const int load_idx    = s_is_gg ? 6 : 5;
    const int quit_idx    = s_is_gg ? 7 : 6;

    int sel = 0;
    int ret = 0;
    bool dirty = false;
    bool redraw = true;
    const bool have_path = (s_state_path[0] != '\0');
    bool have_saved = sms_state_exists();
    char val[20];
    int title_off = 0, title_hold = 20, title_tick = 0;

    while (true) {
        if (redraw) {
            st7789_fill(g_theme->bg);
            ui_pause_header(ui_now_playing(), title_off);

            for (int i = 0; i < N; i++) {
                int y = 40 + i * 20;
                bool on = (i == sel);
                bool dimmed = (i == save_idx && !have_path) ||
                              (i == load_idx && !have_saved);
                uint16_t fg = on ? g_theme->sel_fg :
                              (dimmed ? g_theme->footer_fg : g_theme->item_fg);
                uint16_t bg = on ? g_theme->sel_bg : g_theme->bg;

                if (on) ui_fill_pill(8, y - 3, LCD_W - 16, 18, bg);

                char lbl[24];
                snprintf(lbl, sizeof lbl, "%c %s", on ? '>' : ' ', labels[i]);
                st7789_draw_string(16, y, lbl, fg, bg, 1);

                val[0] = '\0';
                if (i == 1) {
                    snprintf(val, sizeof val, "%u%%", g_settings.lcd_brightness);
                } else if (i == 2) {
                    snprintf(val, sizeof val, "%u%%", g_settings.volume);
                } else if (i == 3) {
                    if (g_settings.frameskip < 0) snprintf(val, sizeof val, "Auto");
                    else snprintf(val, sizeof val, "%d", g_settings.frameskip);
                } else if (i == display_idx) {
                    snprintf(val, sizeof val, "%s",
                             g_settings.gg_scaled ? "Scaled" : "Native");
                }

                if (val[0]) st7789_draw_string(180, y, val, fg, bg, 1);
            }

            ui_footer("D-pad move/adjust  A select  B resume");
            redraw = false;
        }

        if (++title_tick >= 2) {
            title_tick = 0;
            if (title_hold > 0) title_hold--;
            else title_off++;
            ui_pause_title(ui_now_playing(), title_off);
        }

        buttons_update();
        uint16_t ev = buttons_pressed();

        if (ev & ((1u << BTN_B) | (1u << BTN_MENU))) { ret = 0; break; }
        if (ev & (1u << BTN_UP))   { sel = (sel + N - 1) % N; redraw = true; }
        if (ev & (1u << BTN_DOWN)) { sel = (sel + 1) % N;     redraw = true; }

        if (ev & ((1u << BTN_LEFT) | (1u << BTN_RIGHT))) {
            int d = (ev & (1u << BTN_RIGHT)) ? +1 : -1;

            if (sel == 1) {
                int b = (int)g_settings.lcd_brightness + d * 5;
                if (b < 10) b = 10;
                if (b > 100) b = 100;
                g_settings.lcd_brightness = (uint8_t)b;
                st7789_backlight_level(g_settings.lcd_brightness);
                dirty = true;
                redraw = true;

            } else if (sel == 2) {
                int v = (int)g_settings.volume + d * 5;
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                g_settings.volume = (uint8_t)v;
                dirty = true;
                redraw = true;

            } else if (sel == 3) {
                int f = (int)g_settings.frameskip + d;
                if (f < -1) f = 5;
                if (f > 5)  f = -1;
                g_settings.frameskip = (int8_t)f;
                dirty = true;
                redraw = true;

            } else if (sel == display_idx) {
                g_settings.gg_scaled ^= 1u;
                dirty = true;
                __sync_synchronize();
                redraw = true;
            }
        }

        if (ev & (1u << BTN_A)) {
            if (sel == 0) {
                ret = 0;
                break;

            } else if (sel == display_idx) {
                g_settings.gg_scaled ^= 1u;
                dirty = true;
                __sync_synchronize();
                redraw = true;

            } else if (sel == save_idx && have_path) {
                led_set_state(LED_SD_BUSY);
                bool ok = save_sms_state();
                led_set_state(LED_RUNNING);
                if (ok) have_saved = true;
                st7789_fill_rect(16, 204, LCD_W - 32, 10, g_theme->bg);
                st7789_draw_string(16, 204, ok ? "State saved" : "Save failed",
                                   ok ? g_theme->ok : g_theme->err,
                                   g_theme->bg, 1);

            } else if (sel == load_idx && have_saved) {
                led_set_state(LED_SD_BUSY);
                bool ok = load_sms_state();
                led_set_state(LED_RUNNING);
                if (ok) {
                    ret = 0;
                    break;
                }
                st7789_fill_rect(16, 204, LCD_W - 32, 10, g_theme->bg);
                st7789_draw_string(16, 204, "No state / load failed",
                                   g_theme->err, g_theme->bg, 1);

            } else if (sel == quit_idx) {
                ret = 1;
                break;
            }
        }

        sleep_ms(15);
    }

    if (dirty) settings_save();
    return ret;
}

// ---------------------------------------------------------------------------
// Core loop
// ---------------------------------------------------------------------------

static void set_input(uint16_t held) {
    input.pad[0] = 0;
    input.pad[1] = 0;
    input.system = 0;

    if (held & (1u << BTN_UP))    input.pad[0] |= INPUT_UP;
    if (held & (1u << BTN_DOWN))  input.pad[0] |= INPUT_DOWN;
    if (held & (1u << BTN_LEFT))  input.pad[0] |= INPUT_LEFT;
    if (held & (1u << BTN_RIGHT)) input.pad[0] |= INPUT_RIGHT;
    if (held & (1u << BTN_A))     input.pad[0] |= INPUT_BUTTON1;
    if (held & (1u << BTN_B))     input.pad[0] |= INPUT_BUTTON2;

    // The same physical Start key becomes the console's real system key:
    // Game Gear START, or Master System PAUSE (an NMI on original hardware).
    if (held & (1u << BTN_START))
        input.system |= s_is_gg ? INPUT_START : INPUT_PAUSE;
}

static bool bind_staged_rom(void) {
    if (s_rom_size < 0x4000u)
        return false;

    const uint32_t flash_off = ROM_FLASH_OFFSET;
    uint8_t *rom = (uint8_t *)(uintptr_t)flash_ptr(flash_off);
    uint32_t size = s_rom_size;


    // Sega dumps may carry a 512-byte copier header.
    if ((size / 512u) & 1u) {
        if (size <= 512u) return false;
        rom += 512;
        size -= 512u;
    }

    uint32_t pages = size / 0x4000u;
    if (pages == 0 || pages > 255u)
        return false;


    // Set type BEFORE BMP_WIDTH/BMP_HEIGHT are evaluated. Upstream loadrom.c
    // currently does this in the opposite order, which can leave the first GG
    // launch with SMS bitmap dimensions.
    cart.type = s_is_gg ? TYPE_GG : TYPE_SMS;
    cart.rom = rom;
    cart.pages = (uint8)pages;

    sms.use_fm = 0;                  // YM2413 is intentionally not compiled on RP2040
    sms.country = TYPE_OVERSEAS;
    sms.dummy = s_linebuf;

    bitmap.data = s_linebuf;
    bitmap.width = BMP_WIDTH;
    bitmap.height = BMP_HEIGHT;
    bitmap.pitch = BMP_WIDTH;
    bitmap.depth = 8;
    return true;
}

static void show_core_error(const char *msg) {
    st7789_fill(g_theme->bg);
    ui_header(s_is_gg ? "Game Gear" : "Master System");
    st7789_draw_string(8, 60, msg, g_theme->err, g_theme->bg, 1);
    ui_footer("B back");
    led_set_state(LED_ERROR);
    while (true) {
        buttons_update();
        if (buttons_pressed() & (1u << BTN_B)) break;
        sleep_ms(15);
    }
}

void sms_core_run(void) {
    if (!s_rom_size) {
        show_core_error("No staged ROM size");
        led_set_state(LED_IDLE);
        return;
    }

    sms_arena_reset();

    // Move the VDP's large embedded VRAM context and the renderer's single
    // scanline into the shared arena before upstream system_init() first touches
    // them. Binding the ROM first also means upstream's initial reset/render
    // pass sees the correct SMS/GG personality immediately.
    pb_vdp_ptr = (t_vdp *)frens_f_malloc(sizeof(t_vdp));
    s_linebuf = (uint8_t *)frens_f_malloc(SMS_WIDTH);
    if (!pb_vdp_ptr || !s_linebuf) {
        pb_vdp_ptr = NULL;
        s_linebuf = NULL;
        show_core_error("Out of RAM (SMS core)");
        led_set_state(LED_IDLE);
        return;
    }
    memset(pb_vdp_ptr, 0, sizeof(t_vdp));
    memset(s_linebuf, 0, SMS_WIDTH);

    if (!bind_staged_rom()) {
        pb_vdp_ptr = NULL;
        s_linebuf = NULL;
        show_core_error("ROM load failed");
        led_set_state(LED_IDLE);
        return;
    }

    led_set_state(LED_RUNNING);
    st7789_fill(COL_BLACK);

    /*
     * Match upstream pico-smsplus startup:
     *   load/bind ROM -> system_init() -> system_reset() -> run frames.
     *
     * system_reset() establishes the CPU/VDP/SMS/render power-on state and
     * calls the PicoBoy battery-SRAM load hook.
     */
    system_init(SMS_AUD_RATE);
    system_reset();

    // Eight converted scanlines, matching the NES full-resolution streamer,
    // decouple core0 from SPI. GG also gets two 240-pixel scratch rows for the
    // optional persistent Scaled mode; no full framebuffer is added.
    s_ring = (uint8_t *)frens_f_malloc(SMS_RING * SMS_ROW_BYTES);
    if (s_is_gg) {
        s_gg_scaled_prev = (uint8_t *)frens_f_malloc(GG_SCALED_ROW_BYTES);
        s_gg_scaled_cur  = (uint8_t *)frens_f_malloc(GG_SCALED_ROW_BYTES);
    } else {
        s_gg_scaled_prev = NULL;
        s_gg_scaled_cur  = NULL;
    }

    if (!s_ring || (s_is_gg && (!s_gg_scaled_prev || !s_gg_scaled_cur))) {
        system_shutdown();
        pb_vdp_ptr = NULL;
        s_ring = NULL;
        s_gg_scaled_prev = NULL;
        s_gg_scaled_cur = NULL;
        show_core_error("Out of RAM (video ring)");
        led_set_state(LED_IDLE);
        return;
    }
    memset(s_ring, 0, SMS_RING * SMS_ROW_BYTES);
    if (s_gg_scaled_prev) memset(s_gg_scaled_prev, 0, GG_SCALED_ROW_BYTES);
    if (s_gg_scaled_cur)  memset(s_gg_scaled_cur,  0, GG_SCALED_ROW_BYTES);

    snd_sms_open();

    s_wr = s_rd = 0;
    s_frame_done = false;
    s_stream_this = false;
    s_core1_started = false;
    s_core1_run = true;
    s_core1_idle = false;
    multicore_launch_core1(sms_core1_stream);

    absolute_time_t core1_deadline = make_timeout_time_ms(100);
    while (!s_core1_started && !time_reached(core1_deadline))
        tight_loop_contents();

    if (!s_core1_started) {
        s_core1_run = false;
        multicore_reset_core1();
        snd_sms_close();
        system_shutdown();
        s_ring = NULL;
        s_gg_scaled_prev = NULL;
        s_gg_scaled_cur = NULL;
        s_linebuf = NULL;
        pb_vdp_ptr = NULL;
        show_core_error("Core1 failed to start");
        led_set_state(LED_IDLE);
        return;
    }

    absolute_time_t fallback_next = make_timeout_time_us(16667);
    unsigned frame_counter = 0;
    adaptive_frameskip_t auto_fs;
    adaptive_frameskip_reset(&auto_fs);
    fps_overlay_reset();
    bool quit = false;

    while (!quit) {
        buttons_update();
        uint16_t ev = buttons_pressed();

        if (ev & (1u << BTN_MENU)) {
            display_drain(); // core1 closes RAMWR, leaving panel safe for core0 UI
            int r = sms_overlay();
            st7789_fill(COL_BLACK); // clear overlay/pillarbox before next streamed frame
            if (r == 1) {
                quit = true;
                break;
            }
            adaptive_frameskip_reset(&auto_fs);
            fps_overlay_reset();
            buttons_update();
        }

        set_input(buttons_state());

        bool display_ready = (!s_frame_done && s_rd == s_wr);
        const bool auto_mode = (g_settings.frameskip < 0);

        bool render_request;
        if (auto_mode) {
            render_request = adaptive_frameskip_should_render(&auto_fs);
        } else {
            unsigned period = (unsigned)g_settings.frameskip + 1u;
            render_request = ((frame_counter % period) == 0u);
        }
        frame_counter++;

        // sms_frame(skip=1) bypasses SMS Plus rendering while CPU/VDP timing and
        // audio continue. Auto therefore reduces real core work when pressured.
        bool display_blocked = render_request && !display_ready;
        bool skip_render = !render_request || !display_ready;
        s_stream_this = !skip_render;

        sms_frame(skip_render ? 1 : 0);
        if (g_settings.show_fps) fps_overlay_frame();

        if (s_stream_this)
            s_frame_done = true;

        bool audio_paced = false;
        if (snd.enabled && snd.buffer[0] && snd.buffer[1])
            audio_paced = snd_sms_frame(snd.buffer[0], snd.buffer[1], snd.bufsize);

        if (auto_mode) {
            adaptive_frameskip_feedback(
                &auto_fs,
                adaptive_frameskip_signal(display_blocked,
                                          audio_paced ? snd_dma_wait_us() : UINT32_MAX));
        }

        if (!audio_paced) {
            sleep_until(fallback_next);
            fallback_next = delayed_by_us(fallback_next, 16667);
            if (to_us_since_boot(fallback_next) < to_us_since_boot(get_absolute_time()))
                fallback_next = make_timeout_time_us(16667);
        }
    }

    display_drain();
    s_core1_run = false;
    while (!s_core1_idle) tight_loop_contents();
    multicore_reset_core1();

    save_battery_sram();
    snd_sms_close();
    system_shutdown();

    s_ring = NULL;
    s_gg_scaled_prev = NULL;
    s_gg_scaled_cur = NULL;
    s_linebuf = NULL;
    pb_vdp_ptr = NULL;
    led_set_state(LED_IDLE);
}