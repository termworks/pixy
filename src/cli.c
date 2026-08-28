/* The command line: one verb, some options, render and exit. */
#include "pixy.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "palette.h"

int pixy_bench(int argc, char **argv);

extern const char PIXY_BASH_INIT[];
extern const char PIXY_ZSH_INIT[];
extern const char PIXY_FISH_INIT[];
extern const char PIXY_OSLO_INIT[];
extern const char PIXY_HEXE_OSLO_CONFIG[];

/* ---------------------------------------------------------------- colour */

bool pixy_use_colour(void) {
    static int decided = -1;
    if (decided >= 0) return decided == 1;
    const char *term = getenv("TERM");
    decided = (!getenv("NO_COLOR") && !(term && strcmp(term, "dumb") == 0) && isatty(STDOUT_FILENO))
                  ? 1
                  : 0;
    return decided == 1;
}

static void paint(PixyBuf *out, const char *code, const char *text) {
    if (pixy_use_colour()) {
        pixy_buf_fmt(out, "\033[%sm%s\033[0m", code, text);
    } else {
        pixy_buf_str(out, text);
    }
}

const char *pixy_error_prefix(void) {
    if (!getenv("NO_COLOR") && isatty(STDERR_FILENO)) return "\033[1;31mpixy\033[0m";
    return "pixy";
}

/* ------------------------------------------------------------------ help */

static void print_help(void) {
    PixyBuf out = {0};
    paint(&out, "1;38;5;213", "pixy");
    pixy_buf_str(&out, " ");
    paint(&out, "2", PIXY_VERSION);
    pixy_buf_str(&out, "\n");
    paint(&out, "2", "Lua paints your terminal; C hosts it and gets out of the way.");
    pixy_buf_str(&out, "\n\n");

    paint(&out, "1;38;5;213", "USAGE");
    pixy_buf_str(&out, "\n  ");
    paint(&out, "1;38;5;117", "pixy");
    pixy_buf_str(&out, " <zone[.segment][,...]> [options]");
    paint(&out, "2", "      render is the default verb");
    pixy_buf_str(&out, "\n\n");

    paint(&out, "1;38;5;213", "COMMANDS");
    pixy_buf_str(&out, "\n");
    struct {
        const char *name;
        const char *args;
        const char *about;
    } commands[] = {
        {"render", "<zone[.segment][,...]>", "render once and exit"},
        {"stream", "<zone[.segment][,...]>", "animate for a bounded time"},
        {"serve", "[--stdio]", "answer painter requests on stdin and stdout"},
        {"list", "", "every zone and segment the config defines"},
        {"check", "", "load the config and report what it holds"},
        {"names", "[<pack>]", "the vocabulary a pack can draw, one id per line"},
        {"init", "<shell>", "shell integration for bash|zsh|fish|oslo|hexe-oslo"},
        {"pack", "<build|check|list>", "build and inspect sprite packs"},
        {"palette", "<set|use|end|...>", "colour namespaces for the output pixy writes"},
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        char cell[128];
        snprintf(cell, sizeof(cell), "%s %s", commands[i].name, commands[i].args);
        pixy_buf_str(&out, "  ");
        paint(&out, "1;38;5;117", cell);
        for (size_t pad = strlen(cell); pad < 30; pad++) pixy_buf_str(&out, " ");
        pixy_buf_str(&out, " ");
        paint(&out, "2", commands[i].about);
        pixy_buf_str(&out, "\n");
    }

    pixy_buf_str(&out, "\n");
    paint(&out, "1;38;5;213", "OPTIONS");
    pixy_buf_str(&out, "\n");
    struct {
        const char *flag;
        const char *about;
    } options[] = {
        {"--config PATH", "config to load instead of the discovered one"},
        {"--mode line|run|surface", "a line, styled runs as JSON, or a surface"},
        {"--target plain|ansi|bash|zsh", "how a line is escaped"},
        {"--width N  --height N", "the space the caller is offering"},
        {"--set key=value", "a context value, repeatable"},
        {"--context-json  --context-file", "the whole context at once"},
        {"--now-ms MS", "pin the clock, so animation is reproducible"},
        {"--frames-ms MS", "draw the next MS as a filmstrip, all frames at once"},
        {"--palette [N]", "wrap the line in a colour namespace"},
        {"--newline", "end the output with a newline"},
        {"-h, --help  -V, --version", "this text, or the version"},
    };
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        pixy_buf_str(&out, "  ");
        paint(&out, "1;38;5;117", options[i].flag);
        for (size_t pad = strlen(options[i].flag); pad < 32; pad++) pixy_buf_str(&out, " ");
        pixy_buf_str(&out, " ");
        paint(&out, "2", options[i].about);
        pixy_buf_str(&out, "\n");
    }

    pixy_buf_str(&out, "\n");
    paint(&out, "1;38;5;213", "EXAMPLES");
    pixy_buf_str(&out, "\n  ");
    paint(&out, "2", "pixy prompt.left --target ansi --set status=7");
    pixy_buf_str(&out, "\n  ");
    paint(&out, "2", "pixy render status --mode run --width 120");
    pixy_buf_str(&out, "\n  ");
    paint(&out, "2", "pixy names pokemon | head");
    pixy_buf_str(&out, "\n");

    fwrite(out.data, 1, out.len, stdout);
    pixy_buf_free(&out);
}

