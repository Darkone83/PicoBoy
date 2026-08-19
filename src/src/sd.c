#include "sd.h"
#include "pins.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// Raw SD-over-SPI card init. No FatFs. Proves SPI1 wiring, that a card
// is present and negotiates, and reads its type + capacity from the CSD.
// CS is driven manually (GPIO), the usual approach for SD in SPI mode.

#define SD_INIT_HZ  (400u * 1000u)        // <=400 kHz during init, per spec
#define SD_FAST_HZ  (12u * 1000u * 1000u) // conservative post-init clock for breadboard jumpers

static inline void cs_high(void) { gpio_put(SD_CS_PIN, 1); }
static inline void cs_low(void)  { gpio_put(SD_CS_PIN, 0); }

// Captured at init: needed for every subsequent block transfer.
static bool     s_ready      = false;
static bool     s_block_addr = false;   // true = SDHC/SDXC (LBA), false = byte address
static uint32_t s_sectors    = 0;

static uint8_t sd_xfer(uint8_t b) {
    uint8_t rx = 0xFF;
    spi_write_read_blocking(SD_SPI, &b, &rx, 1);
    return rx;
}

// Send a 6-byte command, return R1 (first response byte with bit7 clear).
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
    sd_xfer(0xFF); // flush
    uint8_t f[6] = {
        (uint8_t)(0x40 | cmd),
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
        (uint8_t)(arg >> 8),  (uint8_t)(arg), crc
    };
    for (int i = 0; i < 6; i++) sd_xfer(f[i]);
    uint8_t r1 = 0xFF;
    for (int i = 0; i < 8; i++) { r1 = sd_xfer(0xFF); if (!(r1 & 0x80)) break; }
    return r1;
}

static uint8_t sd_acmd41(uint32_t arg) {
    sd_cmd(55, 0, 0xFF);             // APP_CMD (CRC ignored in SPI mode)
    return sd_cmd(41, arg, 0xFF);    // ACMD41
}

bool sd_init(sd_info_t *info) {
    if (info) { info->type = SD_NONE; info->sectors = 0; info->mb = 0; }

    // CS as manual GPIO output, idle high.
    gpio_init(SD_CS_PIN);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
    cs_high();

    // SPI1 at the slow init rate; route SCK/MOSI/MISO.
    spi_init(SD_SPI, SD_INIT_HZ);
    gpio_set_function(SD_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO_PIN, GPIO_FUNC_SPI);
    gpio_pull_up(SD_MISO_PIN);  // some cards don't drive MISO until selected

    // Power-up: >=74 clocks with CS high and MOSI high (10 bytes of 0xFF).
    cs_high();
    for (int i = 0; i < 10; i++) sd_xfer(0xFF);

    cs_low();  // hold selected through the whole init sequence

    // CMD0: go idle (needs valid CRC 0x95).
    uint8_t r1 = 0xFF;
    for (int i = 0; i < 10; i++) { r1 = sd_cmd(0, 0, 0x95); if (r1 == 0x01) break; sleep_ms(1); }
    if (r1 != 0x01) { cs_high(); sd_xfer(0xFF); return false; }  // no card / not responding

    // CMD8: voltage check (needs valid CRC 0x87, arg 0x1AA). R1=0x01 + 4-byte R7.
    bool v2 = false;
    r1 = sd_cmd(8, 0x000001AA, 0x87);
    if (r1 == 0x01) {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = sd_xfer(0xFF);
        if (r7[2] == 0x01 && r7[3] == 0xAA) v2 = true;  // 2.7-3.6V + echo matches
    }

    // ACMD41: leave idle. HCS bit set for v2 cards.
    uint32_t acmd_arg = v2 ? 0x40000000u : 0x00000000u;
    bool ready = false;
    for (int i = 0; i < 2000; i++) { if (sd_acmd41(acmd_arg) == 0x00) { ready = true; break; } sleep_ms(1); }
    if (!ready) { cs_high(); sd_xfer(0xFF); return false; }

    sd_type_t type;
    if (v2) {
        // CMD58: read OCR, CCS bit (bit30 -> ocr[0] & 0x40) distinguishes SDHC/SDXC.
        bool hc = false;
        r1 = sd_cmd(58, 0, 0xFF);
        if (r1 == 0x00) {
            uint8_t ocr[4];
            for (int i = 0; i < 4; i++) ocr[i] = sd_xfer(0xFF);
            hc = (ocr[0] & 0x40) != 0;
        }
        type = hc ? SD_V2_SDHC : SD_V2_SDSC;
    } else {
        type = SD_V1;
        sd_cmd(16, 512, 0xFF);  // force 512-byte blocks on byte-addressed cards
    }

    // CMD9: read CSD (16 bytes after a 0xFE data token) -> capacity.
    uint32_t sectors = 0;
    if (sd_cmd(9, 0, 0xFF) == 0x00) {
        uint8_t tok = 0xFF;
        for (int i = 0; i < 50000; i++) { tok = sd_xfer(0xFF); if (tok != 0xFF) break; }
        if (tok == 0xFE) {
            uint8_t csd[16];
            for (int i = 0; i < 16; i++) csd[i] = sd_xfer(0xFF);
            sd_xfer(0xFF); sd_xfer(0xFF);  // discard CRC
            if ((csd[0] >> 6) == 1) {
                // CSD v2 (SDHC/SDXC): C_SIZE = bits[69:48]; capacity = (C_SIZE+1) * 512 KB.
                uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16)
                                | ((uint32_t)csd[8] << 8) | (uint32_t)csd[9];
                sectors = (c_size + 1u) * 1024u;   // *512KB / 512B
            } else {
                // CSD v1 (SDSC).
                uint32_t read_bl_len = csd[5] & 0x0F;
                uint32_t c_size = (((uint32_t)(csd[6] & 0x03)) << 10)
                                | ((uint32_t)csd[7] << 2) | ((uint32_t)csd[8] >> 6);
                uint32_t c_mult = (((uint32_t)(csd[9] & 0x03)) << 1) | ((uint32_t)csd[10] >> 7);
                uint64_t bytes = (uint64_t)(c_size + 1u) * (1u << (c_mult + 2)) * (1u << read_bl_len);
                sectors = (uint32_t)(bytes / 512u);
            }
        }
    }

    cs_high();
    sd_xfer(0xFF);
    spi_set_baudrate(SD_SPI, SD_FAST_HZ);  // init done -> speed up

    s_ready      = true;
    s_block_addr = (type == SD_V2_SDHC);   // SDHC/SDXC are block-addressed
    s_sectors    = sectors;

    if (info) {
        info->type = type;
        info->sectors = sectors;
        info->mb = (uint32_t)(((uint64_t)sectors * 512u) / (1024u * 1024u));
    }
    return true;
}

