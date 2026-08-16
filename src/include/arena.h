#pragma once
#include <stdint.h>

// Shared RAM arena. Only ONE emulator core runs at a time, so GB and NES carve
// their large buffers from this single region. Sized to the largest consumer.
//   NES: line ring ~60K + ChrBuf 32K + PPURAM 16K + RAM 8K + SRAM 8K = ~124K
//   GB : fb[2] 90K + cart RAM 32K = ~122K
#define ARENA_BYTES (132u * 1024u)

uint8_t *arena_base(void);   // 4-byte aligned