static bool command_help(const char *name) {
    const char *usage = NULL;
    const char *const *lines = NULL;
    static const char *render_lines[] = {
        "--mode line|run|surface|line by default",
        "--target plain|ansi|bash|zsh|escaping for a line",
        "--width N  --height N|the space on offer",
        "--set key=value|one context value, repeatable",
        "--context-json J  --context-file P|the whole context",
        "--now-ms MS|pin the clock",
        "--ignore-missing|skip selectors the config does not define",
        "--frames-ms MS|every frame of the next MS, so a caller animates from one run",
        NULL,
    };
    static const char *stream_lines[] = {
        "--fps N|frames per second, 1-1000",
        "--duration MS|how long to run, up to 24h",
        NULL,
    };
    static const char *serve_lines[] = {
        "|reads requests on stdin, writes answers on stdout",
        "|exits when stdin closes, so it goes when its caller does",
        NULL,
    };
    static const char *names_lines[] = {
        "<pack>|an installed or embedded pack; pokemon by default",
        "|one id per line, sorted, each one the pack can draw",
        NULL,
    };
    static const char *pack_lines[] = {
        "build <directory> --output <file>|pack a directory of art",
        "check <file>|verify a pack's checksums",
        "list [<file>]|installed packs, or one pack's items",
        NULL,
    };
    static const char *palette_lines[] = {
        "set [<key>=<colour> ...]|define colours; without pairs, the config's",
        "use|claim the slot, until end",
        "end|release it",
        "reset|forget a slot's colours",
        "ask [--wait]|capability query; --wait reads the answer",
        "--slot N|0-31, and 2-31 to claim; the config's by default",
        "--timeout-ms MS|how long --wait listens, 100 by default",
        "|a key is 0-255, fg, bg or cursor; a colour is #rrggbb",
        "|--wait exits 1 when nothing answers, which means unsupported",
        NULL,
    };
    static const char *init_lines[] = {"|prints integration text; writes nothing", NULL};
    static const char *config_lines[] = {
        "--config PATH|load this config instead of the discovered one", NULL};

    if (strcmp(name, "render") == 0) {
        usage = "pixy render <zone[.segment][,...]> [options]";
        lines = render_lines;
    } else if (strcmp(name, "stream") == 0) {
        usage = "pixy stream <zone[.segment][,...]> [options]";
        lines = stream_lines;
    } else if (strcmp(name, "serve") == 0) {
        usage = "pixy serve [--stdio] [--config PATH]";
        lines = serve_lines;
    } else if (strcmp(name, "names") == 0) {
        usage = "pixy names [<pack>]";
        lines = names_lines;
    } else if (strcmp(name, "pack") == 0) {
        usage = "pixy pack <build|check|list>";
        lines = pack_lines;
    } else if (strcmp(name, "palette") == 0) {
        usage = "pixy palette <set|use|end|reset|ask> [--slot N] [<key>=<colour> ...]";
        lines = palette_lines;
    } else if (strcmp(name, "init") == 0) {
        usage = "pixy init <bash|zsh|fish|oslo|hexe-oslo>";
        lines = init_lines;
    } else if (strcmp(name, "list") == 0 || strcmp(name, "check") == 0) {
        usage = "pixy list|check [--config PATH]";
        lines = config_lines;
    } else {
        return false;
    }

    PixyBuf out = {0};
    paint(&out, "1;38;5;117", usage);
    pixy_buf_str(&out, "\n");
    for (size_t i = 0; lines[i]; i++) {
        const char *split = strchr(lines[i], '|');
        char flag[128];
        size_t flag_len = (size_t)(split - lines[i]);
        memcpy(flag, lines[i], flag_len);
        flag[flag_len] = '\0';
        pixy_buf_str(&out, "  ");
        if (flag_len) {
            paint(&out, "1;38;5;117", flag);
            for (size_t pad = flag_len; pad < 36; pad++) pixy_buf_str(&out, " ");
            pixy_buf_str(&out, " ");
        }
        paint(&out, "2", split + 1);
        pixy_buf_str(&out, "\n");
    }
    fwrite(out.data, 1, out.len, stdout);
    pixy_buf_free(&out);
    return true;
}

/* -------------------------------------------------------------- selectors */

static bool valid_selector(const char *name) {
    if (!name || !*name) return false;
    unsigned char first = (unsigned char)name[0];
    if (!isalnum(first)) return false;
    for (const char *at = name + 1; *at; at++) {
        unsigned char ch = (unsigned char)*at;
        if (!isalnum(ch) && ch != '_' && ch != '.' && ch != '-') return false;
    }
    return true;
}

static bool split_selectors(const char *value, char ***out, size_t *count) {
    char *copy = strdup(value);
    if (!copy) return false;
    size_t capacity = 8, found = 0;
    char **items = calloc(capacity, sizeof(char *));
    char *cursor = copy;
    while (cursor) {
        char *comma = strchr(cursor, ',');
        if (comma) *comma = '\0';
        if (!valid_selector(cursor)) {
            free(copy);
            for (size_t i = 0; i < found; i++) free(items[i]);
            free(items);
            return false;
        }
        if (found == capacity) {
            capacity *= 2;
            items = realloc(items, capacity * sizeof(char *));
        }
        items[found++] = strdup(cursor);
        cursor = comma ? comma + 1 : NULL;
    }
    free(copy);
    *out = items;
    *count = found;
    return true;
}

static uint16_t default_width(void) {
    const char *columns = getenv("COLUMNS");
    if (columns) {
        long value = strtol(columns, NULL, 10);
        if (value > 0 && value <= 65535) return (uint16_t)value;
    }
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) return size.ws_col;
    return 80;
}

/* A `--set` value is the number, boolean or null it spells, and text
 * otherwise; an empty one is absent, so an unset shell variable reads as nil. */
static void append_set(PixyBuf *values, bool *first, const char *pair) {
    const char *equals = strchr(pair, '=');
    if (!equals) return;
    size_t name_len = (size_t)(equals - pair);
    const char *value = equals + 1;
    if (!*first) pixy_buf_str(values, ",");
    *first = false;
    pixy_buf_json_string(values, pair, name_len);
    pixy_buf_str(values, ":");
    if (!*value) {
        pixy_buf_str(values, "null");
        return;
    }
    if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0 || strcmp(value, "null") == 0) {
        pixy_buf_str(values, value);
        return;
    }
    char *stop = NULL;
    double number = strtod(value, &stop);
    if (stop && *stop == '\0' && stop != value) {
        if (number == (double)(long long)number) {
            pixy_buf_fmt(values, "%lld", (long long)number);
        } else {
            pixy_buf_fmt(values, "%.17g", number);
        }
        return;
    }
    pixy_buf_json_string(values, value, strlen(value));
}

typedef struct {
    PixyRequest request;
    char config[4096];
    bool newline;
    bool palette;
    long palette_slot;
    bool palette_slot_given;
    PixyPaletteEntry *palette_colours;
    size_t palette_count;
    PixyBuf context;
    unsigned fps;
    unsigned long duration_ms;
} Options;

static void free_options(Options *options);

