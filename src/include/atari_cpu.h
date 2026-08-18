#pragma once
#include <stdint.h>
#include <stdbool.h>

// Whole-instruction 6507 core. Ordinary ROM/RAM cycles are accumulated even
// across instruction boundaries and synchronized only at timing-sensitive
// TIA/RIOT I/O accesses. This keeps exact device-visible cycle totals while
// reducing host scheduler traffic.
void atari_cpu_reset(void);
bool atari_cpu_step_instruction(void);

bool     atari_cpu_halted(void);
uint16_t atari_cpu_pc(void);
uint8_t  atari_cpu_last_opcode(void);

// Platform timing hooks implemented by atari_core.c.
bool atari_machine_advance_cpu(uint32_t cycles);
bool atari_machine_tia_write_complete(void);