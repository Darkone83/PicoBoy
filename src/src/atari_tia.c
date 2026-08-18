/*
 * PicoBoy Atari 2600 TIA model.
 *
 * The register/object model is derived from the small dgrubb/HiFive1-2600 core
 * as carried by xrip/pico-atari2600, but the VGA-specific framebuffer path is
 * removed.  PicoBoy emits one 160-pixel palette-index scanline and lets
 * atari_core hand complete frames to the existing ST7789 display path.
 *
 * M1 target: NTSC, joystick + console switches, no audio synthesis yet.
 */
#include "atari_tia.h"
#include <string.h>

const uint16_t atari_tia_palette565[128] = {
    0x0000, 0x18C3, 0x39C7, 0x5ACB, 0x7BEF, 0xA514, 0xC638, 0xEF7D,
    0x1800, 0x38E0, 0x5A00, 0x8320, 0xA440, 0xCD60, 0xF683, 0xFFC8,
    0x3000, 0x5840, 0x8120, 0xAA40, 0xCB60, 0xF462, 0xFDA7, 0xFEED,
    0x4000, 0x7000, 0x9880, 0xB982, 0xE286, 0xFBAA, 0xFCF0, 0xFE37,
    0x4001, 0x6803, 0x9028, 0xB92C, 0xE230, 0xFB35, 0xFC7A, 0xFDBE,
    0x2809, 0x500C, 0x7831, 0xA116, 0xC21A, 0xEB3F, 0xFC5E, 0xFDBE,
    0x0810, 0x3014, 0x5079, 0x797E, 0x9A7F, 0xC39F, 0xECDF, 0xFE1F,
    0x0012, 0x0837, 0x291C, 0x4A1F, 0x6B3F, 0x945F, 0xB59F, 0xDEDF,
    0x000E, 0x00F5, 0x01FA, 0x22FF, 0x441F, 0x653F, 0x8E7F, 0xB7BF,
    0x0087, 0x018D, 0x02B4, 0x03D9, 0x24FD, 0x461F, 0x6F5F, 0x8FFF,
    0x00E0, 0x0204, 0x034A, 0x046F, 0x1D93, 0x3EB8, 0x5FFD, 0x87FF,
    0x0120, 0x0240, 0x0381, 0x0CA5, 0x2DC9, 0x4F0D, 0x6FF2, 0x97F6,
    0x0100, 0x0220, 0x0B40, 0x2C80, 0x4DA1, 0x6EC5, 0x8FE9, 0xBFED,
    0x00A0, 0x11A0, 0x32C0, 0x53E0, 0x7500, 0x9E40, 0xBF63, 0xEFE7,
    0x1800, 0x38E0, 0x5A00, 0x8320, 0xAC40, 0xCD60, 0xF683, 0xFFC8,
    0x3800, 0x5840, 0x8120, 0xAA40, 0xD340, 0xF463, 0xFDA7, 0xFEEE,
};

typedef struct {
    uint8_t audc;
    uint8_t audf;
    uint8_t audv;

    uint8_t div_counter;
    uint8_t pulse_counter;
    uint8_t noise_counter;

    bool clock_enable;
    bool noise_feedback;
    bool noise_bit;
    bool pulse_hold;
} tia_audio_channel_t;