static bool parse_options(int argc, char **argv, Options *options, bool selector_first) {
    memset(options, 0, sizeof(*options));
    options->request.mode = PIXY_MODE_LINE;
    options->request.has_target = true;
    options->request.target = PIXY_TARGET_ANSI;
    options->request.width = default_width();
    options->request.height = 1;
    options->fps = 12;
    options->duration_ms = 1000;
    options->palette_slot = PIXY_PALETTE_DEFAULT_SLOT;

    PixyBuf sets = {0};
    bool first_set = true;
    const char *context_json = NULL;
    char *context_file_body = NULL;
    char *request_body = NULL;

    int index = 0;
    if (selector_first && argc > 0 && argv[0][0] != '-') {
        if (!split_selectors(argv[0], &options->request.select, &options->request.select_count)) {
            pixy_fail(PIXY_EXIT_USAGE, "invalid selector '%s'", argv[0]);
            goto fail;
        }
        index = 1;
    }

    for (; index < argc; index++) {
        const char *arg = argv[index];
        const char *next = index + 1 < argc ? argv[index + 1] : NULL;
        bool needs_value = false;

        /* `--target=ansi` and `--target ansi` are the same option; the oslo
         * integration writes the first form. */
        char name[64];
        bool joined = false;
        const char *equals = strchr(arg, '=');
        if (arg[0] == '-' && equals && (size_t)(equals - arg) < sizeof(name)) {
            memcpy(name, arg, (size_t)(equals - arg));
            name[equals - arg] = '\0';
            arg = name;
            next = equals + 1;
            joined = true;
        }

        if (strcmp(arg, "--config") == 0) {
            needs_value = true;
            if (next) snprintf(options->config, sizeof(options->config), "%s", next);
        } else if (strcmp(arg, "--mode") == 0) {
            needs_value = true;
            if (next) {
                if (strcmp(next, "line") == 0) options->request.mode = PIXY_MODE_LINE;
                else if (strcmp(next, "run") == 0) options->request.mode = PIXY_MODE_RUN;
                else if (strcmp(next, "surface") == 0) options->request.mode = PIXY_MODE_SURFACE;
                else {
                    pixy_fail(PIXY_EXIT_USAGE, "invalid mode '%s'", next);
                    goto fail;
                }
            }
        } else if (strcmp(arg, "--target") == 0) {
            needs_value = true;
            if (next) {
                options->request.has_target = true;
                if (strcmp(next, "plain") == 0) options->request.target = PIXY_TARGET_PLAIN;
                else if (strcmp(next, "ansi") == 0) options->request.target = PIXY_TARGET_ANSI;
                else if (strcmp(next, "bash") == 0) options->request.target = PIXY_TARGET_BASH;
                else if (strcmp(next, "zsh") == 0) options->request.target = PIXY_TARGET_ZSH;
                else {
                    pixy_fail(PIXY_EXIT_USAGE, "invalid target '%s'", next);
                    goto fail;
                }
            }
        } else if (strcmp(arg, "--width") == 0) {
            needs_value = true;
            if (next) options->request.width = (uint16_t)strtoul(next, NULL, 10);
        } else if (strcmp(arg, "--height") == 0) {
            needs_value = true;
            if (next) options->request.height = (uint16_t)strtoul(next, NULL, 10);
        } else if (strcmp(arg, "--now-ms") == 0) {
            needs_value = true;
            if (next) {
                options->request.has_now_ms = true;
                options->request.now_ms = strtoull(next, NULL, 10);
            }
        } else if (strcmp(arg, "--frames-ms") == 0) {
            needs_value = true;
            if (next) options->request.frames_ms = (uint32_t)strtoul(next, NULL, 10);
        } else if (strcmp(arg, "--fps") == 0) {
            needs_value = true;
            if (next) options->fps = (unsigned)strtoul(next, NULL, 10);
        } else if (strcmp(arg, "--duration") == 0) {
            needs_value = true;
            if (next) options->duration_ms = strtoul(next, NULL, 10);
        } else if (strcmp(arg, "--set") == 0) {
            needs_value = true;
            if (next) append_set(&sets, &first_set, next);
        } else if (strcmp(arg, "--context-json") == 0) {
            needs_value = true;
            context_json = next;
        } else if (strcmp(arg, "--context-file") == 0) {
            needs_value = true;
            if (next) {
                FILE *file = fopen(next, "rb");
                if (!file) {
                    pixy_fail(PIXY_EXIT_USAGE, "failed to read context file %s", next);
                    goto fail;
                }
                PixyBuf body = {0};
                char chunk[4096];
                size_t got;
                while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0)
                    pixy_buf_add(&body, chunk, got);
                fclose(file);
                free(context_file_body);
                context_file_body = body.data;
            }
        } else if (strcmp(arg, "--request") == 0) {
            needs_value = true;
            if (next) {
                PixyBuf body = {0};
                char chunk[4096];
                size_t got;
                if (strcmp(next, "-") == 0) {
                    while ((got = fread(chunk, 1, sizeof(chunk), stdin)) > 0)
                        pixy_buf_add(&body, chunk, got);
                } else {
                    FILE *file = fopen(next, "rb");
                    if (!file) {
                        pixy_fail(PIXY_EXIT_USAGE, "failed to read request file %s", next);
                        goto fail;
                    }
                    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0)
                        pixy_buf_add(&body, chunk, got);
                    fclose(file);
                }
                free(request_body);
                request_body = body.data;
            }
        } else if (strcmp(arg, "--palette") == 0) {
            options->palette = true;
            /* The slot is optional: without one the config decides, so a prompt
             * and its startup `palette set` cannot drift apart. */
            if (next && next[0] >= '0' && next[0] <= '9') {
                char *stop = NULL;
                long slot = strtol(next, &stop, 10);
                /* An unclaimable slot has to fail here. Accepting it would emit
                 * the release without the claim, and that `end` pops whatever
                 * namespace the surrounding application was holding. */
                if (!stop || *stop != '\0' || !pixy_palette_valid_slot(slot, true)) {
                    pixy_fail(PIXY_EXIT_USAGE,
                              "--palette takes a slot 2-%d; 0 is the ordinary palette and 1 the "
                              "terminal's",
                              PIXY_PALETTE_MAX_SLOT);
                    goto fail;
                }
                options->palette_slot = slot;
                options->palette_slot_given = true;
                needs_value = true;
            }
        } else if (strcmp(arg, "--newline") == 0) {
            options->newline = true;
        } else if (strcmp(arg, "--ignore-missing") == 0) {
            options->request.ignore_missing = true;
        } else if (!selector_first && options->request.select_count == 0 && arg[0] != '-') {
            if (!split_selectors(arg, &options->request.select, &options->request.select_count)) {
                pixy_fail(PIXY_EXIT_USAGE, "invalid selector '%s'", arg);
                goto fail;
            }
        } else {
            pixy_fail(PIXY_EXIT_USAGE, "unknown option '%s'", arg);
            goto fail;
        }

        if (needs_value) {
            if (!next) {
                pixy_fail(PIXY_EXIT_USAGE, "%s requires a value", arg);
                goto fail;
            }
            /* A joined value came from this argument, so nothing extra to skip. */
            if (!joined) index++;
        }
    }

    if (request_body) {
        PixyJson *request = pixy_json_parse(request_body, strlen(request_body));
        if (!request) {
            pixy_fail(PIXY_EXIT_USAGE, "invalid request JSON");
            free(request_body);
            goto fail;
        }
        const PixyJson *select = pixy_json_get(request, "select");
        size_t count = pixy_json_count(select);
        if (count) {
            for (size_t i = 0; i < options->request.select_count; i++)
                free(options->request.select[i]);
            free(options->request.select);
            options->request.select = calloc(count, sizeof(char *));
            options->request.select_count = 0;
            for (size_t i = 0; i < count; i++) {
                size_t len = 0;
                const char *text = pixy_json_string(pixy_json_at(select, i), &len);
                if (text)
                    options->request.select[options->request.select_count++] = strndup(text, len);
            }
        }
        const PixyJson *mode = pixy_json_get(request, "mode");
        size_t len = 0;
        const char *text = mode ? pixy_json_string(mode, &len) : NULL;
        if (text) {
            options->request.mode = len == 3 && memcmp(text, "run", 3) == 0 ? PIXY_MODE_RUN
                                    : len == 7 && memcmp(text, "surface", 7) == 0
                                        ? PIXY_MODE_SURFACE
                                        : PIXY_MODE_LINE;
        }
        const PixyJson *target = pixy_json_get(request, "target");
        text = target ? pixy_json_string(target, &len) : NULL;
        if (text) {
            options->request.has_target = true;
            options->request.target = len == 4 && memcmp(text, "ansi", 4) == 0   ? PIXY_TARGET_ANSI
                                      : len == 4 && memcmp(text, "bash", 4) == 0 ? PIXY_TARGET_BASH
                                      : len == 3 && memcmp(text, "zsh", 3) == 0  ? PIXY_TARGET_ZSH
                                                                                : PIXY_TARGET_PLAIN;
        }
        const PixyJson *width = pixy_json_get(request, "width");
        if (width) options->request.width = (uint16_t)pixy_json_number(width);
        const PixyJson *height = pixy_json_get(request, "height");
        if (height) options->request.height = (uint16_t)pixy_json_number(height);
        const PixyJson *now = pixy_json_get(request, "now_ms");
        if (now) {
            options->request.has_now_ms = true;
            options->request.now_ms = (uint64_t)pixy_json_number(now);
        }
        const PixyJson *ahead = pixy_json_get(request, "frames_ms");
        if (ahead) options->request.frames_ms = (uint32_t)pixy_json_number(ahead);
        const PixyJson *ignore = pixy_json_get(request, "ignore_missing");
        if (ignore) options->request.ignore_missing = pixy_json_bool(ignore);
        const PixyJson *context = pixy_json_get(request, "context");
        if (context) pixy_json_write(context, &options->context);
        pixy_json_free(request);
        free(request_body);
        pixy_buf_free(&sets);
        if (!options->context.len) pixy_buf_str(&options->context, "{\"values\":{}}");
        options->request.context_json = options->context.data;
        options->request.context_json_len = options->context.len;
        if (options->request.mode != PIXY_MODE_LINE) options->request.has_target = false;
        return true;
    }

    /* A whole-context argument describes the caller's state completely, so it
     * replaces what `--set` built rather than merging into it. */
    if (context_file_body) {
        pixy_buf_str(&options->context, context_file_body);
        free(context_file_body);
    } else if (context_json) {
        pixy_buf_str(&options->context, context_json);
    } else {
        pixy_buf_str(&options->context, "{\"values\":{");
        if (sets.len) pixy_buf_add(&options->context, sets.data, sets.len);
        pixy_buf_str(&options->context, "}}");
    }
    pixy_buf_free(&sets);
    options->request.context_json = options->context.data;
    options->request.context_json_len = options->context.len;
    if (options->request.mode != PIXY_MODE_LINE) options->request.has_target = false;
    /* Run mode is a description for a host that does its own painting; there is
     * nowhere in it for a sequence to go, so asking is a mistake worth naming
     * rather than a flag that quietly does nothing. */
    if (options->palette && options->request.mode == PIXY_MODE_RUN) {
        pixy_fail(PIXY_EXIT_USAGE,
                  "--palette writes terminal sequences, which run mode cannot carry");
        goto fail;
    }
    return true;

