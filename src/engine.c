/* The bounded Lua configuration host and its C implementation API. */
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "lua_api.h"
#include "palette.h"

struct PixyEngine {
    lua_State *L;
    PixyHost host;
    PixyBudget budget;
    char source_name[4096];
    int config_ref;
    int render_ref;
};

/* --------------------------------------------------------------- limits */

void *pixy_bounded_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    PixyBudget *budget = (PixyBudget *)ud;
    if (nsize == 0) {
        if (ptr) budget->used -= osize;
        free(ptr);
        return NULL;
    }
    size_t had = ptr ? osize : 0;
    if (budget->used - had + nsize > budget->limit) return NULL;
    void *out = realloc(ptr, nsize);
    if (!out) return NULL;
    budget->used = budget->used - had + nsize;
    return out;
}

static void deadline_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    PixyBudget *budget;
    lua_getallocf(L, (void **)&budget);
    if (budget->deadline_ms && pixy_cpu_ms() >= budget->deadline_ms) {
        budget->deadline_ms = 0; /* report once; the error unwinds from here */
        luaL_error(L, "exceeded its deadline");
    }
}

static void arm(PixyEngine *engine, long long ms) {
    engine->budget.deadline_ms = pixy_cpu_ms() + ms;
    lua_sethook(engine->L, deadline_hook, LUA_MASKCOUNT, PIXY_FUEL_PER_SLICE);
}

static void disarm(PixyEngine *engine) {
    engine->budget.deadline_ms = 0;
    lua_sethook(engine->L, NULL, 0, 0);
}

/* ------------------------------------------------------------ validation */

static bool valid_selector(const char *name) {
    if (!name || !*name) return false;
    unsigned char first = (unsigned char)name[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
          (first >= '0' && first <= '9')))
        return false;
    for (const char *at = name + 1; *at; at++) {
        unsigned char ch = (unsigned char)*at;
        bool alnum =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (!alnum && ch != '_' && ch != '.' && ch != '-') return false;
    }
    return true;
}

static bool valid_segment_name(const char *name) {
    if (!name || !*name) return false;
    unsigned char first = (unsigned char)name[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
          (first >= '0' && first <= '9')))
        return false;
    for (const char *at = name + 1; *at; at++) {
        unsigned char ch = (unsigned char)*at;
        bool alnum =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (!alnum && ch != '_' && ch != '-') return false;
    }
    return true;
}

static bool field_is(lua_State *L, int index, const char *field, const char *want) {
    lua_getfield(L, index, field);
    bool ok = lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), want) == 0;
    lua_pop(L, 1);
    return ok;
}

/* Validates one zone and builds its `segment_index`, which `pixy._render` uses
 * to answer a `zone.segment` selector without walking the list. */
static bool validate_zone(lua_State *L, int zone_index, const char *zone_name,
                          const char *source_name) {
    lua_getfield(L, zone_index, "segments");
    if (!lua_istable(L, -1)) {
        pixy_fail(PIXY_EXIT_CONFIG, "%s: zone %s segments must be a list", source_name, zone_name);
        lua_pop(L, 1);
        return false;
    }
    int segments = lua_gettop(L);
    lua_newtable(L);
    int index_table = lua_gettop(L);

    size_t count = 0;
    lua_Integer highest = 0;
    lua_pushnil(L);
    while (lua_next(L, segments) != 0) {
        if (!lua_isinteger(L, -2)) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: zone %s segments must be an array", source_name,
                      zone_name);
            lua_pop(L, 4);
            return false;
        }
        lua_Integer key = lua_tointeger(L, -2);
        if (key <= 0) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: zone %s has an invalid segment index", source_name,
                      zone_name);
            lua_pop(L, 4);
            return false;
        }
        if (key > highest) highest = key;
        count++;

        if (!lua_istable(L, -1) || !field_is(L, lua_gettop(L), "kind", "pixy_segment")) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: zone %s contains a non-pixy.segment value",
                      source_name, zone_name);
            lua_pop(L, 4);
            return false;
        }
        lua_getfield(L, -1, "name");
        const char *segment_name = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
        if (!valid_segment_name(segment_name)) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: invalid segment name %s.%s", source_name, zone_name,
                      segment_name ? segment_name : "?");
            lua_pop(L, 5);
            return false;
        }
        char owned[256];
        snprintf(owned, sizeof(owned), "%s", segment_name);
        lua_pop(L, 1);

        lua_getfield(L, index_table, owned);
        bool duplicate = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (duplicate) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: duplicate segment %s.%s", source_name, zone_name,
                      owned);
            lua_pop(L, 4);
            return false;
        }

        lua_getfield(L, -1, "render");
        bool render_ok = lua_isfunction(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "options");
        bool options_ok = lua_istable(L, -1);
        lua_pop(L, 1);
        if (!render_ok) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: segment %s.%s render value is not a function",
                      source_name, zone_name, owned);
            lua_pop(L, 4);
            return false;
        }
        if (!options_ok) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: segment %s.%s options must be a table", source_name,
                      zone_name, owned);
            lua_pop(L, 4);
            return false;
        }

        lua_pushvalue(L, -1);
        lua_setfield(L, index_table, owned);
        lua_pop(L, 1);
    }

    if (count == 0 || (lua_Integer)count != highest) {
        pixy_fail(PIXY_EXIT_CONFIG, "%s: zone %s requires a dense, non-empty segment list",
                  source_name, zone_name);
        lua_pop(L, 2);
        return false;
    }
    lua_setfield(L, zone_index, "segment_index");
    lua_pop(L, 1);
    return true;
}

