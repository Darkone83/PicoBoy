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
#include "fps_overlay.h"
#include "arena.h"
#include "adaptive_frameskip.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <string.h>
#include <stdio.h>
#include "ff.h"            // .srm cart-RAM saves + split-ROM CLMT
#include "sd.h"            // direct raw-sector fills for SD-backed GBC

#include "minigb_apu.h"  // declares audio_read/audio_write/audio_callback/audio_init

// PicoBoy presents cartridge ROM to Peanut-GB as two direct 16 KiB windows.
// Bank 0 always points at staged XIP flash. The switchable 0x4000-0x7FFF
// window points either into XIP or at one of the two SRAM-backed SD cache banks.
// Mapper writes only mark the window dirty; the first actual ROM read after the
// write resolves the final bank once, avoiding duplicate loads on MBC5 low/high
// bank-register sequences.
struct gb_s;
static const uint8_t *g_rom = NULL;
static const uint8_t *s_romx_window = NULL;
static bool s_romx_dirty = true;
static uint8_t __not_in_flash_func(pb_romx_miss)(struct gb_s *g, uint_fast16_t addr);

#define PEANUT_GB_DIRECT_ROM0_READ(g_, addr_) \
    (g_rom[(uint16_t)(addr_)])
#define PEANUT_GB_DIRECT_ROMX_READ(g_, addr_) \
    ((s_romx_dirty || !s_romx_window) \
        ? pb_romx_miss((g_), (uint_fast16_t)(addr_)) \
        : s_romx_window[(uint16_t)(addr_) - 0x4000u])
#define PEANUT_GB_ROM_BANK_CHANGED(g_) do { \
    (void)(g_); \
    s_romx_dirty = true; \
} while (0)

#define ENABLE_SOUND 1   // route GB sound-register access to minigb_apu
#define PEANUT_FULL_GBC_SUPPORT 1
#include "peanut_gb.h"   // single-header core; included in exactly this one TU
#ifndef PEANUT_GB_CGB_FIXPALETTE_RGB565
#error "PicoBoy GBC performance patch requires the RGB565 fixPalette peanut_gb.h patch"
#endif
#include "gbcolors.h"    // GBC-bootstrap preset palettes + per-game auto-assignment

// GB/GBC uses the same one-hot PicoBoy core slot. The CGB-capable Peanut-GB
// context, cart RAM, video ring, and optional SD ROM-bank cache all live in the
// shared arena only while this core is active.
//
// Core0 runs emulation and converts each completed 160-pixel scanline to packed
// RGB565. Core1 expands the small native-row ring to the existing 266x240
// presentation and owns the ST7789 SPI transactions.

#define GB_W 160
#define GB_H 144

// Fit-to-height, aspect-preserved: 160x144 -> 266x240, pillarboxed.
#define GB_DST_H LCD_H
#define GB_DST_W (GB_W * LCD_H / GB_H)      // 266
#define GB_DST_X ((LCD_W - GB_DST_W) / 2)   // 27
#define GB_DST_Y 0

#define GB_FRAME_US 16742u

#define CART_RAM_BYTES      (32u * 1024u)
#define GB_RING              8u
#define GB_ROW_BYTES         (GB_W * 2u)
#define GB_ROM_BANK_BYTES    0x4000u
// Experimental mapped-bank backend: preserve the same 32 KiB cache budget as
// two complete cartridge banks. Peanut-GB sees a direct 16 KiB memory window
// after a bank is selected, so ordinary opcode/data fetches never walk the LRU.
#define GB_SD_CACHE_BANK_BYTES GB_ROM_BANK_BYTES
#define GB_SD_CACHE_BANKS      2u
#define GB_ROM_PATH_MAX      128u
#define GB_FASTSEEK_CLMT_DWORDS 16u

static struct gb_s *gb = NULL;
static uint8_t *cart_ram = NULL;
static uint8_t *s_ring = NULL;

// Two packed destination-row scratch buffers for Core1's single-window blit.
// Arena-backed rather than core1-stack-resident so double-buffering the row
// pack costs no extra stack (in fact the old single stack row[] is removed).
static uint8_t *s_disp_row[2] = { NULL, NULL };


// Loader-supplied ROM geometry. Small GB/GBC images are all-XIP.
// A large .gbc uses a complete-bank-aligned XIP prefix and keeps only the tail
// on SD. Peanut-GB still sees one linear ROM address space.
static uint32_t s_rom_size = 0;
static uint32_t s_flash_prefix = 0;
static char s_rom_path[GB_ROM_PATH_MAX] = {0};

