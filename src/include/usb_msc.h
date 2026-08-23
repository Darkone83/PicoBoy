#include <stdbool.h>

// Main-menu-only USB SD card mass-storage support.
//
// When disabled, PicoBoy never starts the USB device stack; the connector
// remains available for charging only. When enabled, service this from the
// main menu. Entering a submenu/emulator must call usb_msc_pause().
bool usb_msc_menu_task(bool enabled);  // service TinyUSB; true once host has mounted MSC
void usb_msc_pause(void);              // software-disconnect before SD/FatFs use elsewhere
void usb_msc_mode_wait(void);          // blocking USB MODE screen; B exits safely
void usb_msc_rearm(void);              // clear B-exit suppression; START+MENU uses this