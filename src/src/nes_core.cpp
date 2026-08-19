// PicoBoy NES front-end: port glue for the fhoedemakers/pico-infonesPlus
// InfoNES core (scanline rendering, f_malloc'd buffers, 6-wave APU). Compiled
// as C++ because the core is C++. nes_run() is exposed to the C loader via
// extern "C".
//
// Shipping build: video + input + SD-load, full 6-wave APU mixed to the shared
// I2S (audio is the master clock), and native full-res scanline streaming --
// core0 emulates + packs each 256-wide line into a ring, core1 DMAs it straight
// to the panel (interlaced) so core0 never blocks on SPI. In-game pause overlay
// with brightness/volume/frame-skip and .srm battery saves + .dat save states.
// The FDS core is present (infones_fds); its disk-swap UI is future work.

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_Types.h"
#include "FrensHelpers.h"
#include "nes_settings.h"

extern "C" {
#include "arena.h"
#include "pins.h"
#include "st7789.h"
#include "buttons.h"
#include "led.h"
#include "ui.h"
#include "theme.h"
#include "settings.h"      // g_settings + settings_save() for the in-game overlay
#include "flash.h"
#include "nes_core.h"
#include "ff.h"            // .srm battery SRAM saves
#include "hardware/sync.h"
#include "audio.h"
}
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// NES master palette (port-supplied). fhoedemakers canonical table, their
// RGB888 source converted to our RGB565. InfoNES_System.h declares this
// `extern const`, so defining it here (after the include) gives it external
// linkage to satisfy the core.
// ---------------------------------------------------------------------------
// RGB555 (NOT 565): the core uses bit15 as an internal backdrop-priority flag
// (PalTable backdrop entries get | 0x8000; the renderer tests pixel>>15 for
// sprite-behind-background priority). Keeping bit15 free means the flag never
// collides with colour. InfoNES_PostDrawLine() converts 555->565 for the panel.
const WORD NesPalette[64] = {
    0x318C, 0x0072, 0x0C15, 0x2013, 0x300D, 0x3404, 0x3000, 0x2460, 0x10C0, 0x0120, 0x0120, 0x0122, 0x00CB, 0x0000, 0x0000, 0x0000,
    0x56B5, 0x053B, 0x1CBF, 0x385E, 0x4C37, 0x584C, 0x5480, 0x4500, 0x2980, 0x11E0, 0x0220, 0x0206, 0x01B2, 0x0000, 0x0000, 0x0000,
    0x7FFF, 0x2A9F, 0x421F, 0x59BF, 0x6D9F, 0x7D98, 0x7DCD, 0x6E44, 0x5AA0, 0x4320, 0x2B44, 0x1F4D, 0x1F19, 0x2529, 0x0000, 0x0000,
    0x7FFF, 0x5F9F, 0x675F, 0x733F, 0x7B1F, 0x7F1D, 0x7F39, 0x7B55, 0x7373, 0x6BB3, 0x63B5, 0x5BB8, 0x5BBC, 0x5EF7, 0x0000, 0x0000,
};

// nes_settings.h global (FDS auto-insert/swap; harmless defaults until the FDS UI lands)
NesSettings settings = { { true, true } };

// ---------------------------------------------------------------------------
// Frens:: bump allocator over the shared emulator arena (arena.h). The core
// f_malloc's RAM/SRAM/PPURAM/SPRRAM/ChrBuf (~64 KB) at Init plus any mapper /
// FDS buffers on demand. f_free is a no-op; nes_run() rewinds the pointer at
// each launch, so a fresh run always starts from an empty arena.
// ---------------------------------------------------------------------------
static uint8_t *s_apos = 0;
static uint8_t *s_aend = 0;

static void arena_reset(void)
{
    s_apos = arena_base();
    s_aend = arena_base() + ARENA_BYTES;
}

void *Frens::f_malloc(size_t size)
{
    size = (size + 3u) & ~(size_t)3u;            // 4-byte align
    if (!s_apos || s_apos + size > s_aend) return 0;
    void *p = s_apos;
    s_apos += size;
    return p;
}

void Frens::f_free(void *) {}                     // bump allocator: rewind per run

unsigned int Frens::GetAvailableMemory(void)
{
    return s_apos ? (unsigned int)(s_aend - s_apos) : 0u;
}

bool Frens::isPsramEnabled(void) { return false; }

void Frens::getextensionfromfilename(const char *filename, char *ext, size_t extsize)
{
    if (!ext || extsize == 0) return;
    ext[0] = 0;
    if (!filename) return;
    const char *dot = strrchr(filename, '.');
    if (!dot) return;
    size_t i = 0;
    while (dot[i] && i + 1 < extsize) { ext[i] = dot[i]; i++; }
    ext[i] = 0;
}

// ---------------------------------------------------------------------------
// Display geometry + dual-core framebuffer presentation.
// ---------------------------------------------------------------------------
#define NES_FRAME_US 16639                            // ~60.0988 Hz

// Dual-core display, the PROVEN GB model: a 16-bit double-buffer + a single
// st7789_blit_scaled per frame + FIFO handoff. The full 256x240 frame won't
// double-buffer in RAM, so core0 downscales it 2:1 into a 128x120 framebuffer
// (exact half res, perfect 16:15 aspect) -- two of those are only 60K, fitting
// the 128K arena alongside the ~64K NES core. core1 scales that up to the panel.
// Because the cores touch different buffers (ping-pong), every blit is one whole
// coherent frame -> no inter-core tearing, only the single GB-style scan seam.
#define NES_FB_W 128                                  // half of 256 (exact 2:1 down)
#define NES_FB_H 120                                  // half of 240 (exact 2:1 down)
#define NES_DST_W 256                                 // exact 2x of fb -> clean 2x2 blocks
#define NES_DST_H 240                                 // (128x120 = 1:1 small+crisp if you prefer)
#define NES_DOWNSCALE_AVG 1                           // 1 = 2x2 box average (soft, complete)
                                                      // 0 = nearest (sharp, thin strokes may drop)