/* One way out, so a refused argument does not strand what earlier ones built. */
fail:
    pixy_buf_free(&sets);
    free(context_file_body);
    free(request_body);
    free_options(options);
    return false;
}

static void free_options(Options *options) {
    free(options->palette_colours);
    options->palette_colours = NULL;
    options->palette_count = 0;
    for (size_t i = 0; i < options->request.select_count; i++) free(options->request.select[i]);
    free(options->request.select);
    pixy_buf_free(&options->context);
}

/* ------------------------------------------------------------- commands */

static PixyEngine *open_engine(const char *config_path) {
    PixyPaths paths;
    if (!pixy_paths_discover(&paths)) return NULL;
    PixyConfigSource source;
    if (!pixy_config_load(config_path && config_path[0] ? config_path : NULL, &paths, &source))
        return NULL;
    PixyEngine *engine = pixy_engine_load(&source, &paths);
    pixy_config_free(&source);
    return engine;
}

/* One implementation of claim-and-release for every path that writes cells, so
 * a stream cannot drift from a render. Writes nothing and answers false when the
 * slot is unclaimable, and the caller must not release what it never claimed.
 *
 * The colours go out with the claim rather than from a startup hook. `set` is
 * idempotent, most configs declare nothing and pay nothing, and carrying it here
 * is what makes the prompt survive a `clear`, a reattach or a new pane — and
 * what lets a shell whose configuration cannot run a command still work. */