typedef struct {
    uint8_t w[0x2D];

    // Collision registers are packed for a branchless per-pixel OR. Inputs are
    // separate because INPT0..5 are not collision latches.
    uint64_t collisions;
    uint8_t input[6];

    uint16_t color_clock;
    bool wsync;

    int16_t player_pos[2];
    int16_t missile_pos[2];
    int16_t ball_pos;

    int8_t player_motion[2];
    int8_t missile_motion[2];
    int8_t ball_motion;

    uint8_t grp[2];
    uint8_t grp_delay[2];
    uint8_t enabl_delay;

    // Packed per-pixel occupancy:
    // bit0 P0, bit1 P1, bit2 M0, bit3 M1, bit4 BL, bit5 PF.
    // Replaces six separate 160-byte object planes.  Each object rebuild clears
    // and rewrites only its own bit, so racing-the-beam register writes remain
    // local and cheap.
    uint8_t occupancy_line[160];

    // Pre-resolved palette-index output for left/right halves and all 64
    // occupancy combinations.  Colour/priority decisions move off the hot
    // per-pixel path and are rebuilt only when COLU*/CTRLPF changes.
    uint8_t color_lut[2][64];

    // Joystick trigger + TIA input-latch state.
    bool fire0_pressed;

    // Two hardware TIA audio channels.
    tia_audio_channel_t audio[2];

    // Native TIA audio integration.  The channel waveform only changes on
    // TIA audio phase events or register writes.  Accumulate the true DAC
    // level between phase-1 events, matching the native ~31.4 kHz TIA audio
    // cadence, then rate-convert those native samples to 44.1 kHz.
    uint32_t audio_host_phase;
    uint32_t audio_sum0;
    uint32_t audio_sum1;
    uint16_t audio_sum_clocks;
    uint16_t audio_queue[8];
    uint8_t  audio_q_read;
    uint8_t  audio_q_write;

    uint8_t line[160];
} tia_t;

static tia_t t;

static void audio_phase0(tia_audio_channel_t *a) {
    if (a->clock_enable) {
        a->noise_bit = (a->noise_counter & 0x01u) != 0;

        switch (a->audc & 0x03u) {
            case 0:
            case 1:
                a->pulse_hold = false;
                break;
            case 2:
                a->pulse_hold = (a->noise_counter & 0x1Eu) != 0x02u;
                break;
            case 3:
                a->pulse_hold = !a->noise_bit;
                break;
        }

        if ((a->audc & 0x03u) == 0) {
            bool pulse_noise_diff = ((a->pulse_counter ^ a->noise_counter) & 1u) != 0;
            bool startup_state = !(a->noise_counter || a->pulse_counter != 0x0Au);
            bool pure_tone = (a->audc & 0x0Cu) == 0;
            a->noise_feedback = pulse_noise_diff || startup_state || pure_tone;
        } else {
            bool tap = (((a->noise_counter >> 2) ^ a->noise_counter) & 1u) != 0;
            a->noise_feedback = tap || a->noise_counter == 0;
        }
    }

    a->clock_enable = (a->div_counter == a->audf);

    if (a->div_counter == a->audf || a->div_counter == 0x1Fu)
        a->div_counter = 0;
    else
        a->div_counter++;
}

static void audio_phase1(tia_audio_channel_t *a) {
    if (!a->clock_enable)
        return;

    bool pulse_feedback = false;

    switch (a->audc >> 2) {
        case 0:
            pulse_feedback =
                ((((a->pulse_counter >> 1) ^ a->pulse_counter) & 1u) != 0) &&
                a->pulse_counter != 0x0Au &&
                (a->audc & 0x03u);
            break;
        case 1:
            pulse_feedback = (a->pulse_counter & 0x08u) == 0;
            break;
        case 2:
            pulse_feedback = !a->noise_bit;
            break;
        case 3:
            pulse_feedback = !((a->pulse_counter & 0x02u) ||
                               !(a->pulse_counter & 0x0Eu));
            break;
    }

    a->noise_counter >>= 1;
    if (a->noise_feedback)
        a->noise_counter |= 0x10u;

    if (!a->pulse_hold) {
        a->pulse_counter = (uint8_t)(~(a->pulse_counter >> 1) & 0x07u);
        if (pulse_feedback)
            a->pulse_counter |= 0x08u;
    }
}

static inline uint8_t audio_volume(const tia_audio_channel_t *a) {
    return (a->pulse_counter & 1u) ? a->audv : 0;
}

// Atari's two 4-bit DACs do not mix perfectly linearly. These 31 entries are
// an integer approximation of the passive TIA mono mixer response (0..30 input
// -> 0..32767 unsigned host level). Applying the curve before AC coupling keeps
// low/mid volume codes audible without overdriving full-scale tones.
static const uint16_t s_audio_mix[31] = {
       0,  2114,  4095,  5957,  7709,  9362, 10922, 12398,
   13796, 15123, 16383, 17582, 18724, 19812, 20851, 21844,
   22794, 23703, 24575, 25411, 26213, 26984, 27725, 28439,
   29126, 29788, 30426, 31042, 31637, 32211, 32767
};