typedef struct {
    uint32_t base;
    uint32_t stamp;
    bool valid;
    uint8_t *data;
} gb_rom_cache_t;

static gb_rom_cache_t s_cache[GB_SD_CACHE_BANKS];
static uint32_t s_cache_stamp = 1;

static FIL s_rom_file;
static DWORD *s_rom_clmt = NULL;
static bool s_rom_file_open = false;
static bool s_rom_io_failed = false;

static char g_save_path[96];
static char g_state_path[96];
static size_t g_save_size;

void gb_set_save_path(const char *path) {
    if (!path) path = "";
    snprintf(g_save_path, sizeof g_save_path, "%s", path);
}

void gb_set_state_path(const char *path) {
    if (!path) path = "";
    snprintf(g_state_path, sizeof g_state_path, "%s", path);
}

void gb_set_rom_source(uint32_t total_size,
                       uint32_t flash_prefix,
                       const char *sd_path) {
    s_rom_size = total_size;
    s_flash_prefix = flash_prefix;

    if (!sd_path) sd_path = "";
    snprintf(s_rom_path, sizeof s_rom_path, "%s", sd_path);
}

// DMG palette LUT. CGB games ignore this and use the hardware palettes produced
// by the CGB Peanut branch.
static palette_t g_palette;

static void apply_palette(void) {
    if (!gb || gb->cgb.cgbMode)
        return;

    if (g_settings.palette == 0) {
        char title[16];
        auto_assign_palette(g_palette, gb_colour_hash(gb), gb_get_rom_name(gb, title));
    } else {
        manual_assign_palette(g_palette, (uint8_t)(g_settings.palette - 1));
    }
}

// Auto frame skip state. Skipped frames keep machine/audio timing but suppress
// PicoBoy's scanline conversion and display work.
static adaptive_frameskip_t s_auto_fs;
static bool g_capture_frame = true;

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

static uint8_t *s_ap = NULL;
static uint8_t *s_ae = NULL;

static void gb_arena_reset(void) {
    s_ap = arena_base();
    s_ae = arena_base() + ARENA_WORK_BYTES;
}

static void *gb_arena_alloc(size_t n) {
    uintptr_t p = ((uintptr_t)s_ap + 3u) & ~(uintptr_t)3u;
    uint8_t *q = (uint8_t *)p;

    if (!s_ap || !s_ae || q > s_ae || (size_t)(s_ae - q) < n)
        return NULL;

    s_ap = q + n;
    return q;
}

// ---------------------------------------------------------------------------
// Split-ROM cache
// ---------------------------------------------------------------------------

static void rom_cache_reset(void) {
    s_cache_stamp = 1;
    s_romx_window = NULL;
    s_romx_dirty = true;

    for (unsigned i = 0; i < GB_SD_CACHE_BANKS; i++) {
        s_cache[i].base = 0;
        s_cache[i].stamp = 0;
        s_cache[i].valid = false;
    }
    s_rom_io_failed = false;
}

static bool rom_open_tail(void) {
    if (s_flash_prefix >= s_rom_size)
        return true;                    // all-XIP

    if (!s_rom_path[0])
        return false;

    if (f_open(&s_rom_file, s_rom_path, FA_READ) != FR_OK)
        return false;

    s_rom_file_open = true;

#if FF_USE_FASTSEEK
    // Build a compact cluster-link map for the SD-backed ROM. Sixteen DWORDs
    // cover up to seven contiguous fragments. FastSeek is an optimization only:
    // if the file is more fragmented (or CLMT creation otherwise fails), clear
    // cltbl so FatFs transparently falls back to its normal FAT-chain seek path.
    if (s_rom_clmt) {
        s_rom_clmt[0] = GB_FASTSEEK_CLMT_DWORDS;
        s_rom_file.cltbl = s_rom_clmt;

        if (f_lseek(&s_rom_file, CREATE_LINKMAP) != FR_OK)
            s_rom_file.cltbl = NULL;
    }
#endif

    return true;
}

static void rom_close_tail(void) {
    if (s_rom_file_open) {
        f_close(&s_rom_file);
        s_rom_file_open = false;
    }
}

