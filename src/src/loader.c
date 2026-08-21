#include "loader.h"
#include "st7789.h"
#include "buttons.h"
#include "ui.h"
#include "pins.h"
#include "theme.h"
#include "led.h"
#include "flash.h"
#include "sd.h"
#include "gb_core.h"
#include "nes_core.h"
#include "atari_core.h"
#include "sms_core.h"
#include "arena.h"
#include "pico/stdlib.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---- systems -------------------------------------------------------------
// Folder + extension per system. ext2 is an optional second accepted extension.
// NULL ext2 = single extension.
typedef struct {
    const char *name;
    const char *dir;
    const char *ext;
    const char *ext2;
    bool runnable;
} sys_t;

static const sys_t SYS[] = {
    { "Game Boy",      "gb",   ".gb",  NULL,   true  },
    { "NES",           "nes",  ".nes", NULL,   true  },
    { "Master System", "sms",  ".sms", NULL,   true  },
    { "Game Gear",     "gg",   ".gg",  NULL,   true  },
    { "Atari 2600",    "2600", ".a26", ".bin", true  },
    { "Famicom",       "fc",   ".nes", NULL,   true  },   // Japanese carts, runs on the NES core
};
#define NSYS ((int)(sizeof(SYS) / sizeof(SYS[0])))

// ---- list buffer ---------------------------------------------------------
#define MAX_ENTRIES 128
#define MAX_NAME    80       // ROM filename (LFN); longer names are truncated

/*
 * 128 x 80 = 10,240 bytes. The browser only exists while no emulator is hot,
 * so borrow the bottom of the shared arena instead of reserving this forever.
 * A selected filename is copied to the stack before flash_from_sd() reuses the
 * same arena for its 4 KiB sector staging buffer.
 */
static char (*s_names)[MAX_NAME] = NULL;

static inline void names_bind_arena(void) {
    s_names = (char (*)[MAX_NAME])arena_base();
}

static FATFS s_fs;

// One-sector ROM staging buffer. It lives in the shared emulator arena rather
// than its own 4 KB of BSS: flashing only runs from the menu, before any core
// launches, so the arena is free at that point (the core re-inits it on run).
static uint8_t *s_secbuf = 0;
static uint32_t s_last_flash_size = 0; // size of the most recently flashed ROM

// ---- small UI helpers ----------------------------------------------------
#define ROWS_VISIBLE 10
#define ROW_Y0       40
#define ROW_H        18

// Marquee (selected long name): window after the "> " prefix, inside the pill.
#define MQ_X0      30
#define MQ_X1      304
#define MQ_EVERY   2
#define MQ_STEP    1
#define MQ_GAP     48
#define MQ_HOLD    20
#define MQ_MAXLEN  34

static void status(const char *msg) {
    st7789_fill(COL_BLACK);
    ui_header("Browse ROMs");
    st7789_draw_string(12, 100, msg, COL_WHITE, COL_BLACK, 1);
    ui_footer("");
}

static void show_msg(const char *title, const char *msg, uint16_t col) {
    st7789_fill(COL_BLACK);
    ui_header(title);
    st7789_draw_string(12, 100, msg, col, COL_BLACK, 1);
    ui_footer("B back");
    led_set_state(LED_IDLE);
    while (true) {
        buttons_update();
        if (buttons_pressed() & (1u << BTN_B)) return;
        sleep_ms(15);
    }
}

