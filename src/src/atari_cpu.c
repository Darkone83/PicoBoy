/*
 * PicoBoy cycle-stepped MOS 6507 core.
 *
 * Written for the PicoBoy Atari 2600 integration.  It intentionally works one
 * external bus cycle at a time so TIA writes, WSYNC and cartridge hotspots stay
 * at CPU-cycle granularity.  Legal NMOS 6502 opcodes are implemented, plus the
 * common unofficial NOP encodings.  Other illegal opcodes halt the M1 core and
 * are reported by atari_core so we can add them from real compatibility data.
 */
#include "atari_cpu.h"
#include "atari_cart.h"
#include "atari_tia.h"
#include "atari_riot.h"
#include <string.h>

#if defined(__GNUC__)
#define ATARI_HOT __attribute__((hot, optimize("O3")))
#else
#define ATARI_HOT
#endif

enum {
    F_C = 0x01, F_Z = 0x02, F_I = 0x04, F_D = 0x08,
    F_B = 0x10, F_U = 0x20, F_V = 0x40, F_N = 0x80
};

typedef enum {
    OP_ILL = 0,
    OP_ADC, OP_AND, OP_ASL,
    OP_BCC, OP_BCS, OP_BEQ, OP_BIT, OP_BMI, OP_BNE, OP_BPL, OP_BRK, OP_BVC, OP_BVS,
    OP_CLC, OP_CLD, OP_CLI, OP_CLV, OP_CMP, OP_CPX, OP_CPY,
    OP_DEC, OP_DEX, OP_DEY,
    OP_EOR,
    OP_INC, OP_INX, OP_INY,
    OP_JMP, OP_JSR,
    OP_LDA, OP_LDX, OP_LDY, OP_LSR,
    OP_NOP,
    OP_ORA,
    OP_PHA, OP_PHP, OP_PLA, OP_PLP,
    OP_ROL, OP_ROR, OP_RTI, OP_RTS,
    OP_SBC, OP_SEC, OP_SED, OP_SEI,
    OP_STA, OP_STX, OP_STY,
    OP_TAX, OP_TAY, OP_TSX, OP_TXA, OP_TXS, OP_TYA
} op_t;

typedef enum {
    AM_NONE = 0,
    AM_IMP, AM_ACC, AM_IMM,
    AM_ZP, AM_ZPX, AM_ZPY,
    AM_ABS, AM_ABSX, AM_ABSY,
    AM_INDX, AM_INDY,
    AM_REL,
    AM_ZP_RMW, AM_ZPX_RMW, AM_ABS_RMW, AM_ABSX_RMW,
    AM_JMP_ABS, AM_JMP_IND, AM_JSR, AM_RTS, AM_RTI, AM_BRK,
    AM_PHA, AM_PHP, AM_PLA, AM_PLP
} addrmode_t;

typedef struct {
    uint8_t op;
    uint8_t mode;
} instr_t;

#define I(op,mode) { OP_##op, AM_##mode }

