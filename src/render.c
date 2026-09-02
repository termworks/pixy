#include "render.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "pixy.h"

typedef enum { COLOR_NONE, COLOR_DEFAULT, COLOR_INDEX, COLOR_RGB } ColorKind;

typedef struct {
    ColorKind kind;
    int value[3];
} Color;

typedef struct {
    Color fg;
    Color bg;
    bool bold;
    bool dim;
    bool italic;
    bool underline;
    bool reverse;
} Style;

typedef struct Region {
    int ref;
    char *id;
} Region;

typedef struct {
    char *text;
    size_t len;
    size_t width;
    double flex;
    bool transparent;
    Style style;
    Region *region;
} Run;

typedef struct {
    Run *runs;
    size_t count;
    size_t capacity;
} Line;

typedef struct {
    Line *lines;
    size_t count;
    size_t capacity;
    bool has_next;
    uint64_t next_ms;
} Lines;

typedef struct {
    lua_State *L;
    int context;
    uint16_t width;
    uint16_t height;
    uint64_t now_ms;
    Region **regions;
    size_t region_count;
    size_t region_capacity;
} Render;

static void *grow(void *data, size_t *capacity, size_t count, size_t size) {
    if (count < *capacity) return data;
    size_t next = *capacity ? *capacity * 2 : 8;
    void *result = realloc(data, next * size);
    if (!result) return NULL;
    *capacity = next;
    return result;
}

static void line_free(Line *line) {
    for (size_t i = 0; i < line->count; i++) free(line->runs[i].text);
    free(line->runs);
    memset(line, 0, sizeof(*line));
}

static void lines_free(Lines *lines) {
    for (size_t i = 0; i < lines->count; i++) line_free(&lines->lines[i]);
    free(lines->lines);
    memset(lines, 0, sizeof(*lines));
}

static bool line_add(Line *line, Run run) {
    void *data = grow(line->runs, &line->capacity, line->count, sizeof(Run));
    if (!data) return false;
    line->runs = data;
    line->runs[line->count++] = run;
    return true;
}

static bool lines_add(Lines *lines, Line line) {
    void *data = grow(lines->lines, &lines->capacity, lines->count, sizeof(Line));
    if (!data) return false;
    lines->lines = data;
    lines->lines[lines->count++] = line;
    return true;
}

static bool lines_empty(Lines *lines) {
    Line line = (Line){0};
    return lines_add(lines, line);
}

static Run run_copy(const Run *source) {
    Run result = *source;
    result.text = malloc(source->len + 1);
    if (result.text) {
        memcpy(result.text, source->text, source->len);
        result.text[source->len] = 0;
    }
    return result;
}

static bool line_copy_into(Line *target, const Line *source) {
    for (size_t i = 0; i < source->count; i++) {
        Run copy = run_copy(&source->runs[i]);
        if (!copy.text || !line_add(target, copy)) {
            free(copy.text);
            return false;
        }
    }
    return true;
}

static size_t line_width(const Line *line) {
    size_t width = 0;
    for (size_t i = 0; i < line->count; i++) width += line->runs[i].width;
    return width;
}

static size_t lines_width(const Lines *lines) {
    size_t width = 0;
    for (size_t i = 0; i < lines->count; i++) {
        size_t current = line_width(&lines->lines[i]);
        if (current > width) width = current;
    }
    return width;
}

static void cadence(Lines *target, const Lines *source) {
    if (source->has_next && (!target->has_next || source->next_ms < target->next_ms)) {
        target->has_next = true;
        target->next_ms = source->next_ms;
    }
}

static Color color_field(lua_State *L, int table, const char *name) {
    static const struct {
        const char *name;
        int value;
    } names[] = {{"black", 0}, {"red", 1},     {"green", 2}, {"yellow", 3},
                 {"blue", 4},  {"magenta", 5}, {"cyan", 6},  {"white", 7}};
    Color color = (Color){0};
    lua_getfield(L, table, name);
    int type = lua_type(L, -1);
    if (type == LUA_TNIL) {
        lua_pop(L, 1);
        return color;
    }
    if (type == LUA_TNUMBER) {
        if (!lua_isinteger(L, -1)) luaL_error(L, "palette color must be 0-255");
        lua_Integer value = lua_tointeger(L, -1);
        if (value < 0 || value > 255) luaL_error(L, "palette color must be 0-255");
        color.kind = COLOR_INDEX;
        color.value[0] = (int)value;
    } else if (type == LUA_TSTRING) {
        const char *value = lua_tostring(L, -1);
        if (strcmp(value, "default") == 0) color.kind = COLOR_DEFAULT;
        else {
            bool found = false;
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                if (strcmp(value, names[i].name) == 0) {
                    color.kind = COLOR_INDEX;
                    color.value[0] = names[i].value;
                    found = true;
                    break;
                }
            }
            if (!found) luaL_error(L, "unknown color %s", value);
        }
    } else if (type == LUA_TTABLE && lua_rawlen(L, -1) == 3) {
        color.kind = COLOR_RGB;
        for (int i = 0; i < 3; i++) {
            lua_rawgeti(L, -1, i + 1);
            if (!lua_isinteger(L, -1)) luaL_error(L, "RGB color must be 0-255");
            lua_Integer value = lua_tointeger(L, -1);
            if (value < 0 || value > 255) luaL_error(L, "RGB color must be 0-255");
            color.value[i] = (int)value;
            lua_pop(L, 1);
        }
    } else {
        luaL_error(L, "invalid color");
    }
    lua_pop(L, 1);
    return color;
}

static Style style_merge(lua_State *L, Style base, int table) {
    if (!lua_istable(L, table)) return base;
    Color fg = color_field(L, table, "fg");
    Color bg = color_field(L, table, "bg");
    if (fg.kind != COLOR_NONE) base.fg = fg;
    if (bg.kind != COLOR_NONE) base.bg = bg;
    const char *names[] = {"bold", "dim", "italic", "underline", "reverse"};
    bool *fields[] = {&base.bold, &base.dim, &base.italic, &base.underline, &base.reverse};
    for (size_t i = 0; i < 5; i++) {
        lua_getfield(L, table, names[i]);
        if (!lua_isnil(L, -1)) {
            if (!lua_isboolean(L, -1)) luaL_error(L, "%s style must be boolean", names[i]);
            *fields[i] = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }
    return base;
}

static bool valid_text(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32 || ch == 127) return false;
    }
    return true;
}

static Run make_run(lua_State *L, const char *text, size_t len, Style style) {
    if (!valid_text(text, len)) luaL_error(L, "text contains a control byte");
    Run run = {.len = len, .width = pixy_cell_width(text, len), .style = style};
    run.text = malloc(len + 1);
    if (!run.text) luaL_error(L, "out of memory");
    memcpy(run.text, text, len);
    run.text[len] = 0;
    return run;
}

