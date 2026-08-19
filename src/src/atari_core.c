/*
 * PicoBoy Atari 2600 wrapper / platform glue.
 *
 * 6507 + RIOT + TIA, common carts, controls, TIA audio,
 * PicoBoy pause/config overlay, and the existing Core1 ST7789 display path.
 */
#include "atari_core.h"
#include "atari_cart.h"
#include "atari_cpu.h"
#include "atari_riot.h"
#include "atari_tia.h"
#include "arena.h"
#include "flash.h"
#include "st7789.h"
#include "pins.h"
#include "buttons.h"
#include "ui.h"
#include "theme.h"
#include "led.h"
#include "audio.h"
#include "settings.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <string.h>
#include <stdio.h>

#define ATARI_FB_W 160
#define ATARI_FB_H 192
#define ATARI_FB_BYTES (ATARI_FB_W * ATARI_FB_H)
#define ATARI_PALETTE_BYTES (128u * sizeof(uint16_t))
#define ATARI_WORK_BYTES (2u * ATARI_FB_BYTES)

#define ATARI_DST_W 256
#define ATARI_DST_H 192
#define ATARI_DST_X ((LCD_W - ATARI_DST_W) / 2)   // 32
#define ATARI_DST_Y ((LCD_H - ATARI_DST_H) / 2)   // 24

#define ATARI_PROFILE 1

static uint32_t s_rom_size = 0;
static uint8_t *s_fb[2];
static const uint16_t *s_palette565 = atari_tia_palette565;
static volatile bool s_core1_busy = false;

void atari_core_set_rom_size(uint32_t size) { s_rom_size = size; }

// --------------------------------------------------------------------------
// 2600 system bus
// --------------------------------------------------------------------------
// A12 selects cartridge. With A12 low, A7 selects TIA vs RIOT; RIOT A9 then
// selects RAM vs I/O. This expresses the hardware decode and naturally covers
// the mirrors used by the 6507 stack and RIOT registers.
uint8_t atari_bus_read(uint16_t address) {
    uint16_t a = address & 0x1FFFu;             // 6507 exposes A0..A12 only

    if (a & 0x1000u) return atari_cart_read(a);
    if (!(a & 0x0080u)) return atari_tia_read(a);
    if (!(a & 0x0200u)) return atari_riot_ram_read(a);
    return atari_riot_io_read(a);
}

void atari_bus_write(uint16_t address, uint8_t value) {
    uint16_t a = address & 0x1FFFu;

    if (a & 0x1000u) { atari_cart_write(a, value); return; }
    if (!(a & 0x0080u)) { atari_tia_write(a, value); return; }
    if (!(a & 0x0200u)) { atari_riot_ram_write(a, value); return; }
    atari_riot_io_write(a, value);
}

// --------------------------------------------------------------------------
// Existing PicoBoy display path, Core1 consumer
// --------------------------------------------------------------------------
static void __not_in_flash_func(atari_core1_display)(void) {
    uint8_t xmap[ATARI_DST_W];

    // st7789_stream_row() starts DMA and returns while DMA consumes the source.
    // Ping-pong rows so packing overlaps the previous row transfer.
    uint8_t row[2][ATARI_DST_W * 2];

    for (int x = 0; x < ATARI_DST_W; x++)
        xmap[x] = (uint8_t)((x * ATARI_FB_W) / ATARI_DST_W);

    while (true) {
        uint32_t idx = multicore_fifo_pop_blocking();
        const uint8_t *src = s_fb[idx & 1u];
        int rb = 0;

        // One continuous RAMWR window for the whole 256x192 picture.
        // At PicoBoy's 92 MHz LCD clock this is comfortably below one NTSC
        // frame period and avoids 96 separate CASET/RASET/RAMWR transactions.
        st7789_stream_begin(ATARI_DST_X, ATARI_DST_Y,
                            ATARI_DST_W, ATARI_DST_H);

        for (int y = 0; y < ATARI_DST_H; y++) {
            const uint8_t *srow = &src[y * ATARI_FB_W];
            uint8_t *dst = row[rb];

            for (int x = 0; x < ATARI_DST_W; x++) {
                uint16_t p = s_palette565[srow[xmap[x]] & 0x7F];
                dst[x * 2]     = (uint8_t)(p >> 8);
                dst[x * 2 + 1] = (uint8_t)p;
            }

            st7789_stream_row(dst, ATARI_DST_W);
            rb ^= 1;
        }

        st7789_stream_end();
        s_core1_busy = false;
    }
}

// --------------------------------------------------------------------------
// Controls + Atari console switches
// --------------------------------------------------------------------------
static uint8_t build_switch_byte(uint16_t buttons) {
    // Default console state: Reset/Select released, Color mode, both difficulty
    // switches B. Start/Select act as the two momentary console switches.
    uint8_t sw = 0x0B;

    if (buttons & (1u << BTN_START))  sw &= (uint8_t)~0x01; // Reset
    if (buttons & (1u << BTN_SELECT)) sw &= (uint8_t)~0x02; // Select

    return sw;
}