static const instr_t isa[256] = {
    [0x00]=I(BRK,BRK),
    [0x01]=I(ORA,INDX), [0x05]=I(ORA,ZP),   [0x06]=I(ASL,ZP_RMW),
    [0x08]=I(PHP,PHP),  [0x09]=I(ORA,IMM),  [0x0A]=I(ASL,ACC),
    [0x0D]=I(ORA,ABS),  [0x0E]=I(ASL,ABS_RMW),

    [0x10]=I(BPL,REL),  [0x11]=I(ORA,INDY), [0x15]=I(ORA,ZPX),
    [0x16]=I(ASL,ZPX_RMW), [0x18]=I(CLC,IMP), [0x19]=I(ORA,ABSY),
    [0x1D]=I(ORA,ABSX), [0x1E]=I(ASL,ABSX_RMW),

    [0x20]=I(JSR,JSR),  [0x21]=I(AND,INDX), [0x24]=I(BIT,ZP),
    [0x25]=I(AND,ZP),   [0x26]=I(ROL,ZP_RMW), [0x28]=I(PLP,PLP),
    [0x29]=I(AND,IMM),  [0x2A]=I(ROL,ACC),  [0x2C]=I(BIT,ABS),
    [0x2D]=I(AND,ABS),  [0x2E]=I(ROL,ABS_RMW),

    [0x30]=I(BMI,REL),  [0x31]=I(AND,INDY), [0x35]=I(AND,ZPX),
    [0x36]=I(ROL,ZPX_RMW), [0x38]=I(SEC,IMP), [0x39]=I(AND,ABSY),
    [0x3D]=I(AND,ABSX), [0x3E]=I(ROL,ABSX_RMW),

    [0x40]=I(RTI,RTI),  [0x41]=I(EOR,INDX), [0x45]=I(EOR,ZP),
    [0x46]=I(LSR,ZP_RMW), [0x48]=I(PHA,PHA), [0x49]=I(EOR,IMM),
    [0x4A]=I(LSR,ACC),  [0x4C]=I(JMP,JMP_ABS), [0x4D]=I(EOR,ABS),
    [0x4E]=I(LSR,ABS_RMW),

    [0x50]=I(BVC,REL),  [0x51]=I(EOR,INDY), [0x55]=I(EOR,ZPX),
    [0x56]=I(LSR,ZPX_RMW), [0x58]=I(CLI,IMP), [0x59]=I(EOR,ABSY),
    [0x5D]=I(EOR,ABSX), [0x5E]=I(LSR,ABSX_RMW),

    [0x60]=I(RTS,RTS),  [0x61]=I(ADC,INDX), [0x65]=I(ADC,ZP),
    [0x66]=I(ROR,ZP_RMW), [0x68]=I(PLA,PLA), [0x69]=I(ADC,IMM),
    [0x6A]=I(ROR,ACC),  [0x6C]=I(JMP,JMP_IND), [0x6D]=I(ADC,ABS),
    [0x6E]=I(ROR,ABS_RMW),

    [0x70]=I(BVS,REL),  [0x71]=I(ADC,INDY), [0x75]=I(ADC,ZPX),
    [0x76]=I(ROR,ZPX_RMW), [0x78]=I(SEI,IMP), [0x79]=I(ADC,ABSY),
    [0x7D]=I(ADC,ABSX), [0x7E]=I(ROR,ABSX_RMW),

    [0x81]=I(STA,INDX), [0x84]=I(STY,ZP),   [0x85]=I(STA,ZP),
    [0x86]=I(STX,ZP),   [0x88]=I(DEY,IMP),  [0x8A]=I(TXA,IMP),
    [0x8C]=I(STY,ABS),  [0x8D]=I(STA,ABS),  [0x8E]=I(STX,ABS),

    [0x90]=I(BCC,REL),  [0x91]=I(STA,INDY), [0x94]=I(STY,ZPX),
    [0x95]=I(STA,ZPX),  [0x96]=I(STX,ZPY),  [0x98]=I(TYA,IMP),
    [0x99]=I(STA,ABSY), [0x9A]=I(TXS,IMP),  [0x9D]=I(STA,ABSX),

    [0xA0]=I(LDY,IMM),  [0xA1]=I(LDA,INDX), [0xA2]=I(LDX,IMM),
    [0xA4]=I(LDY,ZP),   [0xA5]=I(LDA,ZP),   [0xA6]=I(LDX,ZP),
    [0xA8]=I(TAY,IMP),  [0xA9]=I(LDA,IMM),  [0xAA]=I(TAX,IMP),
    [0xAC]=I(LDY,ABS),  [0xAD]=I(LDA,ABS),  [0xAE]=I(LDX,ABS),

    [0xB0]=I(BCS,REL),  [0xB1]=I(LDA,INDY), [0xB4]=I(LDY,ZPX),
    [0xB5]=I(LDA,ZPX),  [0xB6]=I(LDX,ZPY),  [0xB8]=I(CLV,IMP),
    [0xB9]=I(LDA,ABSY), [0xBA]=I(TSX,IMP),  [0xBC]=I(LDY,ABSX),
    [0xBD]=I(LDA,ABSX), [0xBE]=I(LDX,ABSY),

    [0xC0]=I(CPY,IMM),  [0xC1]=I(CMP,INDX), [0xC4]=I(CPY,ZP),
    [0xC5]=I(CMP,ZP),   [0xC6]=I(DEC,ZP_RMW), [0xC8]=I(INY,IMP),
    [0xC9]=I(CMP,IMM),  [0xCA]=I(DEX,IMP),  [0xCC]=I(CPY,ABS),
    [0xCD]=I(CMP,ABS),  [0xCE]=I(DEC,ABS_RMW),

    [0xD0]=I(BNE,REL),  [0xD1]=I(CMP,INDY), [0xD5]=I(CMP,ZPX),
    [0xD6]=I(DEC,ZPX_RMW), [0xD8]=I(CLD,IMP), [0xD9]=I(CMP,ABSY),
    [0xDD]=I(CMP,ABSX), [0xDE]=I(DEC,ABSX_RMW),

    [0xE0]=I(CPX,IMM),  [0xE1]=I(SBC,INDX), [0xE4]=I(CPX,ZP),
    [0xE5]=I(SBC,ZP),   [0xE6]=I(INC,ZP_RMW), [0xE8]=I(INX,IMP),
    [0xE9]=I(SBC,IMM),  [0xEA]=I(NOP,IMP),  [0xEC]=I(CPX,ABS),
    [0xED]=I(SBC,ABS),  [0xEE]=I(INC,ABS_RMW),

    [0xF0]=I(BEQ,REL),  [0xF1]=I(SBC,INDY), [0xF5]=I(SBC,ZPX),
    [0xF6]=I(INC,ZPX_RMW), [0xF8]=I(SED,IMP), [0xF9]=I(SBC,ABSY),
    [0xFD]=I(SBC,ABSX), [0xFE]=I(INC,ABSX_RMW),

    // Common NMOS unofficial NOPs.  Keeping their addressing mode/cycle shape
    // prevents otherwise benign padding from stopping a test ROM.
    [0x1A]=I(NOP,IMP), [0x3A]=I(NOP,IMP), [0x5A]=I(NOP,IMP),
    [0x7A]=I(NOP,IMP), [0xDA]=I(NOP,IMP), [0xFA]=I(NOP,IMP),
    [0x80]=I(NOP,IMM), [0x82]=I(NOP,IMM), [0x89]=I(NOP,IMM),
    [0xC2]=I(NOP,IMM), [0xE2]=I(NOP,IMM),
    [0x04]=I(NOP,ZP),  [0x44]=I(NOP,ZP),  [0x64]=I(NOP,ZP),
    [0x14]=I(NOP,ZPX), [0x34]=I(NOP,ZPX), [0x54]=I(NOP,ZPX),
    [0x74]=I(NOP,ZPX), [0xD4]=I(NOP,ZPX), [0xF4]=I(NOP,ZPX),
    [0x0C]=I(NOP,ABS),
    [0x1C]=I(NOP,ABSX), [0x3C]=I(NOP,ABSX), [0x5C]=I(NOP,ABSX),
    [0x7C]=I(NOP,ABSX), [0xDC]=I(NOP,ABSX), [0xFC]=I(NOP,ABSX),
};
#undef I

