#ifndef FRENSHELPERS_H
#define FRENSHELPERS_H
/* PicoBoy: minimal stand-in for fhoedemakers/pico-infonesPlus FrensHelpers.h.
 * The vendored InfoNES core only needs these five Frens:: helpers. f_malloc is
 * a bump allocator over PicoBoy's shared emulator arena (arena.h); f_free is a
 * no-op because nes_run() rewinds the bump pointer at each launch. Implemented
 * in nes_core.cpp. */
#include <cstddef>

namespace Frens
{
    void        *f_malloc(size_t size);
    void         f_free(void *p);
    unsigned int GetAvailableMemory(void);
    bool         isPsramEnabled(void);
    void         getextensionfromfilename(const char *filename, char *ext, size_t extsize);
}

#endif /* FRENSHELPERS_H */