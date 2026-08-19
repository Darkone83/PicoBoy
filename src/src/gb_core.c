#include "gb_core.h"
#include "st7789.h"
#include "buttons.h"
#include "led.h"
#include "ui.h"
#include "theme.h"
#include "flash.h"
#include "pins.h"
#include "audio.h"
#include "settings.h"
#include "arena.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <string.h>
#include <stdio.h>
#include "ff.h"            // .srm cart-RAM saves

#include "minigb_apu.h"  // declares audio_read/audio_write/audio_callback/audio_init
#define ENABLE_SOUND 1   // route GB sound-register access to minigb_apu
#include "peanut_gb.h"   // single-header core; included in exactly this one TU
#include "gbcolors.h"    // GBC-bootstrap preset palettes + per-game auto-assignment

// Dual-core. core0 runs the emulator + input, core1 blits the frame to
// the panel. Double-buffered ping-pong over the inter-core FIFO: core0 fills one
// framebuffer while core1 pushes the other, so emulation overlaps the (SPI-bound)
// blit instead of running after it.

#define GB_W 160
#define GB_H 144
// Fit-to-height, aspect-preserved: 160x144 -> 266x240, pillarboxed (~27px bars).
#define GB_DST_H LCD_H                      // 240
#define GB_DST_W (GB_W * LCD_H / GB_H)      // 266
#define GB_DST_X ((LCD_W - GB_DST_W) / 2)   // 27
#define GB_DST_Y 0

// Native GB frame rate: 4194304 Hz / 70224 cycles = 59.7275 Hz -> 16742 us period.
// Pace the emulation loop to this so the game runs at correct speed regardless of
// how fast the blit finishes. (When 2c audio lands, the I2S drain becomes the real
// clock and this pacer becomes a fallback.)
#define GB_FRAME_US 16742

#define CART_RAM_BYTES (32u * 1024u)         // max GB cart RAM
static struct gb_s gb;
static uint8_t  *cart_ram;                    // -> arena (SML uses none)
static uint16_t (*fb)[GB_W * GB_H];           // -> arena: double buffer (2 * ~45 KB)
static uint16_t *g_draw_fb;                   // buffer the emulator draws into now
static const uint8_t *g_rom;                 // -> XIP ROM window (flash.h ROM_FLASH_OFFSET)

static char g_save_path[96];                 // .srm path, or "" to disable saves
void gb_set_save_path(const char *path) {
    if (path) { strncpy(g_save_path, path, sizeof g_save_path - 1); g_save_path[sizeof g_save_path - 1] = '\0'; }
    else g_save_path[0] = '\0';
}

static char g_state_path[96];                // .dat save-state path, or "" to disable
void gb_set_state_path(const char *path) {
    if (path) { strncpy(g_state_path, path, sizeof g_state_path - 1); g_state_path[sizeof g_state_path - 1] = '\0'; }
    else g_state_path[0] = '\0';
}

static size_t g_save_size;                   // cart-RAM size (from ROM header, capped); set in gb_core_run

// Active colour LUT: [3 source layers: OBJ0/OBJ1/BG][4 shades]. Filled by
// apply_palette() from gbcolors.h -- either auto-assigned per game (settings
// palette 0) or a manual preset (1..13). Each GB pixel carries its source layer
// in LCD_PALETTE_ALL and its shade in LCD_COLOUR.
static palette_t g_palette;

// Refill g_palette from the current Volume/Palette setting. Auto (0) hashes the
// loaded ROM for GBC-bootstrap colour; 1..13 select a fixed preset.
static void apply_palette(void) {
    if (g_settings.palette == 0) {
        char title[16];
        auto_assign_palette(g_palette, gb_colour_hash(&gb), gb_get_rom_name(&gb, title));
    } else {
        manual_assign_palette(g_palette, (uint8_t)(g_settings.palette - 1));
    }
}