typedef struct {
    uint8_t a, x, y, s, p;
    uint16_t pc;

    uint8_t opcode;
    uint8_t decoded_op;
    uint8_t decoded_mode;
    uint8_t store;
    uint8_t cycle;
    uint8_t lo, hi, data;
    uint8_t zp;
    uint16_t addr;
    uint16_t base;
    uint16_t provisional;
    uint16_t target;
    bool page_cross;
    bool halted;
} cpu_t;

static cpu_t c;

static inline void set_flag(uint8_t f, bool on) {
    if (on) c.p |= f; else c.p &= (uint8_t)~f;
}
static inline bool flag(uint8_t f) { return (c.p & f) != 0; }
static inline void set_nz(uint8_t v) {
    set_flag(F_Z, v == 0);
    set_flag(F_N, (v & 0x80) != 0);
}
// 6507 hot bus path. Keep the decode directly beside the interpreter so every
// CPU bus cycle does not pay an extra atari_core.c wrapper call. This is the
// same decode used by atari_core: A12=cart, A7=TIA/RIOT, A9=RIOT RAM/I/O.
static inline __attribute__((always_inline)) uint8_t rd(uint16_t address) {
    uint16_t a = address & 0x1FFFu;

    if (a & 0x1000u) return atari_cart_read(a);
    if (!(a & 0x0080u)) return atari_tia_read(a);
    if (!(a & 0x0200u)) return atari_riot_ram_read(a);
    return atari_riot_io_read(a);
}