static bool validate_config(lua_State *L, int config_index, const char *source_name) {
    lua_getfield(L, config_index, "zones");
    if (!lua_istable(L, -1)) {
        pixy_fail(PIXY_EXIT_CONFIG, "%s: config zones must be a table", source_name);
        lua_pop(L, 1);
        return false;
    }
    int zones = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, zones) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: zone names must be strings", source_name);
            lua_pop(L, 3);
            return false;
        }
        const char *zone_name = lua_tostring(L, -2);
        if (!valid_selector(zone_name)) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: invalid zone name \"%s\"", source_name, zone_name);
            lua_pop(L, 3);
            return false;
        }
        if (!lua_istable(L, -1) || !field_is(L, lua_gettop(L), "kind", "pixy_zone")) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: zone %s is not a pixy.zone", source_name, zone_name);
            lua_pop(L, 3);
            return false;
        }
        char owned[512];
        snprintf(owned, sizeof(owned), "%s", zone_name);
        if (!validate_zone(L, lua_gettop(L), owned, source_name)) {
            lua_pop(L, 3);
            return false;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return true;
}

/* ---------------------------------------------------------------- loading */

/* A configuration is parsed at every prompt to reach the same functions each
 * time, and a real one is tens of kilobytes. The compiled form is kept beside
 * the provider cache, keyed by what the file is and when it was last written,
 * so an edit is picked up on the next render and a stale compile can never be
 * used. Anything unreadable, unloadable or simply absent falls back to the
 * source, which is the only correctness this has to preserve.
 */
#define CONFIG_CACHE_MAGIC "pixyluac1"

static uint64_t fold(uint64_t hash, const void *bytes, size_t len) {
    const unsigned char *at = bytes;
    for (size_t i = 0; i < len; i++) hash = (hash ^ at[i]) * 1099511628211ULL;
    return hash;
}

/* The name carries two hashes: which configuration this is, and which revision
 * of it. The first lets a new revision delete the ones it replaces, so a file
 * edited all afternoon leaves one compiled copy rather than an afternoon's
 * worth. */
static void config_cache_path(const PixyEngine *engine, const PixyConfigSource *source, char *out,
                              size_t size, char *prefix, size_t prefix_size) {
    out[0] = '\0';
    if (prefix) prefix[0] = '\0';
    if (!engine->host.cache_dir[0] || !source->path[0]) return;

    uint64_t which = fold(1469598103934665603ULL, source->path, strlen(source->path));
    which = fold(which, PIXY_VERSION, strlen(PIXY_VERSION));
    which = fold(which, LUA_RELEASE, strlen(LUA_RELEASE));
    /* The revision is the content, not the timestamp. `st_mtime` counts whole
     * seconds, so two edits a moment apart that happen to leave the file the
     * same length are indistinguishable — and the second one would be served
     * the first one's compile. The bytes are already in memory to be parsed;
     * hashing them costs microseconds and cannot be wrong. */
    uint64_t revision = fold(which, source->source, source->source_len);

    char version[4200];
    if (snprintf(version, sizeof(version), "%s/v1", engine->host.cache_dir) >= (int)sizeof(version))
        return;
    if (mkdir(engine->host.cache_dir, 0700) != 0 && errno != EEXIST) return;
    if (mkdir(version, 0700) != 0 && errno != EEXIST) return;
    if (prefix) snprintf(prefix, prefix_size, "%016llx-", (unsigned long long)which);
    if (snprintf(out, size, "%s/%016llx-%016llx.luac", version, (unsigned long long)which,
                 (unsigned long long)revision) >= (int)size) {
        out[0] = '\0';
    }
}

/* Every earlier revision of the same configuration. */
static void config_cache_sweep(const PixyEngine *engine, const char *prefix, const char *keep) {
    char version[4200];
    snprintf(version, sizeof(version), "%s/v1", engine->host.cache_dir);
    DIR *dir = opendir(version);
    if (!dir) return;
    const char *kept = strrchr(keep, '/');
    kept = kept ? kept + 1 : keep;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) continue;
        if (strcmp(entry->d_name, kept) == 0) continue;
        char victim[4500];
        /* This path is about to be unlinked, so a truncated one is the last
         * thing wanted: it would name a different file. */
        if (snprintf(victim, sizeof(victim), "%s/%s", version, entry->d_name) >=
            (int)sizeof(victim))
            continue;
        unlink(victim);
    }
    closedir(dir);
}

