#ifndef PIXY_ENGINE_H
#define PIXY_ENGINE_H

#include "host.h"
#include "pixy.h"

typedef struct lua_State lua_State;

typedef struct {
    size_t used;
    size_t limit;
    long long deadline_ms;
} PixyBudget;

/* Both forms of every bundled module. The bytecode is what gets loaded; the
 * source is the way back when a binary ends up somewhere its bytecode does not
 * fit, which is a slower start rather than no start at all. */
typedef struct {
    const char *name;
    const char *source;
    size_t len;
    const unsigned char *code;
    size_t code_len;
} PixyModule;

void *pixy_bounded_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

/* Encodes what `pixy._render` returned; the shapes are the Lua module's, so
 * these two live beside the engine rather than in the JSON reader. */
bool pixy_encode_runs(lua_State *L, int index, PixyBuf *out);
bool pixy_encode_regions(lua_State *L, int index, PixyBuf *out);

#endif /* PIXY_ENGINE_H */