static inline __attribute__((always_inline)) void wr(uint16_t address, uint8_t value) {
    uint16_t a = address & 0x1FFFu;

    if (a & 0x1000u) { atari_cart_write(a, value); return; }
    if (!(a & 0x0080u)) { atari_tia_write(a, value); return; }
    if (!(a & 0x0200u)) { atari_riot_ram_write(a, value); return; }
    atari_riot_io_write(a, value);
}

static inline bool op_is_store(op_t op) {
    return op == OP_STA || op == OP_STX || op == OP_STY;
}

static inline uint8_t store_value(op_t op) {
    switch (op) {
        case OP_STA: return c.a;
        case OP_STX: return c.x;
        case OP_STY: return c.y;
        default:     return 0xFF;
    }
}

static void do_adc(uint8_t v) {
    const uint8_t a0 = c.a;
    const unsigned cin = flag(F_C) ? 1u : 0u;
    const uint16_t binary = (uint16_t)a0 + v + cin;
    set_flag(F_V, ((~(a0 ^ v) & (a0 ^ (uint8_t)binary)) & 0x80) != 0);

    if (flag(F_D)) {
        unsigned lo = (a0 & 0x0F) + (v & 0x0F) + cin;
        unsigned hi = (a0 >> 4) + (v >> 4);
        if (lo > 9) { lo += 6; hi++; }

        // NMOS flags are derived during the binary/decimal correction path.
        // Z from the binary result is the behaviour relied on by classic code.
        set_flag(F_Z, (uint8_t)binary == 0);
        set_flag(F_N, (hi & 0x08) != 0);

        if (hi > 9) hi += 6;
        set_flag(F_C, hi > 0x0F);
        c.a = (uint8_t)((hi << 4) | (lo & 0x0F));
    } else {
        c.a = (uint8_t)binary;
        set_flag(F_C, binary > 0xFF);
        set_nz(c.a);
    }
}

static void do_sbc(uint8_t v) {
    const uint8_t a0 = c.a;
    const int borrow = flag(F_C) ? 0 : 1;
    const int16_t diff = (int16_t)a0 - (int16_t)v - borrow;
    const uint8_t binres = (uint8_t)diff;

    set_flag(F_V, (((a0 ^ v) & (a0 ^ binres)) & 0x80) != 0);
    set_flag(F_C, diff >= 0);

    if (flag(F_D)) {
        int lo = (a0 & 0x0F) - (v & 0x0F) - borrow;
        int hi = (a0 >> 4) - (v >> 4);
        if (lo < 0) { lo -= 6; hi--; }
        if (hi < 0) hi -= 6;
        c.a = (uint8_t)(((hi << 4) & 0xF0) | (lo & 0x0F));
        // The NMOS decimal flag results are quirky; binary N/Z is the useful
        // compatibility behaviour for the 2600 software that uses BCD.
        set_flag(F_Z, binres == 0);
        set_flag(F_N, (binres & 0x80) != 0);
    } else {
        c.a = binres;
        set_nz(c.a);
    }
}

static void compare(uint8_t r, uint8_t v) {
    uint16_t d = (uint16_t)r - v;
    set_flag(F_C, r >= v);
    set_nz((uint8_t)d);
}

