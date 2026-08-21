#include "audio.h"
#include "pins.h"
#include "settings.h"
#include "i2s.h"
#include "minigb_apu.h"
#include "arena.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include <string.h>

// I2S out to MAX98357 via the DMA driver in third_party/i2s, fed by minigb_apu.
// The LED uses pio0/sm0, so I2S lives on pio1. Volume is applied in software as a
// linear 0-100% scale (settings.volume) -- a true mute at 0, full scale at 100 --
// so the driver's own shift-volume is left at 0 (straight copy).

static i2s_config_t s_cfg;
static bool s_inited = false;

// AUDIO_SAMPLES in minigb_apu.h is computed via floating point (44100 / 59.7275),
// so it's not an integer constant expression and can't size a static array.
// Recompute the identical value (738) with integer math for the buffer dimension.
#define SND_FRAMES   ((AUDIO_SAMPLE_RATE * 70224u) / 4194304u)   // == AUDIO_SAMPLES

// One emulator runs at a time. Put the common stereo PCM staging buffer in
// the 3 KiB tail reserved by arena.h instead of permanently charging .bss.
static int16_t *s_pcmbuf = NULL;

void snd_init(void) {
    if (s_inited) return;
    s_pcmbuf = (int16_t *)arena_audio_base();
    s_cfg = i2s_get_default_config();
    s_cfg.sample_freq     = AUDIO_SAMPLE_RATE;   // 44100
    s_cfg.channel_count   = 2;
    s_cfg.pio             = pio1;                 // pio0 is the LED's
    s_cfg.data_pin        = I2S_DOUT_PIN;
    s_cfg.clock_pin_base  = I2S_BCLK_PIN;         // base = BCLK, base+1 must be LRCLK
    s_cfg.dma_trans_count = AUDIO_SAMPLES;        // one GB frame of 32-bit stereo frames
    s_cfg.volume          = 0;                    // 0 = straight copy; we scale ourselves
    i2s_init(&s_cfg);
    s_inited = true;
}

void snd_apu_reset(void) {
    snd_init();
    audio_init();                                 // minigb_apu: reset channel state
}

// GB audio attenuation under Volume: 2 = original quiet default, 1 = louder,
// 0 = loudest (full scale). Bump the MAX98357 GAIN pin for more without clipping.
#define GB_AUDIO_HEADROOM 1

bool snd_play_frame(void) {
    if (!s_inited) return false;

    audio_callback(NULL, s_pcmbuf, AUDIO_BUFFER_SIZE_BYTES);

    uint8_t vol = g_settings.volume;              // 0..100 (%)
    if (vol == 0) {
        memset(s_pcmbuf, 0, SND_FRAMES * 2u * sizeof(*s_pcmbuf));               // mute -- DMA still runs to keep I2S clocked
    } else {
        // minigb_apu output is hot. GB_AUDIO_HEADROOM is the attenuation shift that
        // rides under the 0-100% Volume setting: 2 = Pico-GB quiet default (-12 dB),
        // 1 = ~2x louder (-6 dB), 0 = full scale (loudest; peaks may sound harsh).
        for (unsigned i = 0; i < SND_FRAMES * 2; i++) {
            int32_t v = (((int32_t)s_pcmbuf[i] * vol) / 100) >> GB_AUDIO_HEADROOM;
            if (v >  32767) v =  32767;           // safety clamp (also covers future boost)
            if (v < -32768) v = -32768;
            s_pcmbuf[i] = (int16_t)v;
        }
    }

    s_cfg.dma_trans_count = AUDIO_SAMPLES;        // GB count (NES may have changed it)
    i2s_dma_write(&s_cfg, s_pcmbuf);   // waits for the previous buffer to finish draining,
    return true;                  // so this call paces the loop to the exact 44100 Hz rate
}

