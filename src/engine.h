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

void *pixy_bounded_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

/* Encodes what `pixy._render` returned; the shapes are the Lua module's, so
 * these two live beside the engine rather than in the JSON reader. */
bool pixy_encode_runs(lua_State *L, int index, PixyBuf *out);
bool pixy_encode_regions(lua_State *L, int index, PixyBuf *out);

#endif /* PIXY_ENGINE_H */