// Samples the physical controls once per emulated frame. Returns newly pressed
// PicoBoy button edges from the same sample so MENU can open the overlay.
static uint16_t poll_input(void) {
    buttons_update();

    uint16_t held = buttons_state();
    uint16_t ev   = buttons_pressed();

    // SWCHA P0 joystick bits are active low:
    // D7 right, D6 left, D5 down, D4 up.
    uint8_t joy = 0xFF;
    if (held & (1u << BTN_RIGHT)) joy &= (uint8_t)~0x80;
    if (held & (1u << BTN_LEFT))  joy &= (uint8_t)~0x40;
    if (held & (1u << BTN_DOWN))  joy &= (uint8_t)~0x20;
    if (held & (1u << BTN_UP))    joy &= (uint8_t)~0x10;
    atari_riot_set_joystick(joy);

    atari_riot_set_switches(build_switch_byte(held));

    // The 2600 joystick has one trigger. Either PicoBoy face button fires.
    atari_tia_set_fire((held & ((1u << BTN_A) | (1u << BTN_B))) != 0);

    return ev;
}

// --------------------------------------------------------------------------
// In-game pause/config overlay
// --------------------------------------------------------------------------
// Keep Atari aligned with the lightweight NES menu. These are the only controls
// that need to be configurable while a game is running.
static int atari_overlay(void) {
    static const char *const labels[] = {
        "Resume", "Brightness", "Volume", "Frame Skip", "Quit"
    };

    const int N = 5;
    int sel = 0;
    int ret = 0;
    bool dirty = false;
    bool redraw = true;
    char val[16];
    int  title_off = 0, title_hold = 20, title_tick = 0;

    while (true) {
        if (redraw) {
            st7789_fill(g_theme->bg);
            ui_pause_header(ui_now_playing(), title_off);

            for (int i = 0; i < N; i++) {
                int y = 40 + i * 20;
                bool on = (i == sel);
                uint16_t fg = on ? g_theme->sel_fg : g_theme->item_fg;
                uint16_t bg = on ? g_theme->sel_bg : g_theme->bg;

                if (on)
                    ui_fill_pill(8, y - 3, LCD_W - 16, 18, bg);

                char lbl[24];
                snprintf(lbl, sizeof lbl, "%c %s", on ? '>' : ' ', labels[i]);
                st7789_draw_string(16, y, lbl, fg, bg, 1);

                val[0] = '\0';
                if (i == 1) {
                    snprintf(val, sizeof val, "%u%%", g_settings.lcd_brightness);
                } else if (i == 2) {
                    snprintf(val, sizeof val, "%u%%", g_settings.volume);
                } else if (i == 3) {
                    if (g_settings.frameskip < 0)
                        snprintf(val, sizeof val, "Auto");
                    else
                        snprintf(val, sizeof val, "%d", g_settings.frameskip);
                }

                if (val[0])
                    st7789_draw_string(180, y, val, fg, bg, 1);
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

        if (ev & ((1u << BTN_B) | (1u << BTN_MENU))) {
            ret = 0;
            break;
        }

        if (ev & (1u << BTN_UP)) {
            sel = (sel + N - 1) % N;
            redraw = true;
        }

        if (ev & (1u << BTN_DOWN)) {
            sel = (sel + 1) % N;
            redraw = true;
        }

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
            }
        }

        if (ev & (1u << BTN_A)) {
            if (sel == 0) {
                ret = 0;
                break;
            }
            if (sel == 4) {
                ret = 1;
                break;
            }
        }

        sleep_ms(15);
    }

    if (dirty)
        settings_save();

    return ret;
}

// --------------------------------------------------------------------------
// Errors
// --------------------------------------------------------------------------
static void show_error(const char *line1, const char *line2) {
    st7789_fill(g_theme->bg);
    ui_header("Atari 2600");
    st7789_draw_string(12, 88, line1, g_theme->err, g_theme->bg, 1);

    if (line2 && line2[0])
        st7789_draw_string(12, 108, line2,
                           g_theme->footer_fg, g_theme->bg, 1);

    ui_footer("B back");
    led_set_state(LED_ERROR);

    while (true) {
        buttons_update();
        if (buttons_pressed() & (1u << BTN_B))
            break;
        sleep_ms(15);
    }

    led_set_state(LED_IDLE);
}

