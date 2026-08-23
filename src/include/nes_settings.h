#ifndef NES_SETTINGS_H
#define NES_SETTINGS_H
/* PicoBoy: minimal stand-in for the fhoedemakers settings.h that InfoNES_FDS
 * references. The core only reads settings.flags.autoInsertDiskA and
 * settings.flags.autoSwapFDS. Kept separate from PicoBoy's own settings.h
 * (which is the UI/NVS settings) to avoid the name collision. The single
 * global `settings` is defined in nes_core.cpp. */

struct NesSettingsFlags
{
    bool autoInsertDiskA;
    bool autoSwapFDS;
};

struct NesSettings
{
    NesSettingsFlags flags;
};

extern NesSettings settings;

#endif /* NES_SETTINGS_H */