// Scrollable picker over s_names[0..count). Returns index, or -1 on B.
static int pick_list(const char *title, int count, const char *footer) {
    int sel = 0, top = 0;
    bool redraw = true;

    // Marquee state for the selected long name.
    bool mq_on = false;
    int mq_y = 0, mq_off = 0, mq_hold = 0, mq_tick = 0;
    const char *mq_name = 0;
    uint16_t mq_fg = 0, mq_bg = 0;

    while (true) {
        if (redraw) {
            st7789_fill(g_theme->bg);

            char cnt[16];
            snprintf(cnt, sizeof cnt, "%d/%d", count ? sel + 1 : 0, count);
            ui_header_right(title, cnt);

            mq_on = false;

            for (int r = 0; r < ROWS_VISIBLE && (top + r) < count; r++) {
                int i = top + r;
                bool on = (i == sel);
                int y = ROW_Y0 + r * ROW_H;
                uint16_t fg = on ? g_theme->sel_fg : g_theme->item_fg;
                uint16_t bg = on ? g_theme->sel_bg : g_theme->bg;

                if (on)
                    ui_fill_pill(8, y - 3, LCD_W - 22, ROW_H - 2, bg);

                bool longname = (int)strlen(s_names[i]) > MQ_MAXLEN;

                if (on && longname) {
                    // Selected + overflowing: fixed ">" prefix, name marquees.
                    st7789_draw_string(14, y, ">", fg, bg, 1);
                    st7789_marquee(MQ_X0, MQ_X1, y, s_names[i],
                                   0, fg, bg, MQ_GAP);
                    mq_on = true;
                    mq_y = y;
                    mq_name = s_names[i];
                    mq_fg = fg;
                    mq_bg = bg;
                    mq_off = 0;
                    mq_hold = MQ_HOLD;
                    mq_tick = 0;
                } else {
                    char line[44];
                    if (longname)
                        snprintf(line, sizeof line, "%c %.31s...",
                                 on ? '>' : ' ', s_names[i]);
                    else
                        snprintf(line, sizeof line, "%c %s",
                                 on ? '>' : ' ', s_names[i]);

                    st7789_draw_string(14, y, line, fg, bg, 1);
                }
            }

            // Scrollbar, only when the list overflows the window.
            if (count > ROWS_VISIBLE) {
                int tx = LCD_W - 6;
                int ty = ROW_Y0 - 2;
                int th = ROWS_VISIBLE * ROW_H;

                st7789_fill_rect(tx, ty, 4, th, g_theme->footer_bg);

                int thumb = th * ROWS_VISIBLE / count;
                if (thumb < 6) thumb = 6;

                int maxtop = count - ROWS_VISIBLE;
                int thy = ty + (th - thumb) * top / (maxtop ? maxtop : 1);

                st7789_fill_rect(tx, thy, 4, thumb, g_theme->accent);
            }

            ui_footer(footer);
            redraw = false;
        }

        // Animate the selected long name: hold briefly, then scroll seamlessly.
        if (mq_on && ++mq_tick >= MQ_EVERY) {
            mq_tick = 0;
            if (mq_hold > 0)
                mq_hold--;
            else
                mq_off += MQ_STEP;

            st7789_marquee(MQ_X0, MQ_X1, mq_y, mq_name,
                           mq_off, mq_fg, mq_bg, MQ_GAP);
        }

        buttons_update();
        uint16_t ev = buttons_pressed();

        if (ev & (1u << BTN_B)) return -1;
        if (ev & (1u << BTN_A)) return sel;

        if (ev & (1u << BTN_UP) && sel > 0) {
            sel--;
            if (sel < top) top = sel;
            redraw = true;
        }

        if (ev & (1u << BTN_DOWN) && sel < count - 1) {
            sel++;
            if (sel >= top + ROWS_VISIBLE)
                top = sel - ROWS_VISIBLE + 1;
            redraw = true;
        }

        // LEFT/RIGHT page by a full screen (only when the list overflows).
        if (count > ROWS_VISIBLE) {
            if ((ev & (1u << BTN_RIGHT)) && sel < count - 1) {
                sel += ROWS_VISIBLE;
                if (sel > count - 1) sel = count - 1;
                if (sel >= top + ROWS_VISIBLE)
                    top = sel - ROWS_VISIBLE + 1;
                redraw = true;
            }

            if ((ev & (1u << BTN_LEFT)) && sel > 0) {
                sel -= ROWS_VISIBLE;
                if (sel < 0) sel = 0;
                if (sel < top) top = sel;
                redraw = true;
            }
        }

        sleep_ms(15);
    }
}

