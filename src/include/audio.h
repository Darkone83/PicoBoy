#pragma once
#include <stdint.h>
#include <stdbool.h>

// Sound path: minigb_apu (Game Boy APU) rendered to PCM, scaled by the Volume
// setting, and pushed to the MAX98357 over DMA-driven I2S (third_party/i2s).

// Idempotent: bring up the I2S DMA driver (claims a pio1 SM + one DMA channel).
void snd_init(void);

// Reset the APU channel state. Call at the start of each game.
void snd_apu_reset(void);

// Render one GB frame of audio (AUDIO_SAMPLES stereo samples), scale by the
// current Volume setting, and queue the DMA transfer. Call once per GB frame.
// Returns true if it fed the DMA -- in which case the DMA drain has paced this
// iteration to the exact audio consumption rate and no other pacer is needed.
bool snd_play_frame(void);

// Diagnostics square-wave tone. Returns true if audio is up.
bool snd_tone(uint32_t hz, uint32_t ms);

// --- NES (InfoNES) audio: 6-wave APU output folded to mono -> shared I2S ---
// Reuses the same I2S DMA backend as the GB path (never run simultaneously).
// snd_nes_open() brings I2S up (idempotent) and resets the accumulator.
// snd_nes_room() reports free stereo frames before the next flush (feeds the
// core's InfoNES_GetSoundBufferSize). snd_nes_output() mixes 'n' samples
// (waves are unsigned 0..255; wave6 may be NULL) to mono, scales by Volume,
// accumulates, and flushes one I2S buffer via DMA when full -- the flush blocks
// on the prior transfer, so it paces NES emulation to the audio rate.
void snd_nes_open(void);
int  snd_nes_room(void);
void snd_nes_output(int n, const uint8_t *w1, const uint8_t *w2, const uint8_t *w3,
                    const uint8_t *w4, const uint8_t *w5, const uint8_t *w6);
void snd_nes_flush(void);
void snd_nes_close(void);


// --- Atari 2600 TIA audio -> shared I2S ------------------------------------
// atari_tia integrates/anti-aliases the two DAC channels on the colour-clock
// timeline and supplies a nonlinear-mixed unsigned 16-bit level at 44.1 kHz.
// This layer AC-couples, scales by Volume, and queues fixed DMA blocks.
void snd_atari_open(void);
void snd_atari_sample(uint16_t level);  // nonlinear TIA mix, 0..32767
bool snd_atari_flush(void);             // drain a partial block at pause/exit
void snd_atari_close(void);