#define NES_DST_X ((LCD_W - NES_DST_W) / 2)           // centered -> 32 at 256w
#define NES_DST_Y ((240   - NES_DST_H) / 2)           // centered -> 0  at 240h

// Full-res scanline streaming. 1 (shipping default) = native 256x232: each
// rendered line is DMA'd straight to the panel, no framebuffer, core1 freed --
// native resolution with ~60K arena back. 0 = the older downscale + core1 blit
// path, kept for A/B bring-up. Flip + rebuild to compare on hardware.
#define NES_STREAM_FULLRES 1
#define NES_STREAM_FIRST 4                             // first visible NES scanline (HSync gate)
#define NES_STREAM_W     NES_DISP_WIDTH                // 256
#define NES_STREAM_H     232                           // lines 4..235 inclusive
#define NES_STREAM_X     ((LCD_W - NES_STREAM_W) / 2)  // 32, centered
#define NES_STREAM_Y     NES_STREAM_FIRST              // 4 -> 4px overscan bars, matches downscale

static volatile bool g_core1_busy = false;            // PadState polls this (stays false in streaming)
static WORD s_workline[NES_DISP_WIDTH];               // core renders the 256-wide line here (both modes)

#if !NES_STREAM_FULLRES
static uint16_t *fb[2] = { 0, 0 };                    // two 128x120 RGB565 buffers, from arena
static volatile int  g_draw = 0;                      // core0 renders into fb[g_draw]
static WORD s_evenrow[NES_FB_W];                      // even line H-averaged, awaiting V-average
#else
// Cross-core native streaming (GB model): core0 emulates + packs native scanlines
// into a small ring; core1 owns the panel and DMAs them out. core0 never blocks on
// SPI, so it stays audio-paced at 60. If core1 falls behind, core0 drops the whole
// frame (produces nothing) instead of stalling -- audio is master, video drops.
#define NES_RING 8                                     // line buffers in the ring (565 panel bytes)
static uint8_t   s_ring[NES_RING][NES_STREAM_W * 2];   // core0 converts 555->565 into here; core1 DMAs it
static volatile uint32_t s_wr = 0;                     // core0: lines produced (monotonic)
static volatile uint32_t s_rd = 0;                     // core1: lines consumed (monotonic)
static volatile bool s_frame_done = false;             // core0 -> core1: frame's last line is queued
static volatile bool s_core1_run  = false;             // keep the core1 loop alive
static volatile bool s_core1_idle = true;              // core1 has exited its loop
static bool s_new_frame   = true;                      // core0: next line begins a frame
static bool s_stream_this = false;                     // core0: streaming (not dropping) this frame

// Interlace: stream even rows one frame, odd the next -> half the panel DMA, full
// 256-wide sharpness, at the cost of slight combing on fast vertical motion. The
// untouched field keeps last frame's rows in panel GRAM. Set 0 for progressive.
#define NES_INTERLACE 1
#if NES_INTERLACE
static volatile int s_field     = 0;                   // toggles 0/1 each streamed frame
static volatile int s_field_cur = 0;                   // field locked for the in-flight frame
#endif

// --- timing probe: prints over USB serial every 120 frames. Set 0 to disable. -
#define NES_PROFILE 1
#if NES_PROFILE
#include <stdio.h>
static volatile uint32_t pf_t0=0, pf_dt=0, pf_wait=0, pf_frames=0, pf_drops=0;
static volatile uint32_t pf_ssum=0, pf_scnt=0;         // core1 stream-time sum / count
#endif
#endif

// Battery SRAM (.srm) save path. Set by the loader before nes_run(); "" disables.
// InfoNES exposes the 8 KB battery RAM as the global SRAM (SRAM_SIZE), and sets
// SRAMwritten when a game writes it -- so a .srm only exists for games that use it.
static char g_nes_save_path[96];
extern "C" void nes_set_save_path(const char *path) {
    if (path) { strncpy(g_nes_save_path, path, sizeof g_nes_save_path - 1);
                g_nes_save_path[sizeof g_nes_save_path - 1] = '\0'; }
    else g_nes_save_path[0] = '\0';
}

// .dat save-state path (the overlay's Save/Load State). Set by the loader before
// nes_run(); "" disables. See save_nes_state()/load_nes_state() below.
static char g_nes_state_path[96];
extern "C" void nes_set_state_path(const char *path) {
    if (path) { strncpy(g_nes_state_path, path, sizeof g_nes_state_path - 1);
                g_nes_state_path[sizeof g_nes_state_path - 1] = '\0'; }
    else g_nes_state_path[0] = '\0';
}

static absolute_time_t s_next;
static bool s_audio_paced = false;                    // true once I2S audio is the clock

// core1: dedicated display, exactly like GB. Block for a buffer index from core0,
// Interlace: blit only even/odd rows each frame, alternating. Halves the per-frame
// SPI cost (~16 ms -> ~8 ms for the 256x240 area), trading a progressive image for
// combing on motion -- exactly what GB does by default. Off here so NES stays fully
// progressive unless you want the core1 headroom on heavy titles; flip to true to
// roughly double the blit margin.
#if !NES_STREAM_FULLRES
volatile bool g_nes_interlace = false;

// Use the DMA-overlapped blit instead of the blocking one. Same image; packs the
// next row while DMA streams the current. Off by default -- flip to true to A/B
// the DMA path on hardware (Step 1 toward full-res scanline streaming).
volatile bool g_nes_dma = false;

