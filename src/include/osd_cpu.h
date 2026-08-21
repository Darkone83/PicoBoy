#ifndef OSD_CPU_H
#define OSD_CPU_H

/*
 * PicoBoy runs on RP2040 (ARM Cortex-M0+), which is little-endian.
 *
 * Upstream pico-smsplus passes -DLSB_FIRST=0, but its source tests this with
 * #ifdef LSB_FIRST rather than #if LSB_FIRST.  When the SMSPlus sources were
 * flattened into PicoBoy it became easy for that per-target CMake definition
 * to be lost.  A missing definition reverses the PAIR register layout and can
 * make the Z80 fault on its very first instruction fetch.
 *
 * Make the vendored core self-contained: LSB_FIRST must exist here.
 */
#ifndef LSB_FIRST
#define LSB_FIRST 1
#endif

typedef unsigned char  UINT8;
typedef unsigned short UINT16;
typedef unsigned int   UINT32;

typedef signed char  INT8;
typedef signed short INT16;
typedef signed int   INT32;

typedef union {
#ifdef LSB_FIRST
    struct {
        UINT8 l, h, h2, h3;
    } b;
    struct {
        UINT16 l, h;
    } w;
#else
    struct {
        UINT8 h3, h2, h, l;
    } b;
    struct {
        UINT16 h, l;
    } w;
#endif
    UINT32 d;
} PAIR;

#endif /* OSD_CPU_H */