// ---- Peanut-GB callbacks ----
static uint8_t rom_read(struct gb_s *g, const uint_fast32_t addr) {
    (void)g; return g_rom[addr];
}
static uint8_t ram_read(struct gb_s *g, const uint_fast32_t addr) {
    (void)g; return cart_ram[addr];
}
static void ram_write(struct gb_s *g, const uint_fast32_t addr, const uint8_t val) {
    (void)g; cart_ram[addr] = val;
}
static void gb_err(struct gb_s *g, const enum gb_error_e err, const uint16_t addr) {
    (void)g; (void)err; (void)addr;   // non-fatal; keep running
}
static void draw_line(struct gb_s *g, const uint8_t *pixels, const uint_fast8_t line) {
    (void)g;
    uint16_t *row = &g_draw_fb[(uint32_t)line * GB_W];
    for (int x = 0; x < GB_W; x++)
        row[x] = g_palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & LCD_COLOUR];
}

// Interlace: on the breadboard the SPI can't push the full upscaled frame in one
// GB period, so we refresh alternating destination rows each frame (~half the SPI
// traffic). Set false for progressive (zero shimmer) once the PCB's faster SPI can
// afford the full frame. 2e's overlay can flip this at runtime.
volatile bool g_gb_interlace = true;

// Set by core0 when it hands core1 a frame; cleared by core1 when the blit is done.
// core0 polls this and only hands over a new frame when core1 is idle -- so a slow
// blit drops video frames instead of stalling core0 (which would starve the audio).
static volatile bool g_core1_busy = false;

// Core1: dedicated display. Blocks for a buffer index from core0, blits it, then
// clears the busy flag. Owns SPI0 during gameplay (core0 doesn't touch the panel).
static void core1_display(void) {
    int field = 0;
    while (true) {
        uint32_t idx = multicore_fifo_pop_blocking();
        if (g_gb_interlace) {
            st7789_blit_scaled_field(GB_DST_X, GB_DST_Y, GB_DST_W, GB_DST_H,
                                     fb[idx], GB_W, GB_H, field);
            field ^= 1;                      // alternate even/odd rows each frame
        } else {
            st7789_blit_scaled(GB_DST_X, GB_DST_Y, GB_DST_W, GB_DST_H,
                               fb[idx], GB_W, GB_H);
        }
        g_core1_busy = false;                // done -- core0 may hand over the next frame
    }
}

static void poll_input(void) {
    uint16_t s = buttons_state();
    uint8_t j = 0xFF;   // GB joypad is active-low
    if (s & (1u << BTN_A))      j &= ~JOYPAD_A;
    if (s & (1u << BTN_B))      j &= ~JOYPAD_B;
    if (s & (1u << BTN_SELECT)) j &= ~JOYPAD_SELECT;
    if (s & (1u << BTN_START))  j &= ~JOYPAD_START;
    if (s & (1u << BTN_RIGHT))  j &= ~JOYPAD_RIGHT;
    if (s & (1u << BTN_LEFT))   j &= ~JOYPAD_LEFT;
    if (s & (1u << BTN_UP))     j &= ~JOYPAD_UP;
    if (s & (1u << BTN_DOWN))   j &= ~JOYPAD_DOWN;
    gb.direct.joypad = j;
}

static void show_error(const char *msg) {
    st7789_fill(g_theme->bg);
    ui_header("Run GB");
    st7789_draw_string(12, 90,  msg,                      g_theme->err,       g_theme->bg, 1);
    st7789_draw_string(12, 110, "Flash a ROM .uf2 first", g_theme->footer_fg, g_theme->bg, 1);
    ui_footer("B back");
    led_set_count(2);              // blink code 2: no ROM staged
    led_set_state(LED_ERROR);
    while (true) {
        buttons_update();
        if (buttons_pressed() & (1u << BTN_B)) break;
        sleep_ms(15);
    }
    led_set_state(LED_IDLE);
}