static inline void audio_queue_level(uint16_t level) {
    uint8_t next = (uint8_t)((t.audio_q_write + 1u) & 7u);
    if (next == t.audio_q_read) {
        // A scanline produces only a few host samples and the core drains the
        // queue after each TIA advance.  Keep the newest timing if a diagnostic
        // advances unusually large chunks without polling.
        t.audio_q_read = (uint8_t)((t.audio_q_read + 1u) & 7u);
    }

    t.audio_queue[t.audio_q_write] = level;
    t.audio_q_write = next;
}

static inline void audio_emit_native_sample(void) {
    if (!t.audio_sum_clocks)
        return;

    // Stella's current TIA model forms one native audio sample at each phase-1
    // event from the average channel volume accumulated since the previous
    // phase-1 event.  The intervals alternate 112/116 colour clocks, averaging
    // 114 clocks (~31.4 kHz NTSC).
    uint32_t a0 = (t.audio_sum0 + (t.audio_sum_clocks >> 1)) / t.audio_sum_clocks;
    uint32_t a1 = (t.audio_sum1 + (t.audio_sum_clocks >> 1)) / t.audio_sum_clocks;
    uint32_t mix = a0 + a1;
    if (mix > 30u) mix = 30u;

    const uint16_t level = s_audio_mix[mix];

    // Convert the native phase-1 cadence to exactly 44.1 kHz without splitting
    // the outer TIA scheduler at every PCM boundary.  One native sample is held
    // for its real colour-clock interval; the rational accumulator emits one or
    // two 44.1 kHz samples as required.
    t.audio_host_phase +=
        (uint32_t)t.audio_sum_clocks * ATARI_TIA_AUDIO_RATE;

    while (t.audio_host_phase >= ATARI_TIA_CLOCK_HZ) {
        t.audio_host_phase -= ATARI_TIA_CLOCK_HZ;
        audio_queue_level(level);
    }

    t.audio_sum0 = 0;
    t.audio_sum1 = 0;
    t.audio_sum_clocks = 0;
}

static void update_fire0_input(void) {
    uint8_t *inpt4 = &t.input[TIA_INPT4 - TIA_INPT0];

    // VBLANK bit 6 enables the TIA trigger latch. While latched, a press drives
    // INPT4 low and it stays low until software clears the latch bit.
    if (t.w[TIA_VBLANK] & 0x40u) {
        if (t.fire0_pressed)
            *inpt4 = 0x00;
    } else {
        *inpt4 = t.fire0_pressed ? 0x00 : 0x80;
    }
}

static inline int wrap160(int v) {
    v %= 160;
    if (v < 0) v += 160;
    return v;
}

static inline int8_t motion4(uint8_t v) {
    // Signed high nibble: $0..$7 = 0..7, $8..$F = -8..-1.
    return (int8_t)((int8_t)v >> 4);
}

static void player_copy_offsets(uint8_t nusiz, const uint8_t **offs,
                                int *count, int *scale)
{
    static const uint8_t o0[] = {0};
    static const uint8_t o1[] = {0,16};
    static const uint8_t o2[] = {0,32};
    static const uint8_t o3[] = {0,16,32};
    static const uint8_t o4[] = {0,64};
    static const uint8_t o6[] = {0,32,64};
    *scale = 1;
    switch (nusiz & 7) {
        default:
        case 0: *offs=o0; *count=1; break;
        case 1: *offs=o1; *count=2; break;
        case 2: *offs=o2; *count=2; break;
        case 3: *offs=o3; *count=3; break;
        case 4: *offs=o4; *count=2; break;
        case 5: *offs=o0; *count=1; *scale=2; break;
        case 6: *offs=o6; *count=3; break;
        case 7: *offs=o0; *count=1; *scale=4; break;
    }
}