// ---- Block I/O -------------------------------------------------------------
// SDHC/SDXC address by 512-byte block; older cards address by byte.
static inline uint32_t addr_of(uint32_t lba) {
    return s_block_addr ? lba : (lba * 512u);
}

// Poll until the card releases the bus (drives 0xFF). Returns false on timeout.
static bool wait_not_busy(uint32_t tries) {
    while (tries--) { if (sd_xfer(0xFF) == 0xFF) return true; }
    return false;
}

bool sd_read_blocks(uint8_t *buf, uint32_t lba, uint32_t count) {
    if (!s_ready) return false;
    cs_low();
    bool ok = true;
    for (uint32_t b = 0; b < count; b++) {
        if (sd_cmd(17, addr_of(lba + b), 0xFF) != 0x00) { ok = false; break; }  // CMD17 READ_SINGLE_BLOCK
        uint8_t tok = 0xFF;
        for (int i = 0; i < 200000; i++) { tok = sd_xfer(0xFF); if (tok != 0xFF) break; }
        if (tok != 0xFE) { ok = false; break; }                                  // data start token
        uint8_t *p = buf + (size_t)b * 512u;
        for (uint32_t i = 0; i < 512; i++) p[i] = sd_xfer(0xFF);
        sd_xfer(0xFF); sd_xfer(0xFF);                                            // discard CRC
    }
    cs_high();
    sd_xfer(0xFF);
    return ok;
}

bool sd_write_blocks(const uint8_t *buf, uint32_t lba, uint32_t count) {
    if (!s_ready) return false;
    cs_low();
    bool ok = true;
    for (uint32_t b = 0; b < count; b++) {
        if (sd_cmd(24, addr_of(lba + b), 0xFF) != 0x00) { ok = false; break; }   // CMD24 WRITE_BLOCK
        sd_xfer(0xFF);                                                           // 1-byte gap
        sd_xfer(0xFE);                                                           // data start token
        const uint8_t *p = buf + (size_t)b * 512u;
        for (uint32_t i = 0; i < 512; i++) sd_xfer(p[i]);
        sd_xfer(0xFF); sd_xfer(0xFF);                                            // dummy CRC
        uint8_t resp = sd_xfer(0xFF);
        if ((resp & 0x1F) != 0x05) { ok = false; break; }                       // data-accepted token
        if (!wait_not_busy(2000000)) { ok = false; break; }                     // card programs the block
    }
    cs_high();
    sd_xfer(0xFF);
    return ok;
}

bool     sd_ready(void)        { return s_ready; }
uint32_t sd_sector_count(void) { return s_sectors; }