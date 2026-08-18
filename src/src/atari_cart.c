#include "atari_cart.h"
#include <string.h>
#include <stddef.h>

#define KB(x) ((uint32_t)(x) * 1024u)

static unsigned count_sig(const uint8_t *rom, uint32_t size,
                          const uint8_t *sig, uint32_t n)
{
    unsigned hits = 0;
    if (!rom || !sig || n == 0 || size < n) return 0;
    for (uint32_t i = 0; i + n <= size; i++)
        if (memcmp(rom + i, sig, n) == 0) hits++;
    return hits;
}

static bool any_sig3(const uint8_t *rom, uint32_t size,
                     const uint8_t sig[][3], unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        if (count_sig(rom, size, sig[i], 3)) return true;
    return false;
}

static bool is_superchip(const uint8_t *rom, uint32_t size)
{
    // Stella's conventional SC heuristic: within every 4K bank, the first
    // 128 bytes are repeated in the next 128 bytes.
    if (!rom || size < KB(8) || (size & 0x0FFFu)) return false;
    for (uint32_t base = 0; base < size; base += 4096u)
        if (memcmp(rom + base, rom + base + 128u, 128u) != 0) return false;
    return true;
}

static bool is_e0(const uint8_t *rom, uint32_t size)
{
    static const uint8_t sig[][3] = {
        {0x8D,0xE0,0x1F}, {0x8D,0xE0,0x5F}, {0x8D,0xE9,0xFF},
        {0x0C,0xE0,0x1F}, {0xAD,0xE0,0x1F}, {0xAD,0xE9,0xFF},
        {0xAD,0xED,0xFF}, {0xAD,0xF3,0xBF}
    };
    return any_sig3(rom, size, sig, sizeof sig / sizeof sig[0]);
}

static bool is_e7(const uint8_t *rom, uint32_t size)
{
    static const uint8_t sig[][3] = {
        {0xAD,0xE2,0xFF}, {0xAD,0xE5,0xFF}, {0xAD,0xE5,0x1F},
        {0xAD,0xE7,0x1F}, {0x0C,0xE7,0x1F}, {0x8D,0xE7,0xFF},
        {0x8D,0xE7,0x1F}
    };
    return any_sig3(rom, size, sig, sizeof sig / sizeof sig[0]);
}

static bool is_3f(const uint8_t *rom, uint32_t size)
{
    static const uint8_t sig[] = { 0x85, 0x3F }; // STA $3F
    return count_sig(rom, size, sig, sizeof sig) >= 2;
}

static bool is_ua(const uint8_t *rom, uint32_t size)
{
    static const uint8_t sig[][3] = {
        {0x8D,0x40,0x02}, {0xAD,0x40,0x02}, {0xBD,0x1F,0x02},
        {0x2C,0xC0,0x02}, {0x8D,0xC0,0x02}, {0xAD,0xC0,0x02},
        {0x2C,0xB0,0x0F}
    };
    return any_sig3(rom, size, sig, sizeof sig / sizeof sig[0]);
}

static bool is_fe(const uint8_t *rom, uint32_t size)
{
    static const uint8_t sig[][5] = {
        {0x20,0x00,0xD0,0xC6,0xC5},
        {0x20,0xC3,0xF8,0xA5,0x82},
        {0xD0,0xFB,0x20,0x73,0xFE},
        {0xD0,0xFB,0x20,0x68,0xFE},
        {0x20,0x00,0xF0,0x84,0xD6}
    };
    for (unsigned i = 0; i < sizeof sig / sizeof sig[0]; i++)
        if (count_sig(rom, size, sig[i], 5)) return true;
    return false;
}

static atari_cart_type_t detect_type(const uint8_t *rom, uint32_t size)
{
    if (!rom || !size) return ATARI_CART_UNKNOWN;

    if (size <= KB(2)) return ATARI_CART_2K;
    if (size == KB(4)) return ATARI_CART_4K;

    if (size == KB(8)) {
        if (is_superchip(rom, size)) return ATARI_CART_F8SC;
        if (is_e0(rom, size))        return ATARI_CART_E0;
        if (is_3f(rom, size))        return ATARI_CART_3F;
        if (is_ua(rom, size))        return ATARI_CART_UA;
        if (is_fe(rom, size))        return ATARI_CART_FE;
        if (is_e7(rom, size))        return ATARI_CART_E7;
        return ATARI_CART_F8;
    }

    if (size == KB(12)) return ATARI_CART_FA;

    if (size == KB(16)) {
        if (is_superchip(rom, size)) return ATARI_CART_F6SC;
        if (is_e7(rom, size))        return ATARI_CART_E7;
        return ATARI_CART_F6;
    }

    if (size == KB(32)) {
        if (is_superchip(rom, size)) return ATARI_CART_F4SC;
        if (is_3f(rom, size))        return ATARI_CART_3F;
        return ATARI_CART_F4;
    }

    return ATARI_CART_UNKNOWN;
}

#if defined(__GNUC__)
#define ATARI_HOT __attribute__((hot, optimize("O3")))
#else
#define ATARI_HOT
#endif