// Translate an aligned file offset through FatFs' CLMT into one or more
// physically contiguous SD runs and read those runs directly with CMD18.
//
// This deliberately depends only on public FIL/FATFS fields already exposed by
// the bundled FatFs configuration. If FastSeek is unavailable, the CLMT could
// not be built, the request is not sector aligned, or the map looks invalid,
// return false and let the normal FatFs path handle the refill.
static bool rom_raw_read_clmt(uint32_t file_off, uint8_t *dst, uint32_t bytes) {
#if FF_USE_FASTSEEK
    if (!s_rom_file_open || !s_rom_file.cltbl || !dst || bytes == 0)
        return false;
    if ((file_off & 511u) || (bytes & 511u))
        return false;

    FATFS *fs = s_rom_file.obj.fs;
    if (!fs || fs->csize == 0 || fs->pdrv != 0)
        return false;

    uint32_t sector_ord = file_off >> 9;
    uint32_t sectors_left = bytes >> 9;
    uint8_t *out = dst;

    while (sectors_left) {
        uint32_t cluster_ord = sector_ord / (uint32_t)fs->csize;
        uint32_t sector_in_cluster = sector_ord % (uint32_t)fs->csize;
        uint32_t rel_cluster = cluster_ord;
        DWORD *tbl = s_rom_file.cltbl + 1;
        DWORD frag_clusters = 0;
        DWORD frag_start = 0;

        for (;;) {
            frag_clusters = *tbl++;
            if (frag_clusters == 0)
                return false;
            frag_start = *tbl++;
            if (rel_cluster < frag_clusters)
                break;
            rel_cluster -= frag_clusters;
        }

        DWORD cl = frag_start + rel_cluster;
        if (cl < 2)
            return false;

        uint64_t lba64 = (uint64_t)fs->database
                       + (uint64_t)(cl - 2u) * (uint64_t)fs->csize
                       + (uint64_t)sector_in_cluster;
        if (lba64 > UINT32_MAX)
            return false;

        uint32_t contiguous = (uint32_t)(frag_clusters - rel_cluster)
                            * (uint32_t)fs->csize
                            - sector_in_cluster;
        uint32_t take = sectors_left < contiguous ? sectors_left : contiguous;
        if (take == 0)
            return false;

        if (!sd_read_blocks(out, (uint32_t)lba64, take))
            return false;

        out += (size_t)take * 512u;
        sector_ord += take;
        sectors_left -= take;
    }

    return true;
#else
    (void)file_off;
    (void)dst;
    (void)bytes;
    return false;
#endif
}

static uint8_t *rom_cache_bank(uint32_t bank_base) {
    for (unsigned i = 0; i < GB_SD_CACHE_BANKS; i++) {
        if (s_cache[i].valid && s_cache[i].base == bank_base) {
            s_cache[i].stamp = s_cache_stamp++;
            return s_cache[i].data;
        }
    }


    // Generic LRU: use an empty slot first, otherwise evict the oldest bank.
    unsigned victim = 0;
    uint32_t oldest = UINT32_MAX;
    for (unsigned i = 0; i < GB_SD_CACHE_BANKS; i++) {
        if (!s_cache[i].valid) {
            victim = i;
            oldest = 0;
            break;
        }
        if (s_cache[i].stamp < oldest) {
            oldest = s_cache[i].stamp;
            victim = i;
        }
    }

    gb_rom_cache_t *c = &s_cache[victim];

    if (!s_rom_file_open) {
        s_rom_io_failed = true;
        return NULL;
    }

    uint32_t remain = (bank_base < s_rom_size) ? (s_rom_size - bank_base) : 0u;
    UINT want = (UINT)(remain < GB_SD_CACHE_BANK_BYTES ? remain : GB_SD_CACHE_BANK_BYTES);
    if (want == 0) {
        s_rom_io_failed = true;
        return NULL;
    }

    bool filled = false;

    // Fastest path: the ROM is sector-aligned and CLMT creation succeeded, so
    // bypass FatFs' per-cluster read loop and issue raw multi-sector reads across
    // each physically-contiguous extent. For an ordinary contiguous 16 KiB bank
    // this is one CMD18 carrying 32 sectors.
    if ((want & 511u) == 0 && rom_raw_read_clmt(bank_base, c->data, want)) {
        filled = true;
    }

    // Compatibility fallback: fragmented CLMT too large, unexpected geometry,
    // raw transfer failure, or a short final non-sector-aligned bank. FastSeek
    // remains active here when its CLMT is available.
    if (!filled) {
        if (s_rom_file.fptr != (FSIZE_t)bank_base &&
            f_lseek(&s_rom_file, (FSIZE_t)bank_base) != FR_OK) {
            s_rom_io_failed = true;
            return NULL;
        }

        UINT br = 0;
        if (f_read(&s_rom_file, c->data, want, &br) != FR_OK || br != want) {
            s_rom_io_failed = true;
            return NULL;
        }
    }

    if (want < GB_SD_CACHE_BANK_BYTES)
        memset(c->data + want, 0xFF, GB_SD_CACHE_BANK_BYTES - want);

    c->base = bank_base;
    c->stamp = s_cache_stamp++;
    c->valid = true;
    return c->data;
}

