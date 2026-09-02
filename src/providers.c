#include "providers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "pixy.h"

static void kind(lua_State *L, const char *name) {
    lua_pushstring(L, name);
    lua_setfield(L, -2, "kind");
}

static void values(lua_State *L, int context) {
    lua_getfield(L, context, "values");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
}

static long table_integer(lua_State *L, int table, const char *primary, const char *alternate,
                          long fallback) {
    table = lua_absindex(L, table);
    lua_getfield(L, table, primary);
    if (!lua_isnumber(L, -1) && alternate) {
        lua_pop(L, 1);
        lua_getfield(L, table, alternate);
    }
    long value = lua_isnumber(L, -1) ? (long)lua_tointeger(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

static bool progress_state_value(lua_State *L, int context, const char **state) {
    static const char *known[] = {"in_progress", "error", "indeterminate", "paused"};
    values(L, context);
    lua_getfield(L, -1, "progress_state");
    const char *found = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    for (size_t i = 0; found && i < 4; i++) {
        if (strcmp(found, known[i]) == 0) {
            *state = found;
            lua_pop(L, 2);
            return true;
        }
    }
    lua_pop(L, 2);
    return false;
}

static bool progress_percent_value(lua_State *L, int context, double *percent) {
    values(L, context);
    lua_getfield(L, -1, "progress_pct");
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 2);
        return false;
    }
    *percent = lua_tonumber(L, -1);
    if (*percent < 0) *percent = 0;
    if (*percent > 100) *percent = 100;
    lua_pop(L, 2);
    return true;
}

static int progress_state(lua_State *L) {
    const char *state = NULL;
    if (progress_state_value(L, 1, &state)) lua_pushstring(L, state);
    else lua_pushnil(L);
    return 1;
}

static int progress_percent(lua_State *L) {
    double percent = 0;
    if (progress_percent_value(L, 1, &percent)) lua_pushnumber(L, percent);
    else lua_pushnil(L);
    return 1;
}

static void repeat(PixyBuf *out, const char *glyph, size_t count) {
    for (size_t i = 0; i < count; i++) pixy_buf_str(out, glyph);
}

static int text_node(lua_State *L, const char *text, size_t len, int style) {
    lua_newtable(L);
    kind(L, "text");
    lua_pushlstring(L, text, len);
    lua_setfield(L, -2, "text");
    if (lua_istable(L, style)) lua_pushvalue(L, style);
    else lua_newtable(L);
    lua_setfield(L, -2, "style");
    return 1;
}

static int progress_bar(lua_State *L) {
    if (lua_isnoneornil(L, 1)) lua_newtable(L);
    else luaL_checktype(L, 1, LUA_TTABLE);
    int options = lua_absindex(L, 1);
    lua_getfield(L, options, "width");
    long width = lua_isnumber(L, -1) ? (long)lua_tonumber(L, -1) : 10;
    lua_pop(L, 1);
    if (width < 1) width = 1;
    lua_getfield(L, options, "percent");
    double percent = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0;
    lua_pop(L, 1);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    double exact = percent / 100 * width;
    size_t filled = (size_t)floor(exact);
    bool partial = exact - filled >= 0.5 && filled < (size_t)width;
    PixyBuf text = {0};
    repeat(&text, "█", filled);
    if (partial) pixy_buf_str(&text, "▓");
    repeat(&text, "░", (size_t)width - filled - (partial ? 1 : 0));
    lua_getfield(L, options, "style");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushinteger(L, 39);
        lua_setfield(L, -2, "fg");
    }
    int result = text_node(L, text.data, text.len, lua_gettop(L));
    pixy_buf_free(&text);
    return result;
}

static int progress_sweep(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    long width = 10;
    lua_getfield(L, 1, "width");
    if (lua_isnumber(L, -1)) width = (long)lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (width < 2) width = 2;
    long block = 3;
    lua_getfield(L, 1, "block");
    if (lua_isnumber(L, -1)) block = (long)lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (block < 1) block = 1;
    if (block >= width) block = width - 1;
    long interval = 90;
    lua_getfield(L, 1, "interval_ms");
    if (lua_isnumber(L, -1)) interval = (long)lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (interval < 1) interval = 1;
    lua_getfield(L, 2, "now_ms");
    long now = (long)lua_tointeger(L, -1);
    lua_pop(L, 1);
    values(L, 2);
    lua_getfield(L, -1, "started_at_ms");
    long started = (long)lua_tointeger(L, -1);
    lua_pop(L, 2);
    long elapsed = now > started ? now - started : 0;
    long span = (width - block) * 2;
    long frame = (elapsed / interval) % span;
    long offset = frame < width - block ? frame : span - frame;
    PixyBuf text = {0};
    repeat(&text, "░", (size_t)offset);
    repeat(&text, "█", (size_t)block);
    repeat(&text, "░", (size_t)(width - block - offset));
    lua_getfield(L, 1, "style");
    text_node(L, text.data, text.len, lua_gettop(L));
    lua_pushinteger(L, interval - elapsed % interval);
    lua_setfield(L, -2, "next_frame_ms");
    pixy_buf_free(&text);
    return 1;
}

