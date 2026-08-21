#pragma once
#include <stdint.h>

// Shared RAM arena. Only ONE emulator core runs at a time, so GB, NES and 
// Atari carve their large buffers from this single region. Sized to the largest
// consumer.
#define ARENA_BYTES       (132u * 1024u)

/*
 * The stereo I2S staging buffer used by every emulator is also mutually
 * exclusive with the core that is running. Reserve 3 KiB at the top of the
 * shared arena instead of keeping another ~2.9 KiB permanently in .bss.
 *
 * Core bump allocators must stop at ARENA_WORK_BYTES. Menu-only users such as
 * the ROM browser may still borrow the lower arena while no emulator is active.
 */
#define ARENA_AUDIO_BYTES (3u * 1024u)
#define ARENA_WORK_BYTES  (ARENA_BYTES - ARENA_AUDIO_BYTES)

uint8_t *arena_base(void);   // 4-byte aligned

static inline uint8_t *arena_audio_base(void) {
    return arena_base() + ARENA_WORK_BYTES;
}