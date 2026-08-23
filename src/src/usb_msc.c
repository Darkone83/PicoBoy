#include "usb_msc.h"

#include "tusb.h"
#include "ff.h"
#include "diskio.h"
#include "sd.h"
#include "arena.h"
#include "st7789.h"
#include "ui.h"
#include "theme.h"
#include "led.h"
#include "buttons.h"
#include "pico/stdlib.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define USB_MSC_PDRV          0
#define USB_MSC_SECTOR_SIZE   512u
#define USB_MSC_INVALID_LBA   0xFFFFFFFFu

static bool     s_usb_inited = false;
static bool     s_soft_connected = false;
static bool     s_storage_prepared = false;
static bool     s_session_suppressed = false; // B exited USB mode; START+MENU re-arms it
static uint32_t s_cache_lba = USB_MSC_INVALID_LBA;
static bool     s_cache_valid = false;
static bool     s_cache_dirty = false;

// The USB sector cache borrows the start of the shared emulator arena. USB MSC
// is only serviced from the main menu, so no emulator/browser owns this memory
// at the same time. This avoids another permanent 512-byte .bss buffer.
static inline uint8_t *sector_cache(void) {
    return arena_base();
}

static bool cache_flush(void) {
    if (!s_cache_dirty || !s_cache_valid)
        return true;

    if (!sd_ready() ||
        !sd_write_blocks(sector_cache(), s_cache_lba, 1))
        return false;

    s_cache_dirty = false;
    return true;
}

static void cache_reset(void) {
    s_cache_lba = USB_MSC_INVALID_LBA;
    s_cache_valid = false;
    s_cache_dirty = false;
}

static bool cache_load(uint32_t lba) {
    if (s_cache_valid && s_cache_lba == lba)
        return true;

    if (!cache_flush())
        return false;

    if (!sd_ready() ||
        !sd_read_blocks(sector_cache(), lba, 1)) {
        cache_reset();
        return false;
    }

    s_cache_lba = lba;
    s_cache_valid = true;
    s_cache_dirty = false;
    return true;
}

static void storage_prepare(void) {
    if (s_storage_prepared)
        return;

    // Register no FatFs object while the host can own the block device.
    // Later PicoBoy operations mount their own filesystem as usual.
    f_mount(NULL, "", 0);

    // Make capacity available before enumeration. If no card is present the MSC
    // LUN remains Not Ready rather than risking an invalid zero-sized disk.
    (void)disk_initialize(USB_MSC_PDRV);

    cache_reset();
    s_storage_prepared = true;
}

static void usb_lazy_init(void) {
    if (s_usb_inited)
        return;

    storage_prepare();

    // RP2040 has one native USB device controller on rhport 0.
    tud_init(0);
    s_usb_inited = true;
    s_soft_connected = true;
}

bool usb_msc_menu_task(bool enabled) {
    if (!enabled) {
        // Turning the setting Off also clears any session-only suppression, so
        // a later Settings -> On starts from a clean armed state.
        s_session_suppressed = false;
        usb_msc_pause();
        return false;
    }

    // B can leave USB storage while keeping the persistent setting On.
    // While suppressed, PicoBoy operates normally/charge-only until the user
    // explicitly presses START+MENU to re-arm MSC.
    if (s_session_suppressed)
        return false;

    storage_prepare();
    usb_lazy_init();

    if (!s_soft_connected) {
        tud_connect();
        s_soft_connected = true;
    }

    tud_task_ext(0, false);
    return tud_mounted();
}

void usb_msc_rearm(void) {
    if (!s_session_suppressed)
        return;

    s_session_suppressed = false;
}

void usb_msc_pause(void) {
    if (!s_usb_inited)
        return;

    (void)cache_flush();
    cache_reset();

    if (s_soft_connected) {
        tud_disconnect();
        s_soft_connected = false;
        // Let TinyUSB retire any queued disconnect work while we're still in
        // the menu context. No USB task runs inside an emulator/submenu.
        tud_task_ext(0, false);
    }

    s_storage_prepared = false;
}