static bool palette_write(const Options *options, bool claiming) {
    if (!options->palette) return false;
    PixyBuf sequence = {0}, wrapped = {0};
    bool ok = true;
    if (claiming) {
        if (options->palette_count) {
            char slot[16];
            snprintf(slot, sizeof(slot), "%ld", options->palette_slot);
            pixy_palette_set(&sequence, slot, options->palette_colours, options->palette_count,
                             options->request.target);
        }
        ok = pixy_palette_use(&sequence, options->palette_slot, options->request.target);
    } else {
        ok = pixy_palette_end(&sequence, options->request.target);
    }
    if (ok && sequence.len) {
        pixy_palette_wrap(&wrapped, sequence.data, options->request.target);
        fwrite(wrapped.data, 1, wrapped.len, stdout);
    }
    pixy_buf_free(&sequence);
    pixy_buf_free(&wrapped);
    return ok;
}

/* Read what the config declared: the colours, and the slot unless one was named. */
static void palette_resolve(Options *options, PixyEngine *engine) {
    if (!options->palette) return;
    long slot = options->palette_slot;
    if (!pixy_engine_palette(engine, &options->palette_colours, &options->palette_count, &slot)) {
        pixy_clear_error();
        options->palette_colours = NULL;
        options->palette_count = 0;
        return;
    }
    if (!options->palette_slot_given) options->palette_slot = slot;
}

static int render_command(int argc, char **argv, bool selector_first) {
    Options options;
    if (!parse_options(argc, argv, &options, selector_first)) return pixy_error_code();
    if (options.request.select_count == 0) {
        pixy_fail(PIXY_EXIT_USAGE, "at least one selector is required");
        free_options(&options);
        return PIXY_EXIT_USAGE;
    }
    PixyEngine *engine = open_engine(options.config);
    if (!engine) {
        free_options(&options);
        return pixy_error_code();
    }
    palette_resolve(&options, engine);
    if (options.request.frames_ms) {
        PixyOutput *frames = NULL;
        size_t count = 0;
        if (!pixy_engine_filmstrip(engine, &options.request, &frames, &count)) {
            pixy_engine_free(engine);
            free_options(&options);
            return pixy_error_code();
        }
        PixyBuf json = {0};
        pixy_filmstrip_json(frames, count, &json);
        fwrite(json.data, 1, json.len, stdout);
        pixy_buf_free(&json);
        if (options.newline) fputc('\n', stdout);
        pixy_frames_free(frames, count);
        pixy_engine_free(engine);
        free_options(&options);
        return 0;
    }
    PixyOutput output;
    if (!pixy_engine_render(engine, &options.request, &output)) {
        pixy_engine_free(engine);
        free_options(&options);
        return pixy_error_code();
    }
    if (output.mode == PIXY_MODE_LINE || output.mode == PIXY_MODE_SURFACE) {
        /* Claim, print, release: every cell written between the two carries the
         * slot, so repainting it later recolours exactly this output. Release
         * only what was claimed — a lone `end` pops whatever the surrounding
         * application was holding. */
        bool claimed = palette_write(&options, true);
        fwrite(output.payload.data, 1, output.payload.len, stdout);
        if (claimed) palette_write(&options, false);
    } else {
        PixyBuf json = {0};
        pixy_output_json(&output, &json);
        fwrite(json.data, 1, json.len, stdout);
        pixy_buf_free(&json);
    }
    if (options.newline) fputc('\n', stdout);
    pixy_output_free(&output);
    pixy_engine_free(engine);
    free_options(&options);
    return 0;
}

static int stream_command(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, &options, true)) return pixy_error_code();
    if (options.fps < 1 || options.fps > 1000) {
        pixy_fail(PIXY_EXIT_USAGE, "fps must be between 1 and 1000");
        free_options(&options);
        return PIXY_EXIT_USAGE;
    }
    if (options.duration_ms > 24u * 60 * 60 * 1000) {
        pixy_fail(PIXY_EXIT_USAGE, "stream duration cannot exceed 24 hours");
        free_options(&options);
        return PIXY_EXIT_USAGE;
    }
    PixyEngine *engine = open_engine(options.config);
    if (!engine) {
        free_options(&options);
        return pixy_error_code();
    }
    palette_resolve(&options, engine);
    long long started = pixy_now_ms();
    long long floor_ms = 1000 / (long long)options.fps;
    PixyBuf rewind = {0};
    PixyBuf previous = {0};
    bool first = true;

    /* Always one frame: `--duration 0` means "draw it once", not "draw nothing". */
    do {
        PixyOutput output;
        if (!pixy_engine_render(engine, &options.request, &output)) break;

        /* A frame that looks the same is not written: the deadline a render
         * reports is when its picture next changes, so writing faster than that
         * only makes the terminal redraw the same bytes. */
        bool changed = previous.len != output.payload.len ||
                       memcmp(previous.data ? previous.data : "", output.payload.data,
                              output.payload.len) != 0;
        if (changed) {
            if (!first && rewind.len) fwrite(rewind.data, 1, rewind.len, stdout);
            /* Per frame rather than once around the whole stream: `use` is
             * idempotent, and a stream killed mid-flight then leaves no slot
             * claimed behind it. */
            bool claimed = palette_write(&options, true);
            fwrite(output.payload.data, 1, output.payload.len, stdout);
            if (claimed) palette_write(&options, false);
            fflush(stdout);
            first = false;
            previous.len = 0;
            pixy_buf_add(&previous, output.payload.data, output.payload.len);
        }
        rewind.len = 0;
        if (output.stream_rewind.len)
            pixy_buf_add(&rewind, output.stream_rewind.data, output.stream_rewind.len);

        long long wait = floor_ms;
        if (output.has_next_frame) {
            /* A delay -- "ask again in N ms" -- never a deadline. */
            long long due = (long long)output.next_frame_ms;
            if (due > wait) wait = due;
        }
        pixy_output_free(&output);

        long long left = (long long)options.duration_ms - (pixy_now_ms() - started);
        if (left <= 0) break;
        if (wait > left) wait = left;
        if (wait < 1) wait = 1;
        struct timespec nap = {wait / 1000, (wait % 1000) * 1000000};
        nanosleep(&nap, NULL);
    } while (pixy_now_ms() - started < (long long)options.duration_ms);

    pixy_buf_free(&previous);
    if (options.newline) fputc('\n', stdout);
    pixy_buf_free(&rewind);
    pixy_engine_free(engine);
    free_options(&options);
    return pixy_failed() ? pixy_error_code() : 0;
}

static const char *config_only(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) return argv[i + 1];
    }
    return NULL;
}