// ---- fs helpers ----------------------------------------------------------
static bool has_ext(const char *name, const char *ext) {
    size_t ln = strlen(name), le = strlen(ext);
    if (ln < le) return false;

    const char *s = name + (ln - le);
    for (size_t i = 0; i < le; i++) {
        char a = s[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

// Copy `romfile` minus its extension into out[n].
static void strip_ext(char *out, size_t n, const char *romfile) {
    snprintf(out, n, "%s", romfile);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

// Remember the last launched game (sys + base name) so "Load last game" can
// restore both the correct emulator and the staged ROM size. Atari needs the
// staged size because its cartridge detector works from the raw image length.
static void persist_last(const char *sysdir, const char *base) {
    FIL f;
    if (f_open(&f, "/lastrom.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        // "<sys>/<base>\n<flash size>\n<ROM flash offset>"
        // Recording the dynamic base invalidates stale staged images after a
        // firmware update changes __flash_binary_end.
        char line[MAX_NAME + 40];
        int len = snprintf(line, sizeof line, "%s/%s\n%lu\n%lu",
                           sysdir, base,
                           (unsigned long)s_last_flash_size,
                           (unsigned long)rom_flash_offset());
        UINT bw = 0;
        f_write(&f, line, (UINT)len, &bw);
        f_close(&f);
    }
}

static void ensure_dirs(void) {
    f_mkdir("/roms");
    f_mkdir("/save");
    f_mkdir("/state"); // FR_EXIST is fine

    char p[32];
    for (int i = 0; i < NSYS; i++) {
        snprintf(p, sizeof p, "/roms/%s", SYS[i].dir);
        f_mkdir(p);

        snprintf(p, sizeof p, "/save/%s", SYS[i].dir);
        f_mkdir(p);

        snprintf(p, sizeof p, "/state/%s", SYS[i].dir);
        f_mkdir(p);
    }
}

// Case-insensitive filename compare for alphabetical sorting.
static int name_cmp(const void *a, const void *b) {
    const char *x = (const char *)a;
    const char *y = (const char *)b;

    for (;; x++, y++) {
        char cx = *x, cy = *y;

        if (cx >= 'A' && cx <= 'Z') cx += 32;
        if (cy >= 'A' && cy <= 'Z') cy += 32;

        if (cx != cy)
            return (unsigned char)cx - (unsigned char)cy;

        if (!cx)
            return 0;
    }
}

// Fill s_names with files in `path` matching `ext` (or `ext2` if non-NULL).
// Returns count (>=0), -1 on error.
static int scan_dir(const char *path, const char *ext, const char *ext2) {
    DIR dir;
    FILINFO fno;

    if (f_opendir(&dir, path) != FR_OK)
        return -1;

    int n = 0;

    while (n < MAX_ENTRIES &&
           f_readdir(&dir, &fno) == FR_OK &&
           fno.fname[0]) {
        if (fno.fattrib & AM_DIR)
            continue;

        if (!has_ext(fno.fname, ext) &&
            !(ext2 && has_ext(fno.fname, ext2)))
            continue;

        strncpy(s_names[n], fno.fname, MAX_NAME - 1);
        s_names[n][MAX_NAME - 1] = '\0';
        n++;
    }

    f_closedir(&dir);

    if (n > 1)
        qsort(s_names, n, MAX_NAME, name_cmp);

    return n;
}

// Stream a ROM file into the flash window, erasing + programming a sector at a
// time. Runs with core1 idle (we're in the menu), so the single-core flash ops
// are safe. Returns true on success.
static bool flash_from_sd(const char *path) {
    FIL fil;

    if (f_open(&fil, path, FA_READ) != FR_OK) {
        show_msg("ROM Loader", "Open failed", COL_RED);
        return false;
    }

    uint32_t size = (uint32_t)f_size(&fil);
    const uint32_t fw_end   = flash_firmware_end_offset();
    const uint32_t rom_base = rom_flash_offset();
    const uint32_t window   = rom_flash_capacity();

    // Flash is erased in complete 4 KiB sectors, so validate the rounded write,
    // not just the raw file length.
    uint32_t staged_bytes = 0;
    if (size != 0 && size <= UINT32_MAX - (FLASH_SECTOR_SIZE - 1u))
        staged_bytes = (size + FLASH_SECTOR_SIZE - 1u) &
                       ~(uint32_t)(FLASH_SECTOR_SIZE - 1u);

    printf("[FLASH] total=%lu fw_end=0x%06lx rom_base=0x%06lx "
           "nvs=0x%06lx capacity=%lu rom=%lu staged=%lu\n",
           (unsigned long)PICO_FLASH_SIZE_BYTES,
           (unsigned long)fw_end,
           (unsigned long)rom_base,
           (unsigned long)NVS_FLASH_OFFSET,
           (unsigned long)window,
           (unsigned long)size,
           (unsigned long)staged_bytes);

    if (size == 0 || staged_bytes == 0 ||
        rom_base >= NVS_FLASH_OFFSET ||
        staged_bytes > window) {
        f_close(&fil);

        if (size == 0) {
            show_msg("ROM Loader", "Empty file", COL_RED);
        } else if (rom_base >= NVS_FLASH_OFFSET) {
            show_msg("ROM Loader", "No safe ROM flash window", COL_RED);
        } else {
            char msg[48];
            snprintf(msg, sizeof msg, "ROM too big (%luK free)",
                     (unsigned long)(window / 1024u));
            show_msg("ROM Loader", msg, COL_RED);
        }
        return false;
    }

    led_set_state(LED_FLASH_BUSY);

    // Borrow the arena for sector staging. No core is running here.
    s_secbuf = arena_base();

    st7789_fill(g_theme->bg);
    ui_header("Loading ROM");
    ui_footer("");

    uint32_t total = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
    uint32_t done = 0;

    ui_progress_pacman(0, (int)total);

    bool ok = true;
    bool verify_failed = false;

    for (uint32_t off = 0; off < size; off += FLASH_SECTOR_SIZE) {
        UINT br = 0;
        uint32_t remain = size - off;
        UINT want = (UINT)(remain < FLASH_SECTOR_SIZE ? remain : FLASH_SECTOR_SIZE);

        if (f_read(&fil, s_secbuf, want, &br) != FR_OK || br != want) {
            ok = false;
            break;
        }

        if (want < FLASH_SECTOR_SIZE)
            memset(s_secbuf + want, 0xFF, FLASH_SECTOR_SIZE - want);

        uint32_t dst = rom_base + off;

        // Fail closed before *every* erase/program operation. This protects both
        // the linked firmware below rom_base and the NVS sector above the window.
        if (!rom_flash_range_valid(dst, FLASH_SECTOR_SIZE)) {
            printf("[FLASH] REFUSED sector dst=0x%06lx len=%u\n",
                   (unsigned long)dst, (unsigned)FLASH_SECTOR_SIZE);
            verify_failed = true;
            break;
        }

        bool verified = false;

        // Program one complete sector, verify the XIP view, and retry once
        // before reporting a flash failure.
        for (int attempt = 0; attempt < 2; attempt++) {
            flash_erase_sector(dst);
            flash_program_sector(dst, s_secbuf);

            if (memcmp(flash_ptr(dst), s_secbuf, FLASH_SECTOR_SIZE) == 0) {
                verified = true;
                break;
            }
        }

        if (!verified) {
            verify_failed = true;
            break;
        }

        ui_progress_pacman((int)(++done), (int)total);
    }

    f_close(&fil);

    if (!ok) {
        show_msg("ROM Loader", "SD read error", COL_RED);
        return false;
    }

    if (verify_failed) {
        show_msg("ROM Loader", "Flash bounds/verify failed", COL_RED);
        return false;
    }

    // Only publish the staged size after every sector has verified.
    s_last_flash_size = size;
    return true;
}

// ---- public flow ---------------------------------------------------------
void loader_browse(void) {
    names_bind_arena();
    status("Mounting SD...");

    if (f_mount(&s_fs, "", 1) != FR_OK) {
        show_msg("Browse ROMs", "SD mount failed", COL_RED);
        return;
    }

    // Creates /roms/2600 along with the existing system directories.
    ensure_dirs();

    while (true) {
        // System picker.
        for (int i = 0; i < NSYS; i++)
            snprintf(s_names[i], MAX_NAME, "%s", SYS[i].name);

        int s = pick_list("Browse ROMs", NSYS, "A open   B back");

        if (s < 0)
            return;

        ui_transition(UI_TRANSITION_FORWARD);

        // ROM picker for the chosen system.
        char path[48];
        snprintf(path, sizeof path, "/roms/%s", SYS[s].dir);

        int n = scan_dir(path, SYS[s].ext, SYS[s].ext2);

        if (n <= 0) {
            char m[40];
            snprintf(m, sizeof m, "Put %s files in /roms/%s", SYS[s].ext, SYS[s].dir);
            show_msg(SYS[s].name, m, COL_GRAY);
            ui_transition(UI_TRANSITION_BACK);
            continue;
        }

        // When a folder holds more ROMs than the list buffer, the picker shows the
        // first MAX_ENTRIES (alphabetical); flag that so it isn't silently truncated.
        const char *ftr;
        if (!SYS[s].runnable)         ftr = "L/R page  B back";
        else if (n == MAX_ENTRIES)    ftr = "A load  L/R page  B back (full)";
        else                          ftr = "A load  L/R page  B back";

        int f = pick_list(SYS[s].name, n, ftr);

        if (f < 0) {
            ui_transition(UI_TRANSITION_BACK);
            continue;
        }

        if (!SYS[s].runnable) {
            show_msg(SYS[s].name, "Not yet supported", COL_PURPLE);
            ui_transition(UI_TRANSITION_BACK);
            continue;
        }

        // flash_from_sd() borrows arena_base() for sector staging, so preserve
        // the selected name before that overwrites the arena-backed list.
        char selected[MAX_NAME];
        strncpy(selected, s_names[f], sizeof selected - 1);
        selected[sizeof selected - 1] = '\0';

        char full[MAX_NAME + 48];
        snprintf(full, sizeof full, "/roms/%s/%s",
                 SYS[s].dir, selected);

        ui_transition(UI_TRANSITION_FORWARD);

        if (flash_from_sd(full)) {
            char base[MAX_NAME];
            strip_ext(base, sizeof base, selected);

            persist_last(SYS[s].dir, base);
            ui_set_now_playing(base);            // shown in the in-game pause overlay
            led_set_state(LED_RUNNING);

            if (strcmp(SYS[s].dir, "nes") == 0 || strcmp(SYS[s].dir, "fc") == 0) {
                // Famicom shares the NES core; paths just live under its own folder.
                char srm[96], dat[96];
                snprintf(srm, sizeof srm,
                         "/save/%s/%s.srm", SYS[s].dir, base);
                snprintf(dat, sizeof dat,
                         "/state/%s/%s.dat", SYS[s].dir, base);
                nes_set_save_path(srm);
                nes_set_state_path(dat);
                nes_run();

            } else if (strcmp(SYS[s].dir, "sms") == 0 || strcmp(SYS[s].dir, "gg") == 0) {
                // SMS and Game Gear share the SMSPlus core and the same staged
                // XIP ROM window. Keep saves/states separated by system folder.
                char srm[96], dat[96];
                snprintf(srm, sizeof srm,
                         "/save/%s/%s.srm", SYS[s].dir, base);
                snprintf(dat, sizeof dat,
                         "/state/%s/%s.dat", SYS[s].dir, base);
                sms_core_set_rom(s_last_flash_size, strcmp(SYS[s].dir, "gg") == 0);
                sms_core_set_save_path(srm);
                sms_core_set_state_path(dat);
                sms_core_run();

            } else if (strcmp(SYS[s].dir, "2600") == 0) {
                // Atari runs directly from the same staged XIP ROM window.
                // Its mapper detector needs the exact raw file size.
                atari_core_set_rom_size(s_last_flash_size);
                atari_core_run();

            } else {
                // Game Boy
                char srm[96], dat[96];

                snprintf(srm, sizeof srm,
                         "/save/%s/%s.srm", SYS[s].dir, base);
                snprintf(dat, sizeof dat,
                         "/state/%s/%s.dat", SYS[s].dir, base);

                gb_set_save_path(srm);
                gb_set_state_path(dat);
                gb_core_run();
            }
        }

        // After a game (or a failed load) fall back to the system picker.
        ui_transition(UI_TRANSITION_BACK);
    }
}

// "Load last game": run whatever is staged in the flash window. When the SD
// metadata is available, restore the correct emulator and the ROM size used by
// Atari cartridge detection.
void loader_launch_last(void) {
    enum {
        SYSK_GB = 0,
        SYSK_NES,
        SYSK_SMS,
        SYSK_ATARI
    };

    int sys_kind = SYSK_GB;
    bool last_is_gg = false;
    uint32_t staged_size = 0;
    uint32_t staged_offset = 0;
    bool have_last = false;   // set once we resolve a valid <sys>/<base> from /lastrom.txt

    gb_set_save_path("");
    gb_set_state_path("");
    nes_set_save_path("");
    nes_set_state_path("");
    sms_core_set_rom(0, false);
    sms_core_set_save_path("");
    sms_core_set_state_path("");
    ui_set_now_playing("");

    if (f_mount(&s_fs, "", 1) == FR_OK) {
        FIL f;

        if (f_open(&f, "/lastrom.txt", FA_READ) == FR_OK) {
            char line[MAX_NAME + 40];
            UINT br = 0;

            f_read(&f, line, sizeof line - 1, &br);
            f_close(&f);

            line[br] = '\0';

            // File:
            //   <sys>/<base>
            //   <staged flash size>
            //   <ROM flash offset used when staged>
            char *nl = strchr(line, '\n');

            if (nl) {
                *nl++ = '\0';

                // Tolerate CRLF or whitespace before the decimal size.
                while (*nl == '\r' || *nl == ' ' || *nl == '\t')
                    nl++;

                staged_size = (uint32_t)strtoul(nl, NULL, 10);

                char *nl2 = strchr(nl, '\n');
                if (nl2) {
                    nl2++;
                    while (*nl2 == '\r' || *nl2 == ' ' || *nl2 == '\t')
                        nl2++;
                    staged_offset = (uint32_t)strtoul(nl2, NULL, 10);
                }
            }

            // Trim trailing CR/space from the name line.
            int nlen = 0;
            while (line[nlen]) nlen++;

            while (nlen &&
                   (line[nlen - 1] == '\r' ||
                    line[nlen - 1] == ' ')) {
                line[--nlen] = '\0';
            }

            char *slash = strchr(line, '/');

            if (slash) {
                *slash = '\0';

                const char *sysdir = line;
                const char *base = slash + 1;

                ui_set_now_playing(base);

                // A firmware update can move the dynamic ROM window. Never boot
                // stale bytes from the old offset; require the metadata written
                // by the current staging scheme and verify its range.
                uint32_t rounded = 0;
                if (staged_size != 0 &&
                    staged_size <= UINT32_MAX - (FLASH_SECTOR_SIZE - 1u)) {
                    rounded = (staged_size + FLASH_SECTOR_SIZE - 1u) &
                              ~(uint32_t)(FLASH_SECTOR_SIZE - 1u);
                }

                have_last =
                    (staged_offset == rom_flash_offset()) &&
                    (rounded != 0) &&
                    (rounded <= rom_flash_capacity());

                if (!have_last) {
                    printf("[FLASH] last ROM stale/unsafe: saved_base=0x%06lx "
                           "current_base=0x%06lx size=%lu\n",
                           (unsigned long)staged_offset,
                           (unsigned long)rom_flash_offset(),
                           (unsigned long)staged_size);
                }

                if (strcmp(sysdir, "nes") == 0 || strcmp(sysdir, "fc") == 0) {
                    sys_kind = SYSK_NES;

                    char srm[96], dat[96];
                    snprintf(srm, sizeof srm,
                             "/save/%s/%s.srm", sysdir, base);
                    snprintf(dat, sizeof dat,
                             "/state/%s/%s.dat", sysdir, base);
                    nes_set_save_path(srm);
                    nes_set_state_path(dat);

                } else if (strcmp(sysdir, "sms") == 0 || strcmp(sysdir, "gg") == 0) {
                    sys_kind = SYSK_SMS;
                    last_is_gg = (strcmp(sysdir, "gg") == 0);

                    char srm[96], dat[96];
                    snprintf(srm, sizeof srm,
                             "/save/%s/%s.srm", sysdir, base);
                    snprintf(dat, sizeof dat,
                             "/state/%s/%s.dat", sysdir, base);
                    sms_core_set_save_path(srm);
                    sms_core_set_state_path(dat);

                } else if (strcmp(sysdir, "2600") == 0) {
                    sys_kind = SYSK_ATARI;

                } else {
                    sys_kind = SYSK_GB;

                    char srm[96], dat[96];
                    snprintf(srm, sizeof srm,
                             "/save/%s/%s.srm", sysdir, base);
                    snprintf(dat, sizeof dat,
                             "/state/%s/%s.dat", sysdir, base);

                    gb_set_save_path(srm);
                    gb_set_state_path(dat);
                }
            }
        }
    }

    if (!have_last) {
        // No /lastrom.txt yet (fresh device or card absent): point the player at
        // Browse ROMs instead of dropping into a core's "no ROM staged" error.
        show_msg("Load last game", "No recent game - Browse ROMs", COL_GRAY);
        return;
    }

    led_set_state(LED_RUNNING);

    if (sys_kind == SYSK_NES) {
        nes_run();

    } else if (sys_kind == SYSK_SMS) {
        if (staged_size == 0) {
            show_msg(last_is_gg ? "Game Gear" : "Master System",
                     "Reopen from Browse ROMs", COL_RED);
            return;
        }

        sms_core_set_rom(staged_size, last_is_gg);
        sms_core_run();

    } else if (sys_kind == SYSK_ATARI) {
        if (staged_size == 0) {
            // Old /lastrom.txt from before Atari size metadata is not safe to
            // guess: 2600 mapper selection depends heavily on image size.
            show_msg("Atari 2600", "Reopen from Browse ROMs", COL_RED);
            return;
        }

        atari_core_set_rom_size(staged_size);
        atari_core_run();

    } else {
        gb_core_run();
    }
}