static Run make_skip(size_t width) {
    Run run = {.len = width, .width = width, .transparent = true};
    run.text = malloc(width + 1);
    if (run.text) {
        memset(run.text, ' ', width);
        run.text[width] = 0;
    }
    return run;
}

static Lines horizontal(lua_State *L, Lines *parts, size_t count) {
    Lines result = (Lines){0};
    size_t height = 1;
    for (size_t i = 0; i < count; i++) {
        if (parts[i].count > height) height = parts[i].count;
        cadence(&result, &parts[i]);
    }
    for (size_t row = 0; row < height; row++) {
        Line line = (Line){0};
        for (size_t part = 0; part < count; part++) {
            size_t block_width = lines_width(&parts[part]);
            size_t used = 0;
            if (row < parts[part].count) {
                const Line *source = &parts[part].lines[row];
                if (!line_copy_into(&line, source)) luaL_error(L, "out of memory");
                used = line_width(source);
            }
            if (block_width > used) {
                Run skip = make_skip(block_width - used);
                if (!skip.text || !line_add(&line, skip)) luaL_error(L, "out of memory");
            }
        }
        if (!lines_add(&result, line)) luaL_error(L, "out of memory");
    }
    return result;
}

static double number_field(lua_State *L, int table, const char *name, double fallback) {
    lua_getfield(L, table, name);
    double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

static const char *string_field(lua_State *L, int table, const char *name, const char *fallback) {
    lua_getfield(L, table, name);
    const char *value = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

static void push_context(Render *render) {
    lua_pushvalue(render->L, render->context);
}

static Region *region_new(Render *render, int options) {
    lua_State *L = render->L;
    lua_getfield(L, options, "id");
    const char *id = lua_tostring(L, -1);
    if (!id || !*id) luaL_error(L, "region requires a valid id");
    for (const char *p = id; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') &&
            ch != '_' && ch != '.' && ch != '-')
            luaL_error(L, "region requires a valid id");
    }
    Region *region = calloc(1, sizeof(*region));
    if (!region) luaL_error(L, "out of memory");
    region->id = strdup(id);
    lua_pop(L, 1);
    lua_pushvalue(L, options);
    region->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    void *data =
        grow(render->regions, &render->region_capacity, render->region_count, sizeof(Region *));
    if (!data) luaL_error(L, "out of memory");
    render->regions = data;
    render->regions[render->region_count++] = region;
    return region;
}

static void annotate(Render *render, Lines *lines, int options) {
    Region *region = region_new(render, options);
    lua_State *L = render->L;
    Style overlay = (Style){0};
    lua_getfield(L, options, "hover_style");
    lua_getfield(L, render->context, "values");
    lua_getfield(L, -1, "hover_region");
    bool hover = lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), region->id) == 0;
    lua_pop(L, 2);
    if (hover) overlay = style_merge(L, overlay, lua_gettop(L));
    lua_pop(L, 1);
    lua_getfield(L, render->context, "values");
    lua_getfield(L, -1, "press_region");
    bool pressed = lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), region->id) == 0;
    lua_pop(L, 1);
    if (pressed) {
        lua_getfield(L, -1, "press_button");
        const char *button = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
        lua_pop(L, 1);
        if (button) {
            lua_getfield(L, options, "press_styles");
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, button);
                overlay = style_merge(L, overlay, lua_gettop(L));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    for (size_t row = 0; row < lines->count; row++) {
        for (size_t i = 0; i < lines->lines[row].count; i++) {
            lines->lines[row].runs[i].region = region;
            Style current = lines->lines[row].runs[i].style;
            if (overlay.fg.kind != COLOR_NONE) current.fg = overlay.fg;
            if (overlay.bg.kind != COLOR_NONE) current.bg = overlay.bg;
            if (overlay.bold) current.bold = true;
            if (overlay.dim) current.dim = true;
            if (overlay.italic) current.italic = true;
            if (overlay.underline) current.underline = true;
            if (overlay.reverse) current.reverse = true;
            lines->lines[row].runs[i].style = current;
        }
    }
}

static Lines flatten(Render *render, int value, Style inherited);
static Lines flatten_list(Render *render, int table, Style inherited, bool vertical);

typedef struct {
    Lines lines;
    double priority;
    size_t index;
    bool present;
} NodePart;

static int node_part_compare(const void *left, const void *right) {
    const NodePart *const *a = left;
    const NodePart *const *b = right;
    if ((*a)->priority < (*b)->priority) return 1;
    if ((*a)->priority > (*b)->priority) return -1;
    return (*a)->index < (*b)->index ? 1 : -1;
}

static Lines flatten_segments_node(Render *render, int table, Style inherited) {
    lua_State *L = render->L;
    size_t count = lua_rawlen(L, table);
    NodePart *parts = calloc(count ? count : 1, sizeof(NodePart));
    NodePart **order = calloc(count ? count : 1, sizeof(NodePart *));
    Lines *visible = calloc(count ? count : 1, sizeof(Lines));
    if (!parts || !order || !visible) luaL_error(L, "out of memory");
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        lua_rawgeti(L, table, (lua_Integer)i + 1);
        parts[i].lines = flatten(render, lua_gettop(L), inherited);
        parts[i].priority = lua_istable(L, -1) ? number_field(L, lua_gettop(L), "priority", 0) : 0;
        parts[i].index = i;
        parts[i].present = true;
        order[i] = &parts[i];
        total += lines_width(&parts[i].lines);
        lua_pop(L, 1);
    }
    qsort(order, count, sizeof(NodePart *), node_part_compare);
    size_t remaining = count;
    for (size_t i = 0; i < count && total > render->width && remaining > 1; i++) {
        size_t width = lines_width(&order[i]->lines);
        if (!width) continue;
        order[i]->present = false;
        total -= width;
        remaining--;
    }
    size_t visible_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (parts[i].present) visible[visible_count++] = parts[i].lines;
    }
    Lines result = horizontal(L, visible, visible_count);
    for (size_t i = 0; i < count; i++) lines_free(&parts[i].lines);
    free(visible);
    free(order);
    free(parts);
    return result;
}

static void append_at(lua_State *L, Line *line, size_t x, const Line *source, size_t width) {
    size_t cursor = line_width(line);
    if (x > cursor) {
        Run skip = make_skip(x - cursor);
        if (!skip.text || !line_add(line, skip)) luaL_error(L, "out of memory");
    }
    if (!line_copy_into(line, source)) luaL_error(L, "out of memory");
    size_t used = line_width(source);
    if (width > used) {
        Run skip = make_skip(width - used);
        if (!skip.text || !line_add(line, skip)) luaL_error(L, "out of memory");
    }
}

