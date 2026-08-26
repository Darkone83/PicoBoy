#pragma once
#include <stdint.h>

// Shared RAM arena. Only ONE emulator core runs at a time, so GB, NES, Atari,
// SMS and Game Gear can carve their large runtime buffers from the same region.
#define ARENA_BYTES       (129u * 1024u)

/*
 * All emulator cores also share one stereo I2S staging buffer. Reserve 3 KiB at
 * the top of the arena instead of keeping another ~2.9 KiB permanently in .bss.
 *
 * Core bump allocators stop at ARENA_WORK_BYTES. Menu-only users such as the ROM
 * browser and USB MSC sector cache may borrow the lower arena while no emulator
 * is active.
 */
#define ARENA_AUDIO_BYTES (3u * 1024u)
#define ARENA_WORK_BYTES  (ARENA_BYTES - ARENA_AUDIO_BYTES)

uint8_t *arena_base(void);   // 4-byte aligned

static inline uint8_t *arena_audio_base(void) {
    return arena_base() + ARENA_WORK_BYTES;
}