static inline void clear_occ_bit(uint8_t bit) {
    const uint8_t keep = (uint8_t)~bit;
    for (int x = 0; x < 160; x++)
        t.occupancy_line[x] &= keep;
}

static void rebuild_player(int p) {
    const uint8_t occ_bit = (uint8_t)(1u << p);
    clear_occ_bit(occ_bit);

    uint8_t pattern = (t.w[p ? TIA_VDELP1 : TIA_VDELP0] & 1)
                    ? t.grp_delay[p] : t.grp[p];
    bool reflect = (t.w[p ? TIA_REFP1 : TIA_REFP0] & 0x08) != 0;

    const uint8_t *offs;
    int copies, scale;
    player_copy_offsets(t.w[p ? TIA_NUSIZ1 : TIA_NUSIZ0],
                        &offs, &copies, &scale);

    for (int cp = 0; cp < copies; cp++) {
        int start = wrap160(t.player_pos[p] + offs[cp]);
        for (int bit = 0; bit < 8; bit++) {
            int srcbit = reflect ? bit : (7 - bit);
            if (!(pattern & (1u << srcbit)))
                continue;

            for (int sx = 0; sx < scale; sx++)
                t.occupancy_line[wrap160(start + bit * scale + sx)] |= occ_bit;
        }
    }
}

static void rebuild_missile(int m) {
    const uint8_t occ_bit = (uint8_t)(1u << (m + 2));
    clear_occ_bit(occ_bit);

    if (!(t.w[m ? TIA_ENAM1 : TIA_ENAM0] & 0x02))
        return;

    const uint8_t *offs;
    int copies, dummy_scale;
    uint8_t nusiz = t.w[m ? TIA_NUSIZ1 : TIA_NUSIZ0];
    player_copy_offsets(nusiz, &offs, &copies, &dummy_scale);
    int width = 1 << ((nusiz >> 4) & 3);

    for (int cp = 0; cp < copies; cp++) {
        int start = wrap160(t.missile_pos[m] + offs[cp]);
        for (int x = 0; x < width; x++)
            t.occupancy_line[wrap160(start + x)] |= occ_bit;
    }
}

static void rebuild_ball(void) {
    const uint8_t occ_bit = 0x10u;
    clear_occ_bit(occ_bit);

    uint8_t en = (t.w[TIA_VDELBL] & 1)
               ? t.enabl_delay : t.w[TIA_ENABL];
    if (!(en & 0x02))
        return;

    int width = 1 << ((t.w[TIA_CTRLPF] >> 4) & 3);
    for (int x = 0; x < width; x++)
        t.occupancy_line[wrap160(t.ball_pos + x)] |= occ_bit;
}

static uint8_t reverse8(uint8_t v) {
    v = (uint8_t)((v >> 4) | (v << 4));
    v = (uint8_t)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
    v = (uint8_t)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
    return v;
}

static void rebuild_playfield(void) {
    const uint8_t occ_bit = 0x20u;
    clear_occ_bit(occ_bit);

    uint32_t pat = (uint32_t)(t.w[TIA_PF0] >> 4)
                 | ((uint32_t)reverse8(t.w[TIA_PF1]) << 4)
                 | ((uint32_t)t.w[TIA_PF2] << 12);
    bool reflect = (t.w[TIA_CTRLPF] & 0x01) != 0;

    for (int i = 0; i < 80; i++) {
        int b = i >> 2;
        if (pat & (1u << b))
            t.occupancy_line[i] |= occ_bit;

        if (reflect) {
            if (pat & (1u << (19 - b)))
                t.occupancy_line[i + 80] |= occ_bit;
        } else if (pat & (1u << b)) {
            t.occupancy_line[i + 80] |= occ_bit;
        }
    }
}