static Lines flatten_regions_node(Render *render, int value, Style inherited) {
    lua_State *L = render->L;
    const char *fields[] = {"left", "center", "right"};
    Lines parts[3];
    memset(parts, 0, sizeof(parts));
    size_t widths[3];
    size_t height = 1;
    Lines result = {0};
    for (size_t i = 0; i < 3; i++) {
        lua_getfield(L, value, fields[i]);
        parts[i] = flatten_list(render, lua_gettop(L), inherited, false);
        lua_pop(L, 1);
        widths[i] = lines_width(&parts[i]);
        if (parts[i].count > height) height = parts[i].count;
        cadence(&result, &parts[i]);
    }
    size_t positions[] = {0, render->width > widths[1] ? (render->width - widths[1]) / 2 : 0,
                          render->width > widths[2] ? render->width - widths[2] : 0};
    for (size_t row = 0; row < height; row++) {
        Line line = {0};
        for (size_t part = 0; part < 3; part++) {
            if (!widths[part] || row >= parts[part].count) continue;
            append_at(L, &line, positions[part], &parts[part].lines[row], widths[part]);
        }
        if (!lines_add(&result, line)) luaL_error(L, "out of memory");
    }
    for (size_t i = 0; i < 3; i++) lines_free(&parts[i]);
    return result;
}

static Lines flatten_list(Render *render, int table, Style inherited, bool vertical) {
    lua_State *L = render->L;
    size_t count = lua_rawlen(L, table);
    if (vertical) {
        Lines result = (Lines){0};
        for (size_t i = 0; i < count; i++) {
            lua_rawgeti(L, table, (lua_Integer)i + 1);
            Lines child = flatten(render, lua_gettop(L), inherited);
            lua_pop(L, 1);
            cadence(&result, &child);
            for (size_t row = 0; row < child.count; row++) {
                Line line = (Line){0};
                if (!line_copy_into(&line, &child.lines[row]) || !lines_add(&result, line))
                    luaL_error(L, "out of memory");
            }
            lines_free(&child);
        }
        if (!result.count) lines_empty(&result);
        return result;
    }
    Lines *parts = calloc(count ? count : 1, sizeof(Lines));
    if (!parts) luaL_error(L, "out of memory");
    for (size_t i = 0; i < count; i++) {
        lua_rawgeti(L, table, (lua_Integer)i + 1);
        parts[i] = flatten(render, lua_gettop(L), inherited);
        lua_pop(L, 1);
    }
    Lines result = horizontal(L, parts, count);
    for (size_t i = 0; i < count; i++) lines_free(&parts[i]);
    free(parts);
    return result;
}

static size_t utf8_prefix(const char *text, size_t len, size_t cells) {
    size_t best = 0;
    for (size_t at = 0; at < len;) {
        unsigned char ch = (unsigned char)text[at];
        size_t step = ch < 0x80 ? 1 : (ch & 0xe0) == 0xc0 ? 2 : (ch & 0xf0) == 0xe0 ? 3 : 4;
        if (at + step > len) step = 1;
        if (pixy_cell_width(text, at + step) > cells) break;
        best = at + step;
        at += step;
    }
    return best;
}

static void truncate_lines(lua_State *L, Lines *lines, size_t limit, const char *marker) {
    size_t marker_len = strlen(marker);
    size_t marker_width = pixy_cell_width(marker, marker_len);
    for (size_t row = 0; row < lines->count; row++) {
        Line *line = &lines->lines[row];
        size_t total = line_width(line);
        if (total <= limit) continue;
        size_t content_limit = marker_width <= limit ? limit - marker_width : limit;
        Line clipped = (Line){0};
        size_t used = 0;
        for (size_t i = 0; i < line->count && used < content_limit; i++) {
            Run *run = &line->runs[i];
            size_t room = content_limit - used;
            if (run->transparent) {
                size_t width = run->width < room ? run->width : room;
                Run skip = make_skip(width);
                skip.region = run->region;
                if (!skip.text || !line_add(&clipped, skip)) luaL_error(L, "out of memory");
                used += width;
            } else {
                size_t bytes =
                    run->width <= room ? run->len : utf8_prefix(run->text, run->len, room);
                if (bytes) {
                    Run copy = make_run(L, run->text, bytes, run->style);
                    copy.region = run->region;
                    if (!line_add(&clipped, copy)) luaL_error(L, "out of memory");
                    used += copy.width;
                }
            }
        }
        if (marker_width <= limit && marker_len) {
            Run mark = make_run(L, marker, marker_len, (Style){0});
            if (!line_add(&clipped, mark)) luaL_error(L, "out of memory");
        }
        line_free(line);
        *line = clipped;
    }
}

