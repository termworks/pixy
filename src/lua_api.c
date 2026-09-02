#include "lua_api.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "pixy.h"
#include "providers.h"
#include "render.h"

static void set_kind(lua_State *L, const char *kind) {
    lua_pushstring(L, kind);
    lua_setfield(L, -2, "kind");
}

static void copy_field(lua_State *L, int from, int to, const char *name) {
    from = lua_absindex(L, from);
    to = lua_absindex(L, to);
    lua_getfield(L, from, name);
    lua_setfield(L, to, name);
}

static int node_with_table(lua_State *L, const char *kind) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    set_kind(L, kind);
    return 1;
}

static int api_text(lua_State *L) {
    size_t len = 0;
    const char *text;
    if (lua_isnoneornil(L, 1)) {
        text = "";
    } else {
        text = luaL_tolstring(L, 1, &len);
    }
    lua_newtable(L);
    set_kind(L, "text");
    if (lua_isnoneornil(L, 1)) lua_pushliteral(L, "");
    else lua_pushlstring(L, text, len);
    lua_setfield(L, -2, "text");
    if (lua_istable(L, 2)) lua_pushvalue(L, 2);
    else lua_newtable(L);
    lua_setfield(L, -2, "style");
    return 1;
}

static int list_node(lua_State *L, const char *kind, const char *field) {
    if (!lua_isnoneornil(L, 1)) luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);
    set_kind(L, kind);
    if (lua_istable(L, 1)) lua_pushvalue(L, 1);
    else lua_newtable(L);
    lua_setfield(L, -2, field);
    return 1;
}

static int api_row(lua_State *L) {
    return list_node(L, "row", "children");
}
static int api_segments(lua_State *L) {
    return list_node(L, "segments", "children");
}
static int api_column(lua_State *L) {
    return list_node(L, "column", "children");
}
static int api_surface(lua_State *L) {
    return list_node(L, "surface", "lines");
}

static int api_regions(lua_State *L) {
    if (!lua_isnoneornil(L, 1)) luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L);
    set_kind(L, "regions");
    const char *fields[] = {"left", "center", "right"};
    for (size_t i = 0; i < 3; i++) {
        if (lua_istable(L, 1)) lua_getfield(L, 1, fields[i]);
        else lua_pushnil(L);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
        }
        lua_setfield(L, -2, fields[i]);
    }
    return 1;
}

static int api_pad(lua_State *L) {
    lua_newtable(L);
    set_kind(L, "pad");
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "value");
    if (lua_isnumber(L, 2)) {
        lua_newtable(L);
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, "left");
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, "right");
    } else if (lua_istable(L, 2)) {
        lua_pushvalue(L, 2);
    } else {
        lua_newtable(L);
    }
    lua_setfield(L, -2, "padding");
    return 1;
}

static int api_when(lua_State *L) {
    if (lua_toboolean(L, 1)) lua_pushvalue(L, 2);
    else lua_pushnil(L);
    return 1;
}

static int value_number_node(lua_State *L, const char *kind, const char *field, double fallback) {
    lua_newtable(L);
    set_kind(L, kind);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "value");
    lua_pushnumber(L, luaL_optnumber(L, 2, fallback));
    lua_setfield(L, -2, field);
    return 1;
}

static int api_priority(lua_State *L) {
    return value_number_node(L, "priority", "priority", 0);
}

static int api_truncate(lua_State *L) {
    lua_newtable(L);
    set_kind(L, "truncate");
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "value");
    lua_pushnumber(L, luaL_optnumber(L, 2, 0));
    lua_setfield(L, -2, "width");
    lua_pushstring(L, luaL_optstring(L, 3, ""));
    lua_setfield(L, -2, "marker");
    return 1;
}

static int api_style(lua_State *L) {
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_newtable(L);
    set_kind(L, "style");
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "value");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "style");
    return 1;
}

static int api_palette(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    lua_getfield(L, -1, "pixy");
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "_palette");
    return 0;
}

static int api_spinner(lua_State *L) {
    return node_with_table(L, "spinner");
}
static int api_sprite(lua_State *L) {
    return node_with_table(L, "sprite");
}

static int api_animate(lua_State *L) {
    if (lua_istable(L, 1)) return node_with_table(L, "animate");
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_newtable(L);
    set_kind(L, "animate");
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "callback");
    if (!lua_isnoneornil(L, 2)) {
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, "interval_ms");
    }
    return 1;
}

static int api_spacer(lua_State *L) {
    double weight = luaL_optnumber(L, 1, 1);
    lua_newtable(L);
    set_kind(L, "spacer");
    lua_pushnumber(L, weight > 0 ? weight : 0);
    lua_setfield(L, -2, "weight");
    return 1;
}

static int api_transparent(lua_State *L) {
    double width = luaL_optnumber(L, 1, 0);
    lua_newtable(L);
    set_kind(L, "transparent");
    lua_pushnumber(L, width > 0 ? width : 0);
    lua_setfield(L, -2, "width");
    return 1;
}