static void rebuild_color_lut(void) {
    const bool priority = (t.w[TIA_CTRLPF] & 0x04u) != 0;
    const bool score    = (t.w[TIA_CTRLPF] & 0x02u) != 0;

    for (int half = 0; half < 2; half++) {
        for (int occ = 0; occ < 64; occ++) {
            const bool p0 = (occ & 0x01) != 0;
            const bool p1 = (occ & 0x02) != 0;
            const bool m0 = (occ & 0x04) != 0;
            const bool m1 = (occ & 0x08) != 0;
            const bool bl = (occ & 0x10) != 0;
            const bool pf = (occ & 0x20) != 0;

            uint8_t color = t.w[TIA_COLUBK];

            if (priority) {
                if (pf || bl) {
                    if (pf && score)
                        color = t.w[half ? TIA_COLUP1 : TIA_COLUP0];
                    else
                        color = t.w[TIA_COLUPF];
                } else if (p0 || m0) {
                    color = t.w[TIA_COLUP0];
                } else if (p1 || m1) {
                    color = t.w[TIA_COLUP1];
                }
            } else {
                if (p0 || m0) {
                    color = t.w[TIA_COLUP0];
                } else if (p1 || m1) {
                    color = t.w[TIA_COLUP1];
                } else if (pf || bl) {
                    if (pf && score)
                        color = t.w[half ? TIA_COLUP1 : TIA_COLUP0];
                    else
                        color = t.w[TIA_COLUPF];
                }
            }

            t.color_lut[half][occ] = (uint8_t)(color >> 1);
        }
    }
}

static void rebuild_objects(void) {
    rebuild_player(0);
    rebuild_player(1);
    rebuild_missile(0);
    rebuild_missile(1);
    rebuild_ball();
}

void atari_tia_reset(void) {
    memset(&t, 0, sizeof t);
    memset(t.input, 0x80, sizeof t.input);
    rebuild_objects();
    rebuild_playfield();
    rebuild_color_lut();
}

uint8_t atari_tia_read(uint16_t address) {
    uint8_t reg = (uint8_t)(address & 0x0F);

    if (reg <= TIA_CXPPMM)
        return (uint8_t)(t.collisions >> (reg * 8u));

    if (reg >= TIA_INPT0 && reg <= TIA_INPT5)
        return t.input[reg - TIA_INPT0];

    return 0x00;
}

static void set_player_pos(int p) {
    int x = (int)t.color_clock - ATARI_TIA_HBLANK;
    t.player_pos[p] = wrap160(x < 0 ? 0 : x);

    rebuild_player(p);

    // RESP only changes the missile when RESMP locks that missile to its player.
    if (t.w[p ? TIA_RESMP1 : TIA_RESMP0] & 0x02) {
        t.missile_pos[p] = t.player_pos[p];
        rebuild_missile(p);
    }
}

static void set_missile_pos(int m) {
    int x = (int)t.color_clock - ATARI_TIA_HBLANK - 2;
    t.missile_pos[m] = wrap160(x < 0 ? 0 : x);
    rebuild_missile(m);
}

static void set_ball_pos(void) {
    int x = (int)t.color_clock - ATARI_TIA_HBLANK - 2;
    t.ball_pos = wrap160(x < 0 ? 0 : x);
    rebuild_ball();
}