// scale-blit it in one call, clear the busy flag. core0 stops it via reset on quit.
static void __not_in_flash_func(core1_display)(void)
{
    int field = 0;
    while (true) {
        uint32_t idx = multicore_fifo_pop_blocking();
        if (g_nes_interlace) {
            st7789_blit_scaled_field(NES_DST_X, NES_DST_Y, NES_DST_W, NES_DST_H,
                                     fb[idx], NES_FB_W, NES_FB_H, field);
            field ^= 1;                                // alternate even/odd rows each frame
        } else if (g_nes_dma) {
            st7789_blit_scaled_dma(NES_DST_X, NES_DST_Y, NES_DST_W, NES_DST_H,
                                   fb[idx], NES_FB_W, NES_FB_H);
        } else {
            st7789_blit_scaled(NES_DST_X, NES_DST_Y, NES_DST_W, NES_DST_H,
                               fb[idx], NES_FB_W, NES_FB_H);
        }
        g_core1_busy = false;                          // done -- core0 may hand over the next frame
    }
}
#endif

#if NES_STREAM_FULLRES
// Core1: dedicated panel streamer. Opens one RAMWR window per frame, DMAs each
// native scanline core0 hands it through the ring, closes when core0 marks the
// frame done. The per-line DMA wait lives HERE, off core0's critical path.
static void __not_in_flash_func(core1_stream)(void)
{
    s_core1_idle = false;
#if NES_INTERLACE
    // Each line lands on its real panel row (NES_STREAM_Y + field + 2*k); the
    // other field keeps last frame's rows. No persistent window -- rows aren't
    // contiguous, so each is its own 1-row DMA.
    bool active = false;
    int  k = 0, field = 0;
#if NES_PROFILE
    uint32_t st0 = 0;
#endif
    while (s_core1_run) {
        if (s_rd != s_wr) {
            if (!active) {
                active = true; k = 0; field = s_field_cur;
#if NES_PROFILE
                st0 = time_us_32();
#endif
            }
            int yrow = NES_STREAM_Y + field + 2 * k;
            st7789_stream_row_at(NES_STREAM_X, yrow, s_ring[s_rd % NES_RING], NES_STREAM_W);
            k++;
            __sync_synchronize();
            s_rd++;
        } else if (s_frame_done) {
            if (active) {
                active = false;
#if NES_PROFILE
                pf_ssum += time_us_32() - st0; pf_scnt++;
#endif
            }
            s_frame_done = false;
        } else {
            tight_loop_contents();                     // idle: no SPI touched (safe for the overlay)
        }
    }
    s_core1_idle = true;
#else
    bool win = false;
#if NES_PROFILE
    uint32_t st0 = 0;
#endif
    while (s_core1_run) {
        if (s_rd != s_wr) {                            // a line is waiting
            if (!win) {
                st7789_stream_begin(NES_STREAM_X, NES_STREAM_Y, NES_STREAM_W, NES_STREAM_H);
                win = true;
#if NES_PROFILE
                st0 = time_us_32();
#endif
            }
            const uint8_t *row = s_ring[s_rd % NES_RING];  // already 565 panel bytes (core0 converted)
            st7789_stream_row(row, NES_STREAM_W);          // waits prev DMA, kicks this one
            __sync_synchronize();
            s_rd++;                                        // release slot (DMA finishes long before reuse)
        } else if (s_frame_done) {                     // drained + frame done -> close the window
            if (win) {
                st7789_stream_end(); win = false;
#if NES_PROFILE
                pf_ssum += time_us_32() - st0; pf_scnt++;
#endif
            }
            s_frame_done = false;                      // ack to core0
        } else {
            tight_loop_contents();                     // idle: no SPI touched (safe for the overlay)
        }
    }
    if (win) st7789_stream_end();                      // clean up on quit
    s_core1_idle = true;
#endif
}
#endif

// ---------------------------------------------------------------------------
// InfoNES_System interface (C++ linkage, per InfoNES_System.h). NOTE: Wait(),
// MemoryCopy(), MemorySet() are inline in the header -- do NOT define them here.
// ---------------------------------------------------------------------------

// Point ROM/VROM into the flash window (the .nes image the loader staged
// there). Fills NesHeader; InfoNES_Reset wires banks + mapper from it.
int InfoNES_ReadRom(const char *pszFileName)
{
    (void)pszFileName;
    const BYTE *base = (const BYTE *)flash_ptr(ROM_FLASH_OFFSET);
    memcpy(&NesHeader, base, sizeof(NesHeader));      // 16-byte iNES header
    if (!(NesHeader.byID[0] == 'N' && NesHeader.byID[1] == 'E' &&
          NesHeader.byID[2] == 'S' && NesHeader.byID[3] == 0x1A))
        return -1;

    const BYTE *p = base + 16;
    if (NesHeader.byInfo1 & 4) p += 512;              // skip trainer if present
    ROM  = (BYTE *)p;                                 // PRG-ROM (in flash)
    p   += (DWORD)NesHeader.byRomSize * 0x4000;
    VROM = (NesHeader.byVRomSize > 0) ? (BYTE *)p : 0;   // CHR-ROM, or NULL = CHR-RAM
    return 0;
}

void InfoNES_ReleaseRom(void) { ROM = 0; VROM = 0; }  // ROM lives in flash

// Average two RGB555 pixels without unpacking channels: the (a^b)&lowbits term
// cancels the carry that would otherwise bleed across channel boundaries. Low bit
// of each 555 channel (B0,G5,R10) = 0x0421. bit15 (backdrop flag) stripped first.
static inline WORD avg555(WORD a, WORD b)
{
    a &= 0x7FFF; b &= 0x7FFF;
    return (WORD)((a + b - ((a ^ b) & 0x0421)) >> 1);
}