static void apply_read(op_t op, uint8_t v) {
    switch (op) {
        case OP_ADC: do_adc(v); break;
        case OP_AND: c.a &= v; set_nz(c.a); break;
        case OP_BIT:
            set_flag(F_Z, (c.a & v) == 0);
            set_flag(F_N, (v & 0x80) != 0);
            set_flag(F_V, (v & 0x40) != 0);
            break;
        case OP_CMP: compare(c.a, v); break;
        case OP_CPX: compare(c.x, v); break;
        case OP_CPY: compare(c.y, v); break;
        case OP_EOR: c.a ^= v; set_nz(c.a); break;
        case OP_LDA: c.a = v; set_nz(c.a); break;
        case OP_LDX: c.x = v; set_nz(c.x); break;
        case OP_LDY: c.y = v; set_nz(c.y); break;
        case OP_ORA: c.a |= v; set_nz(c.a); break;
        case OP_SBC: do_sbc(v); break;
        case OP_NOP: default: break;
    }
}

static uint8_t apply_rmw(op_t op, uint8_t v) {
    switch (op) {
        case OP_ASL:
            set_flag(F_C, (v & 0x80) != 0);
            v <<= 1; set_nz(v); return v;
        case OP_DEC:
            v--; set_nz(v); return v;
        case OP_INC:
            v++; set_nz(v); return v;
        case OP_LSR:
            set_flag(F_C, (v & 0x01) != 0);
            v >>= 1; set_nz(v); return v;
        case OP_ROL: {
            bool oldc = flag(F_C);
            set_flag(F_C, (v & 0x80) != 0);
            v = (uint8_t)((v << 1) | (oldc ? 1 : 0));
            set_nz(v); return v;
        }
        case OP_ROR: {
            bool oldc = flag(F_C);
            set_flag(F_C, (v & 0x01) != 0);
            v = (uint8_t)((v >> 1) | (oldc ? 0x80 : 0));
            set_nz(v); return v;
        }
        default: return v;
    }
}

static void apply_imp(op_t op) {
    switch (op) {
        case OP_CLC: set_flag(F_C, false); break;
        case OP_CLD: set_flag(F_D, false); break;
        case OP_CLI: set_flag(F_I, false); break;
        case OP_CLV: set_flag(F_V, false); break;
        case OP_DEX: c.x--; set_nz(c.x); break;
        case OP_DEY: c.y--; set_nz(c.y); break;
        case OP_INX: c.x++; set_nz(c.x); break;
        case OP_INY: c.y++; set_nz(c.y); break;
        case OP_SEC: set_flag(F_C, true); break;
        case OP_SED: set_flag(F_D, true); break;
        case OP_SEI: set_flag(F_I, true); break;
        case OP_TAX: c.x = c.a; set_nz(c.x); break;
        case OP_TAY: c.y = c.a; set_nz(c.y); break;
        case OP_TSX: c.x = c.s; set_nz(c.x); break;
        case OP_TXA: c.a = c.x; set_nz(c.a); break;
        case OP_TXS: c.s = c.x; break;
        case OP_TYA: c.a = c.y; set_nz(c.a); break;
        case OP_NOP: default: break;
    }
}

static bool branch_taken(op_t op) {
    switch (op) {
        case OP_BCC: return !flag(F_C);
        case OP_BCS: return  flag(F_C);
        case OP_BEQ: return  flag(F_Z);
        case OP_BMI: return  flag(F_N);
        case OP_BNE: return !flag(F_Z);
        case OP_BPL: return !flag(F_N);
        case OP_BVC: return !flag(F_V);
        case OP_BVS: return  flag(F_V);
        default:     return false;
    }
}


// ---------------------------------------------------------------------------
// Whole-instruction execution with cycle-timed bus helpers
// ---------------------------------------------------------------------------
static bool s_timing_abort = false;

// Ordinary ROM/RAM cycles remain pending across instruction boundaries.
// They only need to become visible when the 6507 touches TIA/RIOT I/O.
// This preserves the exact cumulative cycle count at each device access while
// avoiding thousands of scheduler/TIA calls per video frame.
static uint32_t s_pending_cycles = 0;

static inline __attribute__((always_inline)) bool is_tia(uint16_t address) {
    uint16_t a = address & 0x1FFFu;
    return !(a & 0x1000u) && !(a & 0x0080u);
}

