#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Shared PicoBoy Auto frame-skip controller.
 *
 * Auto starts at level 0 (render every frame) and reacts to two real pressure
 * signals supplied by each core:
 *   1) the display consumer is still busy / has backlog
 *   2) the audio DMA writer had almost no wait time, meaning the emulator used
 *      nearly the whole real-time audio period before producing the next block
 *
 * Level N renders one frame then skips N frames. Auto is intentionally capped
 * below the manual 0..5 range so a transient load spike cannot collapse video
 * to an unnecessarily low rate.
 */
#define ADAPTIVE_FS_MAX_LEVEL        3u
#define ADAPTIVE_FS_WARMUP_FRAMES    4u

// i2s_dma_write() wait is a useful approximation of producer headroom.
// <= 0.5 ms: close to underrun / real-time edge.
// >= 2.0 ms: comfortable sustained headroom.
#define ADAPTIVE_FS_DMA_PRESSURE_US  500u
#define ADAPTIVE_FS_DMA_GOOD_US     2000u

// Pressure uses a small score so intermittent misses can accumulate without a
// single transient immediately increasing frame skip. Recovery is deliberately
// much slower than attack to avoid visible 0/1/0/1 oscillation.
#define ADAPTIVE_FS_PRESSURE_RAISE    4u
#define ADAPTIVE_FS_RECOVER_GOOD     60u

typedef enum {
    ADAPTIVE_FS_NEUTRAL = 0,
    ADAPTIVE_FS_GOOD,
    ADAPTIVE_FS_PRESSURE
} adaptive_frameskip_signal_t;

typedef struct {
    uint8_t level;          // 0..ADAPTIVE_FS_MAX_LEVEL
    uint8_t phase;          // wrapper-side render cadence
    uint8_t pressure_score;
    uint8_t good_run;
    uint8_t warmup;
} adaptive_frameskip_t;

static inline void adaptive_frameskip_reset(adaptive_frameskip_t *s) {
    s->level = 0;
    s->phase = 0;
    s->pressure_score = 0;
    s->good_run = 0;
    s->warmup = ADAPTIVE_FS_WARMUP_FRAMES;
}

static inline uint8_t adaptive_frameskip_level(const adaptive_frameskip_t *s) {
    return s->level;
}

static inline bool adaptive_frameskip_should_render(adaptive_frameskip_t *s) {
    if (s->level == 0)
        return true;

    bool render = (s->phase == 0);
    s->phase++;
    if (s->phase > s->level)
        s->phase = 0;
    return render;
}

// UINT32_MAX means no valid audio-DMA pacing sample is available yet.
static inline adaptive_frameskip_signal_t
adaptive_frameskip_signal(bool display_blocked, uint32_t dma_wait_us) {
    if (display_blocked)
        return ADAPTIVE_FS_PRESSURE;

    if (dma_wait_us == UINT32_MAX)
        return ADAPTIVE_FS_NEUTRAL;

    if (dma_wait_us <= ADAPTIVE_FS_DMA_PRESSURE_US)
        return ADAPTIVE_FS_PRESSURE;

    if (dma_wait_us >= ADAPTIVE_FS_DMA_GOOD_US)
        return ADAPTIVE_FS_GOOD;

    return ADAPTIVE_FS_NEUTRAL;
}

static inline void
adaptive_frameskip_feedback(adaptive_frameskip_t *s,
                            adaptive_frameskip_signal_t signal) {
    if (s->warmup) {
        s->warmup--;
        return;
    }

    if (signal == ADAPTIVE_FS_PRESSURE) {
        s->good_run = 0;

        unsigned score = (unsigned)s->pressure_score + 2u;
        s->pressure_score = (uint8_t)(score > 0xFFu ? 0xFFu : score);

        if (s->pressure_score >= ADAPTIVE_FS_PRESSURE_RAISE &&
            s->level < ADAPTIVE_FS_MAX_LEVEL) {
            s->level++;
            s->pressure_score = 0;
            s->good_run = 0;
            s->phase = 1;  // newly-raised level skips the next wrapper frame
        }
        return;
    }

    if (s->pressure_score)
        s->pressure_score--;

    if (signal == ADAPTIVE_FS_GOOD) {
        if (s->level > 0 && s->pressure_score == 0) {
            if (s->good_run < 0xFFu)
                s->good_run++;

            if (s->good_run >= ADAPTIVE_FS_RECOVER_GOOD) {
                s->level--;
                s->phase = 0;  // immediately probe the lower level
                s->pressure_score = 0;
                s->good_run = 0;
            }
        } else {
            s->good_run = 0;
        }
    } else {
        s->good_run = 0;
    }
}