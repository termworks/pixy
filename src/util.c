/* Buffers, the error slot, XDG paths and config discovery. */
#include "pixy.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------- buffers */

void pixy_buf_free(PixyBuf *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static bool buf_reserve(PixyBuf *buf, size_t extra) {
    if (buf->len + extra + 1 <= buf->cap) return true;
    size_t want = buf->cap ? buf->cap : 64;
    while (want < buf->len + extra + 1) {
        if (want > (size_t)1 << 40) return false;
        want *= 2;
    }
    char *next = realloc(buf->data, want);
    if (!next) return false;
    buf->data = next;
    buf->cap = want;
    return true;
}

bool pixy_buf_add(PixyBuf *buf, const char *bytes, size_t len) {
    if (!buf_reserve(buf, len)) return false;
    memcpy(buf->data + buf->len, bytes, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return true;
}

bool pixy_buf_str(PixyBuf *buf, const char *text) {
    return pixy_buf_add(buf, text, strlen(text));
}

bool pixy_buf_fmt(PixyBuf *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list probe;
    va_copy(probe, args);
    int needed = vsnprintf(NULL, 0, fmt, probe);
    va_end(probe);
    if (needed < 0) {
        va_end(args);
        return false;
    }
    if (!buf_reserve(buf, (size_t)needed)) {
        va_end(args);
        return false;
    }
    vsnprintf(buf->data + buf->len, (size_t)needed + 1, fmt, args);
    va_end(args);
    buf->len += (size_t)needed;
    return true;
}

bool pixy_buf_json_string(PixyBuf *buf, const char *text, size_t len) {
    if (!pixy_buf_add(buf, "\"", 1)) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        bool ok = true;
        switch (ch) {
        case '"':
            ok = pixy_buf_str(buf, "\\\"");
            break;
        case '\\':
            ok = pixy_buf_str(buf, "\\\\");
            break;
        case '\n':
            ok = pixy_buf_str(buf, "\\n");
            break;
        case '\r':
            ok = pixy_buf_str(buf, "\\r");
            break;
        case '\t':
            ok = pixy_buf_str(buf, "\\t");
            break;
        case '\b':
            ok = pixy_buf_str(buf, "\\b");
            break;
        case '\f':
            ok = pixy_buf_str(buf, "\\f");
            break;
        default:
            if (ch < 0x20) {
                ok = pixy_buf_fmt(buf, "\\u%04x", ch);
            } else {
                ok = pixy_buf_add(buf, (const char *)&ch, 1);
            }
        }
        if (!ok) return false;
    }
    return pixy_buf_add(buf, "\"", 1);
}

/* ------------------------------------------------------------------ error */

static char error_message[4096];
static int error_code;
static bool error_set;

void pixy_fail(int code, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(error_message, sizeof(error_message), fmt, args);
    va_end(args);
    error_code = code;
    error_set = true;
}

const char *pixy_error_message(void) {
    return error_set ? error_message : "";
}
int pixy_error_code(void) {
    return error_code;
}
bool pixy_failed(void) {
    return error_set;
}
void pixy_clear_error(void) {
    error_set = false;
    error_code = 0;
    error_message[0] = '\0';
}

/* ------------------------------------------------------------------ paths */

/* Callers pass the destination as the base — `join(dir, size, dir, "pixy")` —
 * so the copy has to go through scratch. Formatting a buffer into itself is
 * undefined, and here it silently produced a cache directory that never
 * existed, which turned every cached provider into an uncached one. */
static void join(char *out, size_t size, const char *base, const char *leaf) {
    char scratch[4096];
    snprintf(scratch, sizeof(scratch), "%s/%s", base, leaf);
    snprintf(out, size, "%s", scratch);
}

static bool env_dir(const char *name, char *out, size_t size) {
    const char *value = getenv(name);
    if (!value || value[0] != '/') return false;
    snprintf(out, size, "%s", value);
    return true;
}

bool pixy_paths_discover(PixyPaths *paths) {
    const char *home = getenv("HOME");
    memset(paths, 0, sizeof(*paths));

    if (env_dir("PIXY_CONFIG_DIR", paths->config_dir, sizeof(paths->config_dir))) {
        /* explicit wins */
    } else if (env_dir("XDG_CONFIG_HOME", paths->config_dir, sizeof(paths->config_dir))) {
        join(paths->config_dir, sizeof(paths->config_dir), paths->config_dir, "pixy");
    } else if (home) {
        snprintf(paths->config_dir, sizeof(paths->config_dir), "%s/.config/pixy", home);
    } else {
        pixy_fail(PIXY_EXIT_CONFIG, "no HOME and no XDG_CONFIG_HOME");
        return false;
    }

    if (!env_dir("PIXY_CACHE_DIR", paths->cache_dir, sizeof(paths->cache_dir))) {
        if (env_dir("XDG_CACHE_HOME", paths->cache_dir, sizeof(paths->cache_dir))) {
            join(paths->cache_dir, sizeof(paths->cache_dir), paths->cache_dir, "pixy");
        } else if (home) {
            snprintf(paths->cache_dir, sizeof(paths->cache_dir), "%s/.cache/pixy", home);
        }
    }
    if (!env_dir("PIXY_DATA_DIR", paths->data_dir, sizeof(paths->data_dir))) {
        if (env_dir("XDG_DATA_HOME", paths->data_dir, sizeof(paths->data_dir))) {
            join(paths->data_dir, sizeof(paths->data_dir), paths->data_dir, "pixy/packs");
        } else if (home) {
            snprintf(paths->data_dir, sizeof(paths->data_dir), "%s/.local/share/pixy/packs", home);
        }
    }
    return true;
}

static char *read_file(const char *path, size_t *len, size_t limit) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    PixyBuf buf = {0};
    char chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (buf.len + got > limit) {
            fclose(file);
            pixy_buf_free(&buf);
            return NULL;
        }
        if (!pixy_buf_add(&buf, chunk, got)) {
            fclose(file);
            pixy_buf_free(&buf);
            return NULL;
        }
    }
    fclose(file);
    if (!buf.data && !pixy_buf_add(&buf, "", 0)) return NULL;
    *len = buf.len;
    return buf.data;
}