static void draw_usb_mode(void) {
    uint16_t bg = g_theme->bg;
    uint16_t fg = g_theme->item_fg;
    uint16_t ac = g_theme->accent;
    uint16_t dim = g_theme->footer_fg;

    st7789_fill(bg);
    ui_header("USB Mode");

    ui_fill_pill(28, 66, 320 - 56, 42, g_theme->sel_bg);
    st7789_draw_string(62, 80, "USB MODE ACTIVE",
                       g_theme->sel_fg, g_theme->sel_bg, 2);

    if (sd_ready()) {
        st7789_draw_string(50, 132, "SD card available to PC", fg, bg, 1);

        uint32_t sectors = sd_sector_count();
        if (sectors) {
            char cap[32];
            uint32_t mb = (uint32_t)(((uint64_t)sectors * 512u) / (1024u * 1024u));
            snprintf(cap, sizeof cap, "%lu MB exposed", (unsigned long)mb);
            st7789_draw_string(104, 153, cap, ac, bg, 1);
        }
    } else {
        st7789_draw_string(84, 132, "SD card not ready", ac, bg, 1);
    }

    st7789_draw_string(62, 184, "PicoBoy SD access paused", dim, bg, 1);
    ui_footer("B Exit   START+MENU Re-enter");
}

void usb_msc_mode_wait(void) {
    if (!s_usb_inited)
        return;

    // Host enumeration has completed. While this screen is active, PicoBoy owns
    // no filesystem and no menu/emulator can start.
    storage_prepare();
    led_set_state(LED_SD_BUSY);
    draw_usb_mode();

    // Host "Eject" may flush/stop the LUN but does not return PicoBoy to normal
    // SD ownership. Exit is explicit: B safely disconnects MSC and returns to the
    // menu. START+MENU can re-arm USB storage afterward.
    //
    // B performs a SAFE USB exit:
    //   1) finish servicing any current USB transaction
    //   2) flush the assembled SD sector
    //   3) software-disconnect the MSC device from the host
    //   4) suppress automatic MSC reconnect for this running session
    //   5) return to normal PicoBoy operation (USB may remain plugged in for charging)
    //
    // Clear any stale edge before entering the loop.
    buttons_update();
    (void)buttons_pressed();

    while (true) {
        tud_task_ext(0, false);

        buttons_update();
        if (buttons_pressed() & (1u << BTN_B))
            break;

        sleep_ms(1);
    }

    // Finish any partially assembled host sector before removing the device.
    (void)cache_flush();
    cache_reset();

    if (s_soft_connected) {
        // Make the host see a real device removal before PicoBoy resumes local
        // SD access. Service TinyUSB briefly so the disconnect is propagated.
        tud_disconnect();
        s_soft_connected = false;
        for (unsigned i = 0; i < 50u; i++) {
            tud_task_ext(0, false);
            sleep_ms(1);
        }
    }

    f_mount(NULL, "", 0);
    s_storage_prepared = false;

    // Keep the user's persistent USB SD Access preference On, but suppress
    // automatic reconnect for this running session. START+MENU from the main
    // menu clears this latch and presents the MSC device again.
    s_session_suppressed = true;

    led_set_state(LED_IDLE);
}

// --------------------------------------------------------------------------
// TinyUSB MSC callbacks
// --------------------------------------------------------------------------

void tud_mount_cb(void) {
    storage_prepare();
}

void tud_umount_cb(void) {
    (void)cache_flush();
    cache_reset();
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
}

void tud_resume_cb(void) {}