static Lines flatten_spinner(Render *render, int value, Style inherited) {
    lua_State *L = render->L;
    const char *spinner = string_field(L, value, "spinner", NULL);
    if (!spinner) spinner = string_field(L, value, "kind", NULL);
    if (spinner && strcmp(spinner, "knight_rider") == 0) {
        long width = (long)number_field(L, value, "width", 8);
        long step = (long)number_field(L, value, "step_ms", number_field(L, value, "step", 75));
        long hold = (long)number_field(L, value, "hold_frames", number_field(L, value, "hold", 9));
        long trail = (long)number_field(L, value, "trail_len", number_field(L, value, "trail", 6));
        if (width < 2) width = 2;
        if (width > 32) width = 32;
        if (step < 1) step = 75;
        if (hold < 1) hold = 9;
        if (trail < 1) trail = 6;
        long started = (long)number_field(L, value, "started_at_ms", 0);
        long elapsed = (long)render->now_ms - started;
        if (elapsed < 0) elapsed = 0;
        long cycle = width + hold + width - 1 + hold;
        long frame = (elapsed / step) % cycle;
        long position;
        bool forward;
        long hold_progress = 0;
        if (frame < width) {
            position = frame;
            forward = true;
        } else if (frame < width + hold) {
            position = width - 1;
            forward = true;
            hold_progress = frame - width;
        } else if (frame < width + hold + width - 1) {
            position = width - 2 - (frame - width - hold);
            forward = false;
        } else {
            position = 0;
            forward = false;
            hold_progress = frame - (width + hold + width - 1);
        }
        Lines result = (Lines){0};
        Line line = (Line){0};
        lua_getfield(L, value, "colors");
        int colors = lua_gettop(L);
        size_t color_count = lua_istable(L, colors) ? lua_rawlen(L, colors) : 0;
        for (long cell = 0; cell < width; cell++) {
            long distance = forward ? position - cell : cell - position;
            distance += hold_progress;
            Style style = inherited;
            if (distance >= 0 && distance < trail && color_count) {
                size_t index = (size_t)distance + 1;
                if (index > color_count) index = color_count;
                lua_rawgeti(L, colors, (lua_Integer)index);
                if (lua_isinteger(L, -1)) {
                    style.fg.kind = COLOR_INDEX;
                    style.fg.value[0] = (int)lua_tointeger(L, -1);
                }
                lua_pop(L, 1);
            }
            const char *glyph = distance >= 0 && distance < trail ? "■" : "⬝";
            Run run = make_run(L, glyph, strlen(glyph), style);
            if (!line_add(&line, run)) luaL_error(L, "out of memory");
        }
        lua_pop(L, 1);
        lines_add(&result, line);
        result.has_next = true;
        result.next_ms = (uint64_t)(step - elapsed % step);
        return result;
    }
    lua_getfield(L, value, "frames");
    int frames = lua_gettop(L);
    if (!lua_istable(L, frames)) {
        lua_pop(L, 1);
        lua_createtable(L, 4, 0);
        const char *defaults[] = {"-", "\\", "|", "/"};
        for (int i = 0; i < 4; i++) {
            lua_pushstring(L, defaults[i]);
            lua_rawseti(L, -2, i + 1);
        }
        frames = lua_gettop(L);
    }
    size_t count = lua_rawlen(L, frames);
    Lines result = (Lines){0};
    if (!count) {
        lua_pop(L, 1);
        lines_empty(&result);
        return result;
    }
    long interval = (long)number_field(L, value, "interval_ms", 80);
    if (interval < 1) interval = 1;
    long started = (long)number_field(L, value, "started_at_ms", 0);
    if (!started) {
        lua_getfield(L, render->context, "values");
        started = (long)number_field(L, lua_gettop(L), "started_at_ms", 0);
        lua_pop(L, 1);
    }
    long elapsed = (long)render->now_ms - started;
    if (elapsed < 0) elapsed = 0;
    size_t index = (size_t)(elapsed / interval) % count + 1;
    lua_rawgeti(L, frames, (lua_Integer)index);
    size_t len = 0;
    const char *text = luaL_tolstring(L, -1, &len);
    lua_getfield(L, value, "style");
    Style style = style_merge(L, inherited, lua_gettop(L));
    lua_pop(L, 1);
    Line line = (Line){0};
    Run run = make_run(L, text, len, style);
    line_add(&line, run);
    lines_add(&result, line);
    lua_pop(L, 2);
    if (count > 1) {
        result.has_next = true;
        result.next_ms = (uint64_t)(interval - elapsed % interval);
    }
    return result;
}

static char *asset(Render *render, int value, size_t *len) {
    lua_State *L = render->L;
    const char *pack = string_field(L, value, "pack", NULL);
    const char *name = string_field(L, value, "name", NULL);
    if (!pack || !name || strstr(name, "..")) return NULL;
    unsigned char *bytes = pixy_embedded_item(pack, name, len);
    if (bytes) return (char *)bytes;
    lua_getglobal(L, "__pixy_host");
    lua_getfield(L, -1, "asset");
    lua_pushstring(L, pack);
    lua_pushstring(L, name);
    lua_call(L, 2, 1);
    const char *found = lua_tolstring(L, -1, len);
    char *copy = found ? strndup(found, *len) : NULL;
    lua_pop(L, 2);
    return copy;
}