// ---------------------------------------------------------------------------
// Peanut-GB callbacks
// ---------------------------------------------------------------------------

// Normal GB and all-flash GBC use the same minimal callback shape as the
// original PicoBoy GB core. Once gb_init() has validated the cartridge header,
// Peanut's mapper supplies the ROM address and XIP is indexed directly.
static uint8_t __not_in_flash_func(rom_read_xip)(struct gb_s *g, const uint_fast32_t addr) {
    (void)g;
    return g_rom[(uint32_t)addr];
}

// Resolve the switchable 0x4000-0x7FFF cartridge window only when the mapper
// changes it. Hot ROM reads then become one pointer dereference inside
// PEANUT_GB_DIRECT_ROMX_READ instead of a callback + address/cache lookup.
static uint8_t __not_in_flash_func(pb_romx_miss)(struct gb_s *g, uint_fast16_t addr) {
    uint16_t bank;

    // Match Peanut-GB's existing MBC1 mode-1 mapping semantics. All other
    // supported MBCs use selected_rom_bank directly for the switchable window.
    if (g->mbc == 1 && g->cart_mode_select)
        bank = (uint16_t)(g->selected_rom_bank & 0x1Fu);
    else
        bank = (uint16_t)g->selected_rom_bank;

    bank &= g->num_rom_banks_mask;
    uint32_t bank_base = (uint32_t)bank * GB_ROM_BANK_BYTES;

    if (bank_base >= s_rom_size) {
        s_romx_window = NULL;
        s_romx_dirty = false;
        return 0xFF;
    }

    if (bank_base < s_flash_prefix) {
        s_romx_window = g_rom + bank_base;
    } else {
        s_romx_window = rom_cache_bank(bank_base);
        if (!s_romx_window) {
            s_romx_dirty = true;
            return 0xFF;
        }
    }

    s_romx_dirty = false;
    return s_romx_window[(uint16_t)addr - 0x4000u];
}

static uint8_t __not_in_flash_func(ram_read)(struct gb_s *g, const uint_fast32_t addr) {
    (void)g;
    if ((uint32_t)addr >= CART_RAM_BYTES)
        return 0xFF;
    return cart_ram[addr];
}

static void __not_in_flash_func(ram_write)(struct gb_s *g,
                      const uint_fast32_t addr,
                      const uint8_t val) {
    (void)g;
    if ((uint32_t)addr < CART_RAM_BYTES)
        cart_ram[addr] = val;
}

static void gb_err(struct gb_s *g,
                   const enum gb_error_e err,
                   const uint16_t addr) {
    (void)g;
    (void)err;
    (void)addr;
}

// ---------------------------------------------------------------------------
// Native 160x144 row ring -> Core1 -> 266x240 ST7789
// ---------------------------------------------------------------------------

static volatile uint32_t s_wr = 0;
static volatile uint32_t s_rd = 0;
static volatile bool s_frame_done = false;
static volatile bool s_core1_run = false;
static volatile bool s_core1_idle = true;
static volatile bool s_core1_started = false;
static bool s_stream_this = false;

static inline uint8_t *ring_row(uint32_t i) {
    return s_ring + (i % GB_RING) * GB_ROW_BYTES;
}