void atari_tia_write(uint16_t address, uint8_t value) {
    uint8_t reg = (uint8_t)(address & 0x3F);
    if (reg > TIA_CXCLR) return;

    switch (reg) {
        case TIA_COLUP0:
        case TIA_COLUP1:
        case TIA_COLUPF:
        case TIA_COLUBK:
            t.w[reg] = value;
            rebuild_color_lut();
            return;

        case TIA_VBLANK:
            t.w[reg] = value;
            update_fire0_input();
            return;

        case TIA_AUDC0:
        case TIA_AUDC1: {
            int ch = reg - TIA_AUDC0;
            t.w[reg] = value;
            t.audio[ch].audc = value & 0x0Fu;
            return;
        }

        case TIA_AUDF0:
        case TIA_AUDF1: {
            int ch = reg - TIA_AUDF0;
            t.w[reg] = value;
            t.audio[ch].audf = value & 0x1Fu;
            return;
        }

        case TIA_AUDV0:
        case TIA_AUDV1: {
            int ch = reg - TIA_AUDV0;
            t.w[reg] = value;
            t.audio[ch].audv = value & 0x0Fu;
            return;
        }

        case TIA_WSYNC:
            t.wsync = true;
            return;

        case TIA_RSYNC:
            t.color_clock = 0;
            return;

        case TIA_RESP0: set_player_pos(0); return;
        case TIA_RESP1: set_player_pos(1); return;
        case TIA_RESM0: set_missile_pos(0); return;
        case TIA_RESM1: set_missile_pos(1); return;
        case TIA_RESBL: set_ball_pos(); return;

        case TIA_GRP0:
            // TIA's vertical-delay shadow for P1 is latched by a GRP0 write.
            t.grp_delay[1] = t.grp[1];
            t.grp[0] = value;
            t.w[reg] = value;
            rebuild_player(0);
            if (t.w[TIA_VDELP1] & 1u)
                rebuild_player(1);
            return;

        case TIA_GRP1:
            // GRP1 latches P0 and the delayed ball enable.
            t.grp_delay[0] = t.grp[0];
            t.enabl_delay = t.w[TIA_ENABL];
            t.grp[1] = value;
            t.w[reg] = value;
            rebuild_player(1);
            if (t.w[TIA_VDELP0] & 1u)
                rebuild_player(0);
            if (t.w[TIA_VDELBL] & 1u)
                rebuild_ball();
            return;

        case TIA_PF0: case TIA_PF1: case TIA_PF2:
            t.w[reg] = value; rebuild_playfield(); return;

        case TIA_CTRLPF:
            t.w[reg] = value;
            rebuild_playfield();
            rebuild_ball();
            rebuild_color_lut();
            return;

        case TIA_REFP0: case TIA_VDELP0:
            t.w[reg] = value; rebuild_player(0); return;
        case TIA_REFP1: case TIA_VDELP1:
            t.w[reg] = value; rebuild_player(1); return;
        case TIA_VDELBL:
            t.w[reg] = value; rebuild_ball(); return;

        case TIA_NUSIZ0:
            t.w[reg] = value; rebuild_player(0); rebuild_missile(0); return;
        case TIA_NUSIZ1:
            t.w[reg] = value; rebuild_player(1); rebuild_missile(1); return;

        case TIA_ENAM0:
            t.w[reg] = value; rebuild_missile(0); return;
        case TIA_ENAM1:
            t.w[reg] = value; rebuild_missile(1); return;
        case TIA_ENABL:
            t.w[reg] = value; rebuild_ball(); return;

        case TIA_HMP0: t.w[reg]=value; t.player_motion[0]=motion4(value); return;
        case TIA_HMP1: t.w[reg]=value; t.player_motion[1]=motion4(value); return;
        case TIA_HMM0: t.w[reg]=value; t.missile_motion[0]=motion4(value); return;
        case TIA_HMM1: t.w[reg]=value; t.missile_motion[1]=motion4(value); return;
        case TIA_HMBL: t.w[reg]=value; t.ball_motion=motion4(value); return;

        case TIA_HMOVE:
            t.player_pos[0]=wrap160(t.player_pos[0]-t.player_motion[0]);
            t.player_pos[1]=wrap160(t.player_pos[1]-t.player_motion[1]);
            t.missile_pos[0]=wrap160(t.missile_pos[0]-t.missile_motion[0]);
            t.missile_pos[1]=wrap160(t.missile_pos[1]-t.missile_motion[1]);
            t.ball_pos=wrap160(t.ball_pos-t.ball_motion);
            rebuild_objects();
            return;

        case TIA_HMCLR:
            t.w[TIA_HMP0]=t.w[TIA_HMP1]=t.w[TIA_HMM0]=t.w[TIA_HMM1]=t.w[TIA_HMBL]=0;
            t.player_motion[0]=t.player_motion[1]=0;
            t.missile_motion[0]=t.missile_motion[1]=0;
            t.ball_motion=0;
            return;

        case TIA_RESMP0:
            t.w[reg] = value;
            if (value & 0x02)
                t.missile_pos[0] = t.player_pos[0];
            rebuild_missile(0);
            return;

        case TIA_RESMP1:
            t.w[reg] = value;
            if (value & 0x02)
                t.missile_pos[1] = t.player_pos[1];
            rebuild_missile(1);
            return;

        case TIA_CXCLR:
            t.collisions = 0;
            return;

        default:
            // VSYNC and colour registers are plain latches here.
            t.w[reg] = value;
            return;
    }
}