static int progress_segment(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    const char *state = NULL;
    if (!progress_state_value(L, 2, &state)) {
        lua_pushnil(L);
        return 1;
    }
    double percent = 0;
    if (strcmp(state, "indeterminate") == 0 || !progress_percent_value(L, 2, &percent))
        return progress_sweep(L);
    lua_pushnumber(L, percent);
    lua_setfield(L, 1, "percent");
    progress_bar(L);
    lua_getfield(L, 1, "label");
    bool label = !lua_isboolean(L, -1) || lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (!label) return 1;
    int bar = lua_gettop(L);
    char suffix[32];
    int len = snprintf(suffix, sizeof(suffix), " %d%%", (int)floor(percent + 0.5));
    lua_getfield(L, 1, "style");
    text_node(L, suffix, (size_t)len, lua_gettop(L));
    lua_newtable(L);
    kind(L, "row");
    lua_createtable(L, 2, 0);
    lua_pushvalue(L, bar);
    lua_rawseti(L, -2, 1);
    lua_pushvalue(L, -3);
    lua_rawseti(L, -2, 2);
    lua_setfield(L, -2, "children");
    return 1;
}

static const char *host_read(lua_State *L, const char *path, size_t *len) {
    lua_getglobal(L, "__pixy_host");
    lua_getfield(L, -1, "read");
    lua_pushstring(L, path);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 2);
        return NULL;
    }
    const char *data = lua_tolstring(L, -1, len);
    return data;
}

static int system_env(lua_State *L, const char *name, const char *fallback) {
    lua_getglobal(L, "__pixy_host");
    lua_getfield(L, -1, "env");
    lua_pushstring(L, name);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1) && fallback) {
        lua_pop(L, 1);
        lua_getfield(L, -1, "env");
        lua_pushstring(L, fallback);
        lua_call(L, 1, 1);
    }
    return 1;
}

static int system_hostname(lua_State *L) {
    return system_env(L, "HOSTNAME", NULL);
}
static int system_username(lua_State *L) {
    return system_env(L, "USER", "LOGNAME");
}

static int system_time(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "now_ms");
    lua_pushinteger(L, lua_tointeger(L, -1) / 1000);
    return 1;
}

static int system_clock(lua_State *L) {
    const char *format = luaL_optstring(L, 2, "%H:%M:%S");
    lua_getglobal(L, "os");
    lua_getfield(L, -1, "date");
    lua_pushstring(L, format);
    lua_getfield(L, 1, "now_ms");
    lua_Integer seconds = lua_tointeger(L, -1) / 1000;
    lua_pop(L, 1);
    lua_pushinteger(L, seconds);
    lua_call(L, 2, 1);
    return 1;
}

static int parse_uptime(lua_State *L) {
    const char *data = luaL_checkstring(L, 1);
    char *end = NULL;
    double value = strtod(data, &end);
    if (end == data || value < 0) lua_pushnil(L);
    else lua_pushnumber(L, value);
    return 1;
}

static int system_uptime(lua_State *L) {
    size_t len = 0;
    const char *data = host_read(L, "/proc/uptime", &len);
    if (!data) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushcfunction(L, parse_uptime);
    lua_pushlstring(L, data, len);
    lua_call(L, 1, 1);
    return 1;
}

static int parse_memory(lua_State *L) {
    const char *data = luaL_checkstring(L, 1);
    const char *total_at = strstr(data, "MemTotal:");
    const char *available_at = strstr(data, "MemAvailable:");
    if (!total_at || !available_at) {
        lua_pushnil(L);
        return 1;
    }
    unsigned long long total = strtoull(total_at + 9, NULL, 10);
    unsigned long long available = strtoull(available_at + 13, NULL, 10);
    if (!total || available > total) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)total);
    lua_setfield(L, -2, "total_kib");
    lua_pushinteger(L, (lua_Integer)(total - available));
    lua_setfield(L, -2, "used_kib");
    return 1;
}

static int system_memory(lua_State *L) {
    size_t len = 0;
    const char *data = host_read(L, "/proc/meminfo", &len);
    if (!data) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushcfunction(L, parse_memory);
    lua_pushlstring(L, data, len);
    lua_call(L, 1, 1);
    return 1;
}