static int interaction_node(lua_State *L, const char *kind) {
    lua_newtable(L);
    set_kind(L, kind);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "value");
    if (lua_istable(L, 2)) {
        const char *fields[] = {"id", "actions", "hover_style", "press_styles", "priority"};
        for (size_t i = 0; i < 5; i++) copy_field(L, 2, -1, fields[i]);
    }
    return 1;
}

static int api_region(lua_State *L) {
    return interaction_node(L, "region");
}

static int api_segment(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    if (!*name) return luaL_error(L, "pixy.segment requires a name");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!lua_isnoneornil(L, 3)) luaL_checktype(L, 3, LUA_TTABLE);
    lua_newtable(L);
    set_kind(L, "pixy_segment");
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "name");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "render");
    if (lua_istable(L, 3)) lua_pushvalue(L, 3);
    else lua_newtable(L);
    lua_setfield(L, -2, "options");
    return 1;
}

static int api_zone(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    if (!*name) return luaL_error(L, "pixy.zone requires a zone name");
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getglobal(L, "require");
    lua_pushliteral(L, "pixy");
    lua_call(L, 1, 1);
    lua_getfield(L, -1, "zones");
    lua_newtable(L);
    set_kind(L, "pixy_zone");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "segments");
    lua_setfield(L, -2, name);
    return 0;
}

static void push_context_values(lua_State *L, int context) {
    lua_getfield(L, context, "values");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
}

static int shell_directory(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    push_context_values(L, 1);
    lua_getfield(L, -1, "cwd");
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 2);
    lua_getglobal(L, "__pixy_host");
    lua_getfield(L, -1, "env");
    lua_pushliteral(L, "PWD");
    lua_call(L, 1, 1);
    return 1;
}

static int shell_value(lua_State *L, const char *name) {
    luaL_checktype(L, 1, LUA_TTABLE);
    push_context_values(L, 1);
    lua_getfield(L, -1, name);
    return 1;
}

static int shell_status(lua_State *L) {
    return shell_value(L, "status");
}
static int shell_character(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    push_context_values(L, 1);
    lua_getfield(L, -1, "character");
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    lua_pushliteral(L, "> ");
    return 1;
}

static int git_value(lua_State *L, const char *field, const char *const *argv, size_t argc) {
    luaL_checktype(L, 1, LUA_TTABLE);
    push_context_values(L, 1);
    lua_getfield(L, -1, field);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 2);
    lua_getglobal(L, "__pixy_host");
    lua_getfield(L, -1, "exec");
    lua_createtable(L, (int)argc, 0);
    for (size_t i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_newtable(L);
    lua_pushinteger(L, 100);
    lua_setfield(L, -2, "timeout_ms");
    lua_pushinteger(L, 250);
    lua_setfield(L, -2, "ttl_ms");
    lua_getfield(L, 1, "values");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "cwd");
        if (lua_isstring(L, -1)) lua_setfield(L, -3, "cwd");
        else lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "env");
    if (lua_istable(L, -1)) lua_setfield(L, -2, "env");
    else lua_pop(L, 1);
    lua_call(L, 2, 1);
    lua_getfield(L, -1, "status");
    bool ok = lua_tointeger(L, -1) == 0;
    lua_pop(L, 1);
    if (!ok) {
        lua_pushnil(L);
        return 1;
    }
    lua_getfield(L, -1, "stdout");
    size_t len = 0;
    const char *text = lua_tolstring(L, -1, &len);
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    lua_pushlstring(L, text ? text : "", len);
    return 1;
}

static int git_branch(lua_State *L) {
    static const char *const argv[] = {"git", "--no-optional-locks", "branch", "--show-current"};
    return git_value(L, "git_branch", argv, 4);
}

static int git_status(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    push_context_values(L, 1);
    lua_getfield(L, -1, "git_status");
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 2);
    static const char *const argv[] = {"git", "--no-optional-locks", "status", "--porcelain",
                                       "--untracked-files=no"};
    git_value(L, "__unused", argv, 5);
    size_t len = 0;
    lua_tolstring(L, -1, &len);
    lua_pushstring(L, len ? "dirty" : "clean");
    return 1;
}

static void push_style_index(lua_State *L, int fg, int bg, bool bold) {
    lua_newtable(L);
    if (fg >= 0) {
        lua_pushinteger(L, fg);
        lua_setfield(L, -2, "fg");
    }
    if (bg >= 0) {
        lua_pushinteger(L, bg);
        lua_setfield(L, -2, "bg");
    }
    if (bold) {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "bold");
    }
}

static int push_text_node(lua_State *L, const char *text, size_t len, int fg, int bg, bool bold) {
    lua_newtable(L);
    set_kind(L, "text");
    lua_pushlstring(L, text, len);
    lua_setfield(L, -2, "text");
    push_style_index(L, fg, bg, bold);
    lua_setfield(L, -2, "style");
    return 1;
}