void tud_msc_inquiry_cb(uint8_t lun,
                        uint8_t vendor_id[8],
                        uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;
    const char vid[] = "DARKONE ";
    const char pid[] = "PICOBOY SD      ";
    const char rev[] = "1.0 ";
    memcpy(vendor_id, vid, 8);
    memcpy(product_id, pid, 16);
    memcpy(product_rev, rev, 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;

    if (!sd_ready()) {
        sd_info_t info;
        (void)sd_init(&info);
    }

    if (!sd_ready() || sd_sector_count() == 0) {
        tud_msc_set_sense(0, SCSI_SENSE_NOT_READY, 0x3A, 0x00); // medium not present
        return false;
    }

    return true;
}

void tud_msc_capacity_cb(uint8_t lun,
                         uint32_t *block_count,
                         uint16_t *block_size) {
    (void)lun;
    *block_count = sd_sector_count();
    *block_size = USB_MSC_SECTOR_SIZE;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return sd_ready();
}

bool tud_msc_start_stop_cb(uint8_t lun,
                           uint8_t power_condition,
                           bool start,
                           bool load_eject) {
    (void)lun;
    (void)power_condition;
    (void)load_eject;

    if (!start)
        (void)cache_flush();

    // Host "Eject" only flushes/stops the LUN. PicoBoy remains in USB Mode
    // until the user presses B for the explicit safe-disconnect path.
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun,
                          uint32_t lba,
                          uint32_t offset,
                          void *buffer,
                          uint32_t bufsize) {
    (void)lun;

    if (!sd_ready() || !buffer || bufsize == 0)
        return -1;

    // Fast path: TinyUSB can now hand us several whole 512-byte media blocks in
    // one callback. Send them to the raw SD driver as one contiguous request so
    // count > 1 uses CMD18 READ_MULTIPLE_BLOCK.
    if (offset == 0 &&
        (bufsize % USB_MSC_SECTOR_SIZE) == 0) {
        uint32_t blocks = bufsize / USB_MSC_SECTOR_SIZE;

        if (blocks == 0 ||
            lba >= sd_sector_count() ||
            blocks > sd_sector_count() - lba)
            return -1;

        if (!sd_read_blocks((uint8_t *)buffer, lba, blocks))
            return -1;

        // A direct multi-sector read bypasses the one-sector compatibility
        // cache. Invalidate it if any returned LBA overlaps the cached sector.
        if (s_cache_valid && !s_cache_dirty &&
            s_cache_lba >= lba &&
            s_cache_lba < lba + blocks) {
            cache_reset();
        }

        return (int32_t)bufsize;
    }

    // Compatibility path for TinyUSB configurations smaller than a media block
    // or an unusual partial-sector request. Do not silently span sectors here:
    // normal READ10 traffic with the 2-KiB class buffer is block aligned above.
    if (offset >= USB_MSC_SECTOR_SIZE ||
        bufsize > USB_MSC_SECTOR_SIZE - offset)
        return -1;

    if (!cache_load(lba))
        return -1;

    memcpy(buffer, sector_cache() + offset, bufsize);
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun,
                           uint32_t lba,
                           uint32_t offset,
                           uint8_t *buffer,
                           uint32_t bufsize) {
    (void)lun;

    if (!sd_ready() || !buffer || bufsize == 0)
        return -1;

    // Fast path: complete aligned blocks go directly to the raw SD layer.
    // With CFG_TUD_MSC_EP_BUFSIZE == 2048, ordinary host writes arrive as up to
    // four sectors and sd_write_blocks(..., count > 1) uses CMD25.
    if (offset == 0 &&
        (bufsize % USB_MSC_SECTOR_SIZE) == 0) {
        uint32_t blocks = bufsize / USB_MSC_SECTOR_SIZE;

        if (blocks == 0 ||
            lba >= sd_sector_count() ||
            blocks > sd_sector_count() - lba)
            return -1;

        // Preserve ordering if an unusual partial sector is still dirty.
        if (!cache_flush())
            return -1;

        if (!sd_write_blocks(buffer, lba, blocks))
            return -1;

        // Direct writes make any compatibility-cache copy stale.
        cache_reset();
        return (int32_t)bufsize;
    }

    // Compatibility read/modify/write path for a partial media sector.
    if (offset >= USB_MSC_SECTOR_SIZE ||
        bufsize > USB_MSC_SECTOR_SIZE - offset)
        return -1;

    if (!cache_load(lba))
        return -1;

    memcpy(sector_cache() + offset, buffer, bufsize);
    s_cache_dirty = true;

    if (offset + bufsize >= USB_MSC_SECTOR_SIZE) {
        if (!cache_flush())
            return -1;
    }

    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun,
                        uint8_t const scsi_cmd[16],
                        void *buffer,
                        uint16_t bufsize) {
    (void)buffer;
    (void)bufsize;

    // SYNCHRONIZE CACHE (10). Full-sector writes are blocking/direct; the
    // fallback partial-transfer path may still have one assembled sector.
    if (scsi_cmd[0] == 0x35) {
        return cache_flush() ? 0 : -1;
    }

    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}