bool snd_tone(uint32_t hz, uint32_t ms) {
    snd_init();
    if (!s_inited || hz == 0 || ms == 0) return false;

    uint32_t total = (uint32_t)(((uint64_t)AUDIO_SAMPLE_RATE * ms) / 1000u);
    uint32_t half  = AUDIO_SAMPLE_RATE / (hz * 2u);
    if (half == 0) half = 1;

    // DMA is idle outside gameplay, so feeding the SM FIFO directly is safe here.
    for (uint32_t i = 0; i < total; i++) {
        int16_t s = ((i / half) & 1u) ? (int16_t)8000 : (int16_t)-8000;
        uint32_t frame = ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;  // L | R
        pio_sm_put_blocking(s_cfg.pio, s_cfg.sm, frame);              // self-paces at the sample rate
    }
    sleep_ms(5);
    return true;
}

// ---------------------------------------------------------------------------
// NES (InfoNES) audio. The InfoNES APU is pull-based: per H-sync the core asks
// InfoNES_GetSoundBufferSize() how much room there is, then hands that many
// samples to InfoNES_SoundOutput() as six unsigned 0..255 wave channels (2
// pulse, triangle, noise, DPCM, + optional expansion in wave6). We fold to
// mono for the single MAX98357, scale by Volume with >>2 headroom (the summed
// mix peaks near 41565, well over int16), accumulate one I2S buffer, and flush
// via i2s_dma_write -- which blocks on the prior DMA, pacing the emulator to
// the audio rate (audio-as-master, same as the GB path).
//
// The shared I2S config is left at the GB settings (44100 Hz, AUDIO_SAMPLES
// frames per buffer); InfoNES also runs 44100 Hz, and ~735 samples/NES-frame
// vs the 738-frame buffer is a continuous stream with no pitch error.

// NES makeup gain, applied AFTER DC removal (below). The InfoNES mix is unipolar
// (0..~41565, big DC offset) the amp cannot reproduce, and its AC content is
// low-amplitude -- so we high-pass to centre at 0, then apply a healthy gain.
// Peaks are soft-clipped (nes_soft_clip) rather than hard-clamped, so you can
// push this well past unity without harshness. Raise NUM to taste vs GB.
#define NES_AUDIO_GAIN_NUM 4
#define NES_AUDIO_GAIN_DEN 1

static int s_nes_px = 0;   // DC-blocker: previous input
static int s_nes_hp = 0;   // DC-blocker: previous output

static unsigned s_nesfill = 0;                 // stereo frames accumulated

void snd_nes_open(void) {
    snd_init();                       // shared I2S up (idempotent)
    s_nesfill = 0;
    s_nes_px = s_nes_hp = 0;          // reset the DC blocker for the new game
}

int snd_nes_room(void) {
    if (!s_inited) return 0;
    return (int)(SND_FRAMES - s_nesfill);
}

// Soft clip to +/-32767: linear up to THR, then a smooth knee that asymptotes to
// full scale (never exceeds it). Lets the gain be pushed hard without the harsh
// edge of hard clipping.
static int nes_soft_clip(int x) {
    const int THR = 22000, LIM = 32767, HR = LIM - THR;
    int neg = x < 0, ax = neg ? -x : x;
    if (ax > THR) {
        int over = ax - THR;
        ax = THR + (int)(((long long)over * HR) / (over + HR));
    }
    return neg ? -ax : ax;
}

