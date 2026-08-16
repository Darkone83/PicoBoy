#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>

// PicoBoy boot splash. Centered on a 320x240 panel: x=80, y=40.
#define SPLASH_W 160
#define SPLASH_H 160

extern const uint16_t picoboy_splash[SPLASH_W * SPLASH_H];

#endif // SPLASH_H