/* Pushes the compiled chunk on success. */
static bool config_cache_load(lua_State *L, const char *path, const char *chunk_name) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char magic[sizeof(CONFIG_CACHE_MAGIC)];
    if (fread(magic, 1, sizeof(magic), file) != sizeof(magic) ||
        memcmp(magic, CONFIG_CACHE_MAGIC, sizeof(magic)) != 0) {
        fclose(file);
        return false;
    }
    PixyBuf code = {0};
    char chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) pixy_buf_add(&code, chunk, got);
    fclose(file);
    bool ok = code.len && luaL_loadbuffer(L, code.data, code.len, chunk_name) == LUA_OK;
    if (!ok && code.len) lua_pop(L, 1);
    pixy_buf_free(&code);
    return ok;
}

static int collect_code(lua_State *L, const void *chunk, size_t size, void *ud) {
    (void)L;
    return pixy_buf_add((PixyBuf *)ud, chunk, size) ? 0 : 1;
}

/* Written whole and renamed into place, so a reader never sees half a file. */
static void config_cache_store(lua_State *L, const char *path) {
    PixyBuf code = {0};
    if (lua_dump(L, collect_code, &code, 0) != 0 || !code.len) {
        pixy_buf_free(&code);
        return;
    }
    char temporary[4200];
    snprintf(temporary, sizeof(temporary), "%s.%d", path, (int)getpid());
    FILE *file = fopen(temporary, "wb");
    if (file) {
        bool ok = fwrite(CONFIG_CACHE_MAGIC, 1, sizeof(CONFIG_CACHE_MAGIC), file) ==
                      sizeof(CONFIG_CACHE_MAGIC) &&
                  fwrite(code.data, 1, code.len, file) == code.len;
        ok = fclose(file) == 0 && ok;
        if (!ok || rename(temporary, path) != 0) unlink(temporary);
    }
    pixy_buf_free(&code);
}

static bool valid_module_name(const char *name) {
    if (!name || !*name || strstr(name, "..")) return false;
    for (const char *at = name; *at; at++) {
        unsigned char ch = (unsigned char)*at;
        bool alnum =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (!alnum && ch != '_' && ch != '.' && ch != '-') return false;
    }
    return true;
}

static int config_searcher(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    if (!valid_module_name(name)) {
        lua_pushfstring(L, "\n\tinvalid Pixy Lua module name %s", name);
        return 1;
    }
    char relative[1024];
    size_t len = strlen(name);
    if (len >= sizeof(relative) - 16) {
        lua_pushliteral(L, "\n\tPixy Lua module name is too long");
        return 1;
    }
    memcpy(relative, name, len + 1);
    for (size_t i = 0; i < len; i++) {
        if (relative[i] == '.') relative[i] = '/';
    }
    char candidates[4][1100];
    snprintf(candidates[0], sizeof(candidates[0]), "%s.lua", relative);
    snprintf(candidates[1], sizeof(candidates[1]), "%s/init.lua", relative);
    snprintf(candidates[2], sizeof(candidates[2]), "lua/%s.lua", relative);
    snprintf(candidates[3], sizeof(candidates[3]), "lua/%s/init.lua", relative);
    for (size_t i = 0; i < 4; i++) {
        lua_getglobal(L, "__pixy_host");
        lua_getfield(L, -1, "read");
        lua_pushstring(L, candidates[i]);
        if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isstring(L, -1)) {
            size_t source_len = 0;
            const char *source = lua_tolstring(L, -1, &source_len);
            size_t chunk_len = strlen(candidates[i]) + 2;
            char *chunk = malloc(chunk_len);
            if (!chunk) return luaL_error(L, "out of memory loading %s", candidates[i]);
            snprintf(chunk, chunk_len, "@%s", candidates[i]);
            int status = luaL_loadbuffer(L, source, source_len, chunk);
            free(chunk);
            if (status != LUA_OK) return lua_error(L);
            lua_pushstring(L, candidates[i]);
            return 2;
        }
        lua_settop(L, 1);
    }
    lua_pushfstring(L, "\n\tno Pixy Lua module '%s'", name);
    return 1;
}

static void install_searchers(lua_State *L) {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "searchers");
    lua_rawgeti(L, -1, 1);
    lua_newtable(L);
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, 1);
    lua_pushcfunction(L, config_searcher);
    lua_rawseti(L, -2, 2);
    lua_setfield(L, -4, "searchers");
    lua_pushliteral(L, "");
    lua_setfield(L, -3, "path");
    lua_pop(L, 3);
}