// --------------------------------------------------------------------------
// Chunked machine scheduler
// --------------------------------------------------------------------------
typedef struct {
    bool running;
    bool quit;

    int draw;
    int active_line;
    bool capture_started;

    bool prev_vsync;
    bool frame_edge_pending;

    uint32_t frame_t0;
    uint32_t frames;
    uint32_t drops;
    uint32_t present_seq;

#if ATARI_PROFILE
    uint64_t prof_sum;
    uint32_t prof_min;
    uint32_t prof_max;
    uint32_t prof_n;
#endif
} atari_machine_t;

static atari_machine_t m;

static void machine_process_frame(void);

static bool machine_line_done(void) {
    bool vs = atari_tia_vsync();

    if (m.frame_edge_pending) {
        m.frame_edge_pending = false;
        machine_process_frame();

        // Do not assume every kernel uses exactly 37 software VBLANK lines.
        // Arm the next picture at the frame edge, then begin capture on the
        // first completed scanline after VBLANK is actually released.
        m.active_line = 0;
        m.capture_started = false;

        if (m.quit)
            return false;
    }

    if (!vs) {
        if (!m.capture_started) {
            if (!atari_tia_vblank())
                m.capture_started = true;
        }

        // Once the picture starts, take exactly 192 consecutive lines.  This
        // prevents later VBLANK tricks from shifting the whole LCD frame while
        // still letting each ROM choose its real top-of-picture scanline.
        if (m.capture_started && m.active_line < ATARI_FB_H) {
            memcpy(&s_fb[m.draw][m.active_line * ATARI_FB_W],
                   atari_tia_line(), ATARI_FB_W);
            m.active_line++;
        }
    }

    return !m.quit;
}

static bool machine_advance_tia_clocks(uint32_t clocks) {
    while (clocks && !m.quit) {
        uint32_t line_rem =
            (uint32_t)ATARI_TIA_LINE_CLOCKS - atari_tia_color_clock();
        uint32_t step = clocks < line_rem ? clocks : line_rem;

        uint8_t line_done = atari_tia_advance((uint16_t)step);

        // TIA now performs its own 44.1 kHz integration/anti-aliasing inside
        // the same chunk. Drain the few samples produced by this advance.
        uint16_t sample;
        while (atari_tia_audio_pop(&sample))
            snd_atari_sample(sample);

        clocks -= step;

        if (line_done && !machine_line_done())
            return false;
    }

    return !m.quit;
}

// Called by atari_cpu.c when deferred CPU cycles must become visible to
// TIA/RIOT. Ordinary ROM/RAM bus cycles are accumulated until this point.
bool atari_machine_advance_cpu(uint32_t cycles) {
    if (!m.running || m.quit)
        return false;

    // WSYNC halts the 6507/RIOT until the TIA reaches the next scanline.
    if (atari_tia_wsync()) {
        uint32_t stall =
            (uint32_t)ATARI_TIA_LINE_CLOCKS - atari_tia_color_clock();
        if (!machine_advance_tia_clocks(stall))
            return false;
    }

    // RIOT advances once per actual 6507 clock, but does not advance through
    // the WSYNC stall above (matching the xrip source loop).
    atari_riot_advance(cycles);

    return machine_advance_tia_clocks(cycles * 3u);
}

// TIA writes are already synchronized to their CPU bus-cycle position by
// atari_cpu.c. Latch VSYNC transitions immediately after the register write.
bool atari_machine_tia_write_complete(void) {
    bool vs = atari_tia_vsync();
    if (m.prev_vsync && !vs)
        m.frame_edge_pending = true;
    m.prev_vsync = vs;

    // A WSYNC write asserts RDY immediately after the write cycle. Finish the
    // horizontal line now so even unusual RMW-to-WSYNC code does not execute
    // additional CPU bus cycles during the stall.
    if (atari_tia_wsync()) {
        uint32_t stall =
            (uint32_t)ATARI_TIA_LINE_CLOCKS - atari_tia_color_clock();
        return machine_advance_tia_clocks(stall);
    }

    return !m.quit;
}

