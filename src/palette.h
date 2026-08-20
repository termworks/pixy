#ifndef PIXY_PALETTE_H
#define PIXY_PALETTE_H

#include "pixy.h"

#define PIXY_PALETTE_OSC_DEFAULT 1330u
#define PIXY_PALETTE_MAX_SLOT 31
/* Slot 0 is the ordinary palette and 1 is the terminal's chrome, so a program
 * claims from 2 up. pixy takes the first app slot; a config may say otherwise. */
#define PIXY_PALETTE_DEFAULT_SLOT 2

typedef struct {
    char key[16];    /* 0-255, or fg / bg / cursor */
    char colour[32]; /* #rrggbb, rrggbb or rgb:rr/gg/bb */
} PixyPaletteEntry;

unsigned pixy_palette_osc(void);
bool pixy_palette_valid_slot(long slot, bool for_use);
bool pixy_palette_valid_key(const char *key);
bool pixy_palette_valid_colour(const char *colour);

/* The target picks the terminator as well as the escaping: bash eats the
 * backslash of ST, so a sequence bound for a bash prompt ends in BEL. */
bool pixy_palette_set(PixyBuf *out, const char *slot, const PixyPaletteEntry *entries, size_t count,
                      PixyTarget target);
bool pixy_palette_use(PixyBuf *out, long slot, PixyTarget target);
bool pixy_palette_end(PixyBuf *out, PixyTarget target);
bool pixy_palette_reset(PixyBuf *out, const char *slot, PixyTarget target);
bool pixy_palette_ask(PixyBuf *out, PixyTarget target);
/* Marks a sequence zero-width for the shell that will count the prompt. */
bool pixy_palette_wrap(PixyBuf *out, const char *sequence, PixyTarget target);

/* The `palette` table a configuration may declare, read once at load. */
bool pixy_engine_palette(PixyEngine *engine, PixyPaletteEntry **entries, size_t *count, long *slot);

#endif /* PIXY_PALETTE_H */