static bool read_context_path(lua_State *L, int context, const char *field, const char *fallback) {
    context = lua_absindex(L, context);
    const char *path = fallback;
    values(L, context);
    lua_getfield(L, -1, field);
    if (lua_isstring(L, -1)) path = lua_tostring(L, -1);
    lua_getglobal(L, "__pixy_host");
    lua_getfield(L, -1, "read");
    lua_pushstring(L, path);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 4);
        lua_pushnil(L);
        return false;
    }
    lua_remove(L, -2);
    lua_remove(L, -2);
    lua_remove(L, -2);
    return lua_isstring(L, -1);
}

static int system_battery(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (!read_context_path(L, 1, "battery_capacity_path", "/sys/class/power_supply/BAT0/capacity"))
        return 1;
    size_t len = 0;
    const char *capacity = lua_tolstring(L, -1, &len);
    char *end = NULL;
    long percent = strtol(capacity, &end, 10);
    if (end == capacity || percent < 0 || percent > 100) {
        lua_pushnil(L);
        return 1;
    }
    lua_pop(L, 1);
    read_context_path(L, 1, "battery_status_path", "/sys/class/power_supply/BAT0/status");
    size_t status_len = 0;
    const char *status = lua_tolstring(L, -1, &status_len);
    while (status_len && (status[status_len - 1] == '\n' || status[status_len - 1] == '\r' ||
                          status[status_len - 1] == ' ' || status[status_len - 1] == '\t'))
        status_len--;
    lua_newtable(L);
    lua_pushinteger(L, percent);
    lua_setfield(L, -2, "percent");
    if (status) {
        lua_pushlstring(L, status, status_len);
        lua_setfield(L, -2, "status");
    }
    return 1;
}

static int system_sudo(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    values(L, 1);
    lua_getfield(L, -1, "sudo");
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 2);

    lua_getglobal(L, "__pixy_host");
    lua_getfield(L, -1, "exec");
    lua_createtable(L, 3, 0);
    const char *argv[] = {"sudo", "-n", "true"};
    for (int i = 0; i < 3; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_newtable(L);
    lua_pushinteger(L, 30);
    lua_setfield(L, -2, "timeout_ms");
    long ttl = 5000;
    if (lua_istable(L, 2)) ttl = table_integer(L, 2, "ttl_ms", NULL, 5000);
    lua_pushinteger(L, ttl);
    lua_setfield(L, -2, "ttl_ms");
    lua_getfield(L, 1, "env");
    if (lua_istable(L, -1)) lua_setfield(L, -2, "env");
    else lua_pop(L, 1);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_getfield(L, -1, "timed_out");
    bool timed_out = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (timed_out) {
        lua_pushnil(L);
        return 1;
    }
    lua_getfield(L, -1, "status");
    lua_pushboolean(L, lua_isnumber(L, -1) && lua_tointeger(L, -1) == 0);
    return 1;
}

static int system_random(lua_State *L) {
    luaL_checktype(L, 2, LUA_TTABLE);
    size_t count = lua_rawlen(L, 2);
    if (!count) {
        lua_pushnil(L);
        return 1;
    }
    lua_Integer seed = luaL_optinteger(L, 3, 0);
    if (!seed) {
        values(L, 1);
        lua_getfield(L, -1, "started_at_ms");
        seed = lua_tointeger(L, -1);
        lua_pop(L, 2);
    }
    if (!seed) {
        lua_getfield(L, 1, "now_ms");
        seed = lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    lua_rawgeti(L, 2, seed % (lua_Integer)count + 1);
    return 1;
}

int pixy_lua_open_progress(lua_State *L) {
    luaL_Reg functions[] = {{"state", progress_state},     {"percent", progress_percent},
                            {"bar", progress_bar},         {"sweep", progress_sweep},
                            {"segment", progress_segment}, {NULL, NULL}};
    luaL_newlib(L, functions);
    return 1;
}

int pixy_lua_open_system(lua_State *L) {
    luaL_Reg functions[] = {{"hostname", system_hostname},
                            {"username", system_username},
                            {"time", system_time},
                            {"clock", system_clock},
                            {"uptime", system_uptime},
                            {"memory", system_memory},
                            {"battery", system_battery},
                            {"sudo", system_sudo},
                            {"random", system_random},
                            {"parse_linux_uptime", parse_uptime},
                            {"parse_linux_memory", parse_memory},
                            {NULL, NULL}};
    luaL_newlib(L, functions);
    return 1;
}