void snd_nes_output(int n, const uint8_t *w1, const uint8_t *w2, const uint8_t *w3,
                    const uint8_t *w4, const uint8_t *w5, const uint8_t *w6) {
    if (!s_inited) return;
    uint8_t vol = g_settings.volume;              // 0..100 (%)
    for (int i = 0; i < n; i++) {
        if (s_nesfill >= SND_FRAMES) break;       // frame buffer full (flushed in LoadFrame)
        // Always run the mixer + DC blocker, even while muted. If filter state
        // freezes at Volume=0, unmuting later compares the current waveform to
        // stale state and can produce a short pop/transient. Muting only gates
        // the final sample sent to I2S; the filter continues tracking the APU.
        int a1 = w1[i], a2 = w2[i], a3 = w3[i], a4 = w4[i], a5 = w5[i];
        int a6 = w6 ? w6[i] : 0;
        int l = a1 * 6 + a2 * 3 + a3 * 5 + a4 * 51 + a5 * 80 + a6 * 18;
        int r = a1 * 3 + a2 * 6 + a3 * 5 + a4 * 51 + a5 * 80 + a6 * 18;
        int mono = (l + r) >> 1;                  // 0..~41565, unipolar (DC-offset)

        // DC blocker (one-pole high-pass, R~0.996 / fc~28 Hz): recentre at 0
        // so the amp reproduces the full swing, not a DC pedestal it eats.
        int hp = mono - s_nes_px + (s_nes_hp - (s_nes_hp >> 8));
        s_nes_px = mono;
        s_nes_hp = hp;

        int s = 0;                                // muted output remains true zero
        if (vol) {
            int v = ((hp * vol) / 100) * NES_AUDIO_GAIN_NUM / NES_AUDIO_GAIN_DEN;
            s = nes_soft_clip(v);                 // graceful saturation, not hard clip
        }
        s_pcmbuf[s_nesfill * 2]     = (int16_t)s;
        s_pcmbuf[s_nesfill * 2 + 1] = (int16_t)s; // mono -> both channels
        s_nesfill++;
    }
}

// Flush one NES frame of audio. Sends exactly the samples the APU produced this
// frame (its true 44100 rate, frame-aligned), so the blocking DMA paces the
// emulator to the correct frame rate without the mid-frame jitter of a fixed
// 738-chunk flush. Called once per frame from InfoNES_LoadFrame().
void snd_nes_flush(void) {
    if (!s_inited || s_nesfill == 0) return;
    s_cfg.dma_trans_count = s_nesfill;            // this frame's exact sample count
    i2s_dma_write(&s_cfg, s_pcmbuf);              // blocks on prior DMA -> paces
    s_nesfill = 0;
}

void snd_nes_close(void) { s_nesfill = 0; }

// ---------------------------------------------------------------------------
// Atari 2600 TIA audio.
// The TIA core now integrates its channel DAC levels over real colour-clock
// time and supplies an anti-aliased, nonlinear-mixed 0..32767 sample at 44.1
// kHz. This layer only AC-couples, applies Volume, and feeds the shared I2S DMA.
// Audio remains continuous/fixed-block rather than frame-sized.

static unsigned s_atari_fill = 0;
static bool     s_atari_block_queued = false;
static bool     s_atari_filter_seeded = false;
static int      s_atari_px = 0;
static int      s_atari_hp = 0;

// Continuous DMA blocks: ~11.6 ms at 44.1 kHz.  The previous 256-sample
// block doubled the number of DMA hand-offs while the emulator was still just
// shy of real time.  512 remains below the I2S driver's 738-frame allocation,
// cuts hand-off overhead in half, and gives the producer more jitter margin.
#define ATARI_AUDIO_BLOCK 512u

static void atari_write_block(void) {
    if (!s_inited || s_atari_fill == 0)
        return;

    s_cfg.dma_trans_count = s_atari_fill;
    i2s_dma_write(&s_cfg, s_pcmbuf);
    s_atari_fill = 0;
    s_atari_block_queued = true;
}

void snd_atari_open(void) {
    snd_init();
    s_atari_fill = 0;
    s_atari_block_queued = false;
    s_atari_filter_seeded = false;
    s_atari_px = 0;
    s_atari_hp = 0;
}

