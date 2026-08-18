/*
 * PicoBoy MOS 6532 RIOT model.
 * Based on the register/timer behaviour used by dgrubb/HiFive1-2600 and
 * xrip/pico-atari2600, flattened and tightened for PicoBoy.
 */
#include "atari_riot.h"
#include <string.h>
#include <stdbool.h>

typedef struct {
    uint8_t ram[128];

    uint8_t swcha_in;
    uint8_t swchb_in;
    uint8_t swcha_out, swacnt;
    uint8_t swchb_out, swbcnt;

    uint8_t timer;
    uint16_t divider;
    uint16_t prescale;
    bool underflow;
    bool timer_irq;
} riot_t;

static riot_t r;

void atari_riot_reset(void) {
    memset(&r, 0, sizeof r);
    r.swcha_in = 0xFF;
    // Reset + Select released, colour switch on, both difficulty switches A.
    // This matches the starting switch byte used by the xrip port.
    r.swchb_in = 0x0B;
}

void atari_riot_set_joystick(uint8_t swcha) { r.swcha_in = swcha; }
void atari_riot_set_switches(uint8_t swchb) { r.swchb_in = swchb; }

uint8_t atari_riot_ram_read(uint16_t address) {
    return r.ram[address & 0x7F];
}

void atari_riot_ram_write(uint16_t address, uint8_t value) {
    r.ram[address & 0x7F] = value;
}

static void set_timer(uint16_t divisor, uint8_t value) {
    r.timer = value;
    r.divider = divisor;
    r.prescale = divisor;
    r.underflow = false;
    r.timer_irq = false;
}

void atari_riot_tick(void) {
    if (!r.divider) return;

    if (r.prescale > 1) {
        r.prescale--;
        return;
    }

    r.prescale = r.divider;

    if (!r.underflow) {
        if (r.timer == 0) {
            // After the programmed interval expires the 6532 free-runs at
            // divide-by-one from $FF and raises the timer flag.
            r.timer = 0xFF;
            r.divider = 1;
            r.prescale = 1;
            r.underflow = true;
            r.timer_irq = true;
        } else {
            r.timer--;
        }
    } else {
        r.timer--;
    }
}


void atari_riot_advance(uint32_t cycles) {
    if (!cycles || !r.divider)
        return;

    // Once the programmed interval underflows the real part free-runs at
    // divide-by-one. That common state can be advanced in one subtraction.
    if (r.underflow) {
        r.timer = (uint8_t)(r.timer - (uint8_t)cycles);
        r.prescale = 1;
        return;
    }

    // CPU clocks until the first timer decrement, followed by divider-spaced
    // decrements. Underflow occurs one decrement after the visible timer hits 0.
    uint32_t to_underflow =
        (uint32_t)r.prescale + (uint32_t)r.timer * (uint32_t)r.divider;

    if (cycles >= to_underflow) {
        uint32_t rest = cycles - to_underflow;
        r.timer = 0xFF;
        r.divider = 1;
        r.prescale = 1;
        r.underflow = true;
        r.timer_irq = true;

        if (rest)
            r.timer = (uint8_t)(r.timer - (uint8_t)rest);
        return;
    }

    // No underflow in this span.
    if (cycles < r.prescale) {
        r.prescale = (uint16_t)(r.prescale - cycles);
        return;
    }

    cycles -= r.prescale;
    uint32_t ticks = 1u + cycles / r.divider;
    uint32_t rem = cycles % r.divider;

    r.timer = (uint8_t)(r.timer - (uint8_t)ticks);
    r.prescale = rem ? (uint16_t)(r.divider - rem) : r.divider;
}

uint8_t atari_riot_io_read(uint16_t address) {
    // RIOT peripheral decoding repeats through its mirrors; low five address
    // bits identify the useful register.
    uint8_t reg = (uint8_t)(address & 0x1F);
    switch (reg) {
        case 0x00: { // SWCHA
            // DDR=0 -> external input; DDR=1 -> output latch.
            return (uint8_t)((r.swcha_in & ~r.swacnt) | (r.swcha_out & r.swacnt));
        }
        case 0x01: return r.swacnt;
        case 0x02: return (uint8_t)((r.swchb_in & ~r.swbcnt) | (r.swchb_out & r.swbcnt));
        case 0x03: return r.swbcnt;
        case 0x04: return r.timer;                  // INTIM
        case 0x05: {                                // INSTAT
            uint8_t v = r.timer_irq ? 0x80 : 0x00;
            r.timer_irq = false;
            return v;
        }
        default: return 0xFF;
    }
}

void atari_riot_io_write(uint16_t address, uint8_t value) {
    uint8_t reg = (uint8_t)(address & 0x1F);
    switch (reg) {
        case 0x00: r.swcha_out = value; return;
        case 0x01: r.swacnt    = value; return;
        case 0x02: r.swchb_out = value; return;
        case 0x03: r.swbcnt    = value; return;

        // $294-$297 and their IRQ-enable mirrors $29C-$29F.
        case 0x14: case 0x1C: set_timer(1,    value); return;
        case 0x15: case 0x1D: set_timer(8,    value); return;
        case 0x16: case 0x1E: set_timer(64,   value); return;
        case 0x17: case 0x1F: set_timer(1024, value); return;
        default: return;
    }
}