static bool table_bool(lua_State *L, int table, const char *name, bool fallback) {
    lua_getfield(L, table, name);
    bool value = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

static void sprite_text(lua_State *L, Line *line, const char *text, size_t len, Style style,
                        bool transparent) {
    size_t start = 0;
    while (start < len) {
        size_t stop = start;
        bool spaces = text[start] == ' ';
        while (stop < len && (text[stop] == ' ') == spaces) stop++;
        Run run = spaces && transparent ? make_skip(stop - start)
                                        : make_run(L, text + start, stop - start, style);
        if (!run.text || !line_add(line, run)) luaL_error(L, "out of memory");
        start = stop;
    }
}

static int sgr_number(const char *text, size_t len, size_t *at) {
    int value = 0;
    bool any = false;
    while (*at < len && text[*at] >= '0' && text[*at] <= '9') {
        value = value * 10 + text[*at] - '0';
        (*at)++;
        any = true;
    }
    return any ? value : 0;
}

static void sprite_sgr(lua_State *L, Style *style, Style base, const char *text, size_t len) {
    size_t at = 0;
    while (at <= len) {
        int code = sgr_number(text, len, &at);
        if (code == 0) *style = base;
        else if (code == 1) style->bold = true;
        else if (code == 2) style->dim = true;
        else if (code == 3) style->italic = true;
        else if (code == 4) style->underline = true;
        else if (code == 7) style->reverse = true;
        else if (code == 22) style->bold = style->dim = false;
        else if (code == 23) style->italic = false;
        else if (code == 24) style->underline = false;
        else if (code == 27) style->reverse = false;
        else if (code >= 30 && code <= 37) {
            style->fg.kind = COLOR_INDEX;
            style->fg.value[0] = code - 30;
        } else if (code >= 40 && code <= 47) {
            style->bg.kind = COLOR_INDEX;
            style->bg.value[0] = code - 40;
        } else if (code >= 90 && code <= 97) {
            style->fg.kind = COLOR_INDEX;
            style->fg.value[0] = code - 90 + 8;
        } else if (code >= 100 && code <= 107) {
            style->bg.kind = COLOR_INDEX;
            style->bg.value[0] = code - 100 + 8;
        } else if (code == 39) style->fg.kind = COLOR_DEFAULT;
        else if (code == 49) style->bg.kind = COLOR_DEFAULT;
        else if (code == 38 || code == 48) {
            if (at >= len || text[at++] != ';') luaL_error(L, "invalid SGR color");
            int mode = sgr_number(text, len, &at);
            Color *color = code == 38 ? &style->fg : &style->bg;
            if (mode == 5) {
                if (at >= len || text[at++] != ';') luaL_error(L, "invalid SGR color");
                int value = sgr_number(text, len, &at);
                if (value < 0 || value > 255) luaL_error(L, "invalid SGR color");
                color->kind = COLOR_INDEX;
                color->value[0] = value;
            } else if (mode == 2) {
                color->kind = COLOR_RGB;
                for (int i = 0; i < 3; i++) {
                    if (at >= len || text[at++] != ';') luaL_error(L, "invalid SGR color");
                    color->value[i] = sgr_number(text, len, &at);
                    if (color->value[i] < 0 || color->value[i] > 255)
                        luaL_error(L, "invalid SGR color");
                }
            } else {
                luaL_error(L, "unsupported SGR color mode");
            }
        }
        if (at >= len) break;
        if (text[at++] != ';') luaL_error(L, "invalid SGR parameter");
    }
}

static Line sprite_line(lua_State *L, const char *text, size_t len, Style base, bool transparent) {
    Line line = {0};
    Style current = base;
    size_t start = 0;
    for (size_t at = 0; at < len;) {
        if ((unsigned char)text[at] != 27) {
            at++;
            continue;
        }
        if (at > start) sprite_text(L, &line, text + start, at - start, current, transparent);
        if (at + 1 >= len || text[at + 1] != '[') luaL_error(L, "sprite contains a non-SGR escape");
        const char *end = memchr(text + at + 2, 'm', len - at - 2);
        if (!end || (size_t)(end - text - at) > 128)
            luaL_error(L, "sprite contains an invalid SGR escape");
        sprite_sgr(L, &current, base, text + at + 2, (size_t)(end - text - at - 2));
        at = (size_t)(end - text) + 1;
        start = at;
    }
    if (start < len) sprite_text(L, &line, text + start, len - start, current, transparent);
    return line;
}

static Lines flatten_sprite(Render *render, int value, Style inherited) {
    lua_State *L = render->L;
    char *owned = NULL;
    const char *frame = NULL;
    size_t len = 0;
    lua_getfield(L, value, "frames");
    if (lua_istable(L, -1) && lua_rawlen(L, -1)) {
        size_t count = lua_rawlen(L, -1);
        long interval = (long)number_field(L, value, "interval_ms", 100);
        if (interval < 1) interval = 1;
        long started = (long)number_field(L, value, "started_at_ms", 0);
        long elapsed = (long)render->now_ms - started;
        if (elapsed < 0) elapsed = 0;
        size_t index = (size_t)(elapsed / interval) % count + 1;
        lua_rawgeti(L, -1, (lua_Integer)index);
        frame = lua_tolstring(L, -1, &len);
        if (frame) owned = strndup(frame, len);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (!owned) owned = asset(render, value, &len);
    if (!owned) {
        const char *fallback = string_field(L, value, "fallback_name", NULL);
        if (fallback) {
            lua_pushstring(L, fallback);
            lua_setfield(L, value, "name");
            owned = asset(render, value, &len);
        }
    }
    Lines result = (Lines){0};
    if (!owned) {
        lines_empty(&result);
        return result;
    }
    Style base = inherited;
    lua_getfield(L, value, "style");
    base = style_merge(L, base, lua_gettop(L));
    lua_pop(L, 1);
    bool transparent = table_bool(L, value, "transparent", true);
    const char *start = owned;
    const char *end = owned + len;
    while (start <= end) {
        const char *stop = memchr(start, '\n', (size_t)(end - start));
        if (!stop) stop = end;
        size_t line_len = (size_t)(stop - start);
        if (line_len && start[line_len - 1] == '\r') line_len--;
        Line line = sprite_line(L, start, line_len, base, transparent);
        if (!lines_add(&result, line)) luaL_error(L, "out of memory");
        if (stop == end) break;
        start = stop + 1;
        if (start == end) break;
    }
    free(owned);
    return result;
}

static Lines flatten(Render *render, int value, Style inherited) {
    lua_State *L = render->L;
    value = lua_absindex(L, value);
    Lines result = (Lines){0};
    int type = lua_type(L, value);
    if (type == LUA_TNIL || (type == LUA_TBOOLEAN && !lua_toboolean(L, value))) {
        lines_empty(&result);
        return result;
    }
    if (type == LUA_TSTRING || type == LUA_TNUMBER) {
        size_t len = 0;
        const char *text = lua_tolstring(L, value, &len);
        Line line = (Line){0};
        Run run = make_run(L, text, len, inherited);
        line_add(&line, run);
        lines_add(&result, line);
        return result;
    }
    if (type != LUA_TTABLE)
        luaL_error(L, "segment returned unsupported %s", luaL_typename(L, value));
    const char *kind = string_field(L, value, "kind", NULL);
    if (!kind) luaL_error(L, "segment returned a table without a node kind");
    if (strcmp(kind, "text") == 0) {
        lua_getfield(L, value, "text");
        size_t len = 0;
        const char *text = lua_tolstring(L, -1, &len);
        lua_getfield(L, value, "style");
        Style style = style_merge(L, inherited, lua_gettop(L));
        lua_pop(L, 1);
        Line line = (Line){0};
        Run run = make_run(L, text ? text : "", text ? len : 0, style);
        line_add(&line, run);
        lines_add(&result, line);
        lua_pop(L, 1);
        double next = number_field(L, value, "next_frame_ms", 0);
        if (next > 0) {
            result.has_next = true;
            result.next_ms = (uint64_t)next;
        }
        return result;
    }
    if (strcmp(kind, "transparent") == 0 || strcmp(kind, "spacer") == 0) {
        Line line = (Line){0};
        Run run = make_skip(strcmp(kind, "transparent") == 0
                                ? (size_t)fmax(0, number_field(L, value, "width", 0))
                                : 0);
        if (strcmp(kind, "spacer") == 0) run.flex = fmax(0, number_field(L, value, "weight", 1));
        if (!run.text || !line_add(&line, run) || !lines_add(&result, line))
            luaL_error(L, "out of memory");
        return result;
    }
    if (strcmp(kind, "regions") == 0) return flatten_regions_node(render, value, inherited);
    if (strcmp(kind, "row") == 0 || strcmp(kind, "segments") == 0 || strcmp(kind, "column") == 0 ||
        strcmp(kind, "surface") == 0) {
        const char *field = strcmp(kind, "surface") == 0 ? "lines" : "children";
        lua_getfield(L, value, field);
        if (strcmp(kind, "segments") == 0)
            result = flatten_segments_node(render, lua_gettop(L), inherited);
        else
            result = flatten_list(render, lua_gettop(L), inherited,
                                  strcmp(kind, "column") == 0 || strcmp(kind, "surface") == 0);
        lua_pop(L, 1);
        return result;
    }
    if (strcmp(kind, "pad") == 0) {
        lua_getfield(L, value, "value");
        result = flatten(render, lua_gettop(L), inherited);
        lua_pop(L, 1);
        lua_getfield(L, value, "padding");
        int padding = lua_gettop(L);
        size_t left = (size_t)fmax(0, number_field(L, padding, "left", 0));
        size_t right = (size_t)fmax(0, number_field(L, padding, "right", 0));
        size_t top = (size_t)fmax(0, number_field(L, padding, "top", 0));
        size_t bottom = (size_t)fmax(0, number_field(L, padding, "bottom", 0));
        lua_pop(L, 1);
        for (size_t row = 0; row < result.count; row++) {
            Line padded = (Line){0};
            if (left)
                line_add(&padded,
                         make_run(L, "                    ", left > 20 ? 20 : left, inherited));
            line_copy_into(&padded, &result.lines[row]);
            if (right) {
                Run skip = make_skip(right);
                skip.transparent = false;
                skip.style = inherited;
                line_add(&padded, skip);
            }
            line_free(&result.lines[row]);
            result.lines[row] = padded;
        }
        for (size_t i = 0; i < top; i++) {
            Line blank = (Line){0};
            void *data = grow(result.lines, &result.capacity, result.count, sizeof(Line));
            if (!data) luaL_error(L, "out of memory");
            result.lines = data;
            memmove(result.lines + 1, result.lines, result.count * sizeof(Line));
            result.lines[0] = blank;
            result.count++;
        }
        for (size_t i = 0; i < bottom; i++) lines_add(&result, (Line){0});
        return result;
    }
    if (strcmp(kind, "priority") == 0 || strcmp(kind, "item") == 0 || strcmp(kind, "region") == 0 ||
        strcmp(kind, "style") == 0 || strcmp(kind, "truncate") == 0) {
        Style next = inherited;
        if (strcmp(kind, "style") == 0) {
            lua_getfield(L, value, "style");
            next = style_merge(L, inherited, lua_gettop(L));
            lua_pop(L, 1);
        }
        lua_getfield(L, value, "value");
        result = flatten(render, lua_gettop(L), next);
        lua_pop(L, 1);
        if (strcmp(kind, "item") == 0 || strcmp(kind, "region") == 0) {
            lua_getfield(L, value, "id");
            bool has_id = lua_isstring(L, -1);
            lua_pop(L, 1);
            if (has_id) annotate(render, &result, value);
        }
        if (strcmp(kind, "truncate") == 0) {
            size_t limit = (size_t)fmax(0, number_field(L, value, "width", 0));
            const char *marker = string_field(L, value, "marker", "");
            truncate_lines(L, &result, limit, marker);
        }
        return result;
    }
    if (strcmp(kind, "spinner") == 0) return flatten_spinner(render, value, inherited);
    if (strcmp(kind, "animate") == 0) {
        lua_getfield(L, value, "callback");
        if (!lua_isfunction(L, -1)) luaL_error(L, "animate requires a function");
        push_context(render);
        lua_call(L, 1, 1);
        result = flatten(render, lua_gettop(L), inherited);
        lua_pop(L, 1);
        long interval = (long)number_field(L, value, "interval_ms", 0);
        if (interval > 0) {
            long started = (long)number_field(L, value, "started_at_ms", 0);
            long elapsed = (long)render->now_ms - started;
            if (elapsed < 0) elapsed = 0;
            uint64_t next = (uint64_t)(interval - elapsed % interval);
            if (!result.has_next || next < result.next_ms) {
                result.has_next = true;
                result.next_ms = next;
            }
        }
        return result;
    }
    if (strcmp(kind, "sprite") == 0) return flatten_sprite(render, value, inherited);
    luaL_error(L, "unknown node kind %s", kind);
    return result;
}

static void resolve_flex(Lines *lines, size_t width) {
    for (size_t row = 0; row < lines->count; row++) {
        Line *line = &lines->lines[row];
        size_t content = line_width(line);
        double total = 0;
        for (size_t i = 0; i < line->count; i++) total += line->runs[i].flex;
        if (total <= 0) continue;
        size_t slack = content < width ? width - content : 0;
        double seen = 0;
        size_t given = 0;
        for (size_t i = 0; i < line->count; i++) {
            Run *run = &line->runs[i];
            if (run->flex <= 0) continue;
            seen += run->flex;
            size_t target = (size_t)floor(slack * seen / total);
            size_t amount = target - given;
            free(run->text);
            *run = make_skip(amount);
            run->flex = 1;
            given = target;
        }
    }
}

static void append_color(PixyBuf *out, int prefix, Color color, bool *first) {
    if (color.kind == COLOR_NONE) return;
    if (!*first) pixy_buf_str(out, ";");
    *first = false;
    if (color.kind == COLOR_DEFAULT) pixy_buf_fmt(out, "%d", prefix == 38 ? 39 : 49);
    else if (color.kind == COLOR_INDEX) pixy_buf_fmt(out, "%d;5;%d", prefix, color.value[0]);
    else pixy_buf_fmt(out, "%d;2;%d;%d;%d", prefix, color.value[0], color.value[1], color.value[2]);
}

static void style_sgr(PixyBuf *out, Style style) {
    bool any = style.fg.kind != COLOR_NONE || style.bg.kind != COLOR_NONE || style.bold ||
               style.dim || style.italic || style.underline || style.reverse;
    if (!any) return;
    pixy_buf_str(out, "\033[");
    bool first = true;
    append_color(out, 38, style.fg, &first);
    append_color(out, 48, style.bg, &first);
    const int codes[] = {1, 2, 3, 4, 7};
    const bool fields[] = {style.bold, style.dim, style.italic, style.underline, style.reverse};
    for (size_t i = 0; i < 5; i++) {
        if (!fields[i]) continue;
        if (!first) pixy_buf_str(out, ";");
        first = false;
        pixy_buf_fmt(out, "%d", codes[i]);
    }
    pixy_buf_str(out, "m");
}

static void style_description(PixyBuf *out, Style style) {
    Color fg = style.fg, bg = style.bg;
    if (style.reverse && fg.kind != COLOR_NONE && fg.kind != COLOR_DEFAULT &&
        bg.kind != COLOR_NONE && bg.kind != COLOR_DEFAULT) {
        Color swap = fg;
        fg = bg;
        bg = swap;
    }
    bool first = true;
    Color colors[] = {fg, bg};
    const char *prefix[] = {"fg:", "bg:"};
    for (size_t i = 0; i < 2; i++) {
        if (colors[i].kind == COLOR_NONE || colors[i].kind == COLOR_DEFAULT) continue;
        if (!first) pixy_buf_str(out, " ");
        first = false;
        pixy_buf_str(out, prefix[i]);
        if (colors[i].kind == COLOR_INDEX) pixy_buf_fmt(out, "%d", colors[i].value[0]);
        else
            pixy_buf_fmt(out, "#%02x%02x%02x", colors[i].value[0], colors[i].value[1],
                         colors[i].value[2]);
    }
    const char *names[] = {"bold", "dim", "italic", "underline"};
    bool fields[] = {style.bold, style.dim, style.italic, style.underline};
    for (size_t i = 0; i < 4; i++) {
        if (!fields[i]) continue;
        if (!first) pixy_buf_str(out, " ");
        first = false;
        pixy_buf_str(out, names[i]);
    }
}

static void push_runs(lua_State *L, const Lines *lines) {
    lua_newtable(L);
    if (!lines->count) return;
    const Line *line = &lines->lines[0];
    size_t output = 0;
    for (size_t i = 0; i < line->count; i++) {
        const Run *run = &line->runs[i];
        PixyBuf description = (PixyBuf){0};
        style_description(&description, run->style);
        lua_newtable(L);
        lua_pushlstring(L, run->text, run->len);
        lua_setfield(L, -2, "text");
        lua_pushlstring(L, description.data ? description.data : "", description.len);
        lua_setfield(L, -2, "style");
        lua_rawseti(L, -2, (lua_Integer)++output);
        pixy_buf_free(&description);
    }
}

static void push_regions(Render *render, const Lines *lines) {
    lua_State *L = render->L;
    lua_newtable(L);
    size_t output = 0;
    for (size_t r = 0; r < render->region_count; r++) {
        Region *region = render->regions[r];
        bool found = false;
        size_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
        for (size_t y = 0; y < lines->count; y++) {
            size_t x = 0;
            for (size_t i = 0; i < lines->lines[y].count; i++) {
                const Run *run = &lines->lines[y].runs[i];
                if (run->region == region && run->width) {
                    if (!found) min_x = x, min_y = y, max_x = x + run->width, max_y = y + 1;
                    else {
                        if (x < min_x) min_x = x;
                        if (y < min_y) min_y = y;
                        if (x + run->width > max_x) max_x = x + run->width;
                        if (y + 1 > max_y) max_y = y + 1;
                    }
                    found = true;
                }
                x += run->width;
            }
        }
        if (!found) continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, region->ref);
        lua_pushstring(L, region->id);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, (lua_Integer)min_x);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, (lua_Integer)min_y);
        lua_setfield(L, -2, "y");
        lua_pushinteger(L, (lua_Integer)(max_x - min_x));
        lua_setfield(L, -2, "width");
        lua_pushinteger(L, (lua_Integer)(max_y - min_y));
        lua_setfield(L, -2, "height");
        lua_rawseti(L, -2, (lua_Integer)++output);
    }
}