// The core renders each 256-wide scanline (RGB555, bit15 = backdrop flag) directly
// into the ring slot core1 will consume -- one pass, no core0 convert. Dropped
// frames render into a throwaway buffer. core0 never blocks on the panel.
void __not_in_flash_func(InfoNES_PreDrawLine)(int line)
{
    (void)line;
#if NES_STREAM_FULLRES
    if (s_new_frame) {                                   // first visible line of the frame
        s_new_frame   = false;
        s_stream_this = (!s_frame_done && s_rd == s_wr); // stream only if core1 finished the last frame
#if NES_INTERLACE
        s_field_cur   = s_field;                         // lock this frame's field for core0 + core1
#endif
    }
    InfoNES_SetLineBuffer(s_workline, NES_DISP_WIDTH);   // PPU renders here; core0 converts in PostDrawLine
#else
    InfoNES_SetLineBuffer(s_workline, NES_DISP_WIDTH);
#endif
}

// 2:1 downscale into the back buffer, 555->565. Two modes (NES_DOWNSCALE_AVG):
//   AVG: 2x2 box average -- thin strokes survive as a softer pixel (won't vanish),
//        whole image slightly soft.
//   NEAREST: take even line / even column -- sharp, but a 1px stroke on an odd
//        column/row is dropped entirely.
void __not_in_flash_func(InfoNES_PostDrawLine)(int line)
{
    const WORD *src = s_workline;
#if NES_STREAM_FULLRES
    if (s_stream_this
#if NES_INTERLACE
        && (((line - NES_STREAM_FIRST) & 1) == s_field_cur)   // only this frame's field
#endif
    ) {
#if NES_PROFILE
        uint32_t w0 = time_us_32();
        while ((uint32_t)(s_wr - s_rd) >= NES_RING) tight_loop_contents();   // ring full -> wait for core1
        pf_wait += time_us_32() - w0;
#else
        while ((uint32_t)(s_wr - s_rd) >= NES_RING) tight_loop_contents();
#endif
        uint8_t *db = s_ring[s_wr % NES_RING];           // convert 555->565 into the next slot
        for (int x = 0; x < NES_STREAM_W; x++) {
            WORD v = (WORD)(src[x] & 0x7FFF);
            uint16_t c = (uint16_t)(((v & 0x7FE0) << 1) | (v & 0x1F));
            db[x * 2] = (uint8_t)(c >> 8); db[x * 2 + 1] = (uint8_t)(c & 0xFF);
        }
        __sync_synchronize();
        s_wr++;                     // publish -- core1 may now DMA it straight out
    }
    (void)line;
#elif NES_DOWNSCALE_AVG
    if (!(line & 1)) {                                    // even line: H-avg, stash for the pair
        for (int x = 0; x < NES_FB_W; x++)
            s_evenrow[x] = avg555(src[2 * x], src[2 * x + 1]);
        return;
    }
    int dst_row = line >> 1;                              // odd line: complete the 2x2 block
    if (dst_row >= NES_FB_H) return;
    uint16_t *dst = fb[g_draw] + (size_t)dst_row * NES_FB_W;
    for (int x = 0; x < NES_FB_W; x++) {
        WORD oddh = avg555(src[2 * x], src[2 * x + 1]);  // H-avg this line
        WORD v    = avg555(s_evenrow[x], oddh);          // V-avg with the even line -> 2x2 mean
        dst[x] = (uint16_t)(((v & 0x7FE0) << 1) | (v & 0x1F));   // 555 -> 565
    }
#else
    if (line & 1) return;                                 // nearest: drop odd lines (240 -> 120)
    int dst_row = line >> 1;
    if (dst_row >= NES_FB_H) return;
    uint16_t *dst = fb[g_draw] + (size_t)dst_row * NES_FB_W;
    for (int x = 0; x < NES_FB_W; x++) {
        WORD v = src[x << 1];                             // even column
        dst[x] = (uint16_t)(((v & 0x7FE0) << 1) | (v & 0x1F));   // 555 -> 565
    }
#endif
}

// Once-per-frame hook. Hand the just-rendered back buffer to core1 -- but ONLY if
// core1 is idle. If it's still blitting, DROP (keep emulating into the same buffer)
// so core0 never stalls. Audio is the master clock (snd_nes_flush below paces us).
int InfoNES_LoadFrame(void)
{
    // Drive InfoNES's native frame-skip from the setting. Fixed N (0..5) makes the
    // core skip the whole PPU pixel pipeline (PreDraw/Draw/PostDraw) on N of every
    // N+1 frames -- real CPU savings, not just a dropped blit -- while sprite-0 hit,
    // timing, and audio keep running. Auto (-1) renders every frame and relies on
    // the adaptive core1-busy drop below. FrameCnt==0 == this frame was rendered.
    FrameSkip = (g_settings.frameskip < 0) ? 0 : (WORD)g_settings.frameskip;

#if NES_STREAM_FULLRES
    if (s_stream_this) s_frame_done = true;     // tell core1: frame complete, close after it drains
    s_new_frame = true;                         // next frame re-decides whether to stream
#if NES_INTERLACE
    if (s_stream_this) s_field ^= 1;            // alternate field on each rendered frame
#endif
#if NES_PROFILE
    {
        uint32_t now = time_us_32();
        if (pf_t0) pf_dt += now - pf_t0;
        pf_t0 = now;
        if (!s_stream_this) pf_drops++;
        if (++pf_frames >= 120) {
            printf("NES frame=%luus  core0_wait=%luus  core1_stream=%luus  drops=%lu/120\n",
                   (unsigned long)(pf_dt/120), (unsigned long)(pf_wait/120),
                   (unsigned long)(pf_scnt ? pf_ssum/pf_scnt : 0), (unsigned long)pf_drops);
            pf_dt=pf_wait=pf_frames=pf_drops=pf_ssum=pf_scnt=0;
        }
    }
#endif
#else
    // Video: hand a freshly rendered frame to core1 only if it's idle, else drop.
    // Skipped frames (FrameCnt != 0) produced no new image, so never push one.
    if (FrameCnt == 0 && !g_core1_busy) {
        g_core1_busy = true;
        uint32_t show = (uint32_t)g_draw;
        g_draw ^= 1;                                // emulate next frame into the other buffer
        multicore_fifo_push_blocking(show);         // core1 blits fb[show] while core0 draws the other
    }
#endif

    // Audio + pacing run EVERY frame -- audio is the master clock.
    if (s_audio_paced) {
        snd_nes_flush();                            // frame-aligned audio flush; blocking DMA paces us
        return 0;
    }
    sleep_until(s_next);                            // fallback timer pace if audio isn't running
    s_next = delayed_by_us(s_next, NES_FRAME_US);
    if (to_us_since_boot(s_next) < to_us_since_boot(get_absolute_time()))
        s_next = make_timeout_time_us(NES_FRAME_US);
    return 0;
}