// ---- save states ---------------------------------------------------------
// File = header + the whole gb_s snapshot + cart RAM. The gb_s holds function
// pointers (valid only this session), so on load we snapshot the live ones and
// restore them after reading. State files are build-specific: the gb_size guard
// rejects a snapshot from firmware with a different struct layout.
#define STATE_MAGIC   0x54534250u   // 'PBST'
#define STATE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t gb_size;     // sizeof(struct gb_s) at save time
    uint32_t ram_size;    // cart RAM bytes that follow the struct
} state_hdr_t;

static bool save_state(void) {
    if (!g_state_path[0]) return false;
    FIL f;
    if (f_open(&f, g_state_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    state_hdr_t h = { STATE_MAGIC, STATE_VERSION, 0,
                      (uint32_t)sizeof(struct gb_s), (uint32_t)g_save_size };
    UINT bw = 0;
    bool ok = (f_write(&f, &h, sizeof h, &bw) == FR_OK && bw == sizeof h);
    ok = ok && (f_write(&f, &gb, sizeof gb, &bw) == FR_OK && bw == sizeof gb);
    if (ok && g_save_size)
        ok = ok && (f_write(&f, cart_ram, (UINT)g_save_size, &bw) == FR_OK);
    f_close(&f);
    return ok;
}

static bool load_state(void) {
    if (!g_state_path[0]) return false;
    FIL f;
    if (f_open(&f, g_state_path, FA_READ) != FR_OK) return false;

    state_hdr_t h;
    UINT br = 0;
    bool ok = (f_read(&f, &h, sizeof h, &br) == FR_OK && br == sizeof h
               && h.magic == STATE_MAGIC && h.version == STATE_VERSION
               && h.gb_size == (uint32_t)sizeof(struct gb_s));

    if (ok) {
        // Snapshot the live (valid) pointers before the read clobbers them.
        // __typeof__ keeps each local's type identical to the struct member.
        __typeof__(gb.gb_rom_read)           p_rom  = gb.gb_rom_read;
        __typeof__(gb.gb_cart_ram_read)      p_cr   = gb.gb_cart_ram_read;
        __typeof__(gb.gb_cart_ram_write)     p_cw   = gb.gb_cart_ram_write;
        __typeof__(gb.gb_error)              p_err  = gb.gb_error;
        __typeof__(gb.gb_serial_tx)          p_stx  = gb.gb_serial_tx;
        __typeof__(gb.gb_serial_rx)          p_srx  = gb.gb_serial_rx;
        __typeof__(gb.gb_bootrom_read)       p_boot = gb.gb_bootrom_read;
        __typeof__(gb.display.lcd_draw_line) p_ld   = gb.display.lcd_draw_line;
        void                                *p_priv = gb.direct.priv;

        ok = (f_read(&f, &gb, sizeof gb, &br) == FR_OK && br == sizeof gb);

        gb.gb_rom_read           = p_rom;
        gb.gb_cart_ram_read      = p_cr;
        gb.gb_cart_ram_write     = p_cw;
        gb.gb_error              = p_err;
        gb.gb_serial_tx          = p_stx;
        gb.gb_serial_rx          = p_srx;
        gb.gb_bootrom_read       = p_boot;
        gb.display.lcd_draw_line = p_ld;
        gb.direct.priv           = p_priv;

        if (ok && h.ram_size) {
            UINT want = h.ram_size > CART_RAM_BYTES ? (UINT)CART_RAM_BYTES : (UINT)h.ram_size;
            f_read(&f, cart_ram, want, &br);
        }
    }
    f_close(&f);
    return ok;
}

// True if a save-state file currently exists for this game (so the overlay can
// grey out Load State until there is something to load).
static bool gb_state_exists(void) {
    if (!g_state_path[0]) return false;
    FILINFO fi;
    return f_stat(g_state_path, &fi) == FR_OK;
}

// In-game pause overlay. Runs on core0 while core1 is parked in its FIFO wait
// (so core0 owns the SPI bus). Edits settings live; persists on exit if changed.
// Returns 1 to quit to the menu, 0 to resume play.
static int gb_overlay(void) {
    static const char *const labels[] = {
        "Resume", "Brightness", "Volume", "Palette",
        "Frame Skip", "Save State", "Load State", "Quit"
    };
    const int N = 8;
    int  sel    = 0;
    bool dirty  = false;
    bool redraw = true;
    bool have_saved = gb_state_exists();   // greys Load State until a state is written
    int  ret    = 0;
    char val[16];
    int  title_off = 0, title_hold = 20, title_tick = 0;

    while (true) {
        if (redraw) {
            st7789_fill(g_theme->bg);
            ui_pause_header(ui_now_playing(), title_off);
            for (int i = 0; i < N; i++) {
                int      y  = 40 + i * 20;
                bool     on = (i == sel);
                bool     dimmed = (i == 6) && !have_saved;   // Load State with nothing to load
                uint16_t fg = on ? g_theme->sel_fg : (dimmed ? g_theme->footer_fg : g_theme->item_fg);
                uint16_t bg = on ? g_theme->sel_bg : g_theme->bg;
                if (on) ui_fill_pill(8, y - 3, LCD_W - 16, 18, bg);
                char lbl[24];
                snprintf(lbl, sizeof lbl, "%c %s", on ? '>' : ' ', labels[i]);
                st7789_draw_string(16, y, lbl, fg, bg, 1);
                val[0] = '\0';
                switch (i) {
                    case 1: snprintf(val, sizeof val, "%u%%", g_settings.lcd_brightness); break;
                    case 2: snprintf(val, sizeof val, "%u%%", g_settings.volume);          break;
                    case 3: if (g_settings.palette == 0) snprintf(val, sizeof val, "Auto");
                            else snprintf(val, sizeof val, "P%u", g_settings.palette);      break;
                    case 4: if (g_settings.frameskip < 0) snprintf(val, sizeof val, "Auto");
                            else snprintf(val, sizeof val, "%d", g_settings.frameskip);     break;
                    default: break;
                }
                if (val[0]) st7789_draw_string(180, y, val, fg, bg, 1);
            }
            ui_footer("D-pad move/adjust  A select  B resume");
            redraw = false;
        }

        // Keep long ROM names moving without forcing the rest of the overlay to redraw.
        if (++title_tick >= 2) {
            title_tick = 0;
            if (title_hold > 0) title_hold--;
            else                title_off++;
            ui_pause_title(ui_now_playing(), title_off);
        }

        buttons_update();
        uint16_t ev = buttons_pressed();

        if (ev & ((1u << BTN_B) | (1u << BTN_MENU))) { ret = 0; break; }   // resume

        if (ev & (1u << BTN_UP))   { sel = (sel + N - 1) % N; redraw = true; }
        if (ev & (1u << BTN_DOWN)) { sel = (sel + 1) % N;     redraw = true; }

        if (ev & ((1u << BTN_LEFT) | (1u << BTN_RIGHT))) {
            int d = (ev & (1u << BTN_RIGHT)) ? +1 : -1;
            switch (sel) {
                case 1: {   // Brightness, live PWM
                    int b = (int)g_settings.lcd_brightness + d * 5;
                    if (b < 10) b = 10; if (b > 100) b = 100;
                    g_settings.lcd_brightness = (uint8_t)b;
                    st7789_backlight_level(g_settings.lcd_brightness);
                    dirty = true; redraw = true;
                } break;
                case 2: {   // Volume (snd_play_frame reads it live)
                    int v = (int)g_settings.volume + d * 5;
                    if (v < 0) v = 0; if (v > 100) v = 100;
                    g_settings.volume = (uint8_t)v;
                    dirty = true; redraw = true;
                } break;
                case 3: {   // Palette: 0=Auto, 1..13 presets (applies on resume)
                    int p = (int)g_settings.palette + d;
                    if (p < 0) p = 13; if (p > 13) p = 0;
                    g_settings.palette = (uint8_t)p;
                    apply_palette();
                    dirty = true; redraw = true;
                } break;
                case 4: {   // Frame Skip: -1=Auto, 0..5
                    int f = (int)g_settings.frameskip + d;
                    if (f < -1) f = 5; if (f > 5) f = -1;
                    g_settings.frameskip = (int8_t)f;
                    dirty = true; redraw = true;
                } break;
                default: break;
            }
        }

        if (ev & (1u << BTN_A)) {
            switch (sel) {
                case 0: ret = 0; goto done;                 // Resume
                case 5: {                                   // Save State
                    led_set_state(LED_SD_BUSY);
                    bool ok = save_state();
                    led_set_state(LED_IDLE);
                    if (ok) have_saved = true;               // Load State is now available
                    st7789_fill_rect(20, 210, 290, 10, g_theme->bg);
                    st7789_draw_string(20, 210, ok ? "State saved" : "Save failed",
                                       ok ? g_theme->ok : g_theme->err, g_theme->bg, 1);
                    break;
                }
                case 6: {                                   // Load State -> resume into it
                    if (!have_saved) break;                  // greyed: nothing to load
                    led_set_state(LED_SD_BUSY);
                    bool ok = load_state();
                    led_set_state(LED_IDLE);
                    if (ok) { ret = 0; goto done; }
                    st7789_fill_rect(20, 210, 290, 10, g_theme->bg);
                    st7789_draw_string(20, 210, "No state / load failed", g_theme->err, g_theme->bg, 1);
                    break;
                }
                case 7: ret = 1; goto done;                 // Quit to menu
                default: break;                             // value items: use LEFT/RIGHT
            }
        }
        sleep_ms(15);
    }
done:
    if (dirty) settings_save();
    return ret;
}

void gb_core_run(void) {
    // Carve GB's big buffers from the shared arena (NES reuses the same region).
    fb       = (uint16_t (*)[GB_W * GB_H])arena_base();
    cart_ram = (uint8_t *)arena_base() + 2u * (GB_W * GB_H) * sizeof(uint16_t);

    g_rom = flash_ptr(ROM_FLASH_OFFSET);

    enum gb_init_error_e e = gb_init(&gb, rom_read, ram_read, ram_write, gb_err, NULL);
    if (e != GB_INIT_NO_ERROR) {
        show_error(e == GB_INIT_INVALID_CHECKSUM ? "No / bad ROM in flash"
                                                 : "Unsupported cartridge");
        return;
    }

    memset(cart_ram, 0, CART_RAM_BYTES);    // fresh RAM; .srm (if any) is overlaid below
    gb_init_lcd(&gb, draw_line);
    apply_palette();                         // auto-assign by game hash, or the chosen preset

    // Cart-RAM battery save: size comes from the ROM header (capped to our buffer).
    // Load the .srm into cart RAM if one exists; written back when the game exits.
    g_save_size = 0;
    gb_get_save_size_s(&gb, &g_save_size);
    if (g_save_size > CART_RAM_BYTES) g_save_size = CART_RAM_BYTES;
    if (g_save_size > 0 && g_save_path[0]) {
        FIL f;
        if (f_open(&f, g_save_path, FA_READ) == FR_OK) {
            UINT br = 0;
            f_read(&f, cart_ram, (UINT)g_save_size, &br);
            f_close(&f);
        }
    }

    snd_apu_reset();                         // bring up I2S (idempotent) + reset the APU
    st7789_fill(COL_BLACK);                  // pillarbox bars stay black

    // Prime: render frame 0 and do ONE full progressive blit on core0, so the
    // screen starts complete. (In interlace mode the per-field passes only refresh
    // half the rows; without this the first frame would show half black.) core1
    // isn't launched yet, so there's no SPI contention here.
    int draw_buf = 0;
    g_draw_fb = fb[0];
    poll_input();
    gb_run_frame(&gb);
    st7789_blit_scaled(GB_DST_X, GB_DST_Y, GB_DST_W, GB_DST_H, fb[0], GB_W, GB_H);

    multicore_launch_core1(core1_display);
    g_core1_busy = true;
    multicore_fifo_push_blocking(0);         // hand fb[0] to core1; it will draw fb[0]
    draw_buf = 1;                            // emulate the next frame into the other buffer

    bool quit = false;
    uint32_t frame_count = 0;
    absolute_time_t next_frame = make_timeout_time_us(GB_FRAME_US);
    while (!quit) {
        buttons_update();
        if (buttons_pressed() & (1u << BTN_MENU)) {
            // Pause. Let core1 finish any in-flight blit, then it parks in its
            // FIFO wait (not touching SPI) while core0 drives the overlay.
            while (g_core1_busy) tight_loop_contents();
            led_set_state(LED_IDLE);
            int r = gb_overlay();
            if (r == 1) { quit = true; break; }      // Quit selected
            // Resume: emulate one fresh frame, clear the whole screen (so overlay
            // text in the pillarbox bars is flushed -- the blit only covers the
            // 266-wide game area), then full progressive blit and restart the pipeline.
            led_set_state(LED_RUNNING);
            g_draw_fb = fb[0];
            poll_input();
            gb_run_frame(&gb);
            st7789_fill(COL_BLACK);
            st7789_blit_scaled(GB_DST_X, GB_DST_Y, GB_DST_W, GB_DST_H, fb[0], GB_W, GB_H);
            draw_buf = 1;
            next_frame = make_timeout_time_us(GB_FRAME_US);
            continue;
        }
        g_draw_fb = fb[draw_buf];
        poll_input();
        gb_run_frame(&gb);                   // emulate into the buffer core1 isn't reading

        // Frame-skip gate. Auto (-1): try to show every frame, drop only when
        // core1 is still busy. Fixed N (0..5): only attempt every (N+1)th frame,
        // cutting blit load for heavy titles. Audio is fed every frame regardless.
        bool want_show = (g_settings.frameskip < 0)
                       ? true
                       : ((frame_count % (uint32_t)(g_settings.frameskip + 1)) == 0);
        frame_count++;

        // Hand the frame to core1 ONLY if we want to show it AND core1 is idle. If
        // core1 is still blitting the previous frame (slow SPI), don't wait -- keep
        // emulating into this same buffer (dropping the undisplayed frame). That
        // keeps the audio feed below on a rock-steady cadence no matter how slow the
        // blit is: audio is master, video drops frames. When SPI is fast, nothing drops.
        if (want_show && !g_core1_busy) {
            g_core1_busy = true;
            multicore_fifo_push_blocking((uint32_t)draw_buf);
            draw_buf ^= 1;                   // core1 now owns the old buffer; use the other
        }

        // Audio is the frame clock: i2s_dma_write waits for the previous buffer to
        // drain, pacing this loop to the exact 44100 Hz rate. Only fall back to the
        // sleep pacer if audio isn't running.
        bool audio_paced = snd_play_frame();
        if (!audio_paced) sleep_until(next_frame);
        next_frame = delayed_by_us(next_frame, GB_FRAME_US);
        if (to_us_since_boot(next_frame) < to_us_since_boot(get_absolute_time()))
            next_frame = make_timeout_time_us(GB_FRAME_US);
    }

    while (g_core1_busy) tight_loop_contents();  // let the last blit finish
    multicore_reset_core1();                     // stop the display core cleanly

    // Battery save: write cart RAM back to the .srm now that the game has exited
    // and both cores are quiesced.
    if (g_save_size > 0 && g_save_path[0]) {
        led_set_state(LED_SD_BUSY);
        FIL f;
        if (f_open(&f, g_save_path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT bw = 0;
            f_write(&f, cart_ram, (UINT)g_save_size, &bw);
            f_close(&f);
        }
    }
}