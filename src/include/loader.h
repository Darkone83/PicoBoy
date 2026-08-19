#pragma once

// SD-card ROM loader / browser.
// Mounts the FAT volume (creating the /roms /save /state tree on first use),
// lets the user pick a system folder then a ROM, streams the ROM into the flash
// window, and launches it. Returns to the caller (main menu) when the user backs
// out or quits a game.
void loader_browse(void);

// "Load last game": runs the ROM in the flash window, restoring its .srm save
// path from /lastrom.txt when the card is present.
void loader_launch_last(void);