// ---------------------------------------------------------------------------
// Save states (the overlay's Save/Load State)
// ---------------------------------------------------------------------------
// The state file streams the InfoNES machine directly to SD -- no snapshot RAM
// buffer, exactly like the GB core -- so it costs no arena and fits the budget:
//   header (CPU + PPU registers + current bank mapping)
//   RAM (8K) SRAM (8K) PPURAM (16K) SPRRAM (256) ChrBuf (32K) PalTable APU_Reg
// On load these RAMs are overwritten in place: the arena is a deterministic bump
// allocator, so the same firmware + ROM re-Inits every buffer at the identical
// address, which also lets us store bank pointers as {region,offset} and rebuild
// them. This mirrors classic InfoNES's own .sts format.
//
// CAVEAT (documented, matches upstream): bank *pointers* capture the current
// mapping for every mapper, but each mapper's internal sequencing state (MMC1
// shift reg, MMC3 IRQ latch/counter, VRC IRQ) is static inside its .cpp and is
// NOT captured. Banking is correct on load; scanline-IRQ titles (SMB3, Kirby,
// Mega Man 3-6, ...) may show a sub-frame status-bar hitch immediately after
// load that self-corrects the same frame, because those games reprogram the
// mapper every frame. Simple mappers (NROM/UxROM/CNROM/AxROM/MMC1) restore
// cleanly.

// K6502 CPU registers -- plain file-scope globals in k6502.cpp (unmangled), only
// PC/IRQ/NMI are declared in k6502.h. Redeclaring here is a benign extern.
extern WORD PC;
extern BYTE SP, F, A, X, Y;
extern BYTE IRQ_State, IRQ_Wiring, NMI_State;

#define NES_STATE_MAGIC   0x534E4250u   // 'PBNS'
#define NES_STATE_VERSION 1u

typedef struct { uint8_t region; uint8_t pad[3]; uint32_t off; } nes_bankref_t;

// Encode a live bank pointer as (region, runtime-offset) so it survives a
// quit+relaunch of the same firmware+ROM. region 0 = arena (CHR-RAM pattern
// tables / work RAM), 1 = flash XIP (PRG-ROM / CHR-ROM). Offsets use the live
// bases, so they're independent of link addresses.
static nes_bankref_t nes_bank_encode(const void *p) {
    nes_bankref_t b; b.region = 1; b.pad[0] = b.pad[1] = b.pad[2] = 0; b.off = 0;
    const uint8_t *cp = (const uint8_t *)p;
    const uint8_t *ab = arena_base();
    if (cp >= ab && cp < ab + ARENA_BYTES) { b.region = 0; b.off = (uint32_t)(cp - ab); }
    else                                   { b.region = 1; b.off = (uint32_t)(cp - (const uint8_t *)XIP_BASE); }
    return b;
}
static uint8_t *nes_bank_decode(nes_bankref_t b) {
    return b.region == 0 ? arena_base() + b.off
                         : (uint8_t *)XIP_BASE + b.off;
}

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t hdr_size;
    uint8_t  mapper_no;
    uint8_t  rom_mirroring;
    uint16_t prg_16k_banks;        // NesHeader.byRomSize -- guards against a wrong ROM
    // CPU
    uint16_t pc;
    uint8_t  a, x, y, sp, f;
    uint8_t  irq_state, irq_wiring, nmi_state;
    // PPU registers / scroll / latches
    uint8_t  ppu_r0, ppu_r1, ppu_r2, ppu_r3, ppu_r7;
    uint8_t  ppu_scr_h_byte, ppu_scr_h_bit, ppu_latch_flag, ppu_updown_clip, ppu_nametablebank;
    uint16_t ppu_addr, ppu_temp, ppu_increment, ppu_scanline;
    uint8_t  vram_write_enable, frame_irq_enable, chrbuf_update, pad0;
    uint16_t frame_step, pad1;
    // current bank mapping
    nes_bankref_t rombank[4];
    nes_bankref_t ppubank[16];
} nes_state_hdr_t;

