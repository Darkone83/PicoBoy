#pragma once
#include <stdbool.h>
#include <stdint.h>

// PicoBoy SD bring-up.
// Stage 1 (this file): raw SPI card init -- prove wiring + card responds, read
// type/capacity. NO filesystem. Stage 2 will layer FatFs on top for mount +
// directory + file read/write.

typedef enum { SD_NONE, SD_V1, SD_V2_SDSC, SD_V2_SDHC } sd_type_t;

typedef struct {
    sd_type_t type;
    uint32_t  sectors;   // capacity in 512-byte sectors (0 = unknown)
    uint32_t  mb;        // capacity in MB
} sd_info_t;

// Power up the card over SPI1, run CMD0 / CMD8 / ACMD41 / CMD58, read the CSD.
// Returns true if a card initialized; fills *info (info may be NULL).
bool sd_init(sd_info_t *info);

// Block I/O (512-byte sectors). Valid only after a successful sd_init().
// LBA is a sector index; addressing mode (block vs byte) is handled internally.
bool     sd_read_blocks (uint8_t *buf,        uint32_t lba, uint32_t count);
bool     sd_write_blocks(const uint8_t *buf,  uint32_t lba, uint32_t count);
bool     sd_ready(void);            // true once a card has initialized
uint32_t sd_sector_count(void);     // capacity in 512-byte sectors (0 if unknown)