typedef struct {
    const uint8_t *rom;
    const uint8_t *bank_ptr;
    uint32_t size;
    atari_cart_type_t type;

    uint16_t rom_mask;          // 0x07ff for 2K, 0x0fff otherwise
    uint16_t hotspot_base;      // F4/F6/F8 base; 0 when unbanked
    uint8_t  hotspot_count;
    uint8_t  bank;
    uint8_t  bank_count;
    bool     superchip;

    uint8_t superchip_ram[128];
} cart_runtime_t;

static cart_runtime_t s;

static inline __attribute__((always_inline)) void set_bank(uint8_t bank)
{
    s.bank = bank;
    s.bank_ptr = s.rom + (uint32_t)bank * 4096u;
}

static inline __attribute__((always_inline)) void hotspot_access(uint16_t address)
{
    // Ordinary ROM fetches are nowhere near $1FF4-$1FFB. Make that common
    // path one cheap range check; only an actual hotspot reaches set_bank().
    if (!s.hotspot_count)
        return;

    uint16_t rel = (uint16_t)(address - s.hotspot_base);
    if (rel < s.hotspot_count)
        set_bank((uint8_t)rel);
}

bool atari_cart_mount(const uint8_t *rom, uint32_t size)
{
    memset(&s, 0, sizeof s);
    s.rom = rom;
    s.size = size;
    s.type = detect_type(rom, size);

    switch (s.type) {
        case ATARI_CART_2K:
            s.bank_count = 1;
            s.rom_mask = 0x07FFu;
            s.bank_ptr = rom;
            break;

        case ATARI_CART_4K:
            s.bank_count = 1;
            s.rom_mask = 0x0FFFu;
            s.bank_ptr = rom;
            break;

        case ATARI_CART_F8:
        case ATARI_CART_F8SC:
            s.bank_count = 2;
            s.hotspot_base = 0x1FF8u;
            s.hotspot_count = 2;
            s.rom_mask = 0x0FFFu;
            s.superchip = (s.type == ATARI_CART_F8SC);
            set_bank(1);
            break;

        case ATARI_CART_F6:
        case ATARI_CART_F6SC:
            s.bank_count = 4;
            s.hotspot_base = 0x1FF6u;
            s.hotspot_count = 4;
            s.rom_mask = 0x0FFFu;
            s.superchip = (s.type == ATARI_CART_F6SC);
            set_bank(3);
            break;

        case ATARI_CART_F4:
        case ATARI_CART_F4SC:
            s.bank_count = 8;
            s.hotspot_base = 0x1FF4u;
            s.hotspot_count = 8;
            s.rom_mask = 0x0FFFu;
            s.superchip = (s.type == ATARI_CART_F4SC);
            set_bank(7);
            break;

        default:
            // Detected but intentionally not executable in the current phase.
            s.bank_count = 0;
            s.rom_mask = 0x0FFFu;
            s.bank_ptr = rom;
            break;
    }

    return s.type != ATARI_CART_UNKNOWN;
}

void atari_cart_reset(void)
{
    memset(s.superchip_ram, 0, sizeof s.superchip_ram);

    if (s.bank_count > 1)
        set_bank((uint8_t)(s.bank_count - 1u));
    else if (s.rom)
        s.bank_ptr = s.rom;
}

atari_cart_type_t atari_cart_type(void) { return s.type; }

const char *atari_cart_type_name(void)
{
    switch (s.type) {
        case ATARI_CART_2K:   return "2K";
        case ATARI_CART_4K:   return "4K";
        case ATARI_CART_F8:   return "F8";
        case ATARI_CART_F8SC: return "F8SC";
        case ATARI_CART_F6:   return "F6";
        case ATARI_CART_F6SC: return "F6SC";
        case ATARI_CART_F4:   return "F4";
        case ATARI_CART_F4SC: return "F4SC";
        case ATARI_CART_FA:   return "FA";
        case ATARI_CART_E0:   return "E0";
        case ATARI_CART_FE:   return "FE";
        case ATARI_CART_E7:   return "E7";
        case ATARI_CART_3F:   return "3F";
        case ATARI_CART_UA:   return "UA";
        default:              return "Unknown";
    }
}

bool atari_cart_supported(void)
{
    switch (s.type) {
        case ATARI_CART_2K:
        case ATARI_CART_4K:
        case ATARI_CART_F8:
        case ATARI_CART_F8SC:
        case ATARI_CART_F6:
        case ATARI_CART_F6SC:
        case ATARI_CART_F4:
        case ATARI_CART_F4SC:
            return true;
        default:
            return false;
    }
}

ATARI_HOT uint8_t atari_cart_read(uint16_t address)
{
    if (!s.bank_ptr)
        return 0xFF;

    address &= 0x1FFFu;

    // F8/F6/F4 hotspots switch on either read or write.
    hotspot_access(address);

    if (s.superchip && (address & 0x1F00u) == 0x1000u)
        return s.superchip_ram[address & 0x7Fu];

    return s.bank_ptr[(address & 0x0FFFu) & s.rom_mask];
}

ATARI_HOT void atari_cart_write(uint16_t address, uint8_t value)
{
    address &= 0x1FFFu;

    hotspot_access(address);

    if (s.superchip && address >= 0x1000u && address <= 0x107Fu)
        s.superchip_ram[address & 0x7Fu] = value;
}