void snd_atari_sample(uint16_t level) {
    if (!s_inited)
        return;

    if (level > 32767u)
        level = 32767u;

    if (s_atari_fill >= ATARI_AUDIO_BLOCK)
        atari_write_block();

    // The TIA core now supplies native phase-clock-averaged samples and applies
    // the nonlinear two-channel mixer.  This layer only performs the output
    // AC coupling and user volume.  Do not add makeup gain here: sharpening the
    // source cadence, rather than boosting/clipping it, is the audio fix.
    int raw = (int)level;
    int hp;

    if (!s_atari_filter_seeded) {
        // Start the high-pass at the current DC level instead of generating a
        // full-scale startup transient.
        s_atari_px = raw;
        s_atari_hp = 0;
        s_atari_filter_seeded = true;
        hp = 0;
    } else {
        hp = raw - s_atari_px + (s_atari_hp - (s_atari_hp >> 8));
        s_atari_px = raw;
        s_atari_hp = hp;
    }

    int sample = 0;
    const uint8_t vol = g_settings.volume;
    if (vol) {
        int v = (hp * (int)vol) / 100;

        // Final integer-range safety only; there is no normal-path compressor
        // or makeup stage.
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        sample = v;
    }

    s_pcmbuf[s_atari_fill * 2]     = (int16_t)sample;
    s_pcmbuf[s_atari_fill * 2 + 1] = (int16_t)sample;
    s_atari_fill++;

    if (s_atari_fill >= ATARI_AUDIO_BLOCK)
        atari_write_block();
}

bool snd_atari_flush(void) {
    if (!s_inited)
        return false;

    if (s_atari_fill)
        atari_write_block();

    bool queued = s_atari_block_queued;
    s_atari_block_queued = false;
    return queued;
}

void snd_atari_close(void) {
    s_atari_fill = 0;
    s_atari_block_queued = false;
    s_atari_filter_seeded = false;
    s_atari_px = 0;
    s_atari_hp = 0;
}
// ---------------------------------------------------------------------------
// Sega Master System / Game Gear (SMSPlus) audio.
// SMSPlus produces signed stereo PSG samples at 44.1 kHz. PicoBoy has a mono
// speaker path, so average L/R, AC-couple the result, apply the common Volume
// control and mirror it to both I2S channels. A full SMS frame is 735 samples,
// which fits in the shared 738-frame staging buffer used by GB/NES/Atari.

static bool s_sms_filter_seeded = false;
static int  s_sms_px = 0;
static int  s_sms_hp = 0;

void snd_sms_open(void) {
    snd_init();
    s_sms_filter_seeded = false;
    s_sms_px = 0;
    s_sms_hp = 0;
}

bool snd_sms_frame(const int16_t *left, const int16_t *right, int n) {
    if (!s_inited || !left || !right || n <= 0)
        return false;

    if (n > (int)SND_FRAMES)
        n = (int)SND_FRAMES;

    const uint8_t vol = g_settings.volume;

    for (int i = 0; i < n; i++) {
        int raw = ((int)left[i] + (int)right[i]) / 2;
        int hp;

        // Keep the same low-cost DC blocker used by the other PicoBoy cores.
        // Seed it at the first sample so entering a game cannot create a pop.
        if (!s_sms_filter_seeded) {
            s_sms_px = raw;
            s_sms_hp = 0;
            s_sms_filter_seeded = true;
            hp = 0;
        } else {
            hp = raw - s_sms_px + (s_sms_hp - (s_sms_hp >> 8));
            s_sms_px = raw;
            s_sms_hp = hp;
        }

        int v = vol ? (hp * (int)vol) / 100 : 0;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;

        s_pcmbuf[i * 2]     = (int16_t)v;
        s_pcmbuf[i * 2 + 1] = (int16_t)v;
    }

    s_cfg.dma_trans_count = (uint32_t)n;
    i2s_dma_write(&s_cfg, s_pcmbuf);  // waits for previous DMA: audio is master clock
    return true;
}

void snd_sms_close(void) {
    s_sms_filter_seeded = false;
    s_sms_px = 0;
    s_sms_hp = 0;
}