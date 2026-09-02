#ifndef PIXY_PROVIDERS_H
#define PIXY_PROVIDERS_H

typedef struct lua_State lua_State;

int pixy_lua_open_progress(lua_State *L);
int pixy_lua_open_system(lua_State *L);

#endif