// Occupancy bits: P0,P1,M0,M1,BL,PF. Packed bytes map to CXM0P..CXPPMM.
static uint64_t s_collision_mask[64] = {
    UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
    UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000040), UINT64_C(0x0000000000000080), UINT64_C(0x80000000000000C0),
    UINT64_C(0x0000000000000000), UINT64_C(0x0000000000008000), UINT64_C(0x0000000000004000), UINT64_C(0x800000000000C000),
    UINT64_C(0x4000000000000000), UINT64_C(0x4000000000008040), UINT64_C(0x4000000000004080), UINT64_C(0xC00000000000C0C0),
    UINT64_C(0x0000000000000000), UINT64_C(0x0000000000400000), UINT64_C(0x0000000040000000), UINT64_C(0x8000000040400000),
    UINT64_C(0x0000004000000000), UINT64_C(0x0000004000400040), UINT64_C(0x0000004040000080), UINT64_C(0x80000040404000C0),
    UINT64_C(0x0000400000000000), UINT64_C(0x0000400000408000), UINT64_C(0x0000400040004000), UINT64_C(0x800040004040C000),
    UINT64_C(0x4000404000000000), UINT64_C(0x4000404000408040), UINT64_C(0x4000404040004080), UINT64_C(0xC00040404040C0C0),
    UINT64_C(0x0000000000000000), UINT64_C(0x0000000000800000), UINT64_C(0x0000000080000000), UINT64_C(0x8000000080800000),
    UINT64_C(0x0000008000000000), UINT64_C(0x0000008000800040), UINT64_C(0x0000008080000080), UINT64_C(0x80000080808000C0),
    UINT64_C(0x0000800000000000), UINT64_C(0x0000800000808000), UINT64_C(0x0000800080004000), UINT64_C(0x800080008080C000),
    UINT64_C(0x4000808000000000), UINT64_C(0x4000808000808040), UINT64_C(0x4000808080004080), UINT64_C(0xC00080808080C0C0),
    UINT64_C(0x0080000000000000), UINT64_C(0x0080000000C00000), UINT64_C(0x00800000C0000000), UINT64_C(0x80800000C0C00000),
    UINT64_C(0x008000C000000000), UINT64_C(0x008000C000C00040), UINT64_C(0x008000C0C0000080), UINT64_C(0x808000C0C0C000C0),
    UINT64_C(0x0080C00000000000), UINT64_C(0x0080C00000C08000), UINT64_C(0x0080C000C0004000), UINT64_C(0x8080C000C0C0C000),
    UINT64_C(0x4080C0C000000000), UINT64_C(0x4080C0C000C08040), UINT64_C(0x4080C0C0C0004080), UINT64_C(0xC080C0C0C0C0C0C0),
};

static inline uint8_t pixel_color(int x) {
    const uint8_t occ = t.occupancy_line[x];

    // Collision decode is a single RAM lookup + OR.
    t.collisions |= s_collision_mask[occ];

    if (t.w[TIA_VBLANK] & 0x02u)
        return 0;

    // Colour/priority/score selection was pre-resolved when the relevant TIA
    // registers changed.  The hot raster path is now one occupancy load and
    // one palette-index lookup.
    return t.color_lut[x >= 80][occ];
}

