# PicoBoy

<div align=center>
   <img src="https://github.com/Darkone83/PicoBoy/blob/main/images/Darkone83.png" width=400><img src="https://github.com/Darkone83/PicoBoy/blob/main/images/Picoboy.jpg" width=400>
</div>

A tiny handheld that plays **Game Boy**, **NES**, and **Atari 2600** games, built around a bare
RP2040. Load ROMs from a microSD card, keep your saves, pick your palette, and
carry it in a pocket.

**Questions, help, or just want to show off your build? <a href="https://discord.gg/k2BQhSJ"><img src="https://github.com/Darkone83/PicoBoy/blob/main/images/discord.svg"></a>.**


---

<div align=center>
   <img src="https://github.com/Darkone83/PicoBoy/blob/main/images/PicoBoy_front.jpg"><img src="https://github.com/Darkone83/PicoBoy/blob/main/images/PicoBoy_back.jpg">
</div>

## Features

- **Game Boy (DMG), NES, and Atari 2600** games, loaded from microSD
- **Battery saves** and **save states** for supported Game Boy / NES titles
- **In-game menu** — pause, brightness, volume, frame-skip, system-specific options, quit
- **Atari 2600 NTSC emulation** — `.a26` / `.bin`, 2K, 4K, F8, F6, F4, and Superchip variants
- **Selectable colour palettes** for Game Boy games
- **On-screen battery meter** with charging, full, and low-battery states
- **Themeable UI**
- **Built-in diagnostics** for the screen, buttons, LED, audio, and SD card

---

## What you'll need (not included)

The board doesn't come with a screen, battery, or speaker — grab these
separately. Any of the common modules below will work.