static inline __attribute__((always_inline)) bool is_riot_io(uint16_t address) {
    uint16_t a = address & 0x1FFFu;
    return !(a & 0x1000u) && (a & 0x0080u) && (a & 0x0200u);
}

static inline __attribute__((always_inline)) bool sync_pending_cycles(void) {
    if (!s_pending_cycles)
        return true;

    uint32_t pending = s_pending_cycles;
    s_pending_cycles = 0;

    if (!atari_machine_advance_cpu(pending)) {
        s_timing_abort = true;
        return false;
    }

    return true;
}

static inline __attribute__((always_inline)) uint8_t cycle_read(uint16_t address)
{
    s_pending_cycles++;

    // ROM/RAM fetches are not beam-sensitive. Keep their clocks pending across
    // instructions and synchronize only when a device access can observe time.
    if ((is_tia(address) || is_riot_io(address)) && !sync_pending_cycles())
        return 0xFF;

    return rd(address);
}

static inline __attribute__((always_inline)) void cycle_write(uint16_t address, uint8_t value)
{
    s_pending_cycles++;

    bool tia = is_tia(address);
    if ((tia || is_riot_io(address)) && !sync_pending_cycles())
        return;

    wr(address, value);

    if (tia && !atari_machine_tia_write_complete())
        s_timing_abort = true;
}

static inline __attribute__((always_inline)) void cycle_dummy(uint16_t address)
{
    (void)cycle_read(address);
}

static inline __attribute__((always_inline)) void cycle_push(uint8_t value)
{
    cycle_write((uint16_t)(0x0100u | c.s), value);
    c.s--;
}

void atari_cpu_reset(void)
{
    memset(&c, 0, sizeof c);
    c.s = 0xFD;
    c.p = F_U | F_I;

    // Reset-vector reads happen before the runtime scheduler starts, matching
    // the previous core and the xrip source startup sequence.
    uint8_t lo = rd(0xFFFC);
    uint8_t hi = rd(0xFFFD);
    c.pc = (uint16_t)(((uint16_t)hi << 8) | lo);
    s_pending_cycles = 0;
}

bool atari_cpu_halted(void) { return c.halted; }
uint16_t atari_cpu_pc(void) { return c.pc; }
uint8_t atari_cpu_last_opcode(void) { return c.opcode; }