static int renderer_directory(lua_State *L) {
    shell_directory(L);
    size_t len = 0;
    const char *cwd = lua_tolstring(L, -1, &len);
    PixyBuf text = {0};
    pixy_buf_str(&text, " ");
    pixy_buf_add(&text, cwd ? cwd : "?", cwd ? len : 1);
    pixy_buf_str(&text, " ");
    int result = push_text_node(L, text.data, text.len, 15, 24, true);
    pixy_buf_free(&text);
    return result;
}

static int renderer_status(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    push_context_values(L, 1);
    lua_getfield(L, -1, "status");
    lua_Integer status = lua_tointeger(L, -1);
    if (status == 0) {
        lua_pushnil(L);
        return 1;
    }
    char text[64];
    int len = snprintf(text, sizeof(text), " %lld ", (long long)status);
    return push_text_node(L, text, (size_t)len, 15, 1, false);
}

static int renderer_git(lua_State *L) {
    git_branch(L);
    size_t len = 0;
    const char *branch = lua_tolstring(L, -1, &len);
    if (!branch || !len) {
        lua_pushnil(L);
        return 1;
    }
    PixyBuf text = {0};
    pixy_buf_str(&text, " ");
    pixy_buf_add(&text, branch, len);
    pixy_buf_str(&text, " ");
    int result = push_text_node(L, text.data, text.len, 0, 6, false);
    pixy_buf_free(&text);
    return result;
}

static int renderer_spinner(lua_State *L) {
    lua_newtable(L);
    set_kind(L, "spinner");
    lua_createtable(L, 4, 0);
    const char *frames[] = {"⠋", "⠙", "⠹", "⠸"};
    for (int i = 0; i < 4; i++) {
        lua_pushstring(L, frames[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "frames");
    lua_pushinteger(L, 80);
    lua_setfield(L, -2, "interval_ms");
    return 1;
}

static int renderer_pokemon(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    push_context_values(L, 1);
    lua_getfield(L, -1, "pokemon_name");
    const char *name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "pikachu";
    lua_getfield(L, -2, "sprite_shiny");
    bool shiny = lua_toboolean(L, -1);
    char item[320];
    snprintf(item, sizeof(item), "%s/%s", shiny ? "shiny" : "regular", name);
    lua_newtable(L);
    set_kind(L, "sprite");
    lua_pushliteral(L, "pokemon");
    lua_setfield(L, -2, "pack");
    lua_pushstring(L, item);
    lua_setfield(L, -2, "name");
    lua_pushliteral(L, "regular/pikachu");
    lua_setfield(L, -2, "fallback_name");
    lua_pushliteral(L, "ansi");
    lua_setfield(L, -2, "format");
    lua_pushboolean(L, true);
    lua_setfield(L, -2, "transparent");
    return 1;
}

static int open_shell(lua_State *L) {
    luaL_Reg functions[] = {{"directory", shell_directory},
                            {"status", shell_status},
                            {"character", shell_character},
                            {NULL, NULL}};
    luaL_newlib(L, functions);
    return 1;
}

static int open_git(lua_State *L) {
    luaL_Reg functions[] = {{"branch", git_branch}, {"status", git_status}, {NULL, NULL}};
    luaL_newlib(L, functions);
    return 1;
}

int pixy_lua_open(lua_State *L) {
    static const luaL_Reg functions[] = {
        {"text", api_text},
        {"row", api_row},
        {"segments", api_segments},
        {"regions", api_regions},
        {"column", api_column},
        {"pad", api_pad},
        {"when", api_when},
        {"priority", api_priority},
        {"truncate", api_truncate},
        {"style", api_style},
        {"palette", api_palette},
        {"spinner", api_spinner},
        {"animate", api_animate},
        {"surface", api_surface},
        {"spacer", api_spacer},
        {"transparent", api_transparent},
        {"sprite", api_sprite},
        {"region", api_region},
        {"segment", api_segment},
        {"zone", api_zone},
        {NULL, NULL},
    };
    luaL_newlib(L, functions);
    lua_pushcfunction(L, pixy_lua_render);
    lua_setfield(L, -2, "_render");
    luaL_Reg renderers[] = {
        {"directory", renderer_directory}, {"status", renderer_status},   {"git", renderer_git},
        {"spinner", renderer_spinner},     {"pokemon", renderer_pokemon}, {NULL, NULL}};
    luaL_newlib(L, renderers);
    lua_setfield(L, -2, "renderers");
    lua_newtable(L);
    lua_setfield(L, -2, "zones");
    lua_getglobal(L, "__pixy_host");
    lua_setfield(L, -2, "host");
    open_git(L);
    lua_setfield(L, -2, "git");
    open_shell(L);
    lua_setfield(L, -2, "shell");
    pixy_lua_open_progress(L);
    lua_setfield(L, -2, "progress");
    pixy_lua_open_system(L);
    lua_setfield(L, -2, "system");
    return 1;
}

void pixy_lua_register_modules(lua_State *L) {
    luaL_requiref(L, "pixy", pixy_lua_open, 1);
    lua_pop(L, 1);
}
