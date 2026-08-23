#include "sd.h"
#include "pins.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// Raw SD-over-SPI card init. No FatFs. Proves SPI1 wiring, that a card
// is present and negotiates, and reads its type + capacity from the CSD.
// CS is driven manually (GPIO), the usual approach for SD in SPI mode.

#define SD_INIT_HZ  (400u * 1000u)        // <=400 kHz during init, per spec
#define SD_FAST_HZ  (50u * 1000u * 1000u) // request ~50 MHz; at 296 MHz clk_peri RP2040 selects ~49.33 MHz

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

// Bulk transfers use the RP2040 SPI FIFO in one SDK call instead of calling
// spi_write_read_blocking(..., 1) once per byte.  This is especially important
// for 512-byte SD payloads: the previous path paid C/SDK call overhead 512
// times for every sector transferred.
static inline void sd_read_bulk(uint8_t *dst, size_t len) {
    spi_read_blocking(SD_SPI, 0xFF, dst, len);
}

static inline void sd_write_bulk(const uint8_t *src, size_t len) {
    spi_write_blocking(SD_SPI, src, len);
}

// Send a 6-byte command, return R1 (first response byte with bit7 clear).
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
    sd_xfer(0xFF); // flush
    uint8_t f[6] = {
        (uint8_t)(0x40 | cmd),
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
        (uint8_t)(arg >> 8),  (uint8_t)(arg), crc
    };
    sd_write_bulk(f, sizeof f);
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
        sd_read_bulk(r7, sizeof r7);
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
            sd_read_bulk(ocr, sizeof ocr);
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
            sd_read_bulk(csd, sizeof csd);
            uint8_t crc[2];
            sd_read_bulk(crc, sizeof crc);  // discard CRC
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

// Stop an active CMD18 READ_MULTIPLE_BLOCK stream.
//
// CMD12 is special in SPI mode: the card returns one stuff byte before the R1
// response. After the R1 response it may hold MISO low while terminating the
// stream, so wait until the card releases the bus before deselecting it.
static bool sd_stop_read_multi(void) {
    const uint8_t f[6] = {
        (uint8_t)(0x40u | 12u), 0, 0, 0, 0, 0xFF
    };

    // Do not use sd_cmd() here: CMD12 has the extra stuff byte and an R1b-style
    // busy phase that ordinary commands do not.
    sd_write_bulk(f, sizeof f);

    (void)sd_xfer(0xFF);  // mandatory CMD12 stuff byte

    uint8_t r1 = 0xFF;
    for (int i = 0; i < 16; i++) {
        r1 = sd_xfer(0xFF);
        if (!(r1 & 0x80))
            break;
    }

    if (r1 != 0x00)
        return false;

    return wait_not_busy(200000u);
}

bool sd_read_blocks(uint8_t *buf, uint32_t lba, uint32_t count) {
    if (!s_ready || !buf || count == 0)
        return false;

    cs_low();

    // Preserve the already-validated single-sector path exactly. This keeps
    // metadata reads, USB's common 512-byte accesses, and other small I/O on
    // CMD17 while allowing FatFs contiguous reads to use a streaming command.
    if (count == 1) {
        bool ok = true;

        if (sd_cmd(17, addr_of(lba), 0xFF) != 0x00) {
            ok = false;
        } else {
            uint8_t tok = 0xFF;
            for (int i = 0; i < 200000; i++) {
                tok = sd_xfer(0xFF);
                if (tok != 0xFF)
                    break;
            }

            if (tok != 0xFE) {
                ok = false;
            } else {
                sd_read_bulk(buf, 512u);
                uint8_t crc[2];
                sd_read_bulk(crc, sizeof crc);  // discard CRC
            }
        }

        cs_high();
        sd_xfer(0xFF);
        return ok;
    }

    // Contiguous multi-sector read: issue one CMD18 and keep CS asserted while
    // the card streams each 512-byte block. A 16 KiB GBC cache fill becomes one
    // command + 32 data blocks instead of 32 independent CMD17 transactions.
    bool ok = (sd_cmd(18, addr_of(lba), 0xFF) == 0x00);

    if (ok) {
        for (uint32_t b = 0; b < count; b++) {
            uint8_t tok = 0xFF;

            for (int i = 0; i < 200000; i++) {
                tok = sd_xfer(0xFF);
                if (tok != 0xFF)
                    break;
            }

            if (tok != 0xFE) {
                ok = false;
                break;
            }

            uint8_t *p = buf + (size_t)b * 512u;
            sd_read_bulk(p, 512u);

            uint8_t crc[2];
            sd_read_bulk(crc, sizeof crc);  // discard CRC
        }

        // Once CMD18 has been accepted, always terminate the stream even if a
        // later data token timed out. This leaves the card in a defined state
        // for the next filesystem request.
        if (!sd_stop_read_multi())
            ok = false;
    }

    cs_high();
    sd_xfer(0xFF);
    return ok;
}

bool sd_write_blocks(const uint8_t *buf, uint32_t lba, uint32_t count) {
    if (!s_ready || !buf || count == 0)
        return false;

    cs_low();

    // Preserve the proven single-sector path.
    if (count == 1) {
        bool ok = true;

        if (sd_cmd(24, addr_of(lba), 0xFF) != 0x00) {
            ok = false;
        } else {
            sd_xfer(0xFF);              // 1-byte gap
            sd_xfer(0xFE);              // single-block data start token
            sd_write_bulk(buf, 512u);

            const uint8_t crc[2] = { 0xFF, 0xFF };
            sd_write_bulk(crc, sizeof crc);

            uint8_t resp = sd_xfer(0xFF);
            if ((resp & 0x1F) != 0x05)
                ok = false;
            else if (!wait_not_busy(2000000u))
                ok = false;
        }

        cs_high();
        sd_xfer(0xFF);
        return ok;
    }

    // Give SD cards a hint about the upcoming multi-block write count. ACMD23
    // is an optimization hint; cards that reject it still proceed with CMD25.
    (void)sd_cmd(55, 0, 0xFF);
    (void)sd_cmd(23, count, 0xFF);      // ACMD23 SET_WR_BLK_ERASE_COUNT

    bool started = (sd_cmd(25, addr_of(lba), 0xFF) == 0x00);  // WRITE_MULTIPLE_BLOCK
    bool ok = started;

    if (started) {
        for (uint32_t b = 0; b < count; b++) {
            if (!wait_not_busy(2000000u)) {
                ok = false;
                break;
            }

            sd_xfer(0xFC);              // multi-block write data token

            const uint8_t *p = buf + (size_t)b * 512u;
            sd_write_bulk(p, 512u);

            const uint8_t crc[2] = { 0xFF, 0xFF };
            sd_write_bulk(crc, sizeof crc);

            uint8_t resp = sd_xfer(0xFF);
            if ((resp & 0x1F) != 0x05) {
                ok = false;
                break;
            }

            // Do not start the next block until the card finishes programming
            // the current one.
            if (!wait_not_busy(2000000u)) {
                ok = false;
                break;
            }
        }

        // Once CMD25 is accepted, always terminate the multi-block stream even
        // after a later block failure so the card returns to command state.
        sd_xfer(0xFD);                  // STOP_TRAN token
        if (!wait_not_busy(2000000u))
            ok = false;
    }

    cs_high();
    sd_xfer(0xFF);
    return ok;
}

bool     sd_ready(void)        { return s_ready; }
uint32_t sd_sector_count(void) { return s_sectors; }