static void __not_in_flash_func(draw_line)(struct gb_s *g,
                                            const uint8_t *pixels,
                                            const uint_fast8_t line) {
    if (!g_capture_frame || !s_stream_this || !s_ring || line >= GB_H)
        return;

    while ((uint32_t)(s_wr - s_rd) >= GB_RING)
        tight_loop_contents();

    uint8_t *dst = ring_row(s_wr);

    if (g->cgb.cgbMode) {
#if defined(PEANUT_GB_CGB_FIXPALETTE_STREAM16)
        /* Newer CGB hot path: fixPalette[] already contains RGB565 with its
         * bytes pre-swapped for the ST7789 stream.  The arena row is aligned,
         * so preserve that representation with one halfword store per pixel. */
        uint16_t *dst16 = (uint16_t *)dst;
        for (int x = 0; x < GB_W; x++)
            dst16[x] = g->cgb.fixPalette[pixels[x] & 0x3Fu];
#else
        /* Original PicoBoy RGB565 fixPalette representation.  Keep explicit
         * big-endian byte packing when Peanut has not enabled STREAM16. */
        for (int x = 0; x < GB_W; x++) {
            uint16_t c = g->cgb.fixPalette[pixels[x] & 0x3Fu];
            dst[x * 2]     = (uint8_t)(c >> 8);
            dst[x * 2 + 1] = (uint8_t)c;
        }
#endif
    } else {
        for (int x = 0; x < GB_W; x++) {
            uint16_t c =
                g_palette[(pixels[x] & LCD_PALETTE_ALL) >> 4]
                         [pixels[x] & LCD_COLOUR];
            dst[x * 2]     = (uint8_t)(c >> 8);
            dst[x * 2 + 1] = (uint8_t)c;
        }
    }

    __sync_synchronize();
    s_wr++;
}

static void __not_in_flash_func(core1_display)(void) {
    uint8_t xmap[GB_DST_W];

    // Same horizontal nearest-neighbour geometry as the previous GB path.
    // Build the map once on Core1 rather than doing division in the hot row loop.
    for (int x = 0; x < GB_DST_W; x++)
        xmap[x] = (uint8_t)((x * GB_W) / GB_DST_W);

    uint32_t send = 0;
    int sy = 0;
    int dy = 0;
    int cur = 0;
    bool win = false;

    s_core1_idle = false;
    __sync_synchronize();
    s_core1_started = true;

    while (s_core1_run) {
        if (send != s_wr) {
            // Open ONE RAMWR window over the whole 266x240 active region on the
            // first row of each presented frame. The panel auto-advances after
            // each streamed row, so there is no per-row CASET/RASET/RAMWR setup
            // and CS is held for the entire frame instead of toggled per row.
            // This mirrors the streaming idiom already proven by the SMS and
            // Atari cores. Pillarbox columns (0..26, 293..319) are never touched
            // and stay black from the pre-launch st7789_fill(COL_BLACK).
            if (!win) {
                st7789_stream_begin(GB_DST_X, GB_DST_Y, GB_DST_W, GB_DST_H);
                win = true;
            }

            const uint8_t *src = ring_row(send);
            uint8_t *dst = s_disp_row[cur];

            for (int x = 0; x < GB_DST_W; x++) {
                int sx = xmap[x];
                dst[x * 2]     = src[sx * 2];
                dst[x * 2 + 1] = src[sx * 2 + 1];
            }

            /* FPS is stamped only into the final panel-format row.  Never
             * touch the GB/CGB source ring: CGB may use a private STREAM16
             * palette representation there. */
            if (g_settings.show_fps && sy < 7)
                fps_overlay_row_be(dst, GB_DST_W, sy);

            // Source row is now copied into the Core1-owned packed buffer. The
            // DMA reads from s_disp_row[], not the ring, so release the arena
            // ring slot immediately and let Core0 keep emulating.
            send++;
            __sync_synchronize();
            s_rd = send;

            // 144 -> 240 nearest-neighbour vertical scaling, done on this side
            // because the panel does not scale: stream this packed row once for
            // every destination line that maps to this source line, in strictly
            // increasing order into the open contiguous window.
            //
            // st7789_stream_row() waits for the previous row's DMA and then
            // starts this one, so:
            //   - re-streaming the SAME buffer for a doubled line is safe (the
            //     second launch cannot begin until the first completes; the
            //     buffer is unchanged between them), and
            //   - packing the NEXT source row into the OTHER buffer overlaps the
            //     in-flight DMA of this row (double-buffering).
            while (dy < GB_DST_H && ((dy * GB_H) / GB_DST_H) == sy) {
                st7789_stream_row(dst, GB_DST_W);
                dy++;
            }

            sy++;
            cur ^= 1;

        } else if (s_frame_done) {
            // End of a presented frame: drain the final row DMA and release CS
            // BEFORE clearing s_frame_done, so Core0's display_drain()/overlay
            // path never touches SPI while a window is still open.
            if (win) {
                st7789_stream_end();
                win = false;
            }

            __sync_synchronize();
            s_rd = send;
            s_frame_done = false;
            sy = 0;
            dy = 0;

        } else {
            tight_loop_contents();
        }
    }

    if (win)
        st7789_stream_end();
    s_rd = send;
    s_core1_idle = true;
}

