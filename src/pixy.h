/* pixy: a Lua terminal painter.
 *
 * Rust hosted this before; the Lua under lua/ is unchanged, because the
 * configuration language is the contract and a rewrite is not an excuse to
 * break it. What lives here is the host: limits, I/O, packs and the CLI.
 */
#ifndef PIXY_H
#define PIXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PIXY_VERSION_STRING
#define PIXY_VERSION_STRING "0.0.0"
#endif
#define PIXY_VERSION PIXY_VERSION_STRING

/* Exit codes, unchanged from the Rust build: scripts depend on them. */
#define PIXY_EXIT_USAGE 2
#define PIXY_EXIT_CONFIG 3
#define PIXY_EXIT_RENDER 4
#define PIXY_EXIT_TRANSPORT 5

#define PIXY_MEMORY_LIMIT (32u * 1024u * 1024u)
#define PIXY_RENDER_DEADLINE_MS 100
#define PIXY_LOAD_DEADLINE_MS 250
#define PIXY_FUEL_PER_SLICE 4096
#define PIXY_MAX_CONFIG_SIZE (1024u * 1024u)
#define PIXY_MAX_READ (64u * 1024u)
#define PIXY_MAX_EXEC_OUTPUT (64u * 1024u)
#define PIXY_MAX_EXEC_TIMEOUT_MS 2000
#define PIXY_MAX_EXEC_ARGS 128
#define PIXY_MAX_CACHE_TTL_MS (60u * 60u * 1000u)
#define PIXY_MAX_RENDER_IO_MS 2000
#define PIXY_OUTPUT_LIMIT (1024u * 1024u)

/* ---------------------------------------------------------------- buffers */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} PixyBuf;

void pixy_buf_free(PixyBuf *buf);
bool pixy_buf_add(PixyBuf *buf, const char *bytes, size_t len);
bool pixy_buf_str(PixyBuf *buf, const char *text);
bool pixy_buf_fmt(PixyBuf *buf, const char *fmt, ...);
bool pixy_buf_json_string(PixyBuf *buf, const char *text, size_t len);

/* ------------------------------------------------------------------ error */

/* One error slot per process: a CLI reports one failure and exits. */
void pixy_fail(int code, const char *fmt, ...);
const char *pixy_error_message(void);
int pixy_error_code(void);
bool pixy_failed(void);
void pixy_clear_error(void);

/* ------------------------------------------------------------------ paths */

typedef struct {
    char config_dir[4096];
    char cache_dir[4096];
    char data_dir[4096];
} PixyPaths;

bool pixy_paths_discover(PixyPaths *paths);

typedef struct {
    char name[4096]; /* "@/path/to/init.lua" or "@pixy/default.lua" */
    char path[4096]; /* empty when the source is not a file */
    char directory[4096];
    char *source;
    size_t source_len;
} PixyConfigSource;

bool pixy_config_load(const char *explicit_path, const PixyPaths *paths, PixyConfigSource *out);
void pixy_config_free(PixyConfigSource *source);

/* ------------------------------------------------------------------- json */

typedef enum {
    PIXY_JSON_NULL,
    PIXY_JSON_BOOL,
    PIXY_JSON_NUMBER,
    PIXY_JSON_STRING,
    PIXY_JSON_ARRAY,
    PIXY_JSON_OBJECT,
} PixyJsonKind;

typedef struct PixyJson PixyJson;

PixyJson *pixy_json_parse(const char *text, size_t len);
void pixy_json_free(PixyJson *value);
PixyJsonKind pixy_json_kind(const PixyJson *value);
bool pixy_json_bool(const PixyJson *value);
double pixy_json_number(const PixyJson *value);
const char *pixy_json_string(const PixyJson *value, size_t *len);
size_t pixy_json_count(const PixyJson *value);
const PixyJson *pixy_json_at(const PixyJson *value, size_t index);
const char *pixy_json_key(const PixyJson *value, size_t index, size_t *len);
const PixyJson *pixy_json_get(const PixyJson *value, const char *key);
/* Re-emits a parsed value, for handing a context value back to Lua verbatim. */
bool pixy_json_write(const PixyJson *value, PixyBuf *out);

/* ------------------------------------------------------------------ packs */

typedef struct {
    char name[256];
    uint32_t raw_size;
    uint64_t checksum;
    unsigned char *bytes;
    size_t len;
} PixyPackItem;

typedef struct {
    char source[256];
    char license[256];
    char attribution[256];
    PixyPackItem *items;
    size_t count;
} PixyPack;

bool pixy_pack_load(const char *path, PixyPack *out);
void pixy_pack_free(PixyPack *pack);
bool pixy_pack_build(const char *directory, const char *output, const char *source,
                     const char *license, const char *attribution);
/* The embedded Pokemon archive: `regular/<name>` and `shiny/<name>`. */
unsigned char *pixy_embedded_item(const char *pack, const char *name, size_t *len);
bool pixy_embedded_names(const char *pack, char ***names, size_t *count);
size_t pixy_embedded_count(void);
const char *pixy_embedded_source(void);
const char *pixy_embedded_license(void);
const char *pixy_embedded_attribution(void);

/* ----------------------------------------------------------------- engine */

typedef struct PixyEngine PixyEngine;

typedef enum { PIXY_MODE_LINE, PIXY_MODE_RUN, PIXY_MODE_SURFACE } PixyMode;
typedef enum { PIXY_TARGET_PLAIN, PIXY_TARGET_ANSI, PIXY_TARGET_BASH, PIXY_TARGET_ZSH } PixyTarget;

typedef struct {
    char **select;
    size_t select_count;
    PixyMode mode;
    bool has_target;
    PixyTarget target;
    uint16_t width;
    uint16_t height;
    bool has_now_ms;
    uint64_t now_ms;
    bool ignore_missing;
    /* Context as JSON, so it reaches Lua exactly as a caller wrote it. */
    const char *context_json;
    size_t context_json_len;
} PixyRequest;

typedef struct {
    PixyMode mode;
    PixyBuf payload;      /* line text, or the ANSI of a surface */
    PixyBuf runs_json;    /* run mode: the runs array */
    PixyBuf regions_json; /* the regions array, empty when there are none */
    size_t width;
    size_t height;
    bool has_next_frame;
    uint64_t next_frame_ms;
    PixyBuf stream_rewind;
} PixyOutput;

void pixy_output_free(PixyOutput *output);

PixyEngine *pixy_engine_load(const PixyConfigSource *source, const PixyPaths *paths);
void pixy_engine_free(PixyEngine *engine);
bool pixy_engine_render(PixyEngine *engine, const PixyRequest *request, PixyOutput *out);
bool pixy_engine_inventory(PixyEngine *engine, char ***names, size_t *count, size_t *zones,
                           size_t *segments);
const char *pixy_engine_source_name(const PixyEngine *engine);

/* Serialises an output the way `--mode run|surface` prints it. */
bool pixy_output_json(const PixyOutput *output, PixyBuf *out);

/* -------------------------------------------------------------------- cli */

int pixy_main(int argc, char **argv);
int pixy_serve(const char *socket_path, const char *config_path, bool force);
bool pixy_use_colour(void);

/* ------------------------------------------------------------------ width */

/* Terminal cell width of a UTF-8 string, matching what the Rust build used. */
size_t pixy_cell_width(const char *text, size_t len);

#endif /* PIXY_H */