static int list_command(int argc, char **argv) {
    PixyEngine *engine = open_engine(config_only(argc, argv));
    if (!engine) return pixy_error_code();
    char **names = NULL;
    size_t count = 0;
    if (!pixy_engine_inventory(engine, &names, &count, NULL, NULL)) {
        pixy_engine_free(engine);
        return pixy_error_code();
    }
    for (size_t i = 0; i < count; i++) {
        printf("%s\n", names[i]);
        free(names[i]);
    }
    free(names);
    pixy_engine_free(engine);
    return 0;
}

static int check_command(int argc, char **argv) {
    PixyEngine *engine = open_engine(config_only(argc, argv));
    if (!engine) return pixy_error_code();
    char **names = NULL;
    size_t count = 0, zones = 0, segments = 0;
    if (!pixy_engine_inventory(engine, &names, &count, &zones, &segments)) {
        pixy_engine_free(engine);
        return pixy_error_code();
    }
    /* A render forgives a broken palette so a prompt still draws, which leaves
     * check as the one place the mistake can surface. */
    PixyPaletteEntry *palette = NULL;
    size_t colours = 0;
    long slot = PIXY_PALETTE_DEFAULT_SLOT;
    bool palette_ok = pixy_engine_palette(engine, &palette, &colours, &slot);
    free(palette);
    if (!palette_ok) {
        for (size_t i = 0; i < count; i++) free(names[i]);
        free(names);
        pixy_engine_free(engine);
        return pixy_error_code();
    }
    printf("ok %s (%zu zones, %zu segments", pixy_engine_source_name(engine), zones, segments);
    if (colours) printf(", %zu palette colours in slot %ld", colours, slot);
    printf(")\n");
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    pixy_engine_free(engine);
    return 0;
}

static int names_command(int argc, char **argv) {
    if (argc > 1) {
        pixy_fail(PIXY_EXIT_USAGE, "usage: pixy names [<pack>]");
        return PIXY_EXIT_USAGE;
    }
    const char *pack = argc == 1 ? argv[0] : "pokemon";
    char **names = NULL;
    size_t count = 0;
    if (pixy_embedded_names(pack, &names, &count)) {
        for (size_t i = 0; i < count; i++) {
            printf("%s\n", names[i]);
            free(names[i]);
        }
        free(names);
        return 0;
    }
    PixyPaths paths;
    if (!pixy_paths_discover(&paths)) return pixy_error_code();
    char path[4300];
    snprintf(path, sizeof(path), "%s/%s.pixypack", paths.data_dir, pack);
    PixyPack loaded;
    if (!pixy_pack_load(path, &loaded)) {
        pixy_clear_error();
        pixy_fail(PIXY_EXIT_USAGE, "unknown pack '%s'; `pixy pack list` shows what is installed",
                  pack);
        return PIXY_EXIT_USAGE;
    }
    for (size_t i = 0; i < loaded.count; i++) {
        const char *slash = strrchr(loaded.items[i].name, '/');
        printf("%s\n", slash ? slash + 1 : loaded.items[i].name);
    }
    pixy_pack_free(&loaded);
    return 0;
}

static int init_command(int argc, char **argv) {
    if (argc != 1) {
        pixy_fail(PIXY_EXIT_USAGE, "usage: pixy init <bash|zsh|fish|oslo|hexe-oslo>");
        return PIXY_EXIT_USAGE;
    }
    const char *text = NULL;
    if (strcmp(argv[0], "bash") == 0) text = PIXY_BASH_INIT;
    else if (strcmp(argv[0], "zsh") == 0) text = PIXY_ZSH_INIT;
    else if (strcmp(argv[0], "fish") == 0) text = PIXY_FISH_INIT;
    else if (strcmp(argv[0], "oslo") == 0) text = PIXY_OSLO_INIT;
    else if (strcmp(argv[0], "hexe-oslo") == 0) text = PIXY_HEXE_OSLO_CONFIG;
    if (!text) {
        pixy_fail(PIXY_EXIT_USAGE, "usage: pixy init <bash|zsh|fish|oslo|hexe-oslo>");
        return PIXY_EXIT_USAGE;
    }
    fputs(text, stdout);
    return 0;
}

/* Installed packs follow the embedded one, in name order. */
static void list_installed_packs(const char *directory) {
    DIR *dir = opendir(directory);
    if (!dir) return;
    char names[256][256];
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < 256) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".pixypack") != 0) continue;
        /* A name that does not fit is skipped, not shortened: a shortened one
         * names a pack nobody can ask for. */
        if (strlen(entry->d_name) >= sizeof(names[count])) continue;
        snprintf(names[count], sizeof(names[count]), "%s", entry->d_name);
        count++;
    }
    closedir(dir);
    qsort(names, count, sizeof(names[0]), (int (*)(const void *, const void *))strcmp);
    for (size_t i = 0; i < count; i++) {
        char path[4400];
        if (snprintf(path, sizeof(path), "%s/%s", directory, names[i]) >= (int)sizeof(path))
            continue;
        PixyPack pack;
        if (!pixy_pack_load(path, &pack)) {
            pixy_clear_error();
            continue;
        }
        char stem[sizeof(names[0])];
        memcpy(stem, names[i], sizeof(stem));
        char *dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        printf("%s\t%zu\t%s\n", stem, pack.count, pack.source);
        pixy_pack_free(&pack);
    }
}

