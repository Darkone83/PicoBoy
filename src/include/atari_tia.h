#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ATARI_TIA_VISIBLE_W 160
#define ATARI_TIA_VISIBLE_H 192
#define ATARI_TIA_HBLANK    68
#define ATARI_TIA_LINE_CLOCKS 228
#define ATARI_TIA_CLOCK_HZ    3579545u   // NTSC colour clock
#define ATARI_TIA_AUDIO_RATE  44100u

// TIA write registers.
enum {
    TIA_VSYNC=0x00, TIA_VBLANK=0x01, TIA_WSYNC=0x02, TIA_RSYNC=0x03,
    TIA_NUSIZ0=0x04, TIA_NUSIZ1=0x05,
    TIA_COLUP0=0x06, TIA_COLUP1=0x07, TIA_COLUPF=0x08, TIA_COLUBK=0x09,
    TIA_CTRLPF=0x0A, TIA_REFP0=0x0B, TIA_REFP1=0x0C,
    TIA_PF0=0x0D, TIA_PF1=0x0E, TIA_PF2=0x0F,
    TIA_RESP0=0x10, TIA_RESP1=0x11, TIA_RESM0=0x12, TIA_RESM1=0x13,
    TIA_RESBL=0x14,
    TIA_AUDC0=0x15, TIA_AUDC1=0x16, TIA_AUDF0=0x17, TIA_AUDF1=0x18,
    TIA_AUDV0=0x19, TIA_AUDV1=0x1A,
    TIA_GRP0=0x1B, TIA_GRP1=0x1C,
    TIA_ENAM0=0x1D, TIA_ENAM1=0x1E, TIA_ENABL=0x1F,
    TIA_HMP0=0x20, TIA_HMP1=0x21, TIA_HMM0=0x22, TIA_HMM1=0x23,
    TIA_HMBL=0x24, TIA_VDELP0=0x25, TIA_VDELP1=0x26, TIA_VDELBL=0x27,
    TIA_RESMP0=0x28, TIA_RESMP1=0x29, TIA_HMOVE=0x2A,
    TIA_HMCLR=0x2B, TIA_CXCLR=0x2C
};

// TIA read registers.
enum {
    TIA_CXM0P=0x00, TIA_CXM1P=0x01, TIA_CXP0FB=0x02, TIA_CXP1FB=0x03,
    TIA_CXM0FB=0x04, TIA_CXM1FB=0x05, TIA_CXBLPF=0x06, TIA_CXPPMM=0x07,
    TIA_INPT0=0x08, TIA_INPT1=0x09, TIA_INPT2=0x0A, TIA_INPT3=0x0B,
    TIA_INPT4=0x0C, TIA_INPT5=0x0D
};

void atari_tia_reset(void);
uint8_t atari_tia_read(uint16_t address);
void    atari_tia_write(uint16_t address, uint8_t value);

// Advance up to the end of the current scanline in one chunk.
 // Returns 1 when the chunk completes the 228-colour-clock line, else 0.
 // The caller deliberately splits at line boundaries so atari_tia_line()
 // always remains the just-completed line when a line is reported.
 uint8_t atari_tia_advance(uint16_t clocks);

 // Compatibility helpers used by diagnostics/older code.
 bool atari_tia_tick(void);
 bool atari_tia_tick_clocks(uint8_t clocks);

const uint8_t *atari_tia_line(void);

bool atari_tia_vsync(void);
bool atari_tia_vblank(void);
bool atari_tia_wsync(void);
uint16_t atari_tia_color_clock(void);

void atari_tia_set_fire(bool pressed);

// Instantaneous raw DAC sum, retained for diagnostics.
uint8_t atari_tia_audio_level(void);

// Pop one time-averaged, nonlinear-mixed 44.1 kHz TIA sample (0..32767).
// atari_tia_advance() generates these internally, so the platform scheduler
// no longer has to split TIA advancement at every PCM sample boundary.
bool atari_tia_audio_pop(uint16_t *level);

// NTSC palette used by the PicoBoy display consumer.  TIA pixels are 0..127.
extern const uint16_t atari_tia_palette565[128];