/* Turning what Lua returned into the JSON a caller reads. The field order is
 * the Rust build's, because a painter or a test may compare bytes. */
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"

bool pixy_encode_runs(lua_State *L, int index, PixyBuf *out) {
    if (!lua_istable(L, index)) return pixy_buf_str(out, "[]");
    if (!pixy_buf_str(out, "[")) return false;
    lua_Integer len = (lua_Integer)lua_rawlen(L, index);
    for (lua_Integer i = 1; i <= len; i++) {
        if (i > 1 && !pixy_buf_str(out, ",")) return false;
        lua_rawgeti(L, index, i);
        if (!pixy_buf_str(out, "{\"text\":")) return false;
        lua_getfield(L, -1, "text");
        size_t text_len = 0;
        const char *text = lua_tolstring(L, -1, &text_len);
        if (!pixy_buf_json_string(out, text ? text : "", text_len)) return false;
        lua_pop(L, 1);
        if (!pixy_buf_str(out, ",\"style\":")) return false;
        lua_getfield(L, -1, "style");
        size_t style_len = 0;
        const char *style = lua_tolstring(L, -1, &style_len);
        if (!pixy_buf_json_string(out, style ? style : "", style_len)) return false;
        lua_pop(L, 1);
        if (!pixy_buf_str(out, "}")) return false;
        lua_pop(L, 1);
    }
    return pixy_buf_str(out, "]");
}

/* Sorted, because a table's iteration order is Lua's business and a painter
 * comparing frames byte for byte should not see it change. */
typedef struct {
    char key[128];
    char value[256];
} Pair;

static int compare_pairs(const void *left, const void *right) {
    return strcmp(((const Pair *)left)->key, ((const Pair *)right)->key);
}

static bool encode_actions(lua_State *L, int index, PixyBuf *out) {
    Pair pairs[32];
    size_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0 && count < 32) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) {
            snprintf(pairs[count].key, sizeof(pairs[count].key), "%s", lua_tostring(L, -2));
            snprintf(pairs[count].value, sizeof(pairs[count].value), "%s", lua_tostring(L, -1));
            count++;
        }
        lua_pop(L, 1);
    }
    qsort(pairs, count, sizeof(Pair), compare_pairs);
    if (!pixy_buf_str(out, "{")) return false;
    for (size_t i = 0; i < count; i++) {
        if (i && !pixy_buf_str(out, ",")) return false;
        if (!pixy_buf_json_string(out, pairs[i].key, strlen(pairs[i].key))) return false;
        if (!pixy_buf_str(out, ":")) return false;
        if (!pixy_buf_json_string(out, pairs[i].value, strlen(pairs[i].value))) return false;
    }
    return pixy_buf_str(out, "}");
}

bool pixy_encode_regions(lua_State *L, int index, PixyBuf *out) {
    lua_Integer len = (lua_Integer)lua_rawlen(L, index);
    if (len == 0) return true;
    if (!pixy_buf_str(out, "[")) return false;
    for (lua_Integer i = 1; i <= len; i++) {
        if (i > 1 && !pixy_buf_str(out, ",")) return false;
        lua_rawgeti(L, index, i);
        int region = lua_gettop(L);

        lua_getfield(L, region, "id");
        size_t id_len = 0;
        const char *id = lua_tolstring(L, -1, &id_len);
        if (!pixy_buf_str(out, "{\"id\":")) return false;
        if (!pixy_buf_json_string(out, id ? id : "", id_len)) return false;
        lua_pop(L, 1);

        static const char *numbers[] = {"x", "y", "width", "height"};
        for (size_t n = 0; n < sizeof(numbers) / sizeof(numbers[0]); n++) {
            lua_getfield(L, region, numbers[n]);
            if (!pixy_buf_fmt(out, ",\"%s\":%lld", numbers[n], (long long)lua_tointeger(L, -1)))
                return false;
            lua_pop(L, 1);
        }

        lua_getfield(L, region, "actions");
        if (lua_istable(L, -1)) {
            if (!pixy_buf_str(out, ",\"actions\":")) return false;
            if (!encode_actions(L, lua_gettop(L), out)) return false;
        }
        lua_pop(L, 1);

        static const char *strings[] = {"hover_style"};
        for (size_t n = 0; n < sizeof(strings) / sizeof(strings[0]); n++) {
            lua_getfield(L, region, strings[n]);
            if (lua_isstring(L, -1)) {
                size_t value_len = 0;
                const char *value = lua_tolstring(L, -1, &value_len);
                if (value_len) {
                    if (!pixy_buf_fmt(out, ",\"%s\":", strings[n])) return false;
                    if (!pixy_buf_json_string(out, value, value_len)) return false;
                }
            }
            lua_pop(L, 1);
        }

        lua_getfield(L, region, "press_styles");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            bool any = lua_next(L, -2) != 0;
            if (any) lua_pop(L, 2);
            if (any) {
                if (!pixy_buf_str(out, ",\"press_styles\":")) return false;
                if (!encode_actions(L, lua_gettop(L), out)) return false;
            }
        }
        lua_pop(L, 1);

        if (!pixy_buf_str(out, "}")) return false;
        lua_pop(L, 1);
    }
    return pixy_buf_str(out, "]");
}

bool pixy_output_json(const PixyOutput *output, PixyBuf *out) {
    if (output->mode == PIXY_MODE_SURFACE) {
        if (!pixy_buf_str(out, "{\"mode\":\"surface\",\"ansi\":")) return false;
        if (!pixy_buf_json_string(out, output->payload.data ? output->payload.data : "",
                                  output->payload.len))
            return false;
        if (!pixy_buf_fmt(out, ",\"width\":%zu,\"height\":%zu", output->width, output->height))
            return false;
    } else {
        if (!pixy_buf_str(out, "{\"mode\":\"run\",\"runs\":")) return false;
        if (!pixy_buf_str(out, output->runs_json.len ? output->runs_json.data : "[]")) return false;
        if (!pixy_buf_fmt(out, ",\"width\":%zu", output->width)) return false;
    }
    if (output->has_next_frame) {
        if (!pixy_buf_fmt(out, ",\"next_frame_ms\":%llu",
                          (unsigned long long)output->next_frame_ms))
            return false;
    } else {
        if (!pixy_buf_str(out, ",\"next_frame_ms\":null")) return false;
    }
    if (output->regions_json.len) {
        if (!pixy_buf_str(out, ",\"regions\":")) return false;
        if (!pixy_buf_str(out, output->regions_json.data)) return false;
    }
    return pixy_buf_str(out, "}");
}