// Advance a chunk without re-entering the TIA state machine for every colour
// clock.  Register writes still split chunks at the exact CPU bus-cycle point,
// so pixels before/after a racing-the-beam write see the correct register state.
uint8_t atari_tia_advance(uint16_t clocks) {
    if (!clocks)
        return 0;

    uint16_t line_rem = (uint16_t)(ATARI_TIA_LINE_CLOCKS - t.color_clock);
    if (clocks > line_rem)
        clocks = line_rem;

    while (clocks) {
        const uint16_t start = t.color_clock;

        // The audio device only needs four real state-transition boundaries
        // per scanline.  Host 44.1 kHz boundaries are handled later by the
        // rational resampler, so they no longer fragment the machine scheduler.
        uint16_t event_clock;
        uint8_t event_kind;  // 0=phase0, 1=phase1, 2=line wrap
        if (start < 10)       { event_clock = 10;  event_kind = 0; }
        else if (start < 38)  { event_clock = 38;  event_kind = 1; }
        else if (start < 82)  { event_clock = 82;  event_kind = 0; }
        else if (start < 150) { event_clock = 150; event_kind = 1; }
        else                  { event_clock = 228; event_kind = 2; }

        uint16_t to_event = (uint16_t)(event_clock - start);
        uint16_t step = clocks < to_event ? clocks : to_event;

        // Channel DAC levels are constant through this segment.  Accumulate
        // their real time contribution for the next native phase-1 sample.
        uint8_t vol0 = audio_volume(&t.audio[0]);
        uint8_t vol1 = audio_volume(&t.audio[1]);
        t.audio_sum0 += (uint32_t)vol0 * step;
        t.audio_sum1 += (uint32_t)vol1 * step;
        t.audio_sum_clocks = (uint16_t)(t.audio_sum_clocks + step);

        // Raster work remains only on visible colour clocks.
        uint16_t end_clock = (uint16_t)(start + step);
        uint16_t vis0 = start < ATARI_TIA_HBLANK ? ATARI_TIA_HBLANK : start;
        uint16_t vis1 = end_clock;
        if (vis1 > ATARI_TIA_LINE_CLOCKS) vis1 = ATARI_TIA_LINE_CLOCKS;

        for (uint16_t cc = vis0; cc < vis1; cc++) {
            int x = (int)cc - ATARI_TIA_HBLANK;
            t.line[x] = pixel_color(x);
        }

        t.color_clock = end_clock;
        clocks = (uint16_t)(clocks - step);

        if (step == to_event) {
            if (event_kind == 0) {
                audio_phase0(&t.audio[0]);
                audio_phase0(&t.audio[1]);
            } else if (event_kind == 1) {
                // The accumulated interval belongs to the waveform state that
                // existed before this phase-1 edge.  Advance the hardware state,
                // then publish that completed native sample, as Stella does.
                audio_phase1(&t.audio[0]);
                audio_phase1(&t.audio[1]);
                audio_emit_native_sample();
            } else {
                t.color_clock = 0;
                t.wsync = false;
                return 1;
            }
        }
    }

    return 0;
}

bool atari_tia_tick(void) {
    return atari_tia_advance(1) != 0;
}

bool atari_tia_tick_clocks(uint8_t clocks) {
    bool line_done = false;
    while (clocks) {
        uint16_t remain = (uint16_t)(ATARI_TIA_LINE_CLOCKS - t.color_clock);
        uint16_t n = clocks < remain ? clocks : remain;
        line_done |= atari_tia_advance(n) != 0;
        clocks = (uint8_t)(clocks - n);
    }
    return line_done;
}

const uint8_t *atari_tia_line(void) { return t.line; }
bool atari_tia_vsync(void)  { return (t.w[TIA_VSYNC]  & 0x02) != 0; }
bool atari_tia_vblank(void) { return (t.w[TIA_VBLANK] & 0x02) != 0; }
bool atari_tia_wsync(void)  { return t.wsync; }
uint16_t atari_tia_color_clock(void) { return t.color_clock; }

void atari_tia_set_fire(bool pressed) {
    t.fire0_pressed = pressed;
    update_fire0_input();
}

uint8_t atari_tia_audio_level(void) {
    return (uint8_t)(audio_volume(&t.audio[0]) + audio_volume(&t.audio[1]));
}

bool atari_tia_audio_pop(uint16_t *level) {
    if (t.audio_q_read == t.audio_q_write)
        return false;

    if (level)
        *level = t.audio_queue[t.audio_q_read];

    t.audio_q_read = (uint8_t)((t.audio_q_read + 1u) & 7u);
    return true;
}