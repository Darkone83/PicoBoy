# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# Pull in the Pico SDK (uses PICO_SDK_PATH env var, or fetches from git).
include(pico_sdk_import.cmake)

# Target board. Breadboard phase uses a stock Raspberry Pi Pico.
# Change to pico2 if you breadboard on an RP2350 board.
set(PICO_BOARD pico CACHE STRING "Board type")

project(picoboy C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(picoboy
    src/main.c
    src/st7789.c
    src/ws2812.c
    src/buttons.c
    src/ui.c
    src/diag.c
    src/audio.c
    src/sd.c
    src/splash.c
    src/settings.c
    src/gb_core.c
    src/led.c
    src/flash.c
    src/minigb_apu.c
    src/i2s.c
    src/ffunicode.c
    src/loader.c
    src/ff.c
    src/diskio.c
    src/nes_core.cpp
    src/infones_mapper.cpp
    src/infones_papu.cpp
    src/k6502.cpp
    src/infones.cpp
    src/infones_nsf.cpp
    src/infones_fds.cpp
    src/theme.c
    src/battery.c
    src/atari_core.c
    src/atari_cart.c
    src/atari_cpu.c
    src/atari_riot.c
    src/atari_tia.c
)

# NOTE: third_party/peanut-gb/peanut_gb.h is vendored but intentionally NOT
# compiled or included yet — the emulator is present but not wired.

# Compile the WS2812 PIO program into a generated header.
pico_generate_pio_header(picoboy ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

# Compile the I2S PIO program (MAX98357 audio out).
 pico_generate_pio_header(picoboy ${CMAKE_CURRENT_LIST_DIR}/src/i2s.pio)

target_include_directories(picoboy PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)

target_link_libraries(picoboy
    pico_stdlib
    hardware_spi
    hardware_pio
    hardware_clocks
    hardware_pwm
    pico_multicore
    hardware_flash
    hardware_vreg
    hardware_dma
    hardware_adc
)

# Debug/console over USB CDC, not UART (no pins spent on the breadboard).
pico_enable_stdio_usb(picoboy 1)
pico_enable_stdio_uart(picoboy 0)

# .uf2 / .elf / map etc.
pico_add_extra_outputs(picoboy)