static void push_line(Render *render, const Lines *lines, const char *target) {
    lua_State *L = render->L;
    if (lines->count > 1) luaL_error(L, "line output cannot contain multiple lines");
    PixyBuf out = (PixyBuf){0};
    const Line *line = lines->count ? &lines->lines[0] : NULL;
    for (size_t i = 0; line && i < line->count; i++) {
        const Run *run = &line->runs[i];
        if (strcmp(target, "plain") == 0) {
            pixy_buf_add(&out, run->text, run->len);
            continue;
        }
        PixyBuf styled = (PixyBuf){0};
        style_sgr(&styled, run->style);
        if (strcmp(target, "bash") == 0 && styled.len) pixy_buf_str(&out, "\\[");
        if (strcmp(target, "zsh") == 0 && styled.len) pixy_buf_str(&out, "%{");
        pixy_buf_add(&out, styled.data ? styled.data : "", styled.len);
        if (strcmp(target, "bash") == 0 && styled.len) pixy_buf_str(&out, "\\]");
        if (strcmp(target, "zsh") == 0 && styled.len) pixy_buf_str(&out, "%}");
        pixy_buf_add(&out, run->text, run->len);
        if (styled.len) {
            if (strcmp(target, "bash") == 0) pixy_buf_str(&out, "\\[\033[0m\\]");
            else if (strcmp(target, "zsh") == 0) pixy_buf_str(&out, "%{\033[0m%}");
            else pixy_buf_str(&out, "\033[0m");
        }
        pixy_buf_free(&styled);
    }
    lua_pushlstring(L, out.data ? out.data : "", out.len);
    pixy_buf_free(&out);
}

