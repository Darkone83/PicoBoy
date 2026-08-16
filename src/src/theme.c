#include "theme.h"

// Built-in themes, authored in plain RGB via RGB565(); tweak freely. Entry 0
// (Purple) is the default brand look. Each gives the header a deeper shade than
// the accent so the accent underline + selection bar read as one bright family.
static const theme_t THEMES[] = {
    { "Purple",
      /* bg        */ RGB565(  0,   0,   0),
      /* header_bg */ RGB565( 92,  44, 140),
      /* header_fg */ RGB565(255, 255, 255),
      /* accent    */ RGB565(168,  85, 247),   // the brand purple
      /* footer_bg */ RGB565( 32,  28,  44),
      /* footer_fg */ RGB565(200, 196, 214),
      /* item_fg   */ RGB565(236, 236, 240),
      /* sel_bg    */ RGB565(168,  85, 247),
      /* sel_fg    */ RGB565(255, 255, 255),
      /* ok        */ RGB565( 64, 220, 120),
      /* warn      */ RGB565(248, 200,  64),
      /* err       */ RGB565(240,  72,  72) },

    { "Midnight",
      RGB565(  4,   6,  16), RGB565( 24,  44, 108), RGB565(255, 255, 255),
      RGB565( 64, 150, 255), RGB565( 14,  20,  40), RGB565(150, 176, 224),
      RGB565(210, 224, 255), RGB565( 40, 110, 230), RGB565(255, 255, 255),
      RGB565( 64, 220, 160), RGB565(250, 210,  90), RGB565(255,  96,  96) },

    { "Amber",
      RGB565(  0,   0,   0), RGB565( 90,  50,   0), RGB565(255, 210, 120),
      RGB565(255, 176,  32), RGB565( 40,  24,   0), RGB565(220, 170,  96),
      RGB565(248, 196, 104), RGB565(255, 176,  32), RGB565(  0,   0,   0),
      RGB565(120, 220,  80), RGB565(255, 200,  64), RGB565(255,  96,  64) },

    { "Matrix",
      RGB565(  0,   0,   0), RGB565(  0,  56,  20), RGB565(180, 255, 180),
      RGB565(  0, 230,  80), RGB565(  0,  24,   8), RGB565( 96, 200, 120),
      RGB565( 64, 220, 100), RGB565(  0, 230,  80), RGB565(  0,   0,   0),
      RGB565( 80, 255, 120), RGB565(230, 230,  80), RGB565(255,  96,  96) },

    { "Slate",
      RGB565( 18,  20,  26), RGB565( 56,  62,  78), RGB565(240, 242, 248),
      RGB565(176, 196, 224), RGB565( 28,  30,  38), RGB565(168, 176, 192),
      RGB565(220, 224, 232), RGB565(176, 196, 224), RGB565( 12,  14,  18),
      RGB565( 96, 210, 140), RGB565(244, 206,  96), RGB565(240,  96,  96) },

    { "Crimson",
      RGB565(  0,   0,   0), RGB565(120,  28,  40), RGB565(255, 255, 255),
      RGB565(235,  70,  80), RGB565( 34,  14,  18), RGB565(214, 168, 172),
      RGB565(238, 224, 226), RGB565(235,  70,  80), RGB565(255, 255, 255),
      RGB565( 96, 210, 130), RGB565(248, 200,  80), RGB565(255, 120,  90) },

    { "Ocean",
      RGB565(  2,  12,  18), RGB565( 14,  64,  84), RGB565(228, 248, 252),
      RGB565( 44, 200, 214), RGB565(  8,  28,  36), RGB565(150, 196, 206),
      RGB565(206, 236, 240), RGB565( 44, 200, 214), RGB565(  2,  20,  26),
      RGB565( 80, 224, 168), RGB565(244, 214, 110), RGB565(255, 118, 110) },

    { "Sunset",
      RGB565(  6,   2,  10), RGB565(110,  36,  74), RGB565(255, 244, 248),
      RGB565(255, 122,  92), RGB565( 30,  12,  22), RGB565(224, 168, 176),
      RGB565(246, 224, 220), RGB565(255, 122,  92), RGB565( 36,  10,   8),
      RGB565(120, 214, 150), RGB565(250, 206,  96), RGB565(244,  92,  92) },

    { "Mono",
      RGB565(  0,   0,   0), RGB565( 58,  58,  64), RGB565(255, 255, 255),
      RGB565(228, 228, 234), RGB565( 22,  22,  26), RGB565(170, 170, 178),
      RGB565(224, 224, 230), RGB565(228, 228, 234), RGB565(  0,   0,   0),
      RGB565(150, 210, 150), RGB565(236, 212, 130), RGB565(238, 130, 130) },

    { "DMG",
      RGB565( 15,  56,  15), RGB565( 48,  98,  48), RGB565(224, 248, 208),
      RGB565(155, 188,  15), RGB565( 15,  56,  15), RGB565(139, 172,  15),
      RGB565(139, 172,  15), RGB565(155, 188,  15), RGB565( 15,  56,  15),
      RGB565(155, 188,  15), RGB565(224, 200,  40), RGB565(224, 120,  40) },
};

#define NTHEMES ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

const theme_t *g_theme = &THEMES[0];

int         theme_count(void)  { return NTHEMES; }
const char *theme_name(int i)  { return (i >= 0 && i < NTHEMES) ? THEMES[i].name : ""; }
int         theme_index(void)  { return (int)(g_theme - THEMES); }

void theme_select(int i) {
    if (i < 0)         i = 0;
    if (i >= NTHEMES)  i = NTHEMES - 1;
    g_theme = &THEMES[i];
}