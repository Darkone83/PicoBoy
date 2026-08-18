#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ATARI_CART_UNKNOWN = 0,
    ATARI_CART_2K,
    ATARI_CART_4K,
    ATARI_CART_F8,
    ATARI_CART_F8SC,
    ATARI_CART_F6,
    ATARI_CART_F6SC,
    ATARI_CART_F4,
    ATARI_CART_F4SC,
    ATARI_CART_FA,
    ATARI_CART_E0,
    ATARI_CART_FE,
    ATARI_CART_E7,
    ATARI_CART_3F,
    ATARI_CART_UA,
} atari_cart_type_t;

// Attach a raw .a26/.bin image. The image remains owned by PicoBoy's staged
// flash window, so the cart layer stores a pointer rather than copying it.
bool atari_cart_mount(const uint8_t *rom, uint32_t size);
void atari_cart_reset(void);

atari_cart_type_t atari_cart_type(void);
const char *atari_cart_type_name(void);
bool atari_cart_supported(void);

// Cartridge-bus access. M0 implements the conventional 2K/4K and Atari
// F8/F6/F4 (+ Superchip) families. More unusual hotspots are detected now
// and will be filled in as the core comes online.
uint8_t atari_cart_read(uint16_t address);
void atari_cart_write(uint16_t address, uint8_t value);