static bool save_nes_state(void) {
    if (!g_nes_state_path[0]) return false;
    FIL f;
    if (f_open(&f, g_nes_state_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;

    nes_state_hdr_t h;
    memset(&h, 0, sizeof h);
    h.magic = NES_STATE_MAGIC; h.version = NES_STATE_VERSION; h.hdr_size = (uint16_t)sizeof h;
    h.mapper_no = MapperNo; h.rom_mirroring = ROM_Mirroring; h.prg_16k_banks = NesHeader.byRomSize;
    h.pc = PC; h.a = A; h.x = X; h.y = Y; h.sp = SP; h.f = F;
    h.irq_state = IRQ_State; h.irq_wiring = IRQ_Wiring; h.nmi_state = NMI_State;
    h.ppu_r0 = PPU_R0; h.ppu_r1 = PPU_R1; h.ppu_r2 = PPU_R2; h.ppu_r3 = PPU_R3; h.ppu_r7 = PPU_R7;
    h.ppu_scr_h_byte = PPU_Scr_H_Byte; h.ppu_scr_h_bit = PPU_Scr_H_Bit;
    h.ppu_latch_flag = PPU_Latch_Flag; h.ppu_updown_clip = PPU_UpDown_Clip;
    h.ppu_nametablebank = PPU_NameTableBank;
    h.ppu_addr = PPU_Addr; h.ppu_temp = PPU_Temp; h.ppu_increment = PPU_Increment;
    h.ppu_scanline = PPU_Scanline;
    h.vram_write_enable = byVramWriteEnable; h.frame_irq_enable = FrameIRQ_Enable;
    h.chrbuf_update = ChrBufUpdate; h.frame_step = FrameStep;
    for (int i = 0; i < 4;  i++) h.rombank[i] = nes_bank_encode(ROMBANK[i]);
    for (int i = 0; i < 16; i++) h.ppubank[i] = nes_bank_encode(PPUBANK[i]);

    UINT bw = 0;
    bool ok = (f_write(&f, &h, sizeof h, &bw) == FR_OK && bw == sizeof h);
    if (ok) ok = (f_write(&f, RAM,     RAM_SIZE,     &bw) == FR_OK && bw == RAM_SIZE);
    if (ok) ok = (f_write(&f, SRAM,    SRAM_SIZE,    &bw) == FR_OK && bw == SRAM_SIZE);
    if (ok) ok = (f_write(&f, PPURAM,  PPURAM_SIZE,  &bw) == FR_OK && bw == PPURAM_SIZE);
    if (ok) ok = (f_write(&f, SPRRAM,  SPRRAM_SIZE,  &bw) == FR_OK && bw == SPRRAM_SIZE);
    if (ok && ChrBuf)
            ok = (f_write(&f, ChrBuf,  CHRBUF_SIZE,  &bw) == FR_OK && bw == CHRBUF_SIZE);
    if (ok) ok = (f_write(&f, PalTable, sizeof(WORD) * 32, &bw) == FR_OK);
    if (ok) ok = (f_write(&f, APU_Reg,  0x18,             &bw) == FR_OK);
    f_close(&f);
    return ok;
}

static bool load_nes_state(void) {
    if (!g_nes_state_path[0]) return false;
    FIL f;
    if (f_open(&f, g_nes_state_path, FA_READ) != FR_OK) return false;

    nes_state_hdr_t h;
    UINT br = 0;
    bool ok = (f_read(&f, &h, sizeof h, &br) == FR_OK && br == sizeof h
               && h.magic == NES_STATE_MAGIC && h.version == NES_STATE_VERSION
               && h.hdr_size == (uint16_t)sizeof h
               && h.mapper_no == MapperNo && h.prg_16k_banks == NesHeader.byRomSize);

    if (ok) {
        if (ok) ok = (f_read(&f, RAM,    RAM_SIZE,    &br) == FR_OK && br == RAM_SIZE);
        if (ok) ok = (f_read(&f, SRAM,   SRAM_SIZE,   &br) == FR_OK && br == SRAM_SIZE);
        if (ok) ok = (f_read(&f, PPURAM, PPURAM_SIZE, &br) == FR_OK && br == PPURAM_SIZE);
        if (ok) ok = (f_read(&f, SPRRAM, SPRRAM_SIZE, &br) == FR_OK && br == SPRRAM_SIZE);
        if (ok && ChrBuf)
                ok = (f_read(&f, ChrBuf, CHRBUF_SIZE, &br) == FR_OK && br == CHRBUF_SIZE);
        if (ok) ok = (f_read(&f, PalTable, sizeof(WORD) * 32, &br) == FR_OK);
        if (ok) ok = (f_read(&f, APU_Reg,  0x18,             &br) == FR_OK);
    }
    f_close(&f);
    if (!ok) return false;

    // Registers + current bank mapping.
    PC = h.pc; A = h.a; X = h.x; Y = h.y; SP = h.sp; F = h.f;
    IRQ_State = h.irq_state; IRQ_Wiring = h.irq_wiring; NMI_State = h.nmi_state;
    PPU_R0 = h.ppu_r0; PPU_R1 = h.ppu_r1; PPU_R2 = h.ppu_r2; PPU_R3 = h.ppu_r3; PPU_R7 = h.ppu_r7;
    PPU_Scr_H_Byte = h.ppu_scr_h_byte; PPU_Scr_H_Bit = h.ppu_scr_h_bit;
    PPU_Latch_Flag = h.ppu_latch_flag; PPU_UpDown_Clip = h.ppu_updown_clip;
    PPU_NameTableBank = h.ppu_nametablebank;
    PPU_Addr = h.ppu_addr; PPU_Temp = h.ppu_temp; PPU_Increment = h.ppu_increment;
    PPU_Scanline = h.ppu_scanline;
    byVramWriteEnable = h.vram_write_enable; FrameIRQ_Enable = h.frame_irq_enable;
    ChrBufUpdate = h.chrbuf_update; FrameStep = h.frame_step;
    for (int i = 0; i < 4;  i++) ROMBANK[i] = nes_bank_decode(h.rombank[i]);
    for (int i = 0; i < 16; i++) PPUBANK[i] = nes_bank_decode(h.ppubank[i]);
    ChrBufUpdate = 0xFF;   // force a CHR cache refresh from the restored banks
    return true;
}

// True if a save-state file currently exists for this game (greys Load State
// until there is something to load).
static bool nes_state_exists(void) {
    if (!g_nes_state_path[0]) return false;
    FILINFO fi;
    return f_stat(g_nes_state_path, &fi) == FR_OK;
}

// In-game pause overlay, mirroring the GB one. Runs on core0 while core1 is parked
// in its FIFO wait, so core0 owns the SPI bus. Edits brightness/volume live (NES
// audio reads g_settings.volume per frame; backlight is PWM), persists on exit if
// changed. NES has a fixed hardware palette so there's no Palette item, but Frame
// Skip is offered (-1=Auto, 0..5) to cut blit load on heavy titles. Save/Load State
// stream the machine to the .dat set by the loader (see save_nes_state above).
// Returns 1 = quit to menu, 0 = resume.
static int nes_overlay(void)
{
    static const char *const labels[] = {
        "Resume", "Brightness", "Volume", "Frame Skip", "Save State", "Load State", "Quit"
    };
    const int N = 7;
    const bool have_path = (g_nes_state_path[0] != '\0');   // Save State enabled
    bool have_saved = nes_state_exists();                   // Load State enabled
    int  sel = 0;
    bool dirty = false, redraw = true;
    int  ret = 0;
    char val[16];
    int  title_off = 0, title_hold = 20, title_tick = 0;

    while (true) {
        if (redraw) {
            st7789_fill(g_theme->bg);
            ui_pause_header(ui_now_playing(), title_off);
            for (int i = 0; i < N; i++) {
                int      y  = 40 + i * 20;
                bool     on = (i == sel);
                bool     dimmed = (i == 4 && !have_path) || (i == 5 && !have_saved);   // greyed
                uint16_t fg = on ? g_theme->sel_fg : (dimmed ? g_theme->footer_fg : g_theme->item_fg);
                uint16_t bg = on ? g_theme->sel_bg : g_theme->bg;
                if (on) ui_fill_pill(8, y - 3, LCD_W - 16, 18, bg);
                char lbl[24];
                snprintf(lbl, sizeof lbl, "%c %s", on ? '>' : ' ', labels[i]);
                st7789_draw_string(16, y, lbl, fg, bg, 1);
                val[0] = '\0';
                if (i == 1) snprintf(val, sizeof val, "%u%%", g_settings.lcd_brightness);
                else if (i == 2) snprintf(val, sizeof val, "%u%%", g_settings.volume);
                else if (i == 3) { if (g_settings.frameskip < 0) snprintf(val, sizeof val, "Auto");
                                   else snprintf(val, sizeof val, "%d", g_settings.frameskip); }
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
            if (sel == 1) {                                 // Brightness, live PWM
                int b = (int)g_settings.lcd_brightness + d * 5;
                if (b < 10) b = 10; if (b > 100) b = 100;
                g_settings.lcd_brightness = (uint8_t)b;
                st7789_backlight_level(g_settings.lcd_brightness);
                dirty = true; redraw = true;
            } else if (sel == 2) {                          // Volume (audio reads it live)
                int v = (int)g_settings.volume + d * 5;
                if (v < 0) v = 0; if (v > 100) v = 100;
                g_settings.volume = (uint8_t)v;
                dirty = true; redraw = true;
            } else if (sel == 3) {                          // Frame Skip: -1=Auto, 0..5
                int f = (int)g_settings.frameskip + d;
                if (f < -1) f = 5; if (f > 5) f = -1;
                g_settings.frameskip = (int8_t)f;
                dirty = true; redraw = true;
            }
        }

        if (ev & (1u << BTN_A)) {
            if (sel == 0) { ret = 0; break; }               // Resume
            else if (sel == 4 && have_path) {               // Save State
                led_set_state(LED_SD_BUSY);
                bool sok = save_nes_state();
                led_set_state(LED_RUNNING);
                if (sok) have_saved = true;                 // Load State is now available
                st7789_fill_rect(16, 188, LCD_W - 32, 10, g_theme->bg);
                st7789_draw_string(16, 188, sok ? "State saved" : "Save failed",
                                   sok ? g_theme->ok : g_theme->err, g_theme->bg, 1);
            }
            else if (sel == 5 && have_saved) {              // Load State -> resume into it
                led_set_state(LED_SD_BUSY);
                bool lok = load_nes_state();
                led_set_state(LED_RUNNING);
                if (lok) { ret = 0; break; }
                st7789_fill_rect(16, 188, LCD_W - 32, 10, g_theme->bg);
                st7789_draw_string(16, 188, "No state / load failed", g_theme->err, g_theme->bg, 1);
            }
            else if (sel == 6) { ret = 1; break; }          // Quit to menu
        }
        sleep_ms(15);
    }
    if (dirty) settings_save();
    return ret;
}

void InfoNES_PadState(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem)
{
    buttons_update();

    // MENU opens the pause overlay (edge-triggered, so holding it doesn't re-open).
    // Wait for core1 to finish its in-flight blit, then it parks in the FIFO wait
    // while the overlay drives the panel from core0. Clear the screen on resume so
    // overlay text doesn't linger in the pillarbox bars (the game blit is 256-wide).
    if (buttons_pressed() & (1u << BTN_MENU)) {
#if NES_STREAM_FULLRES
        while (s_frame_done || s_rd != s_wr) tight_loop_contents();  // core1 drains + closes, then idle-spins
#else
        while (g_core1_busy) tight_loop_contents();
#endif
        int r = nes_overlay();
        st7789_fill(COL_BLACK);
        if (r == 1) {                                       // Quit -> end InfoNES_Cycle
            *pdwPad1 = 0; *pdwPad2 = 0; *pdwSystem = PAD_SYS_QUIT;
            return;
        }
        buttons_update();                                   // refresh edges after the modal
    }

    uint16_t s = buttons_state();
    DWORD p = 0;
    if (s & (1u << BTN_A))      p |= 1u << 0;
    if (s & (1u << BTN_B))      p |= 1u << 1;
    if (s & (1u << BTN_SELECT)) p |= 1u << 2;
    if (s & (1u << BTN_START))  p |= 1u << 3;
    if (s & (1u << BTN_UP))     p |= 1u << 4;
    if (s & (1u << BTN_DOWN))   p |= 1u << 5;
    if (s & (1u << BTN_LEFT))   p |= 1u << 6;
    if (s & (1u << BTN_RIGHT))  p |= 1u << 7;
    *pdwPad1   = p;
    *pdwPad2   = 0;
    *pdwSystem = 0;                                          // MENU handled above, not a quit
}

// ---- Audio: InfoNES 6-wave APU -> mono -> shared I2S (see audio.c). The
//      blocking flush in snd_nes_output() paces the emulator (audio-as-master). ----
void InfoNES_SoundInit(void) {}
int  InfoNES_SoundOpen(int samples_per_sync, int sample_rate)
{
    (void)samples_per_sync; (void)sample_rate;      // core runs 44100; shared I2S already at 44100
    snd_nes_open();
    s_audio_paced = true;
    return 0;
}
void InfoNES_SoundClose(void) { snd_nes_close(); s_audio_paced = false; }
int  InfoNES_GetSoundBufferSize(void) { return snd_nes_room(); }
void InfoNES_SoundOutput(int samples, BYTE *w1, BYTE *w2, BYTE *w3, BYTE *w4, BYTE *w5, BYTE *w6)
{
    snd_nes_output(samples, w1, w2, w3, w4, w5, w6);
}

void InfoNES_DebugPrint(const char *pszMsg)        { (void)pszMsg; }
void InfoNES_MessageBox(const char *pszMsg, ...)   { (void)pszMsg; }
void InfoNES_Error(const char *pszMsg, ...)        { (void)pszMsg; }
int  InfoNES_Menu(void)                            { return 0; }   // unused (no InfoNES_Main)

// ---------------------------------------------------------------------------
// run entry (called from the C loader after staging the .nes into flash)
// ---------------------------------------------------------------------------
extern "C" void nes_run(void)
{
    arena_reset();                                  // empty arena for this launch
    led_set_state(LED_RUNNING);
    st7789_fill(COL_BLACK);                          // clears the screen incl. pillarbox bars

    InfoNES_SetRegion(INFONES_REGION_NTSC);          // must precede Init (sets scan/APU timing)
    InfoNES_Init();                                  // f_malloc's core buffers + K6502 + scan table

    s_next = make_timeout_time_us(NES_FRAME_US);

    if (InfoNES_Load("") == 0) {                     // ReadRom (flash) + Reset (banks/mapper)
#if !NES_STREAM_FULLRES
        // Two 16-bit framebuffers from the arena -> true double-buffer (60K total).
        size_t fbsz = (size_t)NES_FB_W * NES_FB_H * 2u;
        fb[0] = (uint16_t *)Frens::f_malloc(fbsz);
        fb[1] = (uint16_t *)Frens::f_malloc(fbsz);
        if (!fb[0] || !fb[1]) {                      // arena too small (shouldn't happen at 128x120)
            st7789_fill(g_theme->bg);
            ui_header("NES");
            st7789_draw_string(8, 40, "Out of RAM (fb)", g_theme->err, g_theme->bg, 1);
            ui_footer("B back");
            led_set_count(4);              // blink code 4: out of memory
            led_set_state(LED_ERROR);
            while (true) { buttons_update(); if (buttons_pressed() & (1u << BTN_B)) break; sleep_ms(15); }
            led_set_count(2);
            InfoNES_Fin(); led_set_state(LED_IDLE); return;
        }
        memset(fb[0], 0, fbsz);
        memset(fb[1], 0, fbsz);
#endif

        // Restore battery SRAM if a .srm exists for this game (Init/Reset zeroed it).
        if (g_nes_save_path[0]) {
            FIL f;
            if (f_open(&f, g_nes_save_path, FA_READ) == FR_OK) {
                UINT br = 0;
                f_read(&f, SRAM, SRAM_SIZE, &br);
                f_close(&f);
            }
        }
        SRAMwritten = false;                            // only re-save if the game writes during play

        // Start the display core (GB pattern): core0 emulates + audio + fills the
        // back buffer; core1 blocks on the FIFO, scale-blits whatever core0 hands it.
#if !NES_STREAM_FULLRES
        g_draw = 0;
        g_core1_busy = false;
        multicore_launch_core1(core1_display);
#else
        s_wr = s_rd = 0; s_frame_done = false;       // core1 owns the panel; core0 just produces lines
        s_new_frame = true; s_stream_this = false;
        s_core1_run = true; s_core1_idle = false;
        multicore_launch_core1(core1_stream);
#endif

        InfoNES_Cycle();                             // runs until PAD_SYS_QUIT (MENU)

#if !NES_STREAM_FULLRES
        while (g_core1_busy) tight_loop_contents();  // let the last blit finish
        multicore_reset_core1();                     // stop the display core cleanly
#else
        s_core1_run = false;                         // ask core1 to finish + exit
        while (!s_core1_idle) tight_loop_contents(); // it closes any open window on the way out
        multicore_reset_core1();                     // stop the display core cleanly
#endif

        // Battery save: write SRAM back to the .srm if the game dirtied it.
        if (g_nes_save_path[0] && SRAMwritten) {
            led_set_state(LED_SD_BUSY);
            FIL f;
            if (f_open(&f, g_nes_save_path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
                UINT bw = 0;
                f_write(&f, SRAM, SRAM_SIZE, &bw);
                f_close(&f);
            }
            SRAMwritten = false;
        }
    } else {
        st7789_fill(g_theme->bg);
        ui_header("NES");
        st7789_draw_string(8, 40, "ROM load failed", g_theme->err, g_theme->bg, 1);
        ui_footer("B back");
        led_set_count(3);              // blink code 3: ROM load failed
        led_set_state(LED_ERROR);
        while (true) { buttons_update(); if (buttons_pressed() & (1u << BTN_B)) break; sleep_ms(15); }
    }

    led_set_count(2);                                // restore default blink count
    InfoNES_Fin();                                   // f_free's (no-op)
    led_set_state(LED_IDLE);
}