static void machine_process_frame(void) {
    uint32_t now = time_us_32();
    uint32_t raw_us = now - m.frame_t0;

    bool want_show = (g_settings.frameskip < 0)
                   ? true
                   : ((m.present_seq %
                       (uint32_t)(g_settings.frameskip + 1)) == 0);
    m.present_seq++;

    if (m.active_line > 0 && want_show) {
        if (!s_core1_busy) {
            s_core1_busy = true;
            multicore_fifo_push_blocking((uint32_t)m.draw);
            m.draw ^= 1;
        } else {
            m.drops++;
        }
    } else if (m.active_line > 0 && !want_show) {
        m.drops++;
    }

    memset(s_fb[m.draw], 0, ATARI_FB_BYTES);
    m.frames++;

    // Atari audio is continuously queued in fixed-size DMA blocks by
    // snd_atari_sample().  Do not flush a short/variable block at every VSYNC:
    // that couples audio timing to raster jitter and creates audible gaps.
    uint16_t ev = poll_input();
    if (ev & (1u << BTN_MENU)) {
        // Drain only at an intentional pause boundary, not every video frame.
        snd_atari_flush();

        while (s_core1_busy)
            tight_loop_contents();

        int r = atari_overlay();
        st7789_fill(COL_BLACK);

        if (r == 1) {
            m.quit = true;
        } else {
            snd_atari_open();
            poll_input();
            m.frame_t0 = time_us_32();
        }
    }

#if ATARI_PROFILE
    if (m.frames > 1) {
        m.prof_sum += raw_us;
        if (raw_us < m.prof_min) m.prof_min = raw_us;
        if (raw_us > m.prof_max) m.prof_max = raw_us;
        m.prof_n++;
    }

    if (m.prof_n >= 120) {
        printf("[2600] %s core=%luus min=%lu max=%lu drops=%lu "
               "pc=%04x audio=avg441 sched=deferred video=full rom=sram\n",
               atari_cart_type_name(),
               (unsigned long)(m.prof_sum / m.prof_n),
               (unsigned long)m.prof_min,
               (unsigned long)m.prof_max,
               (unsigned long)m.drops,
               (unsigned)atari_cpu_pc());

        m.prof_sum = 0;
        m.prof_min = 0xFFFFFFFFu;
        m.prof_max = 0;
        m.prof_n = 0;
        m.drops = 0;
    }
#endif


    if (!m.quit)
        m.frame_t0 = time_us_32();
}

// --------------------------------------------------------------------------
// Run
// --------------------------------------------------------------------------
void atari_core_run(void) {
    const uint8_t *rom_flash = flash_ptr(ROM_FLASH_OFFSET);
    uint8_t *arena = arena_base();

    // Atari has a large amount of unused space in the shared emulator arena:
    // two 160x192 byte framebuffers consume only 61,440 bytes.  Put the active
    // cartridge and palette in the remaining arena so the 6507 no longer does
    // random ROM fetches through RP2040 XIP while Core1 is also drawing.
    //
    // This does NOT increase PicoBoy's static RAM usage; the arena already
    // exists for the mutually-exclusive emulator cores.
    const uint32_t palette_pad = (uint32_t)((s_rom_size + 1u) & ~1u);
    const uint32_t workspace_needed =
        ATARI_WORK_BYTES + palette_pad + ATARI_PALETTE_BYTES;

    if (!s_rom_size || workspace_needed > ARENA_BYTES) {
        show_error("Atari ROM too large", "");
        return;
    }

    s_fb[0] = arena;
    s_fb[1] = arena + ATARI_FB_BYTES;

    uint8_t *rom_ram = arena + ATARI_WORK_BYTES;
    uint16_t *palette_ram =
        (uint16_t *)(rom_ram + palette_pad);

    memcpy(rom_ram, rom_flash, s_rom_size);
    memcpy(palette_ram, atari_tia_palette565, ATARI_PALETTE_BYTES);
    s_palette565 = palette_ram;

    if (!atari_cart_mount(rom_ram, s_rom_size)) {
        show_error("Unknown/invalid ROM", "");
        return;
    }

    if (!atari_cart_supported()) {
        char msg[48];
        snprintf(msg, sizeof msg, "Mapper %s pending", atari_cart_type_name());
        show_error(msg, "Use 2K/4K/F8/F6/F4");
        return;
    }

    memset(s_fb[0], 0, ATARI_FB_BYTES);
    memset(s_fb[1], 0, ATARI_FB_BYTES);

    atari_cart_reset();
    atari_riot_reset();
    atari_tia_reset();
    atari_cpu_reset();

    poll_input();
    snd_atari_open();

    st7789_fill(COL_BLACK);
    led_set_state(LED_RUNNING);

    multicore_reset_core1();
    multicore_fifo_drain();
    s_core1_busy = false;
    multicore_launch_core1(atari_core1_display);

    memset(&m, 0, sizeof m);
    m.running = true;
    m.draw = 0;
    m.capture_started = false;
    m.prev_vsync = atari_tia_vsync();
    m.frame_t0 = time_us_32();

#if ATARI_PROFILE
    m.prof_min = 0xFFFFFFFFu;
#endif

    while (!m.quit && !atari_cpu_halted()) {
        if (!atari_cpu_step_instruction())
            break;
    }

    m.running = false;

    snd_atari_flush();
    snd_atari_close();

    while (s_core1_busy)
        tight_loop_contents();

    multicore_reset_core1();

    if (atari_cpu_halted()) {
        char a[48], b[48];
        snprintf(a, sizeof a,
                 "Illegal opcode $%02X", atari_cpu_last_opcode());
        snprintf(b, sizeof b,
                 "PC $%04X", (unsigned)atari_cpu_pc());
        show_error(a, b);
    } else {
        led_set_state(LED_IDLE);
    }
}