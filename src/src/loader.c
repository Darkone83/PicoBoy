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
    { "Game Boy",     "gb",   ".gb",  NULL,   true  },
    { "NES",          "nes",  ".nes", NULL,   true  },
    { "Atari 2600",   "2600", ".a26", ".bin", true  },
    { "Famicom",      "fc",   ".nes", NULL,   false },
};
#define NSYS ((int)(sizeof(SYS) / sizeof(SYS[0])))

// ---- list buffer ---------------------------------------------------------
#define MAX_ENTRIES 128
#define MAX_NAME    80       // ROM filename (LFN); longer names are truncated
static char s_names[MAX_ENTRIES][MAX_NAME];

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
        // "<sys>/<base>\n<flash size>"
        char line[MAX_NAME + 24];
        int len = snprintf(line, sizeof line, "%s/%s\n%lu",
                           sysdir, base, (unsigned long)s_last_flash_size);
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
    uint32_t window = NVS_FLASH_OFFSET - ROM_FLASH_OFFSET;

    if (size == 0 || size > window) {
        f_close(&fil);
        show_msg("ROM Loader",
                 size == 0 ? "Empty file" : "ROM too big for flash",
                 COL_RED);
        return false;
    }

    s_last_flash_size = size;

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

    for (uint32_t off = 0; off < size; off += FLASH_SECTOR_SIZE) {
        UINT br = 0;

        if (f_read(&fil, s_secbuf, FLASH_SECTOR_SIZE, &br) != FR_OK) {
            ok = false;
            break;
        }

        if (br < FLASH_SECTOR_SIZE)
            memset(s_secbuf + br, 0xFF, FLASH_SECTOR_SIZE - br);

        flash_erase_sector(ROM_FLASH_OFFSET + off);

        for (uint32_t p = 0;
             p < FLASH_SECTOR_SIZE;
             p += FLASH_PAGE_SIZE) {
            flash_program_page(ROM_FLASH_OFFSET + off + p, s_secbuf + p);
        }

        ui_progress_pacman((int)(++done), (int)total);
    }

    f_close(&fil);

    if (!ok) {
        show_msg("ROM Loader", "SD read error", COL_RED);
        return false;
    }

    return true;
}

// ---- public flow ---------------------------------------------------------
void loader_browse(void) {
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
            snprintf(m, sizeof m, "No %s ROMs", SYS[s].ext);
            show_msg(SYS[s].name, m, COL_GRAY);
            ui_transition(UI_TRANSITION_BACK);
            continue;
        }

        int f = pick_list(
            SYS[s].name,
            n,
            SYS[s].runnable
                ? "A load  L/R page  B back"
                : "L/R page  B back"
        );

        if (f < 0) {
            ui_transition(UI_TRANSITION_BACK);
            continue;
        }

        if (!SYS[s].runnable) {
            show_msg(SYS[s].name, "Core coming in phase 4", COL_PURPLE);
            ui_transition(UI_TRANSITION_BACK);
            continue;
        }

        char full[MAX_NAME + 48];
        snprintf(full, sizeof full, "/roms/%s/%s",
                 SYS[s].dir, s_names[f]);

        ui_transition(UI_TRANSITION_FORWARD);

        if (flash_from_sd(full)) {
            char base[MAX_NAME];
            strip_ext(base, sizeof base, s_names[f]);

            persist_last(SYS[s].dir, base);
            led_set_state(LED_RUNNING);

            if (strcmp(SYS[s].dir, "nes") == 0) {
                char srm[96];
                snprintf(srm, sizeof srm,
                         "/save/%s/%s.srm", SYS[s].dir, base);
                nes_set_save_path(srm);
                nes_run();

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
        SYSK_ATARI
    };

    int sys_kind = SYSK_GB;
    uint32_t staged_size = 0;

    gb_set_save_path("");
    gb_set_state_path("");
    nes_set_save_path("");

    if (f_mount(&s_fs, "", 1) == FR_OK) {
        FIL f;

        if (f_open(&f, "/lastrom.txt", FA_READ) == FR_OK) {
            char line[MAX_NAME + 24];
            UINT br = 0;

            f_read(&f, line, sizeof line - 1, &br);
            f_close(&f);

            line[br] = '\0';

            // File:
            //   <sys>/<base>
            //   <staged flash size>
            char *nl = strchr(line, '\n');

            if (nl) {
                *nl++ = '\0';

                // Tolerate CRLF or whitespace before the decimal size.
                while (*nl == '\r' || *nl == ' ' || *nl == '\t')
                    nl++;

                staged_size = (uint32_t)strtoul(nl, NULL, 10);
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

                if (strcmp(sysdir, "nes") == 0) {
                    sys_kind = SYSK_NES;

                    char srm[96];
                    snprintf(srm, sizeof srm,
                             "/save/%s/%s.srm", sysdir, base);
                    nes_set_save_path(srm);

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

    led_set_state(LED_RUNNING);

    if (sys_kind == SYSK_NES) {
        nes_run();

    } else if (sys_kind == SYSK_ATARI) {
        if (staged_size == 0) {
            // Old /lastrom.txt from before Atari size metadata is not safe to
            // guess: 2600 mapper selection depends heavily on image size.
            show_msg("Atari 2600", "Missing ROM size metadata", COL_RED);
            return;
        }

        atari_core_set_rom_size(staged_size);
        atari_core_run();

    } else {
        gb_core_run();
    }
}