static int pack_command(int argc, char **argv) {
    if (argc == 0) {
        pixy_fail(PIXY_EXIT_USAGE, "usage: pixy pack <build|check|list>");
        return PIXY_EXIT_USAGE;
    }
    if (strcmp(argv[0], "build") == 0) {
        const char *directory = argc > 1 ? argv[1] : NULL;
        const char *output = NULL, *source = "", *license = "", *attribution = "";
        for (int i = 2; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--output") == 0) output = argv[i + 1];
            else if (strcmp(argv[i], "--source") == 0) source = argv[i + 1];
            else if (strcmp(argv[i], "--license") == 0) license = argv[i + 1];
            else if (strcmp(argv[i], "--attribution") == 0) attribution = argv[i + 1];
        }
        if (!directory || !output) {
            pixy_fail(PIXY_EXIT_USAGE, "usage: pixy pack build <directory> --output <file>");
            return PIXY_EXIT_USAGE;
        }
        if (!pixy_pack_build(directory, output, source, license, attribution))
            return pixy_error_code();
        return 0;
    }
    if (strcmp(argv[0], "check") == 0) {
        if (argc != 2) {
            pixy_fail(PIXY_EXIT_USAGE, "usage: pixy pack check <file>");
            return PIXY_EXIT_USAGE;
        }
        PixyPack pack;
        if (!pixy_pack_load(argv[1], &pack)) return pixy_error_code();
        printf("ok %zu items\n", pack.count);
        pixy_pack_free(&pack);
        return 0;
    }
    if (strcmp(argv[0], "list") == 0) {
        if (argc == 1) {
            printf("pokemon\t%zu\t%s (embedded)\n", pixy_embedded_count(), pixy_embedded_source());
            PixyPaths paths;
            if (pixy_paths_discover(&paths)) list_installed_packs(paths.data_dir);
            return 0;
        }
        if (argc != 2) {
            pixy_fail(PIXY_EXIT_USAGE, "usage: pixy pack list <file>");
            return PIXY_EXIT_USAGE;
        }
        PixyPack pack;
        if (!pixy_pack_load(argv[1], &pack)) return pixy_error_code();
        printf("source\t%s\n", pack.source);
        printf("license\t%s\n", pack.license);
        for (size_t i = 0; i < pack.count; i++) {
            printf("%s\t%u\tfnv1a64:%016llx\n", pack.items[i].name, pack.items[i].raw_size,
                   (unsigned long long)pack.items[i].checksum);
        }
        pixy_pack_free(&pack);
        return 0;
    }
    pixy_fail(PIXY_EXIT_USAGE, "unknown pack command '%s'", argv[0]);
    return PIXY_EXIT_USAGE;
}

/* `pixy palette …` is a front door onto the sequences a config would emit, so
 * anything the CLI does an application can do, byte for byte. */
static int palette_command(int argc, char **argv) {
    if (argc == 0) {
        pixy_fail(PIXY_EXIT_USAGE, "usage: pixy palette <set|use|end|reset|ask> [--slot N]");
        return PIXY_EXIT_USAGE;
    }
    const char *verb = argv[0];
    const char *config_path = NULL;
    /* Every argument is checked whole. Truncating one would be worse than
     * refusing it: `--slot 00000002` cut to seven characters addresses slot 0,
     * the ordinary palette for the entire pane. */
    char slot[16];
    snprintf(slot, sizeof(slot), "%d", PIXY_PALETTE_DEFAULT_SLOT);
    bool slot_given = false;
    bool wait_for_reply = false;
    long timeout_ms = 100;

    enum { MAX_ENTRIES = 256 };
    PixyPaletteEntry *given = calloc(MAX_ENTRIES, sizeof(PixyPaletteEntry));
    if (!given) {
        pixy_fail(PIXY_EXIT_TRANSPORT, "out of memory");
        return PIXY_EXIT_TRANSPORT;
    }
    size_t given_count = 0;
    int code = 0;
    for (int i = 1; i < argc && code == 0; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--wait") == 0) {
            wait_for_reply = true;
            continue;
        }
        if (strcmp(arg, "--timeout-ms") == 0) {
            if (i + 1 >= argc) {
                pixy_fail(PIXY_EXIT_USAGE, "%s requires a value", arg);
                code = PIXY_EXIT_USAGE;
                break;
            }
            char *stop = NULL;
            timeout_ms = strtol(argv[++i], &stop, 10);
            if (!stop || *stop != '\0' || timeout_ms < 1 || timeout_ms > 10000) {
                pixy_fail(PIXY_EXIT_USAGE, "a timeout is 1-10000 milliseconds");
                code = PIXY_EXIT_USAGE;
            }
            continue;
        }
        bool is_slot = strcmp(arg, "--slot") == 0 || strcmp(arg, "--ns") == 0;
        if (is_slot || strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc) {
                pixy_fail(PIXY_EXIT_USAGE, "%s requires a value", arg);
                code = PIXY_EXIT_USAGE;
                break;
            }
            const char *value = argv[++i];
            if (!is_slot) {
                config_path = value;
            } else if (strlen(value) >= sizeof(slot)) {
                pixy_fail(PIXY_EXIT_USAGE, "'%s' is not a slot 0-%d", value, PIXY_PALETTE_MAX_SLOT);
                code = PIXY_EXIT_USAGE;
            } else {
                snprintf(slot, sizeof(slot), "%s", value);
                slot_given = true;
            }
            continue;
        }
        const char *equals = strchr(arg, '=');
        if (!equals) {
            pixy_fail(PIXY_EXIT_USAGE, "unknown palette argument '%s'; expected <key>=<colour>",
                      arg);
            code = PIXY_EXIT_USAGE;
            break;
        }
        if (given_count == MAX_ENTRIES) {
            pixy_fail(PIXY_EXIT_USAGE, "at most %d entries at a time", MAX_ENTRIES);
            code = PIXY_EXIT_USAGE;
            break;
        }
        PixyPaletteEntry *entry = &given[given_count];
        size_t key_len = (size_t)(equals - arg);
        if (key_len >= sizeof(entry->key) || strlen(equals + 1) >= sizeof(entry->colour)) {
            pixy_fail(PIXY_EXIT_USAGE, "'%s' is longer than a palette entry can be", arg);
            code = PIXY_EXIT_USAGE;
            break;
        }
        memcpy(entry->key, arg, key_len);
        entry->key[key_len] = '\0';
        snprintf(entry->colour, sizeof(entry->colour), "%s", equals + 1);
        if (!pixy_palette_valid_key(entry->key)) {
            pixy_fail(PIXY_EXIT_USAGE, "'%s' is not an index 0-255, fg, bg or cursor", entry->key);
            code = PIXY_EXIT_USAGE;
            break;
        }
        if (!pixy_palette_valid_colour(entry->colour)) {
            pixy_fail(PIXY_EXIT_USAGE, "'%s' is not #rrggbb, rrggbb or rgb:rr/gg/bb",
                      entry->colour);
            code = PIXY_EXIT_USAGE;
            break;
        }
        given_count++;
    }
    if (code != 0) {
        free(given);
        return code;
    }

    bool selecting = strcmp(verb, "use") == 0;
    bool slotless = strcmp(verb, "end") == 0 || strcmp(verb, "ask") == 0;
    if (slot_given && slotless) {
        pixy_fail(PIXY_EXIT_USAGE, "%s takes no slot: %s", verb,
                  slot_given && strcmp(verb, "end") == 0
                      ? "end pops whatever use pushed"
                      : "ask asks about the terminal, not a slot");
        free(given);
        return PIXY_EXIT_USAGE;
    }
    /* `*` addresses every slot already in use, so it patches and forgets but
     * cannot be claimed — there is no one namespace for it to select. */
    bool star = strcmp(slot, "*") == 0;
    if (star && selecting) {
        pixy_fail(PIXY_EXIT_USAGE, "use takes one slot 2-%d, not '*'", PIXY_PALETTE_MAX_SLOT);
        free(given);
        return PIXY_EXIT_USAGE;
    }
    if (!star && !slotless) {
        char *stop = NULL;
        long value = strtol(slot, &stop, 10);
        if (!stop || *stop != '\0' || !pixy_palette_valid_slot(value, selecting)) {
            pixy_fail(PIXY_EXIT_USAGE, "slot must be %s-%d%s", selecting ? "2" : "0",
                      PIXY_PALETTE_MAX_SLOT,
                      selecting ? " (0 is the palette, 1 the terminal's)" : "");
            free(given);
            return PIXY_EXIT_USAGE;
        }
        /* Emit the number, not how it was typed: `007` and `7` address one slot
         * and should replay as one sequence. */
        snprintf(slot, sizeof(slot), "%ld", value);
    }

    PixyBuf out = {0};
    if (strcmp(verb, "set") == 0) {
        if (given_count == 0) {
            /* Nothing named on the command line: emit what the config declared,
             * which is where a prompt's colours belong. */
            PixyEngine *engine = open_engine(config_path);
            if (!engine) {
                free(given);
                return pixy_error_code();
            }
            PixyPaletteEntry *entries = NULL;
            size_t count = 0;
            long config_slot = PIXY_PALETTE_DEFAULT_SLOT;
            bool ok = pixy_engine_palette(engine, &entries, &count, &config_slot);
            pixy_engine_free(engine);
            if (!ok) {
                free(given);
                return pixy_error_code();
            }
            if (!slot_given && !star) snprintf(slot, sizeof(slot), "%ld", config_slot);
            pixy_palette_set(&out, slot, entries, count, PIXY_TARGET_ANSI);
            free(entries);
        } else {
            pixy_palette_set(&out, slot, given, given_count, PIXY_TARGET_ANSI);
        }
    } else if (strcmp(verb, "use") == 0) {
        pixy_palette_use(&out, strtol(slot, NULL, 10), PIXY_TARGET_ANSI);
    } else if (strcmp(verb, "end") == 0) {
        pixy_palette_end(&out, PIXY_TARGET_ANSI);
    } else if (strcmp(verb, "reset") == 0) {
        pixy_palette_reset(&out, slot, PIXY_TARGET_ANSI);
    } else if (strcmp(verb, "ask") == 0) {
        /* Without --wait this stays an emitter like every other verb, so it
         * composes. With it, pixy does the round trip itself and reports what
         * came back; silence is the protocol's "unsupported", so it is an
         * answer to branch on rather than a failure to report. */
        if (wait_for_reply) {
            unsigned answered_osc = 0;
            long answered_max = 0;
            if (!pixy_palette_query(timeout_ms, &answered_osc, &answered_max)) {
                free(given);
                return PIXY_EXIT_UNSUPPORTED;
            }
            printf("%u %ld\n", answered_osc, answered_max);
        } else {
            pixy_palette_ask(&out, PIXY_TARGET_ANSI);
        }
    } else {
        pixy_fail(PIXY_EXIT_USAGE, "unknown palette command '%s'", verb);
        code = PIXY_EXIT_USAGE;
    }
    if (code == 0 && out.len) fwrite(out.data, 1, out.len, stdout);
    pixy_buf_free(&out);
    free(given);
    return code;
}