ATARI_HOT bool atari_cpu_step_instruction(void)
{
    if (c.halted)
        return false;

    s_timing_abort = false;

    // Cycle 1: opcode fetch.
    c.opcode = cycle_read(c.pc++);
    if (s_timing_abort)
        return false;

    const instr_t ins = isa[c.opcode];
    const op_t op = (op_t)ins.op;
    const addrmode_t mode = (addrmode_t)ins.mode;

    if (op == OP_ILL) {
        c.halted = true;
        return false;
    }

    const bool store = op_is_store(op);

    switch (mode) {
        case AM_IMP:
            cycle_dummy(c.pc);
            apply_imp(op);
            break;

        case AM_ACC:
            cycle_dummy(c.pc);
            c.a = apply_rmw(op, c.a);
            break;

        case AM_IMM: {
            uint8_t v = cycle_read(c.pc++);
            apply_read(op, v);
            break;
        }

        case AM_ZP: {
            uint16_t a = cycle_read(c.pc++);
            if (store) cycle_write(a, store_value(op));
            else       apply_read(op, cycle_read(a));
            break;
        }

        case AM_ZPX:
        case AM_ZPY: {
            uint8_t base = cycle_read(c.pc++);
            cycle_dummy(base);
            uint8_t idx = (mode == AM_ZPX) ? c.x : c.y;
            uint16_t a = (uint8_t)(base + idx);
            if (store) cycle_write(a, store_value(op));
            else       apply_read(op, cycle_read(a));
            break;
        }

        case AM_ABS: {
            uint8_t lo = cycle_read(c.pc++);
            uint8_t hi = cycle_read(c.pc++);
            uint16_t a = (uint16_t)(((uint16_t)hi << 8) | lo);
            if (store) cycle_write(a, store_value(op));
            else       apply_read(op, cycle_read(a));
            break;
        }

        case AM_ABSX:
        case AM_ABSY: {
            uint8_t lo = cycle_read(c.pc++);
            uint8_t hi = cycle_read(c.pc++);
            uint8_t idx = (mode == AM_ABSX) ? c.x : c.y;
            uint16_t base = (uint16_t)(((uint16_t)hi << 8) | lo);
            uint16_t a = (uint16_t)(base + idx);
            uint16_t provisional = (uint16_t)((base & 0xFF00u) |
                                               ((uint8_t)(lo + idx)));
            bool cross = (a & 0xFF00u) != (base & 0xFF00u);

            uint8_t v = cycle_read(provisional);   // real read or store dummy
            if (store) {
                cycle_write(a, store_value(op));
            } else if (cross) {
                apply_read(op, cycle_read(a));
            } else {
                apply_read(op, v);
            }
            break;
        }

        case AM_INDX: {
            uint8_t zp = cycle_read(c.pc++);
            cycle_dummy(zp);
            zp = (uint8_t)(zp + c.x);
            uint8_t lo = cycle_read(zp);
            uint8_t hi = cycle_read((uint8_t)(zp + 1));
            uint16_t a = (uint16_t)(((uint16_t)hi << 8) | lo);
            if (store) cycle_write(a, store_value(op));
            else       apply_read(op, cycle_read(a));
            break;
        }

        case AM_INDY: {
            uint8_t zp = cycle_read(c.pc++);
            uint8_t lo = cycle_read(zp);
            uint8_t hi = cycle_read((uint8_t)(zp + 1));
            uint16_t base = (uint16_t)(((uint16_t)hi << 8) | lo);
            uint16_t a = (uint16_t)(base + c.y);
            uint16_t provisional = (uint16_t)((base & 0xFF00u) |
                                               ((uint8_t)(lo + c.y)));
            bool cross = (a & 0xFF00u) != (base & 0xFF00u);

            uint8_t v = cycle_read(provisional);
            if (store) {
                cycle_write(a, store_value(op));
            } else if (cross) {
                apply_read(op, cycle_read(a));
            } else {
                apply_read(op, v);
            }
            break;
        }

        case AM_REL: {
            int8_t off = (int8_t)cycle_read(c.pc++);
            if (branch_taken(op)) {
                uint16_t oldpc = c.pc;
                uint16_t target = (uint16_t)(c.pc + off);

                cycle_dummy(c.pc);

                if ((target & 0xFF00u) != (oldpc & 0xFF00u)) {
                    c.pc = (uint16_t)((oldpc & 0xFF00u) |
                                      (target & 0x00FFu));
                    cycle_dummy(c.pc);
                }

                c.pc = target;
            }
            break;
        }

        case AM_ZP_RMW: {
            uint16_t a = cycle_read(c.pc++);
            uint8_t v = cycle_read(a);
            cycle_write(a, v);                  // NMOS dummy write
            v = apply_rmw(op, v);
            cycle_write(a, v);
            break;
        }

        case AM_ZPX_RMW: {
            uint8_t base = cycle_read(c.pc++);
            cycle_dummy(base);
            uint16_t a = (uint8_t)(base + c.x);
            uint8_t v = cycle_read(a);
            cycle_write(a, v);
            v = apply_rmw(op, v);
            cycle_write(a, v);
            break;
        }

        case AM_ABS_RMW: {
            uint8_t lo = cycle_read(c.pc++);
            uint8_t hi = cycle_read(c.pc++);
            uint16_t a = (uint16_t)(((uint16_t)hi << 8) | lo);
            uint8_t v = cycle_read(a);
            cycle_write(a, v);
            v = apply_rmw(op, v);
            cycle_write(a, v);
            break;
        }

        case AM_ABSX_RMW: {
            uint8_t lo = cycle_read(c.pc++);
            uint8_t hi = cycle_read(c.pc++);
            uint16_t base = (uint16_t)(((uint16_t)hi << 8) | lo);
            uint16_t a = (uint16_t)(base + c.x);
            uint16_t provisional = (uint16_t)((base & 0xFF00u) |
                                               ((uint8_t)(lo + c.x)));
            cycle_dummy(provisional);
            uint8_t v = cycle_read(a);
            cycle_write(a, v);
            v = apply_rmw(op, v);
            cycle_write(a, v);
            break;
        }

        case AM_JMP_ABS: {
            uint8_t lo = cycle_read(c.pc++);
            uint8_t hi = cycle_read(c.pc);
            c.pc = (uint16_t)(((uint16_t)hi << 8) | lo);
            break;
        }

        case AM_JMP_IND: {
            uint8_t lo = cycle_read(c.pc++);
            uint8_t hi = cycle_read(c.pc++);
            uint16_t ptr = (uint16_t)(((uint16_t)hi << 8) | lo);
            uint8_t tlo = cycle_read(ptr);
            uint16_t ptr2 = (uint16_t)((ptr & 0xFF00u) |
                                        ((ptr + 1u) & 0x00FFu));
            uint8_t thi = cycle_read(ptr2);      // NMOS JMP wrap bug
            c.pc = (uint16_t)(((uint16_t)thi << 8) | tlo);
            break;
        }

        case AM_JSR: {
            uint8_t lo = cycle_read(c.pc++);
            cycle_dummy((uint16_t)(0x0100u | c.s));
            cycle_push((uint8_t)(c.pc >> 8));
            cycle_push((uint8_t)c.pc);
            uint8_t hi = cycle_read(c.pc);
            c.pc = (uint16_t)(((uint16_t)hi << 8) | lo);
            break;
        }

        case AM_RTS: {
            cycle_dummy(c.pc);
            cycle_dummy((uint16_t)(0x0100u | c.s));
            c.s++;
            uint8_t lo = cycle_read((uint16_t)(0x0100u | c.s));
            c.s++;
            uint8_t hi = cycle_read((uint16_t)(0x0100u | c.s));
            c.pc = (uint16_t)(((uint16_t)hi << 8) | lo);
            cycle_dummy(c.pc);
            c.pc++;
            break;
        }

        case AM_RTI: {
            cycle_dummy(c.pc);
            cycle_dummy((uint16_t)(0x0100u | c.s));
            c.s++;
            c.p = (uint8_t)((cycle_read((uint16_t)(0x0100u | c.s)) & ~F_B) | F_U);
            c.s++;
            uint8_t lo = cycle_read((uint16_t)(0x0100u | c.s));
            c.s++;
            uint8_t hi = cycle_read((uint16_t)(0x0100u | c.s));
            c.pc = (uint16_t)(((uint16_t)hi << 8) | lo);
            break;
        }

        case AM_BRK: {
            cycle_dummy(c.pc++);
            cycle_push((uint8_t)(c.pc >> 8));
            cycle_push((uint8_t)c.pc);
            cycle_push((uint8_t)(c.p | F_B | F_U));
            set_flag(F_I, true);
            uint8_t lo = cycle_read(0xFFFE);
            uint8_t hi = cycle_read(0xFFFF);
            c.pc = (uint16_t)(((uint16_t)hi << 8) | lo);
            break;
        }

        case AM_PHA:
            cycle_dummy(c.pc);
            cycle_push(c.a);
            break;

        case AM_PHP:
            cycle_dummy(c.pc);
            cycle_push((uint8_t)(c.p | F_B | F_U));
            break;

        case AM_PLA:
            cycle_dummy(c.pc);
            cycle_dummy((uint16_t)(0x0100u | c.s));
            c.s++;
            c.a = cycle_read((uint16_t)(0x0100u | c.s));
            set_nz(c.a);
            break;

        case AM_PLP:
            cycle_dummy(c.pc);
            cycle_dummy((uint16_t)(0x0100u | c.s));
            c.s++;
            c.p = (uint8_t)((cycle_read((uint16_t)(0x0100u | c.s)) & ~F_B) | F_U);
            break;

        default:
            c.halted = true;
            return false;
    }

    // No instruction-boundary synchronization is required. If the next
    // instruction is also ROM/RAM-only, its cycles simply join the same
    // pending span. The next TIA/RIOT I/O access flushes the exact total.
    return !c.halted && !s_timing_abort;
}