PixyEngine *pixy_engine_load(const PixyConfigSource *source, const PixyPaths *paths) {
    if (source->source_len > PIXY_MAX_CONFIG_SIZE) {
        pixy_fail(PIXY_EXIT_CONFIG, "%s exceeds 1 MiB", source->name);
        return NULL;
    }
    PixyEngine *engine = calloc(1, sizeof(PixyEngine));
    if (!engine) {
        pixy_fail(PIXY_EXIT_CONFIG, "out of memory");
        return NULL;
    }
    engine->budget.limit = PIXY_MEMORY_LIMIT;
    engine->L = lua_newstate(pixy_bounded_alloc, &engine->budget);
    if (!engine->L) {
        free(engine);
        pixy_fail(PIXY_EXIT_CONFIG, "could not create a Lua state");
        return NULL;
    }
    lua_State *L = engine->L;
    /* Only the libraries a configuration is meant to have. `luaL_openlibs`
     * would also hand it `io`, `debug` and `os.execute`, and then "reads go
     * through the host or not at all" would be a description of the host API
     * rather than a restriction -- `io.open` reaches straight past the trusted
     * roots. Opening fewer libraries is also less to build at every prompt. */
    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},       {LUA_LOADLIBNAME, luaopen_package},
        {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_OSLIBNAME, luaopen_os},     {NULL, NULL},
    };
    for (const luaL_Reg *library = libraries; library->func; library++) {
        luaL_requiref(L, library->name, library->func, 1);
        lua_pop(L, 1);
    }

    /* `os` is here for the clock. The rest of it acts on the machine. */
    static const char *const withheld[] = {"execute", "remove",    "rename", "tmpname",
                                           "exit",    "setlocale", NULL};
    lua_getglobal(L, LUA_OSLIBNAME);
    for (const char *const *name = withheld; *name; name++) {
        lua_pushnil(L);
        lua_setfield(L, -2, *name);
    }
    lua_pop(L, 1);

    /* A config reads through the host or not at all. */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");

    snprintf(engine->host.roots[0], sizeof(engine->host.roots[0]), "%s", source->directory);
    snprintf(engine->host.roots[1], sizeof(engine->host.roots[1]), "%s", paths->data_dir);
    snprintf(engine->host.roots[2], sizeof(engine->host.roots[2]), "/proc");
    snprintf(engine->host.roots[3], sizeof(engine->host.roots[3]), "/sys");
    engine->host.root_count = 4;
    snprintf(engine->host.data_dir, sizeof(engine->host.data_dir), "%s", paths->data_dir);
    snprintf(engine->host.cache_dir, sizeof(engine->host.cache_dir), "%s", paths->cache_dir);
    pixy_host_install(L, &engine->host);
    pixy_host_begin_render(&engine->host);

    arm(engine, PIXY_LOAD_DEADLINE_MS);

    pixy_lua_register_modules(L);
    install_searchers(L);

    lua_getglobal(L, "require");
    lua_pushstring(L, "pixy");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        pixy_fail(PIXY_EXIT_CONFIG, "loading pixy: %s", lua_tostring(L, -1));
        pixy_engine_free(engine);
        return NULL;
    }
    lua_getfield(L, -1, "_render");
    if (!lua_isfunction(L, -1)) {
        pixy_fail(PIXY_EXIT_CONFIG, "the pixy module has no _render");
        pixy_engine_free(engine);
        return NULL;
    }
    engine->render_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);

    char cached[4200], cache_prefix[32];
    config_cache_path(engine, source, cached, sizeof(cached), cache_prefix, sizeof(cache_prefix));
    if (cached[0] && config_cache_load(L, cached, source->name)) {
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: lua error: @%s", source->name, lua_tostring(L, -1));
            pixy_engine_free(engine);
            return NULL;
        }
    } else {
        if (luaL_loadbuffer(L, source->source, source->source_len, source->name) != LUA_OK) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: lua error: @%s", source->name, lua_tostring(L, -1));
            pixy_engine_free(engine);
            return NULL;
        }
        if (cached[0]) {
            config_cache_store(L, cached);
            config_cache_sweep(engine, cache_prefix, cached);
        }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: lua error: @%s", source->name, lua_tostring(L, -1));
            pixy_engine_free(engine);
            return NULL;
        }
    }
    if (!lua_isnil(L, -1)) {
        pixy_fail(PIXY_EXIT_CONFIG, "%s: a config must register values and return nothing",
                  source->name);
        pixy_engine_free(engine);
        return NULL;
    }
    lua_pop(L, 1);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    lua_getfield(L, -1, "pixy");
    lua_newtable(L);
    lua_getfield(L, -2, "zones");
    lua_setfield(L, -2, "zones");
    lua_getfield(L, -2, "_palette");
    if (!lua_isnil(L, -1)) lua_setfield(L, -2, "palette");
    else lua_pop(L, 1);
    lua_replace(L, -4);
    lua_pop(L, 2);
    if (!validate_config(L, lua_gettop(L), source->name)) {
        pixy_engine_free(engine);
        return NULL;
    }
    engine->config_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    snprintf(engine->source_name, sizeof(engine->source_name), "%s", source->name);
    disarm(engine);
    lua_gc(L, LUA_GCCOLLECT, 0);
    /* Generational suits this shape: a render allocates a burst of short-lived
     * nodes and drops them, which an incremental sweep pays for again and again. */
    lua_gc(L, LUA_GCGEN, 0, 0);
    return engine;
}