/* Answers on stdin and stdout, and only there.
 *
 * There used to be a `--socket` server as well, and it was the wrong shape for
 * what a painter is: one process on the machine that every caller shared, whose
 * accept loop serialised them, whose config outlived the binary that wrote it,
 * and that nothing ever shut down. A caller that spawns pixy owns it instead --
 * it starts with them, answers only them, and exits when their end of the pipe
 * closes, which needs no shutdown protocol at all. */
static int serve_command(int argc, char **argv) {
    const char *config_path = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "--stdio") == 0) continue; /* the only transport; accepted so
                                                             * existing callers keep working */
    }
    return pixy_serve_stdio(config_path);
}

int pixy_main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "help") == 0) {
        print_help();
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0 ||
        strcmp(argv[1], "version") == 0) {
        printf("pixy %s\n", PIXY_VERSION);
        return 0;
    }

    const char *command = argv[1];
    int rest = argc - 2;
    char **args = argv + 2;

    for (int i = 0; i < rest; i++) {
        if (strcmp(args[i], "--help") == 0 || strcmp(args[i], "-h") == 0) {
            if (command_help(command)) return 0;
        }
    }

    if (strcmp(command, "render") == 0) return render_command(rest, args, true);
    if (strcmp(command, "stream") == 0) return stream_command(rest, args);
    if (strcmp(command, "serve") == 0) return serve_command(rest, args);
    if (strcmp(command, "list") == 0) return list_command(rest, args);
    if (strcmp(command, "check") == 0) return check_command(rest, args);
    if (strcmp(command, "names") == 0) return names_command(rest, args);
    if (strcmp(command, "init") == 0) return init_command(rest, args);
    if (strcmp(command, "pack") == 0) return pack_command(rest, args);
    if (strcmp(command, "palette") == 0) return palette_command(rest, args);
    if (strcmp(command, "__bench") == 0) return pixy_bench(rest, args);

    if (valid_selector(command)) {
        int code = render_command(argc - 1, argv + 1, true);
        /* A bare word is a selector, so a typo reaches Lua and comes back as
         * "unknown zone" — true, but unhelpful when a verb was meant. */
        if (code == PIXY_EXIT_RENDER && strstr(pixy_error_message(), "unknown zone or segment")) {
            pixy_clear_error();
            pixy_fail(PIXY_EXIT_USAGE,
                      "no zone or command named '%s'; `pixy list` shows the zones, `pixy --help` "
                      "the commands",
                      command);
            return PIXY_EXIT_USAGE;
        }
        return code;
    }
    pixy_fail(PIXY_EXIT_USAGE, "unknown command or selector '%s'", command);
    return PIXY_EXIT_USAGE;
}
