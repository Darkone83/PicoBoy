#pragma once
// PicoBoy pin map. Pick one pin-map variant + set the LCD SPI clock.
//
// The variant ONLY controls where SELECT/MENU live and whether VBAT_SENSE exists.
// The LCD SPI clock is INDEPENDENT (below), so you can run native/PCB pins at the
// slow breadboard clock during bring-up, then crank it on the real PCB.

#define PICOBOY_PCB            // native/full board: SELECT=23, MENU=24, VBAT on GPIO29
// #define PICOBOY_BREADBOARD  // stock Pico header: SELECT=20, MENU=22, no VBAT

// LCD SPI clock -- independent of the pin map.
// SPI clock for the panel. Breadboard: start ~48 MHz; if the display shows
// corrupt pixels/tearing, drop toward 31 MHz; if it's clean, try 62 MHz.
// (PCB traces handle 62.5 MHz cleanly.)
#define LCD_SPI_HZ    (92000000)

// ---- Display: ST7789 over SPI0 ----
#define LCD_SPI       spi0
#define LCD_SCK_PIN   2
#define LCD_MOSI_PIN  3
#define LCD_DC_PIN    4
#define LCD_CS_PIN    5
#define LCD_RST_PIN   6
#define LCD_BL_PIN    7

// ---- microSD over SPI1 ----
#define SD_SPI        spi1
#define SD_SCK_PIN    10
#define SD_MOSI_PIN   11
#define SD_MISO_PIN   12
#define SD_CS_PIN     13

// ---- I2S audio over PIO ----
#define I2S_BCLK_PIN  16
#define I2S_LRCLK_PIN 17
#define I2S_DOUT_PIN  18

// ---- SK6812 status LED ----
#define RGB_PIN       19

// ---- Buttons (active-low, internal pull-ups; press shorts GPIO -> GND) ----
#define BTN_UP_PIN     0
#define BTN_DOWN_PIN   1
#define BTN_LEFT_PIN   8
#define BTN_RIGHT_PIN  9
#define BTN_A_PIN      14
#define BTN_B_PIN      15
#define BTN_START_PIN  21

#ifdef PICOBOY_BREADBOARD
  #define BTN_SELECT_PIN 20
  #define BTN_MENU_PIN   22
#else  // PICOBOY_PCB / native
  #define BTN_SELECT_PIN 23
  #define BTN_MENU_PIN   24
  #define VBAT_SENSE_PIN 29   // ADC3: VBAT through the 100k:100k divider
  // Charge state is estimated from VOLTAGE alone (battery.c). That is approximate:
  // charging is a laggy trend heuristic and full/charging near the top is soft.
  // For DETERMINISTIC charge + full, wire TP4056 CHRG# (and STDBY#) to a spare GPIO
  // and define them here -- battery.c uses the pins automatically when present.
  // Free GPIOs on this board: 20, 22, 25, 26, 27, 28.
  #define CHRG_PIN   20   // TP4056 CHRG#  : LOW = charging (external 10k pull-up)
  #define STDBY_PIN  22   // TP4056 STDBY# : LOW = charge complete (full)
#endif

// ---- Display geometry (320x240 landscape) ----
#define LCD_W         320
#define LCD_H         240
#define LCD_X_OFFSET  0
#define LCD_Y_OFFSET  0
#define LCD_MADCTL    0x60    // flip MX/MY/MV to match panel mounting