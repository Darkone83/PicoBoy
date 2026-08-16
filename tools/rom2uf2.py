#!/usr/bin/env python3
"""rom2uf2.py - package a Game Boy ROM into a UF2 that drops it into PicoBoy's
ROM staging window (flash offset 0x80000 = XIP 0x10080000).

This is the phase-2 ROM path: drag picoboy.uf2 (the firmware) and the ROM .uf2
onto the RPI-RP2 drive separately -- they target different flash regions, so
neither disturbs the other. The firmware then reads the ROM straight from XIP at
0x10080000 ("Load last game"). It stands in for the eventual SD -> flash staging
(phase 3), which lands the ROM at the same fixed address.

The --offset MUST match ROM_FLASH_OFFSET in include/flash.h (0x80000), and must
sit above the program's size so they don't overlap (program is ~200 KB today).

Usage:
    python rom2uf2.py game.gb [out.uf2] [--offset 0x80000] [--family rp2040] [--flash-size 2M]
"""
import argparse
import os
import struct
import sys

UF2_MAGIC0      = 0x0A324655
UF2_MAGIC1      = 0x9E5D5157
UF2_MAGIC_END   = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
FLASH_BASE      = 0x10000000
PAYLOAD         = 256
NVS_SECTOR      = 4096   # top sector reserved for settings (see flash.h)

FAMILY = {
    "rp2040":        0xE48BFF56,
    "rp2350-arm-s":  0xE48BFF59,
    "rp2350-riscv":  0xE48BFF5A,
    "rp2350-arm-ns": 0xE48BFF5B,
}


def parse_size(s):
    s = s.strip().lower()
    if s.endswith("m"):
        return int(s[:-1]) * 1024 * 1024
    if s.endswith("k"):
        return int(s[:-1]) * 1024
    return int(s, 0)


def main():
    ap = argparse.ArgumentParser(description="Package a GB ROM as a PicoBoy UF2.")
    ap.add_argument("rom")
    ap.add_argument("out", nargs="?", help="output .uf2 (default: <rom>_rom.uf2)")
    ap.add_argument("--offset", default="0x80000", help="flash offset (default 0x80000)")
    ap.add_argument("--family", default="rp2040", choices=sorted(FAMILY))
    ap.add_argument("--flash-size", default="2M",
                    help="target flash size for the overflow check (default 2M)")
    args = ap.parse_args()

    offset = int(args.offset, 0)
    family = FAMILY[args.family]
    base   = FLASH_BASE + offset
    out    = args.out or (os.path.splitext(args.rom)[0] + "_rom.uf2")

    with open(args.rom, "rb") as f:
        rom = f.read()
    if not rom:
        sys.exit("error: empty ROM")

    # GB ROMs are always a multiple of 32 KB.
    if len(rom) % (32 * 1024):
        print(f"warning: ROM size 0x{len(rom):X} is not a 32 KB multiple - packing anyway",
              file=sys.stderr)

    # Does it fit between the ROM offset and the NVS sector at the top of flash?
    flash  = parse_size(args.flash_size)
    window = (flash - NVS_SECTOR) - offset
    if len(rom) > window:
        print(f"warning: ROM is 0x{len(rom):X} bytes but the window on a {args.flash_size} part "
              f"is only 0x{window:X}; it would overrun the NVS sector. Use a larger --flash-size "
              f"(4 MB board) or a smaller ROM.", file=sys.stderr)

    title = "?"
    if len(rom) > 0x143:
        title = rom[0x134:0x143].split(b"\x00")[0].decode("ascii", "replace").strip()
    print(f"ROM '{title}'  {len(rom)} bytes -> {out}")
    print(f"target 0x{base:08X}  (flash offset 0x{offset:X}, family {args.family})")

    pad  = (-len(rom)) % PAYLOAD
    data = rom + b"\xff" * pad            # pad the last page with 0xFF (flash-erased value)
    nblk = len(data) // PAYLOAD

    with open(out, "wb") as f:
        for i in range(nblk):
            chunk = data[i * PAYLOAD:(i + 1) * PAYLOAD]
            block  = struct.pack("<IIIIIIII",
                                 UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY,
                                 base + i * PAYLOAD, PAYLOAD, i, nblk, family)
            block += chunk + b"\x00" * (476 - PAYLOAD)
            block += struct.pack("<I", UF2_MAGIC_END)
            assert len(block) == 512
            f.write(block)

    print(f"wrote {out}: {nblk} blocks ({nblk * 512} bytes)")


if __name__ == "__main__":
    main()