static void push_surface(Render *render, Lines *lines) {
    lua_State *L = render->L;
    if (lines->count > render->height) {
        for (size_t i = render->height; i < lines->count; i++) line_free(&lines->lines[i]);
        lines->count = render->height;
    }
    truncate_lines(L, lines, render->width, "");
    PixyBuf out = (PixyBuf){0};
    for (size_t row = 0; row < lines->count; row++) {
        if (row) pixy_buf_str(&out, "\r\n");
        const Line *line = &lines->lines[row];
        for (size_t i = 0; i < line->count; i++) {
            const Run *run = &line->runs[i];
            if (run->transparent) {
                if (run->width) pixy_buf_fmt(&out, "\033[%zuC", run->width);
            } else {
                style_sgr(&out, run->style);
                pixy_buf_add(&out, run->text, run->len);
                if (run->style.fg.kind != COLOR_NONE || run->style.bg.kind != COLOR_NONE ||
                    run->style.bold || run->style.dim || run->style.italic ||
                    run->style.underline || run->style.reverse)
                    pixy_buf_str(&out, "\033[0m");
            }
        }
    }
    lua_pushlstring(L, out.data ? out.data : "", out.len);
    pixy_buf_free(&out);
}

typedef struct {
    Lines lines;
    double priority;
    size_t index;
    bool present;
} Entry;

typedef struct {
    size_t index;
    double priority;
} Removal;

static int removal_compare(const void *left, const void *right) {
    const Entry *const *a = left;
    const Entry *const *b = right;
    if ((*a)->priority < (*b)->priority) return 1;
    if ((*a)->priority > (*b)->priority) return -1;
    return (*a)->index < (*b)->index ? 1 : -1;
}

static int part_removal_compare(const void *left, const void *right) {
    const Removal *a = left;
    const Removal *b = right;
    if (a->priority < b->priority) return 1;
    if (a->priority > b->priority) return -1;
    return a->index < b->index ? 1 : -1;
}

static void set_context(Render *render, int request) {
    lua_State *L = render->L;
    lua_getfield(L, request, "context");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    render->context = lua_absindex(L, -1);
    lua_pushinteger(L, (lua_Integer)render->now_ms);
    lua_setfield(L, render->context, "now_ms");
    lua_pushinteger(L, render->width);
    lua_setfield(L, render->context, "width");
    lua_pushinteger(L, render->height);
    lua_setfield(L, render->context, "height");
    lua_getfield(L, render->context, "values");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, render->context, "values");
    }
    lua_pop(L, 1);
}

