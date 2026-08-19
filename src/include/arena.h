#pragma once
#include <stdint.h>

// Shared RAM arena. Only ONE emulator core runs at a time, so GB, NES and 
// Atari carve their large buffers from this single region. Sized to the largest
// consumer.
#define ARENA_BYTES (132u * 1024u)

uint8_t *arena_base(void);   // 4-byte aligned