| Part | What to get | AliExpress | Amazon |
|------|-------------|------------|--------|
| **Screen** | ST7789 SPI LCD, 2.0", 320×240 | <a href="https://www.aliexpress.us/item/3256808368541153.html?spm=a2g0o.productlist.main.6.40acT8G7T8G7jS&algo_pvid=effa9ddf-9ffd-4e0b-b128-f5c414eab6c4&algo_exp_id=effa9ddf-9ffd-4e0b-b128-f5c414eab6c4-5&pdp_ext_f=%7B%22order%22%3A%221745%22%2C%22eval%22%3A%221%22%2C%22fromPage%22%3A%22search%22%7D&pdp_npi=6%40dis%21USD%215.77%215.77%21%21%215.77%215.77%21%402101ca9517868995956817004e0ecc%2112000056541994979%21sea%21US%21196794698%21X%211%210%21n_tag%3A-29919%3Bd%3A6d39815e%3Bm03_new_user%3A-29895&curPageLogUid=vtQ1EQVk15Ub&utparam-url=scene%3Asearch%7Cquery_from%3A%7Cx_object_id%3A1005008554855905%7C_p_origin_prod%3A">Buy Here!</a> | <a href="https://www.amazon.com/module-240x320-interface-drive-ST7789/dp/B0BZMLPXSH/ref=sr_1_6?crid=AYGJ3EFODFZE&dib=eyJ2IjoiMSJ9.h5gWizoOa3RElA6YM95PEZKRv2hzN1FFDYwkGI4GawxhgSixxkdxXPlTxfufIzrpt5M9aOzoif88adedhHxpBieVo0e7odDRlmAMPCTnxiO8hQLpFsUjH6UPoZ6ktkpazuM_GoVvvogFVRWGTWetdnTKvVwX5lBbf6cg-b3m8_K2xDLEZqnYAW8VgTGsI-WBL9n8kwqym-4XE3tAbK8Pa0MpzpIuV8SUQ3Mfz1ICHM4.4P2PqaTreuIGa0Mw0SyJUktbVJFria4pLNG0H3yol0E&dib_tag=se&keywords=2.4+inch+spi+ips+lcd&qid=1786899769&sprefix=2.4+inch+spi+ips+lc%2Caps%2C268&sr=8-6">Buy Here!</a> |
| **Battery** | Single-cell 3.7 V Li-ion / LiPo (e.g. 1200 mAh) with JST-PH | <a href="https://www.aliexpress.us/item/3256808498412741.html?spm=a2g0o.productlist.main.7.40d02XmM2XmM7Y&algo_pvid=0c918750-6933-4f1b-b45f-28968c4aea59&algo_exp_id=0c918750-6933-4f1b-b45f-28968c4aea59-6&pdp_ext_f=%7B%22order%22%3A%22276%22%2C%22eval%22%3A%221%22%2C%22fromPage%22%3A%22search%22%7D&pdp_npi=6%40dis%21USD%217.00%214.90%21%21%2146.97%2132.88%21%4021030cd817868996982394651e0df5%2112000046231812291%21sea%21US%21196794698%21X%211%210%21n_tag%3A-29919%3Bd%3A6d39815e%3Bm03_new_user%3A-29895&curPageLogUid=alfVtxiirrF3&utparam-url=scene%3Asearch%7Cquery_from%3A%7Cx_object_id%3A1005008684727493%7C_p_origin_prod%3A">Buy Here!</a> | <a href="https://www.amazon.com/1200mAh-Battery-Rechargeable-Lithium-Connector/dp/B09FLG39NX/ref=sr_1_5?crid=ZG4BANVNAOVM&dib=eyJ2IjoiMSJ9.0PAzXmMDBpw0zzNJZ7VkLGOxafB-EF6I1LImTLS1nLRph_P-jC9dgnxcCBn1lWDMj56-pLOtHbWIRurZIZaBRKsFAV-rNny27UYdptyky73X-COeWuT9t0gtH3zWJca2-74uIuUeQArp_ZLlrG2Fg9QVZ1ls-ZbMwi5TS4N7K20PyeHW6d44fiHjAU2f597ih52ZmvUxubwsn8jEddd3R2dTOlAtfYYuGqXIn9Tpn9J_W7Q7gssAYwUKViJFZL2YbDXjbbzf3n6_bmkS_8eMt1P60zIJ5leU-L82KSQhPs0.z6MkunAoTNoSLUh3E2QknWANfAWxKYUvWHwtsYYhmxc&dib_tag=se&keywords=lipo+battery+3.7+1200mah+703450&qid=1786899919&sprefix=lipo+battery+3.7+1200mah+703450%2Caps%2C226&sr=8-5">Buy Here!</a> |
| **Speaker** | 8 Ω speaker, small (1–2 W) | <a href="https://www.aliexpress.us/item/3256805513376202.html?spm=a2g0o.order_list.order_list_main.322.18c31802aJjwYg&gatewayAdapt=glo2usa">Buy Here!</a> | <a href="https://www.amazon.com/Speaker-Ohm-Full-Range-Accessories/dp/B0FM3QN15Q/ref=sr_1_22?crid=21E20I3CFB1JB&dib=eyJ2IjoiMSJ9.ZtJ54-Kti2E_3a1P9YcNzSPfGc8bZiJRdrYypVdc2ribpZhIhAVCUW9r-yNUnWD9kRrC0hrbc6mdv8BtcBfEKr3DZRDaFN1bpsjM_013vRl1QNn9RHIBg4CYv2OIqjvm22q7mX2YnNM633f9bNVj7SMsldVWeatCpalrN8-DAAeE2TgYhOJUPFnXy9647x2GPVSD-wUhE586EMXFU3sHwA.w8_psATD_XKFHbJiDA4prctu-YlYQut1yE9riJio7i0&dib_tag=se&keywords=speaker+project&qid=1786899968&sprefix=speaker+project%2Caps%2C252&sr=8-22&xpid=Rr12e7JkuMOdV">Buy Here!</a> |

> Everything else (RP2040, amplifier, charger, LED, buttons) is on the board —
> see [`/pcb`](pcb) for gerbers, the bill of materials, and pick-and-place files.

Enclosure STL files will live in `/stl` (coming soon).

---

## Flashing the firmware