static bool bool_field(lua_State *L, int table, const char *name) {
    lua_getfield(L, table, name);
    bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

int pixy_lua_render(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    Render render = {.L = L,
                     .width = (uint16_t)number_field(L, 2, "width", 80),
                     .height = (uint16_t)number_field(L, 2, "height", 24),
                     .now_ms = (uint64_t)number_field(L, 2, "now_ms", 0)};
    set_context(&render, 2);
    lua_getfield(L, 1, "zones");
    int zones = lua_gettop(L);
    lua_getfield(L, 2, "select");
    int selectors = lua_gettop(L);
    size_t select_count = lua_rawlen(L, selectors);
    Entry *entries = calloc(select_count ? select_count : 1, sizeof(Entry));
    if (!entries) return luaL_error(L, "out of memory");
    size_t count = 0;
    for (size_t selected = 0; selected < select_count; selected++) {
        lua_rawgeti(L, selectors, (lua_Integer)selected + 1);
        const char *selector = luaL_checkstring(L, -1);
        lua_getfield(L, zones, selector);
        char zone_name[512] = {0};
        const char *segment_name = NULL;
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            const char *dot = strrchr(selector, '.');
            if (dot && (size_t)(dot - selector) < sizeof(zone_name)) {
                memcpy(zone_name, selector, (size_t)(dot - selector));
                segment_name = dot + 1;
                lua_getfield(L, zones, zone_name);
            } else {
                lua_pushnil(L);
            }
        } else {
            snprintf(zone_name, sizeof(zone_name), "%s", selector);
        }
        if (!lua_istable(L, -1)) {
            lua_pop(L, 2);
            if (!bool_field(L, 2, "ignore_missing")) {
                free(entries);
                return luaL_error(L, "unknown zone or segment %s", selector);
            }
            continue;
        }
        int zone = lua_gettop(L);
        lua_getfield(L, zone, "segments");
        int segments = lua_gettop(L);
        size_t segment_count = lua_rawlen(L, segments);
        Lines *parts = calloc(segment_count ? segment_count : 1, sizeof(Lines));
        double *priorities = calloc(segment_count ? segment_count : 1, sizeof(double));
        if (!parts || !priorities) return luaL_error(L, "out of memory");
        size_t part_count = 0;
        for (size_t i = 0; i < segment_count; i++) {
            lua_rawgeti(L, segments, (lua_Integer)i + 1);
            int segment = lua_gettop(L);
            lua_getfield(L, segment, "name");
            const char *name = lua_tostring(L, -1);
            bool wanted = !segment_name || (name && strcmp(name, segment_name) == 0);
            lua_pop(L, 1);
            if (wanted) {
                lua_getfield(L, segment, "render");
                push_context(&render);
                if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
                    const char *message = lua_tostring(L, -1);
                    return luaL_error(L, "segment %s.%s: %s", zone_name, name ? name : "?",
                                      message ? message : "render failed");
                }
                parts[part_count] = flatten(&render, lua_gettop(L), (Style){0});
                lua_pop(L, 1);
                lua_getfield(L, segment, "options");
                int options = lua_gettop(L);
                priorities[part_count] = number_field(L, options, "priority", 0);
                lua_getfield(L, options, "id");
                bool interactive = lua_isstring(L, -1);
                lua_pop(L, 1);
                if (interactive) annotate(&render, &parts[part_count], options);
                lua_pop(L, 1);
                part_count++;
            }
            lua_pop(L, 1);
            if (segment_name && wanted) break;
        }
        if (segment_name && part_count == 0 && !bool_field(L, 2, "ignore_missing"))
            return luaL_error(L, "unknown segment %s", selector);
        bool *present = calloc(part_count ? part_count : 1, sizeof(bool));
        Removal *order = calloc(part_count ? part_count : 1, sizeof(Removal));
        Lines *kept = calloc(part_count ? part_count : 1, sizeof(Lines));
        if (!present || !order || !kept) return luaL_error(L, "out of memory");
        size_t zone_width = 0;
        for (size_t i = 0; i < part_count; i++) {
            present[i] = true;
            order[i] = (Removal){.index = i, .priority = priorities[i]};
            zone_width += lines_width(&parts[i]);
        }
        qsort(order, part_count, sizeof(Removal), part_removal_compare);
        size_t remaining = part_count;
        for (size_t i = 0; i < part_count && zone_width > render.width && remaining > 1; i++) {
            size_t index = order[i].index;
            size_t width = lines_width(&parts[index]);
            if (!width) continue;
            present[index] = false;
            zone_width -= width;
            remaining--;
        }
        size_t kept_count = 0;
        for (size_t i = 0; i < part_count; i++) {
            if (present[i]) kept[kept_count++] = parts[i];
        }
        Lines combined = horizontal(L, kept, kept_count);
        free(kept);
        free(order);
        free(present);
        for (size_t i = 0; i < part_count; i++) lines_free(&parts[i]);
        free(parts);
        free(priorities);
        entries[count] = (Entry){.lines = combined, .priority = 0, .index = count, .present = true};
        count++;
        lua_pop(L, 3);
    }
    size_t total = 0;
    for (size_t i = 0; i < count; i++) total += lines_width(&entries[i].lines);
    if (total > render.width && count > 1) {
        Entry **order = calloc(count, sizeof(Entry *));
        if (!order) return luaL_error(L, "out of memory");
        for (size_t i = 0; i < count; i++) order[i] = &entries[i];
        qsort(order, count, sizeof(Entry *), removal_compare);
        size_t present = count;
        for (size_t i = 0; i < count && total > render.width && present > 1; i++) {
            size_t width = lines_width(&order[i]->lines);
            if (!width) continue;
            order[i]->present = false;
            total -= width;
            present--;
        }
        free(order);
    }
    Lines *visible = calloc(count ? count : 1, sizeof(Lines));
    size_t visible_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].present) visible[visible_count++] = entries[i].lines;
    }
    Lines lines = horizontal(L, visible, visible_count);
    resolve_flex(&lines, render.width);
    if (lines_width(&lines) > render.width) truncate_lines(L, &lines, render.width, "");
    lua_newtable(L);
    int output = lua_gettop(L);
    const char *mode = string_field(L, 2, "mode", "line");
    lua_pushstring(L, mode);
    lua_setfield(L, output, "mode");
    if (strcmp(mode, "run") == 0) {
        push_runs(L, &lines);
        lua_setfield(L, output, "runs");
    } else if (strcmp(mode, "surface") == 0) {
        push_surface(&render, &lines);
        lua_setfield(L, output, "ansi");
        lua_pushinteger(L, (lua_Integer)lines.count);
        lua_setfield(L, output, "height");
    } else {
        const char *target = string_field(L, 2, "target", "plain");
        push_line(&render, &lines, target);
        lua_setfield(L, output, "text");
    }
    lua_pushinteger(L, (lua_Integer)lines_width(&lines));
    lua_setfield(L, output, "width");
    if (lines.has_next) {
        lua_pushinteger(L, (lua_Integer)lines.next_ms);
        lua_setfield(L, output, "next_frame_ms");
    }
    push_regions(&render, &lines);
    lua_setfield(L, output, "regions");
    lua_pushliteral(L, "\r\033[K");
    lua_setfield(L, output, "_stream_rewind");
    for (size_t i = 0; i < count; i++) lines_free(&entries[i].lines);
    free(visible);
    free(entries);
    lines_free(&lines);
    for (size_t i = 0; i < render.region_count; i++) {
        luaL_unref(L, LUA_REGISTRYINDEX, render.regions[i]->ref);
        free(render.regions[i]->id);
        free(render.regions[i]);
    }
    free(render.regions);
    lua_pushvalue(L, output);
    return 1;
}
