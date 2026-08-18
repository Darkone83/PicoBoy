#pragma once
#include <stdint.h>

// MOS 6532 RIOT: 128 bytes RAM, console/joystick I/O and interval timer.
void atari_riot_reset(void);
void atari_riot_tick(void);               // one 6507 clock
void atari_riot_advance(uint32_t cycles);   // arithmetic multi-cycle advance

uint8_t atari_riot_ram_read(uint16_t address);
void    atari_riot_ram_write(uint16_t address, uint8_t value);
uint8_t atari_riot_io_read(uint16_t address);
void    atari_riot_io_write(uint16_t address, uint8_t value);

// External active-low inputs sampled by the emulated RIOT.
void atari_riot_set_joystick(uint8_t swcha);
void atari_riot_set_switches(uint8_t swchb);