1. Grab `picoboy.uf2` from the [`/firmware`](firmware) folder.
2. Hold the **BOOTSEL** button while plugging PicoBoy into USB — it shows up as a
   drive called **RPI-RP2**.
3. Drag `picoboy.uf2` onto that drive. It reboots into PicoBoy automatically.

That's it. To update later, just repeat with a newer `picoboy.uf2`.

---

## SD card setup

Format a microSD card as **FAT32**, then drop your ROMs into the matching folder.
PicoBoy **creates all of these folders for you** on first boot with a card
inserted, so you can also just boot once and then copy ROMs in. If you'd rather
lay it out yourself first, here's the structure:

```
SD card root
├── roms/
│   ├── gb/     <- Game Boy ROMs  (.gb)
│   ├── nes/    <- NES ROMs       (.nes)
│   ├── 2600/   <- Atari 2600     (.a26, .bin)
│   └── fc/     <- Famicom        (.nes, not playable yet)
├── save/       <- battery saves  (created automatically)
│   ├── gb/
│   └── nes/
└── state/      <- save states    (created automatically)
    └── gb/
```

You only ever need to touch `roms/gb`, `roms/nes`, and `roms/2600` — saves and states are
written for you where supported. (ROMs are not included; use games you legally own.)

---

## Using PicoBoy

### Controls

| Button | In menus | In a game |
|--------|----------|-----------|
| **D-pad** | Move / adjust | Game input |
| **A** | Open / confirm | Game input |
| **B** | Back / cancel | Game input |
| **START / SELECT** | — | Game input |
| **MENU** | — | Open the in-game menu |

### Main menu

- **Browse ROMs** — pick a system, then choose a game from the SD card.
- **Load last game** — jump straight back into the last ROM you played.
- **Settings** — see below.

### Settings

- **LCD Bright** — screen backlight level.
- **LED Bright** — status LED brightness.
- **Volume** — audio level.
- **Theme** — UI colour theme.
- **Flash Ops** — *Clear ROM* (wipe the staged ROM) and *Reset Settings*
  (restore defaults).
- **Diagnostics** — quick hardware tests (display, buttons, RGB LED, test tone,
  SD probe).

### In-game menu

Press **MENU** while playing to pause and open the overlay: resume, brightness,
volume, palette, frame-skip, save state, load state, and quit back to the menu.

---

## LED status reference

PicoBoy has a single RGB status LED. The **colour** tells you the category, and
**steady vs. blinking** tells you resting vs. busy.

| Colour | Pattern | Meaning |
|--------|---------|---------|
| Purple | Solid | Booting |
| White | Solid | At rest (menus, idle) |
| Magenta | Solid | Game running |
| Blue | Blinking | Loading / writing in progress |
| Blue | Solid | Write finished |
| Cyan | Counted blinks | Flash maintenance (Clear ROM / Reset Settings) |
| Pink | Counted blinks | SD card activity |
| Red | Counted blinks | Error (the blink count is the code) |
| Green | Solid | Battery full |
| Orange | Solid | Charging |
| Amber | Slow blink | Battery low |

### Error codes (red blinking LED)
 
When something goes wrong the status LED blinks **red** in a repeating group —
**count the blinks** to identify the problem — and the screen shows the same
reason in words. Press **B** to back out.
 
| Blinks | On-screen message | What it means | What to do |
|:------:|-------------------|---------------|------------|
| **2** | `Flash a ROM .uf2 first` | *Load last game* was chosen but no Game Boy ROM is staged in flash | Use **Browse ROMs** from an SD card, or bake one in with `rom2uf2` (below) |
| **3** | `ROM load failed` | A game couldn't be read or loaded from the SD card | Check it's a valid ROM in the right `roms/` folder, on a seated FAT32 card |
| **4** | `Out of RAM (fb)` | Not enough memory to set up the frame buffers (rare) | Restart; report it if it keeps happening |

### Error messages

When something goes wrong the status LED blinks **red** and the screen shows the
reason. Press **B** to back out. The common ones:

| On-screen message | What it means | What to do |
|-------------------|---------------|------------|
| `Flash a ROM .uf2 first` | *Load last game* was chosen but no Game Boy ROM is staged in flash | Use **Browse ROMs** from an SD card, or bake one in with `rom2uf2` (below) |
| `ROM load failed` | A NES ROM couldn't be read from the SD card | Check it's a valid `.nes` in `roms/nes` and the card is seated and FAT32 |
| `Out of RAM (fb)` | Not enough memory to set up the NES frame buffers (rare) | Restart; report it if it keeps happening |

---

## Adding a Game Boy ROM without an SD card

For quick testing, you can bake a single **Game Boy** ROM into its own `.uf2` and
drop it onto PicoBoy next to the firmware — no SD card needed.

```
python tools/rom2uf2.py game.gb
```

That produces `game_rom.uf2`. Flash it the same way as the firmware (hold
**BOOTSEL**, drag it onto **RPI-RP2**), then choose **Load last game**. The ROM
lands in a separate area of flash from the firmware, so the two don't interfere.

> This path is **Game Boy only**. NES and Atari 2600 games load from the SD card.

---

## Repository layout

```
/src        Main project source
/firmware   Precompiled UF2
/images     Project images
/tools      Tools, scripts (rom2uf2.py) and helpers
/pcb        Gerbers, BOM, and pick-and-place
/stl        Enclosure files (coming soon)
```

---

## Credits

PicoBoy stands on the shoulders of these projects — please keep these
attributions intact if you fork or redistribute.

- **[Peanut-GB](https://github.com/deltabeard/Peanut-GB)** and
  **[minigb_apu](https://github.com/deltabeard/minigb_apu)** — Game Boy emulation
  and audio, by deltabeard (Mahyar Koshkouei). **MIT License**. (Peanut-GB also
  incorporates palette data from the SameBoy project, MIT.)
- **[InfoNES](https://github.com/jay-kumogata/InfoNES)** — NES emulation,
  originally by Jay Kumogata. **Freeware**.
- **[pico-infones](https://github.com/shuichitakano/pico-infones)** — Raspberry Pi
  Pico port by Shuichi Takano. **GPL-3.0**.
- **[pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus)** —
  RP2040/RP2350 InfoNES work by Frank Hoedemakers used as part of PicoBoy's NES
  port lineage. **GPL-3.0**.
- **[pico-atari2600](https://github.com/xrip/pico-atari2600)** — Atari 2600
  Raspberry Pi Pico emulator by Ilya Maslennikov. **MIT License**.
- **[HiFive1-2600](https://github.com/dgrubb/HiFive1-2600)** — original embedded
  6507 / TIA / RIOT emulator by David Grubb that pico-atari2600 was based on.
- **[Stella](https://github.com/stella-emu/stella)** — Atari 2600 accuracy and
  timing reference used while validating PicoBoy's adapted TIA/audio behavior.
  Stella code is **not embedded in PicoBoy**; Stella is GPL-2.0-or-later.
- **[FatFs](http://elm-chan.org/fsw/ff/)** — SD card filesystem, by ChaN.
  BSD-style license.
- **I²S audio driver** — MAX98357 output, by Vincent Mistler (MIT), with the PIO
  program based on Raspberry Pi's `pico-extras` audio_i2s (BSD-3-Clause).
- **[Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)** — Raspberry
  Pi Ltd. BSD-3-Clause. (WS2812 LED PIO adapted from `pico-examples`.)

Each third-party component retains its own license; see the respective project
for full terms. PicoBoy's own source is distributed under GPL-3.0.

---

## License

PicoBoy is released under the **GNU General Public License v3.0**. See
[`LICENSE`](LICENSE) for the full text.

---

## Community

Come hang out, get help, or share your build on <a href="https://discord.gg/k2BQhSJ"><img src="https://github.com/Darkone83/PicoBoy/blob/main/images/discord.svg"></a>.

## Contributing

Contributions are welcome — see [`CONTRIBUTING.md`](CONTRIBUTING.MD) to get
started.