void pixy_engine_free(PixyEngine *engine) {
    if (!engine) return;
    if (engine->L) lua_close(engine->L);
    pixy_host_free(&engine->host);
    free(engine);
}

const char *pixy_engine_source_name(const PixyEngine *engine) {
    return engine->source_name;
}

/* --------------------------------------------------------------- request */

static const char *mode_name(PixyMode mode) {
    switch (mode) {
    case PIXY_MODE_RUN:
        return "run";
    case PIXY_MODE_SURFACE:
        return "surface";
    default:
        return "line";
    }
}

static const char *target_name(PixyTarget target) {
    switch (target) {
    case PIXY_TARGET_ANSI:
        return "ansi";
    case PIXY_TARGET_BASH:
        return "bash";
    case PIXY_TARGET_ZSH:
        return "zsh";
    default:
        return "plain";
    }
}

/* JSON -> Lua, so a context reaches a config exactly as its caller wrote it. */
static void push_json(lua_State *L, const PixyJson *value) {
    switch (pixy_json_kind(value)) {
    case PIXY_JSON_NULL:
        lua_pushnil(L);
        break;
    case PIXY_JSON_BOOL:
        lua_pushboolean(L, pixy_json_bool(value));
        break;
    case PIXY_JSON_NUMBER: {
        double number = pixy_json_number(value);
        if (number == (double)(long long)number) {
            lua_pushinteger(L, (lua_Integer)number);
        } else {
            lua_pushnumber(L, number);
        }
        break;
    }
    case PIXY_JSON_STRING: {
        size_t len = 0;
        const char *text = pixy_json_string(value, &len);
        lua_pushlstring(L, text, len);
        break;
    }
    case PIXY_JSON_ARRAY: {
        lua_newtable(L);
        for (size_t i = 0; i < pixy_json_count(value); i++) {
            push_json(L, pixy_json_at(value, i));
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        break;
    }
    case PIXY_JSON_OBJECT: {
        lua_newtable(L);
        for (size_t i = 0; i < pixy_json_count(value); i++) {
            size_t key_len = 0;
            const char *key = pixy_json_key(value, i, &key_len);
            const PixyJson *child = pixy_json_at(value, i);
            if (pixy_json_kind(child) == PIXY_JSON_NULL) continue;
            lua_pushlstring(L, key, key_len);
            push_json(L, child);
            lua_settable(L, -3);
        }
        break;
    }
    }
}

static void collect_env(PixyHost *host, const PixyJson *context) {
    const PixyJson *env = context ? pixy_json_get(context, "env") : NULL;
    size_t count = pixy_json_count(env);
    if (!env || pixy_json_kind(env) != PIXY_JSON_OBJECT || count == 0) {
        pixy_host_set_env(host, NULL, NULL, 0);
        return;
    }
    char **names = calloc(count, sizeof(char *));
    char **values = calloc(count, sizeof(char *));
    if (!names || !values) {
        free(names);
        free(values);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        size_t key_len = 0;
        const char *key = pixy_json_key(env, i, &key_len);
        const PixyJson *child = pixy_json_at(env, i);
        names[i] = strndup(key, key_len);
        size_t value_len = 0;
        const char *text = pixy_json_string(child, &value_len);
        values[i] = text ? strndup(text, value_len) : NULL;
    }
    pixy_host_set_env(host, names, values, count);
}

bool pixy_engine_render(PixyEngine *engine, const PixyRequest *request, PixyOutput *out) {
    lua_State *L = engine->L;
    memset(out, 0, sizeof(*out));

    PixyJson *context = NULL;
    if (request->context_json && request->context_json_len) {
        context = pixy_json_parse(request->context_json, request->context_json_len);
        if (!context) {
            pixy_fail(PIXY_EXIT_USAGE, "invalid context JSON");
            return false;
        }
    }
    collect_env(&engine->host, context);

    lua_rawgeti(L, LUA_REGISTRYINDEX, engine->render_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, engine->config_ref);

    lua_newtable(L);
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "version");
    lua_newtable(L);
    for (size_t i = 0; i < request->select_count; i++) {
        lua_pushstring(L, request->select[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "select");
    lua_pushstring(L, mode_name(request->mode));
    lua_setfield(L, -2, "mode");
    if (request->has_target) {
        lua_pushstring(L, target_name(request->target));
        lua_setfield(L, -2, "target");
    }
    lua_pushinteger(L, request->width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, request->height);
    lua_setfield(L, -2, "height");
    lua_pushinteger(
        L, (lua_Integer)(request->has_now_ms ? (long long)request->now_ms : pixy_unix_ms()));
    lua_setfield(L, -2, "now_ms");
    lua_pushboolean(L, request->ignore_missing);
    lua_setfield(L, -2, "ignore_missing");

    lua_newtable(L);
    const PixyJson *values = context ? pixy_json_get(context, "values") : NULL;
    if (values && pixy_json_kind(values) == PIXY_JSON_OBJECT) {
        push_json(L, values);
    } else {
        lua_newtable(L);
    }
    lua_setfield(L, -2, "values");
    const PixyJson *env = context ? pixy_json_get(context, "env") : NULL;
    if (env && pixy_json_kind(env) == PIXY_JSON_OBJECT) {
        push_json(L, env);
    } else {
        lua_newtable(L);
    }
    lua_setfield(L, -2, "env");
    lua_setfield(L, -2, "context");

    pixy_host_begin_render(&engine->host);
    arm(engine, PIXY_RENDER_DEADLINE_MS);
    int status = lua_pcall(L, 2, 1, 0);
    disarm(engine);
    pixy_json_free(context);

    if (status != LUA_OK) {
        pixy_fail(PIXY_EXIT_RENDER, "%s: lua error: @%s", engine->source_name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (!lua_istable(L, -1)) {
        pixy_fail(PIXY_EXIT_RENDER, "render returned no output table");
        lua_pop(L, 1);
        return false;
    }

    int output = lua_gettop(L);
    lua_getfield(L, output, "mode");
    const char *mode = lua_isstring(L, -1) ? lua_tostring(L, -1) : "line";
    out->mode = strcmp(mode, "run") == 0       ? PIXY_MODE_RUN
                : strcmp(mode, "surface") == 0 ? PIXY_MODE_SURFACE
                                               : PIXY_MODE_LINE;
    lua_pop(L, 1);

    if (out->mode == PIXY_MODE_LINE) {
        lua_getfield(L, output, "text");
        size_t len = 0;
        const char *text = lua_tolstring(L, -1, &len);
        if (text) pixy_buf_add(&out->payload, text, len);
        lua_pop(L, 1);
    } else if (out->mode == PIXY_MODE_SURFACE) {
        lua_getfield(L, output, "ansi");
        size_t len = 0;
        const char *text = lua_tolstring(L, -1, &len);
        if (text) pixy_buf_add(&out->payload, text, len);
        lua_pop(L, 1);
        lua_getfield(L, output, "height");
        out->height = (size_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    } else {
        lua_getfield(L, output, "runs");
        pixy_encode_runs(L, lua_gettop(L), &out->runs_json);
        lua_pop(L, 1);
    }

    lua_getfield(L, output, "width");
    out->width = (size_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, output, "next_frame_ms");
    if (lua_isnumber(L, -1)) {
        out->has_next_frame = true;
        out->next_frame_ms = (uint64_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, output, "regions");
    if (lua_istable(L, -1)) pixy_encode_regions(L, lua_gettop(L), &out->regions_json);
    lua_pop(L, 1);

    lua_getfield(L, output, "_stream_rewind");
    if (lua_isstring(L, -1)) {
        size_t len = 0;
        const char *text = lua_tolstring(L, -1, &len);
        pixy_buf_add(&out->stream_rewind, text, len);
    }
    lua_pop(L, 1);

    lua_pop(L, 1);
    if (out->payload.len > PIXY_OUTPUT_LIMIT) {
        pixy_fail(PIXY_EXIT_RENDER, "output exceeds 1 MiB");
        return false;
    }
    return true;
}

void pixy_output_free(PixyOutput *output) {
    pixy_buf_free(&output->payload);
    pixy_buf_free(&output->runs_json);
    pixy_buf_free(&output->regions_json);
    pixy_buf_free(&output->stream_rewind);
}

/* ------------------------------------------------------------- filmstrip */

#define PIXY_MAX_FRAMES 64

/* The drawn picture, ignoring the cadence and the hit regions riding with it:
 * two frames that paint the same cells are the same frame. */
static bool same_picture(const PixyOutput *left, const PixyOutput *right) {
    if (left->mode != right->mode || left->width != right->width || left->height != right->height)
        return false;
    if (left->payload.len != right->payload.len || left->runs_json.len != right->runs_json.len)
        return false;
    if (left->payload.len && memcmp(left->payload.data, right->payload.data, left->payload.len))
        return false;
    if (left->runs_json.len &&
        memcmp(left->runs_json.data, right->runs_json.data, left->runs_json.len))
        return false;
    return true;
}

bool pixy_engine_filmstrip(PixyEngine *engine, const PixyRequest *request, PixyOutput **frames_out,
                           size_t *count_out) {
    *frames_out = NULL;
    *count_out = 0;

    PixyOutput *frames = calloc(PIXY_MAX_FRAMES, sizeof *frames);
    if (!frames) {
        pixy_fail(PIXY_EXIT_RENDER, "out of memory");
        return false;
    }

    PixyRequest step = *request;
    uint64_t base = request->has_now_ms ? request->now_ms : (uint64_t)pixy_unix_ms();
    step.has_now_ms = true;

    size_t count = 0;
    uint64_t ahead = 0;
    while (count < PIXY_MAX_FRAMES) {
        step.now_ms = base + ahead;
        PixyOutput frame;
        if (!pixy_engine_render(engine, &step, &frame)) {
            pixy_frames_free(frames, count);
            return false;
        }
        /* The cycle closed: the caller loops back to the first frame itself. */
        if (count > 0 && same_picture(&frames[0], &frame)) {
            pixy_output_free(&frame);
            break;
        }
        /* A held picture -- the pause at the end of a sweep -- is one frame
         * shown for longer, not the same cells sent many times. */
        if (count > 0 && same_picture(&frames[count - 1], &frame)) {
            frames[count - 1].next_frame_ms += frame.next_frame_ms;
            uint64_t held = frame.next_frame_ms;
            pixy_output_free(&frame);
            if (!held) break;
            ahead += held;
            if (ahead >= request->frames_ms) break;
            continue;
        }
        frames[count++] = frame;
        /* Nothing moves, so there is no second frame to hold. */
        if (!frame.has_next_frame || frame.next_frame_ms == 0) break;
        ahead += frame.next_frame_ms;
        /* Past the horizon the caller asks again anyway, and content that is
         * not on a cycle -- a clock -- must never be replayed from a stale
         * strip. */
        if (ahead >= request->frames_ms) break;
    }

    *frames_out = frames;
    *count_out = count;
    return true;
}

void pixy_frames_free(PixyOutput *frames, size_t count) {
    if (!frames) return;
    for (size_t i = 0; i < count; i++) pixy_output_free(&frames[i]);
    free(frames);
}

/* ------------------------------------------------------------- inventory */

static int compare_names(const void *left, const void *right) {
    return strcmp(*(const char **)left, *(const char **)right);
}

bool pixy_engine_inventory(PixyEngine *engine, char ***names_out, size_t *count_out,
                           size_t *zones_out, size_t *segments_out) {
    lua_State *L = engine->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, engine->config_ref);
    lua_getfield(L, -1, "zones");
    if (!lua_istable(L, -1)) {
        pixy_fail(PIXY_EXIT_CONFIG, "config zones must be a table");
        lua_pop(L, 2);
        return false;
    }
    size_t capacity = 64, count = 0, zones = 0, segments = 0;
    char **names = calloc(capacity, sizeof(char *));
    if (!names) {
        lua_pop(L, 2);
        return false;
    }
    int zones_index = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, zones_index) != 0) {
        const char *zone_name = lua_tostring(L, -2);
        zones++;
        if (count + 2 >= capacity) {
            capacity *= 2;
            char **grown = realloc(names, capacity * sizeof(char *));
            if (!grown) break;
            names = grown;
        }
        names[count++] = strdup(zone_name);
        lua_getfield(L, -1, "segments");
        if (lua_istable(L, -1)) {
            lua_Integer len = (lua_Integer)lua_rawlen(L, -1);
            for (lua_Integer i = 1; i <= len; i++) {
                lua_rawgeti(L, -1, i);
                lua_getfield(L, -1, "name");
                const char *segment_name = lua_tostring(L, -1);
                if (segment_name) {
                    if (count + 2 >= capacity) {
                        capacity *= 2;
                        char **grown = realloc(names, capacity * sizeof(char *));
                        if (!grown) break;
                        names = grown;
                    }
                    char joined[600];
                    snprintf(joined, sizeof(joined), "%s.%s", zone_name, segment_name);
                    names[count++] = strdup(joined);
                    segments++;
                }
                lua_pop(L, 2);
            }
        }
        lua_pop(L, 2);
    }
    lua_pop(L, 2);
    qsort(names, count, sizeof(char *), compare_names);
    *names_out = names;
    *count_out = count;
    if (zones_out) *zones_out = zones;
    if (segments_out) *segments_out = segments;
    return true;
}

/* ------------------------------------------------------------- palette */

/* A configuration may register the colours its indexes should resolve to. */
static int compare_palette_entries(const void *left, const void *right) {
    const PixyPaletteEntry *a = left, *b = right;
    bool a_index = a->key[0] >= '0' && a->key[0] <= '9';
    bool b_index = b->key[0] >= '0' && b->key[0] <= '9';
    if (a_index != b_index) return a_index ? -1 : 1;
    if (a_index) return (int)(atol(a->key) - atol(b->key));
    return strcmp(a->key, b->key);
}

bool pixy_engine_palette(PixyEngine *engine, PixyPaletteEntry **entries_out, size_t *count_out,
                         long *slot_out) {
    lua_State *L = engine->L;
    *entries_out = NULL;
    *count_out = 0;
    if (slot_out) *slot_out = PIXY_PALETTE_DEFAULT_SLOT;

    lua_rawgeti(L, LUA_REGISTRYINDEX, engine->config_ref);
    lua_getfield(L, -1, "palette");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 2);
        return true;
    }
    /* Declaring a palette that is not a table is a mistake worth naming: the
     * alternative is a prompt that silently keeps the terminal's colours. */
    if (!lua_istable(L, -1)) {
        pixy_fail(PIXY_EXIT_CONFIG, "%s: palette must be a table, not a %s", engine->source_name,
                  luaL_typename(L, -1));
        lua_pop(L, 2);
        return false;
    }
    int palette = lua_gettop(L);

    lua_getfield(L, palette, "slot");
    if (!lua_isnil(L, -1)) {
        if (!lua_isinteger(L, -1)) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: palette slot must be a whole number 2-%d",
                      engine->source_name, PIXY_PALETTE_MAX_SLOT);
            lua_pop(L, 3);
            return false;
        }
        long slot = (long)lua_tointeger(L, -1);
        if (!pixy_palette_valid_slot(slot, true)) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: palette slot %ld is not claimable; use 2-%d",
                      engine->source_name, slot, PIXY_PALETTE_MAX_SLOT);
            lua_pop(L, 3);
            return false;
        }
        if (slot_out) *slot_out = slot;
    }
    lua_pop(L, 1);

    size_t capacity = 16, count = 0;
    PixyPaletteEntry *entries = calloc(capacity, sizeof(PixyPaletteEntry));
    if (!entries) {
        lua_pop(L, 2);
        return false;
    }

    lua_pushnil(L);
    while (lua_next(L, palette) != 0) {
        /* The key is read but never converted in place: doing that to a key
         * mid-traversal is what breaks lua_next. */
        char key[16] = {0};
        if (lua_type(L, -2) == LUA_TNUMBER) {
            if (!lua_isinteger(L, -2)) {
                pixy_fail(PIXY_EXIT_CONFIG, "%s: palette index %g is not a whole number 0-255",
                          engine->source_name, lua_tonumber(L, -2));
                free(entries);
                lua_pop(L, 4);
                return false;
            }
            snprintf(key, sizeof(key), "%lld", (long long)lua_tointeger(L, -2));
        } else if (lua_type(L, -2) == LUA_TSTRING) {
            const char *name = lua_tostring(L, -2);
            if (strcmp(name, "slot") == 0) {
                lua_pop(L, 1);
                continue;
            }
            if (strlen(name) >= sizeof(key)) {
                pixy_fail(PIXY_EXIT_CONFIG, "%s: palette key \"%s\" is not fg, bg or cursor",
                          engine->source_name, name);
                free(entries);
                lua_pop(L, 4);
                return false;
            }
            snprintf(key, sizeof(key), "%s", name);
        }
        /* A number would convert to a string here and read as a colour; only a
         * string was ever meant, so anything else is named as the error it is. */
        if (lua_type(L, -1) != LUA_TSTRING) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: palette colour for %s must be a string, not a %s",
                      engine->source_name, key[0] ? key : "?", luaL_typename(L, -1));
            free(entries);
            lua_pop(L, 4);
            return false;
        }
        const char *colour = lua_tostring(L, -1);
        if (!pixy_palette_valid_key(key)) {
            pixy_fail(PIXY_EXIT_CONFIG,
                      "%s: palette key \"%s\" is not an index 0-255, fg, bg or cursor",
                      engine->source_name, key);
            free(entries);
            lua_pop(L, 4);
            return false;
        }
        if (!pixy_palette_valid_colour(colour)) {
            pixy_fail(PIXY_EXIT_CONFIG, "%s: palette colour \"%s\" for %s is not #rrggbb",
                      engine->source_name, colour, key);
            free(entries);
            lua_pop(L, 4);
            return false;
        }
        if (count == capacity) {
            capacity *= 2;
            PixyPaletteEntry *grown = realloc(entries, capacity * sizeof(PixyPaletteEntry));
            if (!grown) {
                pixy_fail(PIXY_EXIT_TRANSPORT, "out of memory reading the palette");
                free(entries);
                lua_pop(L, 4);
                return false;
            }
            entries = grown;
        }
        snprintf(entries[count].key, sizeof(entries[count].key), "%s", key);
        snprintf(entries[count].colour, sizeof(entries[count].colour), "%s", colour);
        count++;
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    /* Lua hands a table back in hash order, which would make the same config
     * emit a different sequence between runs. Indexes ascending then the names
     * keeps a replay byte-identical and a diff readable. */
    qsort(entries, count, sizeof(entries[0]), compare_palette_entries);
    *entries_out = entries;
    *count_out = count;
    return true;
}
