// PicoBoy Sega Master System / Game Gear front-end.
//
// Core: fhoedemakers/pico-smsplus (SMS Plus lineage), pinned in CMake.
// PicoBoy supplies the flash-backed ROM, shared-arena allocator, ST7789
// scanline streamer, controls, I2S audio, battery SRAM and .dat save-state UI.
// Only one emulator core is active at a time, matching the existing GB/NES/
// Atari architecture.

#include "sms_core.h"
#include "arena.h"
#include "flash.h"
#include "st7789.h"
#include "pins.h"
#include "buttons.h"
#include "ui.h"
#include "theme.h"
#include "settings.h"
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
static uint32_t s_alloc_count = 0;

static void sms_arena_reset(void) {
    s_apos = arena_base();
    s_aend = arena_base() + ARENA_WORK_BYTES;
    s_alloc_count = 0;

    printf("[SMS] arena base=%p work=%lu audio_tail=%lu\n",
           (void *)s_apos,
           (unsigned long)ARENA_WORK_BYTES,
           (unsigned long)ARENA_AUDIO_BYTES);
}

void *frens_f_malloc(size_t size) {
    if (!s_apos || !s_aend) {
        printf("[SMS] alloc refused: arena not initialized\n");
        return NULL;
    }

    size_t requested = size;
    size = (size + 3u) & ~3u;
    size_t free_before = (size_t)(s_aend - s_apos);

    if (free_before < size) {
        printf("[SMS] OOM alloc#%lu request=%lu aligned=%lu free=%lu\n",
               (unsigned long)(s_alloc_count + 1u),
               (unsigned long)requested,
               (unsigned long)size,
               (unsigned long)free_before);
        return NULL;
    }

    void *p = s_apos;
    s_apos += size;
    s_alloc_count++;

    printf("[SMS] alloc#%lu req=%lu used=%lu free=%lu ptr=%p\n",
           (unsigned long)s_alloc_count,
           (unsigned long)requested,
           (unsigned long)(s_apos - arena_base()),
           (unsigned long)(s_aend - s_apos),
           p);
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
// NATIVE:
//   160x144 -> 160x144, centered at 80,48. Every source pixel is one LCD pixel.
//
// 2X ZOOM:
//   center-crop 160x120 from the 160x144 GG viewport, then duplicate every
//   source pixel to an exact 2x2 LCD block -> 320x240. This fills the panel
//   without fractional scaling or blur. The tradeoff is 12 source lines
//   cropped from both the top and bottom.
enum {
    GG_DISPLAY_NATIVE = 0,
    GG_DISPLAY_ZOOM_2X = 1,
};

#define GG_NATIVE_DST_W    160
#define GG_NATIVE_DST_H    144
#define GG_NATIVE_DST_X    80
#define GG_NATIVE_DST_Y    48

#define GG_ZOOM_SRC_H      120
#define GG_ZOOM_CROP_Y     ((GG_NATIVE_H - GG_ZOOM_SRC_H) / 2)  // 12
#define GG_ZOOM_DST_W      320
#define GG_ZOOM_DST_H      240
#define GG_ZOOM_DST_X      0
#define GG_ZOOM_DST_Y      0

// Session-local by design: some games tolerate the 2x vertical crop better
// than others. Native is the correctness-first default on each game launch.
static volatile uint8_t s_gg_display_mode = GG_DISPLAY_NATIVE;

#define SMS_RING           4u
#define SMS_ROW_MAX_W      320u
#define SMS_ROW_BYTES      (SMS_ROW_MAX_W * 2u)

static uint8_t *s_ring = NULL; // SMS_RING * SMS_ROW_BYTES, allocated from arena
static volatile uint32_t s_wr = 0;
static volatile uint32_t s_rd = 0;
static volatile bool s_frame_done = false;
static volatile bool s_core1_run = false;
static volatile bool s_core1_idle = true;
static volatile bool s_core1_started = false;
static bool s_stream_this = false;
static bool s_boot_line_trace = false;

static inline uint8_t *ring_row(uint32_t index) {
    return s_ring + (index % SMS_RING) * SMS_ROW_BYTES;
}

static void __not_in_flash_func(sms_core1_stream)(void) {
    bool win = false;
    int src_row = 0;
    int width = SMS_NATIVE_W;
    int dx = SMS_DST_X;
    int dy = SMS_DST_Y;
    int dh = SMS_NATIVE_H;
    bool frame_gg_zoom = false;

    s_core1_idle = false;
    __sync_synchronize();
    s_core1_started = true;

    while (s_core1_run) {
        if (s_rd != s_wr) {
            if (!win) {
                // The overlay may change GG view mode while Core1 is parked.
                // Snapshot the mode when a new frame begins so geometry stays
                // constant for the whole RAMWR transaction.
                frame_gg_zoom = s_is_gg &&
                                (s_gg_display_mode == GG_DISPLAY_ZOOM_2X);

                if (!s_is_gg) {
                    width = SMS_NATIVE_W;
                    dx = SMS_DST_X;
                    dy = SMS_DST_Y;
                    dh = SMS_NATIVE_H;
                } else if (frame_gg_zoom) {
                    width = GG_ZOOM_DST_W;
                    dx = GG_ZOOM_DST_X;
                    dy = GG_ZOOM_DST_Y;
                    dh = GG_ZOOM_DST_H;
                } else {
                    width = GG_NATIVE_DST_W;
                    dx = GG_NATIVE_DST_X;
                    dy = GG_NATIVE_DST_Y;
                    dh = GG_NATIVE_DST_H;
                }

                st7789_stream_begin(dx, dy, width, dh);
                win = true;
                src_row = 0;
            }

            const uint8_t *row = ring_row(s_rd);

            if (frame_gg_zoom) {
                // Exact integer 2x vertical expansion: one emulated source row
                // becomes two identical LCD rows.
                st7789_stream_row(row, width);
                st7789_stream_row(row, width);
            } else {
                st7789_stream_row(row, width);
            }

            src_row++;
            __sync_synchronize();
            s_rd++;
        } else if (s_frame_done) {
            if (win) {
                st7789_stream_end();
                win = false;
            }
            s_frame_done = false; // acknowledge complete frame to core0
        } else {
            tight_loop_contents();
        }
    }

    if (win) st7789_stream_end();
    s_core1_idle = true;
}

// Called by upstream render.c after composing one palette-indexed VDP line.
// Like the NES full-resolution path, core0 only converts the line to panel 565;
// Core1 owns the SPI transaction and DMA waits.
void __not_in_flash_func(sms_render_line)(int line, const uint8_t *buffer) {
    if (!s_stream_this || !buffer || !s_ring) return;

    if (s_boot_line_trace && line < 8) {
        printf("[SMS] hook line=%d enter wr=%lu rd=%lu depth=%lu\n",
               line,
               (unsigned long)s_wr,
               (unsigned long)s_rd,
               (unsigned long)(s_wr - s_rd));
    }

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
        const bool zoom = (s_gg_display_mode == GG_DISPLAY_ZOOM_2X);

        if (!zoom) {
            if (line < GG_SRC_Y || line >= GG_SRC_Y + GG_NATIVE_H) return;
        } else {
            const int first = GG_SRC_Y + GG_ZOOM_CROP_Y;
            const int last  = first + GG_ZOOM_SRC_H;
            if (line < first || line >= last) return;
        }

        while ((uint32_t)(s_wr - s_rd) >= SMS_RING)
            tight_loop_contents();

        uint8_t *dst = ring_row(s_wr);
        const uint8_t *src = buffer + GG_SRC_X;

        if (!zoom) {
            // Pixel-perfect native Game Gear output.
            for (int x = 0; x < GG_NATIVE_W; x++) {
                uint16_t c = s_pal565[src[x] & 0x1Fu];
                dst[x * 2]     = (uint8_t)(c >> 8);
                dst[x * 2 + 1] = (uint8_t)c;
            }
        } else {
            // Exact integer 2x horizontal expansion. Each source pixel becomes
            // two adjacent RGB565 LCD pixels; Core1 duplicates the row vertically.
            for (int x = 0; x < GG_NATIVE_W; x++) {
                uint16_t c = s_pal565[src[x] & 0x1Fu];
                uint8_t hi = (uint8_t)(c >> 8);
                uint8_t lo = (uint8_t)c;
                int o = x * 4;
                dst[o + 0] = hi;
                dst[o + 1] = lo;
                dst[o + 2] = hi;
                dst[o + 3] = lo;
            }
        }
    }

    __sync_synchronize();
    s_wr++;

    if (s_boot_line_trace && line < 8) {
        printf("[SMS] hook line=%d queued wr=%lu rd=%lu depth=%lu\n",
               line,
               (unsigned long)s_wr,
               (unsigned long)s_rd,
               (unsigned long)(s_wr - s_rd));
    }
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
                             s_gg_display_mode == GG_DISPLAY_ZOOM_2X
                                 ? "2x Zoom" : "Native");
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
                s_gg_display_mode =
                    (s_gg_display_mode == GG_DISPLAY_NATIVE)
                        ? GG_DISPLAY_ZOOM_2X
                        : GG_DISPLAY_NATIVE;
                __sync_synchronize();
                redraw = true;
            }
        }

        if (ev & (1u << BTN_A)) {
            if (sel == 0) {
                ret = 0;
                break;

            } else if (sel == display_idx) {
                s_gg_display_mode =
                    (s_gg_display_mode == GG_DISPLAY_NATIVE)
                        ? GG_DISPLAY_ZOOM_2X
                        : GG_DISPLAY_NATIVE;
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
    if (s_rom_size < 0x4000u) {
        printf("[SMS] ROM rejected: raw size %lu < 16 KiB\n",
               (unsigned long)s_rom_size);
        return false;
    }

    const uint32_t flash_off = ROM_FLASH_OFFSET;
    uint8_t *rom = (uint8_t *)(uintptr_t)flash_ptr(flash_off);
    uint32_t size = s_rom_size;

    printf("[SMS] bind type=%s raw=%lu flash_off=0x%06lx xip=%p\n",
           s_is_gg ? "GG" : "SMS",
           (unsigned long)s_rom_size,
           (unsigned long)flash_off,
           (void *)rom);

    // Sega dumps may carry a 512-byte copier header.
    if ((size / 512u) & 1u) {
        if (size <= 512u) return false;
        rom += 512;
        size -= 512u;
    }

    uint32_t pages = size / 0x4000u;
    if (pages == 0 || pages > 255u) {
        printf("[SMS] ROM rejected: effective=%lu pages=%lu\n",
               (unsigned long)size, (unsigned long)pages);
        return false;
    }

    printf("[SMS] ROM effective=%lu pages=%lu header=%s end_off=0x%06lx\n",
           (unsigned long)size,
           (unsigned long)pages,
           (rom != flash_ptr(flash_off)) ? "512" : "none",
           (unsigned long)(flash_off + s_rom_size));

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
    // Native is the correctness-first default for every GG launch. Users can
    // opt into exact 2x center-crop zoom from the in-game overlay.
    s_gg_display_mode = GG_DISPLAY_NATIVE;

    printf("[SMSDBG3] sms_core_run HEADLESS-PROBE build\n");

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
     * Match upstream pico-smsplus startup exactly:
     *
     *   load_rom()
     *   system_init()
     *   system_reset()
     *   run frames
     *
     * system_init() performs the dynamic allocations and initializes each
     * subsystem. system_reset() then establishes the final CPU/VDP/SMS/render
     * power-on state and calls our system_load_sram() hook.
     *
     * Previously PicoBoy called system_load_sram() directly and skipped the
     * post-init system_reset().
     */
    printf("[SMS] system_init begin used=%lu free=%lu\n",
           (unsigned long)(s_apos - arena_base()),
           (unsigned long)(s_aend - s_apos));
    system_init(SMS_AUD_RATE);
    printf("[SMS] system_init done used=%lu free=%lu snd=%d buf=%d\n",
           (unsigned long)(s_apos - arena_base()),
           (unsigned long)(s_aend - s_apos),
           snd.enabled, snd.bufsize);

    printf("[SMS] system_reset begin\n");
    system_reset();
    printf("[SMS] system_reset done pc=%04x used=%lu free=%lu\n",
           (unsigned)(z80_get_pc() & 0xFFFFu),
           (unsigned long)(s_apos - arena_base()),
           (unsigned long)(s_aend - s_apos));

    {
        PAIR probe;
        probe.d = 0x11223344u;
        const uint8_t *raw = (const uint8_t *)&probe;
        printf("[SMS] endian PAIR bytes=%02x %02x %02x %02x "
               "b.l=%02x b.h=%02x w.l=%04x\n",
               raw[0], raw[1], raw[2], raw[3],
               probe.b.l, probe.b.h, probe.w.l);
    }

    /*
     * Decisive startup probe: execute one complete emulated frame with
     * rendering disabled BEFORE launching Core 1 or opening the PicoBoy I2S
     * output path. SMSPlus still runs VDP timing, Z80 execution and PSG
     * generation, but sms_render_line() is never called.
     *
     * If "headless frame done" prints, the SMSPlus core is alive and the
     * remaining fault is in display/Core1 integration. If it does not print,
     * Core1/LCD cannot be the cause.
     */
    printf("[SMSDBG3] headless frame begin pc=%04x rom0=%02x %02x %02x %02x\n",
           (unsigned)(z80_get_pc() & 0xFFFFu),
           cart.rom[0], cart.rom[1], cart.rom[2], cart.rom[3]);

    sms_frame(1);

    printf("[SMSDBG3] headless frame done pc=%04x snd=%d wr=%lu rd=%lu\n",
           (unsigned)(z80_get_pc() & 0xFFFFu),
           snd.enabled,
           (unsigned long)s_wr,
           (unsigned long)s_rd);

    // Return to a clean power-on state before the visible first frame.
    printf("[SMSDBG3] reset after headless probe\n");
    system_reset();
    printf("[SMSDBG3] reset after probe done pc=%04x\n",
           (unsigned)(z80_get_pc() & 0xFFFFu));

    // Four converted scanlines is enough to decouple core0 from SPI without a
    // framebuffer. SMS is 256x192 1:1. GG is either native 160x144 or an exact
    // integer 2x center-crop zoom at 320x240.
    s_ring = (uint8_t *)frens_f_malloc(SMS_RING * SMS_ROW_BYTES);
    if (!s_ring) {
        system_shutdown();
        pb_vdp_ptr = NULL;
        show_core_error("Out of RAM (video ring)");
        led_set_state(LED_IDLE);
        return;
    }
    memset(s_ring, 0, SMS_RING * SMS_ROW_BYTES);
    printf("[SMS] video ring ready used=%lu free=%lu\n",
           (unsigned long)(s_apos - arena_base()),
           (unsigned long)(s_aend - s_apos));

    printf("[SMS] audio open\n");
    snd_sms_open();
    printf("[SMS] audio ready\n");

    s_wr = s_rd = 0;
    s_frame_done = false;
    s_stream_this = false;
    s_core1_started = false;
    s_core1_run = true;
    s_core1_idle = false;
    printf("[SMS] core1 launch\n");
    multicore_launch_core1(sms_core1_stream);

    absolute_time_t core1_deadline = make_timeout_time_ms(100);
    while (!s_core1_started && !time_reached(core1_deadline))
        tight_loop_contents();

    printf("[SMS] core1 launched started=%d\n", s_core1_started ? 1 : 0);
    if (!s_core1_started) {
        s_core1_run = false;
        multicore_reset_core1();
        snd_sms_close();
        system_shutdown();
        s_ring = NULL;
        s_linebuf = NULL;
        pb_vdp_ptr = NULL;
        show_core_error("Core1 failed to start");
        led_set_state(LED_IDLE);
        return;
    }

    absolute_time_t fallback_next = make_timeout_time_us(16667);
    unsigned frame_counter = 0;
    bool quit = false;

    printf("[SMSDBG3] entering visible loop\n");

    while (!quit) {
        if (frame_counter == 0u) printf("[SMSDBG3] buttons_update begin\n");
        buttons_update();
        if (frame_counter == 0u) printf("[SMSDBG3] buttons_update done\n");
        uint16_t ev = buttons_pressed();

        if (ev & (1u << BTN_MENU)) {
            display_drain(); // core1 closes RAMWR, leaving panel safe for core0 UI
            int r = sms_overlay();
            st7789_fill(COL_BLACK); // clear overlay/pillarbox before next streamed frame
            if (r == 1) {
                quit = true;
                break;
            }
            buttons_update();
        }

        set_input(buttons_state());

        bool display_ready = (!s_frame_done && s_rd == s_wr);
        bool fixed_skip = false;
        if (g_settings.frameskip >= 0) {
            unsigned period = (unsigned)g_settings.frameskip + 1u;
            fixed_skip = ((frame_counter % period) != 0u);
        }
        frame_counter++;

        // Auto (-1) renders whenever Core1 is ready. Fixed N skips N of every
        // N+1 frames, and also drops rendering if display ever falls behind.
        bool skip_render = fixed_skip || !display_ready;
        s_stream_this = !skip_render;

        if (frame_counter == 1u) {
            s_boot_line_trace = true;
            printf("[SMS] frame0 begin render=%d pc=%04x rom0=%02x %02x %02x %02x\n",
                   s_stream_this ? 1 : 0,
                   (unsigned)(z80_get_pc() & 0xFFFFu),
                   cart.rom[0], cart.rom[1], cart.rom[2], cart.rom[3]);
        }

        sms_frame(skip_render ? 1 : 0);

        if (frame_counter == 1u) {
            s_boot_line_trace = false;
            printf("[SMS] frame0 done wr=%lu rd=%lu snd=%d pc=%04x\n",
                   (unsigned long)s_wr, (unsigned long)s_rd, snd.enabled,
                   (unsigned)(z80_get_pc() & 0xFFFFu));
        }

        if (s_stream_this)
            s_frame_done = true;

        // One SMS/GG frame is exactly 735 samples at the upstream 44.1-kHz /
        // 60-Hz contract. Blocking on the previous DMA makes audio the master
        // clock, matching PicoBoy's NES path.
        bool audio_paced = false;
        if (snd.enabled && snd.buffer[0] && snd.buffer[1])
            audio_paced = snd_sms_frame(snd.buffer[0], snd.buffer[1], snd.bufsize);

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
    s_linebuf = NULL;
    pb_vdp_ptr = NULL;
    led_set_state(LED_IDLE);
}