/* The default configuration, used when the user has none. Kept byte-identical
 * to what the Rust build embedded. */
extern const char PIXY_DEFAULT_CONFIG[];

bool pixy_config_load(const char *explicit_path, const PixyPaths *paths, PixyConfigSource *out) {
    memset(out, 0, sizeof(*out));
    char candidate[4096];
    const char *chosen = NULL;

    if (explicit_path && explicit_path[0]) {
        chosen = explicit_path;
    } else {
        const char *from_env = getenv("PIXY_CONFIG");
        if (from_env && from_env[0]) {
            chosen = from_env;
        } else {
            join(candidate, sizeof(candidate), paths->config_dir, "init.lua");
            if (access(candidate, R_OK) == 0) chosen = candidate;
        }
    }

    if (!chosen) {
        snprintf(out->name, sizeof(out->name), "@pixy/default.lua");
        snprintf(out->directory, sizeof(out->directory), "%s", paths->config_dir);
        out->source_len = strlen(PIXY_DEFAULT_CONFIG);
        out->source = malloc(out->source_len + 1);
        if (!out->source) {
            pixy_fail(PIXY_EXIT_CONFIG, "out of memory");
            return false;
        }
        memcpy(out->source, PIXY_DEFAULT_CONFIG, out->source_len + 1);
        return true;
    }

    size_t len = 0;
    char *source = read_file(chosen, &len, PIXY_MAX_CONFIG_SIZE);
    if (!source) {
        pixy_fail(PIXY_EXIT_CONFIG, "failed to read %s config %s: %s (os error %d)",
                  explicit_path && explicit_path[0] ? "explicit" : "discovered", chosen,
                  strerror(errno), errno);
        return false;
    }
    /* `name` carries a leading `@`, so a path of exactly the buffer's length
     * would lose its last character -- and that name is what every error
     * message quotes back. */
    if (snprintf(out->name, sizeof(out->name), "@%s", chosen) >= (int)sizeof(out->name)) {
        pixy_fail(PIXY_EXIT_CONFIG, "config path is too long: %s", chosen);
        free(source);
        return false;
    }
    snprintf(out->path, sizeof(out->path), "%s", chosen);
    snprintf(out->directory, sizeof(out->directory), "%s", chosen);
    char *slash = strrchr(out->directory, '/');
    if (slash && slash != out->directory) {
        *slash = '\0';
    } else if (slash) {
        out->directory[1] = '\0';
    } else {
        snprintf(out->directory, sizeof(out->directory), ".");
    }
    out->source = source;
    out->source_len = len;
    return true;
}

void pixy_config_free(PixyConfigSource *source) {
    free(source->source);
    source->source = NULL;
    source->source_len = 0;
}
