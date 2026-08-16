#include "audio.h"
#include "pins.h"
#include "settings.h"
#include "i2s.h"
#include "minigb_apu.h"
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

void snd_init(void) {
    if (s_inited) return;
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
    static int16_t smp[SND_FRAMES * 2];           // stereo interleaved L,R
    if (!s_inited) return false;

    audio_callback(NULL, smp, AUDIO_BUFFER_SIZE_BYTES);

    uint8_t vol = g_settings.volume;              // 0..100 (%)
    if (vol == 0) {
        memset(smp, 0, sizeof smp);               // mute -- DMA still runs to keep I2S clocked
    } else {
        // minigb_apu output is hot. GB_AUDIO_HEADROOM is the attenuation shift that
        // rides under the 0-100% Volume setting: 2 = Pico-GB quiet default (-12 dB),
        // 1 = ~2x louder (-6 dB), 0 = full scale (loudest; peaks may sound harsh).
        for (unsigned i = 0; i < SND_FRAMES * 2; i++) {
            int32_t v = (((int32_t)smp[i] * vol) / 100) >> GB_AUDIO_HEADROOM;
            if (v >  32767) v =  32767;           // safety clamp (also covers future boost)
            if (v < -32768) v = -32768;
            smp[i] = (int16_t)v;
        }
    }

    s_cfg.dma_trans_count = AUDIO_SAMPLES;        // GB count (NES may have changed it)
    i2s_dma_write(&s_cfg, smp);   // waits for the previous buffer to finish draining,
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

#define NES_AUDIO_HEADROOM 0         // >>2 on the mono mix; lower = louder

static int16_t  s_nesbuf[SND_FRAMES * 2];   // one I2S buffer, stereo L/R
static unsigned s_nesfill = 0;                 // stereo frames accumulated

void snd_nes_open(void) {
    snd_init();                       // shared I2S up (idempotent)
    s_nesfill = 0;
}

int snd_nes_room(void) {
    if (!s_inited) return 0;
    return (int)(SND_FRAMES - s_nesfill);
}

void snd_nes_output(int n, const uint8_t *w1, const uint8_t *w2, const uint8_t *w3,
                    const uint8_t *w4, const uint8_t *w5, const uint8_t *w6) {
    if (!s_inited) return;
    uint8_t vol = g_settings.volume;              // 0..100 (%)
    for (int i = 0; i < n; i++) {
        if (s_nesfill >= SND_FRAMES) break;       // frame buffer full (flushed in LoadFrame)
        int s = 0;
        if (vol) {
            int a1 = w1[i], a2 = w2[i], a3 = w3[i], a4 = w4[i], a5 = w5[i];
            int a6 = w6 ? w6[i] : 0;
            int l = a1 * 6 + a2 * 3 + a3 * 5 + a4 * 51 + a5 * 80 + a6 * 18;
            int r = a1 * 3 + a2 * 6 + a3 * 5 + a4 * 51 + a5 * 80 + a6 * 18;
            int mono = (l + r) >> 1;
            s = ((mono * vol) / 100) >> NES_AUDIO_HEADROOM;
            if (s > 32767) s = 32767;             // mix is unipolar (>= 0)
        }
        s_nesbuf[s_nesfill * 2]     = (int16_t)s;
        s_nesbuf[s_nesfill * 2 + 1] = (int16_t)s; // mono -> both channels
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
    i2s_dma_write(&s_cfg, s_nesbuf);              // blocks on prior DMA -> paces
    s_nesfill = 0;
}

void snd_nes_close(void) { s_nesfill = 0; }