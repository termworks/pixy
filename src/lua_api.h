#ifndef PIXY_LUA_API_H
#define PIXY_LUA_API_H

typedef struct lua_State lua_State;

int pixy_lua_open(lua_State *L);
void pixy_lua_register_modules(lua_State *L);

#endif