static void display_drain(void) {
    while (s_frame_done || s_rd != s_wr)
        tight_loop_contents();
}

static void poll_input(void) {
    uint16_t s = buttons_state();

    // Peanut-GB's pinned CGB branch exposes the active-low joypad through
    // direct.joypad_bits rather than the newer JOYPAD_* mask macros.
    gb->direct.joypad = 0xFF;
    if (s & (1u << BTN_A))      gb->direct.joypad_bits.a = 0;
    if (s & (1u << BTN_B))      gb->direct.joypad_bits.b = 0;
    if (s & (1u << BTN_SELECT)) gb->direct.joypad_bits.select = 0;
    if (s & (1u << BTN_START))  gb->direct.joypad_bits.start = 0;
    if (s & (1u << BTN_RIGHT))  gb->direct.joypad_bits.right = 0;
    if (s & (1u << BTN_LEFT))   gb->direct.joypad_bits.left = 0;
    if (s & (1u << BTN_UP))     gb->direct.joypad_bits.up = 0;
    if (s & (1u << BTN_DOWN))   gb->direct.joypad_bits.down = 0;
}

static void show_error(const char *msg) {
    st7789_fill(g_theme->bg);
    ui_header("Run GB / GBC");
    st7789_draw_string(12, 90,  msg,                      g_theme->err,       g_theme->bg, 1);
    st7789_draw_string(12, 110, "Browse ROMs from SD first", g_theme->footer_fg, g_theme->bg, 1);
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
#define STATE_VERSION 2u

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
    ok = ok && (f_write(&f, gb, sizeof *gb, &bw) == FR_OK && bw == sizeof *gb);
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
        __typeof__(gb->gb_rom_read)           p_rom  = gb->gb_rom_read;
        __typeof__(gb->gb_cart_ram_read)      p_cr   = gb->gb_cart_ram_read;
        __typeof__(gb->gb_cart_ram_write)     p_cw   = gb->gb_cart_ram_write;
        __typeof__(gb->gb_error)              p_err  = gb->gb_error;
        __typeof__(gb->gb_serial_tx)          p_stx  = gb->gb_serial_tx;
        __typeof__(gb->gb_serial_rx)          p_srx  = gb->gb_serial_rx;
        __typeof__(gb->gb_bootrom_read)       p_boot = gb->gb_bootrom_read;
        __typeof__(gb->display.lcd_draw_line) p_ld   = gb->display.lcd_draw_line;
        void                                *p_priv = gb->direct.priv;

        ok = (f_read(&f, gb, sizeof *gb, &br) == FR_OK && br == sizeof *gb);

        gb->gb_rom_read           = p_rom;
        gb->gb_cart_ram_read      = p_cr;
        gb->gb_cart_ram_write     = p_cw;
        gb->gb_error              = p_err;
        gb->gb_serial_tx          = p_stx;
        gb->gb_serial_rx          = p_srx;
        gb->gb_bootrom_read       = p_boot;
        gb->display.lcd_draw_line = p_ld;
        gb->direct.priv           = p_priv;

        // Render suppression is a live frontend scheduling decision, not
        // persistent emulated state. Never inherit it from a save-state blob.
        gb->direct.skip_render    = 0;

        // selected_rom_bank/cart_mode_select came from the snapshot. Resolve
        // the cartridge window lazily on the first post-load ROM fetch.
        s_romx_window = NULL;
        s_romx_dirty = true;

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
                    case 3:
                            if (gb && gb->cgb.cgbMode)
                                snprintf(val, sizeof val, "Hardware");
                            else if (g_settings.palette == 0)
                                snprintf(val, sizeof val, "Auto");
                            else
                                snprintf(val, sizeof val, "P%u", g_settings.palette);
                            break;
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
                case 3: {   // DMG palette; CGB games use hardware palettes.
                    if (gb && gb->cgb.cgbMode) {
                        redraw = true;
                        break;
                    }
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
    gb_arena_reset();

    gb = (struct gb_s *)gb_arena_alloc(sizeof(struct gb_s));
    cart_ram = (uint8_t *)gb_arena_alloc(CART_RAM_BYTES);
    s_ring = (uint8_t *)gb_arena_alloc(GB_RING * GB_ROW_BYTES);

    // Packed 266px destination rows for Core1's single-window streaming blit.
    s_disp_row[0] = (uint8_t *)gb_arena_alloc(GB_DST_W * 2u);
    s_disp_row[1] = (uint8_t *)gb_arena_alloc(GB_DST_W * 2u);

    bool split = (s_rom_size != 0 && s_flash_prefix < s_rom_size);

    for (unsigned i = 0; i < GB_SD_CACHE_BANKS; i++) {
        s_cache[i].data = split
                        ? (uint8_t *)gb_arena_alloc(GB_SD_CACHE_BANK_BYTES)
                        : NULL;
    }

    // FastSeek CLMT is borrowed from the one-hot arena only for SD-backed GBC.
    // 16 DWORDs = 64 bytes and describes up to seven file fragments. Allocation
    // is deliberately optional; NULL simply leaves the normal FatFs seek path.
    s_rom_clmt = split
               ? (DWORD *)gb_arena_alloc(GB_FASTSEEK_CLMT_DWORDS * sizeof(DWORD))
               : NULL;

    if (!gb || !cart_ram || !s_ring ||
        !s_disp_row[0] || !s_disp_row[1] ||
        (split && (!s_cache[0].data || !s_cache[1].data))) {
        show_error("Out of RAM (GB/GBC)");
        return;
    }

    g_rom = flash_ptr(ROM_FLASH_OFFSET);

    // Backward-compatible fallback for an old all-flash GB launch that did not
    // pass explicit geometry. The header's ROM-size code is available in the
    // guaranteed staged first bank.
    if (s_rom_size == 0) {
        static const uint32_t normal_sizes[] = {
            32u*1024u, 64u*1024u, 128u*1024u, 256u*1024u,
            512u*1024u, 1024u*1024u, 2048u*1024u, 4096u*1024u,
            8192u*1024u
        };
        uint8_t code = g_rom[0x148];
        if (code <= 8) {
            s_rom_size = normal_sizes[code];
        } else if (code == 0x52) {
            s_rom_size = 72u * GB_ROM_BANK_BYTES;
        } else if (code == 0x53) {
            s_rom_size = 80u * GB_ROM_BANK_BYTES;
        } else if (code == 0x54) {
            s_rom_size = 96u * GB_ROM_BANK_BYTES;
        }
        s_flash_prefix = s_rom_size;
        split = false;
    }

    if (s_rom_size == 0 || s_flash_prefix == 0 ||
        s_flash_prefix > s_rom_size) {
        show_error("Bad ROM geometry");
        return;
    }

    // A split prefix must end at a complete 16-KiB cartridge bank so every
    // mapped bank is wholly XIP or wholly cached from SD.
    if (split && (s_flash_prefix & (GB_ROM_BANK_BYTES - 1u))) {
        show_error("Split ROM not bank aligned");
        return;
    }

    memset(gb, 0, sizeof *gb);
    memset(cart_ram, 0, CART_RAM_BYTES);
    memset(s_ring, 0, GB_RING * GB_ROW_BYTES);
    rom_cache_reset();

    if (split) {
        if (!rom_open_tail()) {
            rom_close_tail();
            show_error("SD ROM tail unavailable");
            return;
        }
        // Do not preload an arbitrary tail bank. The mapper-driven ROMX window
        // will demand-fill the first bank actually selected by the game.
        s_rom_io_failed = false;
    }

    // The generic ROM callback is now only needed by Peanut-GB's cartridge
    // header/title helpers, all of which live in guaranteed staged bank 0. CPU
    // ROM reads use the direct ROM0/ROMX windows patched into peanut_gb.h.
    enum gb_init_error_e e =
        gb_init(gb, rom_read_xip, ram_read, ram_write, gb_err, NULL);

    if (e != GB_INIT_NO_ERROR) {
        rom_close_tail();
        show_error(e == GB_INIT_INVALID_CHECKSUM ? "No / bad GB ROM"
                                                 : "Unsupported cartridge");
        return;
    }

    gb_init_lcd(gb, draw_line);
    apply_palette();

    g_save_size = 0;
    g_save_size = (size_t)gb_get_save_size(gb);

    // Keep the first GBC experiment fail-closed for large cartridge RAM rather
    // than allowing Peanut's callback to walk beyond the arena allocation.
    if (g_save_size > CART_RAM_BYTES) {
        rom_close_tail();
        show_error("Cart RAM >32K unsupported");
        return;
    }

    if (g_save_size > 0 && g_save_path[0]) {
        FIL f;
        if (f_open(&f, g_save_path, FA_READ) == FR_OK) {
            UINT br = 0;
            f_read(&f, cart_ram, (UINT)g_save_size, &br);
            f_close(&f);
        }
    }

    snd_apu_reset();
    adaptive_frameskip_reset(&s_auto_fs);
    fps_overlay_reset();
    g_capture_frame = true;
    gb->direct.skip_render = 0;

    s_wr = s_rd = 0;
    s_frame_done = false;
    s_stream_this = false;
    s_core1_run = true;
    s_core1_idle = false;
    s_core1_started = false;

    st7789_fill(COL_BLACK);
    multicore_launch_core1(core1_display);

    absolute_time_t start_deadline = make_timeout_time_ms(100);
    while (!s_core1_started && !time_reached(start_deadline))
        tight_loop_contents();

    if (!s_core1_started) {
        s_core1_run = false;
        multicore_reset_core1();
        rom_close_tail();
        show_error("Display core failed");
        return;
    }

    bool quit = false;
    uint32_t frame_count = 0;
    absolute_time_t next_frame = make_timeout_time_us(GB_FRAME_US);

    while (!quit) {
        buttons_update();

        if (buttons_pressed() & (1u << BTN_MENU)) {
            display_drain();
            led_set_state(LED_IDLE);

            int r = gb_overlay();

            if (r == 1) {
                quit = true;
                break;
            }

            led_set_state(LED_RUNNING);
            adaptive_frameskip_reset(&s_auto_fs);
            fps_overlay_reset();
            gb->direct.skip_render = 0;
            st7789_fill(COL_BLACK);
            buttons_update();
            next_frame = make_timeout_time_us(GB_FRAME_US);
            continue;
        }

        const bool auto_mode = (g_settings.frameskip < 0);
        bool want_show;

        if (auto_mode) {
            want_show = adaptive_frameskip_should_render(&s_auto_fs);
        } else {
            want_show =
                ((frame_count %
                  (uint32_t)(g_settings.frameskip + 1)) == 0);
        }
        frame_count++;

        bool display_ready = (!s_frame_done && s_rd == s_wr);
        bool display_blocked = want_show && !display_ready;

        s_stream_this = want_show && display_ready;
        g_capture_frame = s_stream_this;

        /*
         * Previously PicoBoy only suppressed draw_line(), so Peanut still spent
         * most of the CGB PPU cost constructing scanlines that were discarded.
         * Tell Peanut up-front whether this frame will actually be presented.
         */
        gb->direct.skip_render = s_stream_this ? 0u : 1u;

        poll_input();
        gb_run_frame(gb);
        if (g_settings.show_fps) fps_overlay_frame();

        if (s_stream_this) {
            __sync_synchronize();
            s_frame_done = true;
        }

        if (s_rom_io_failed) {
            quit = true;
        }

        bool audio_paced = snd_play_frame();

        if (auto_mode) {
            adaptive_frameskip_feedback(
                &s_auto_fs,
                adaptive_frameskip_signal(
                    display_blocked,
                    audio_paced ? snd_dma_wait_us() : UINT32_MAX));
        }

        if (!audio_paced)
            sleep_until(next_frame);

        next_frame = delayed_by_us(next_frame, GB_FRAME_US);
        if (to_us_since_boot(next_frame) <
            to_us_since_boot(get_absolute_time())) {
            next_frame = make_timeout_time_us(GB_FRAME_US);
        }
    }

    display_drain();
    s_core1_run = false;
    while (!s_core1_idle)
        tight_loop_contents();
    multicore_reset_core1();

    if (g_save_size > 0 && g_save_path[0]) {
        led_set_state(LED_SD_BUSY);
        FIL f;
        if (f_open(&f, g_save_path,
                   FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT bw = 0;
            f_write(&f, cart_ram, (UINT)g_save_size, &bw);
            f_close(&f);
        }
    }

    rom_close_tail();

    if (s_rom_io_failed)
        show_error("SD ROM read failed");
}