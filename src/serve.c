/* The painter socket.
 *
 * A host connects, sends one request, reads one response and closes. Each
 * message is a four-byte big-endian length followed by one JSON value. The
 * selector in a request is a zone name, so what a view contains is decided by
 * the configuration and never by this file.
 */
#include "pixy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "host.h"

#define MAX_FRAME (1024u * 1024)
#define IO_TIMEOUT_SEC 2

static bool resolve_socket(const char *given, char *out, size_t size) {
    if (given && given[0]) {
        snprintf(out, size, "%s", given);
        return true;
    }
    const char *from_env = getenv("HEXE_PAINTER_SOCKET");
    if (from_env && from_env[0]) {
        snprintf(out, size, "%s", from_env);
        return true;
    }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char directory[4000];
    if (runtime && runtime[0]) {
        snprintf(directory, sizeof(directory), "%s/hexe", runtime);
    } else {
        snprintf(directory, sizeof(directory), "/tmp/hexe-%u", (unsigned)getuid());
    }
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        pixy_fail(PIXY_EXIT_TRANSPORT, "failed to create %s: %s", directory, strerror(errno));
        return false;
    }
    snprintf(out, size, "%s/painter.sock", directory);
    return true;
}

static bool socket_answers(const char *path) {
    size_t length = strlen(path);
    struct sockaddr_un address;
    /* Too long to be a socket address is too long to be one anybody is
     * listening on; truncating would ask about a different path. */
    if (length >= sizeof(address.sun_path)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, length + 1);
    bool live = connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0;
    close(fd);
    return live;
}

static bool read_exact(int fd, void *into, size_t want) {
    unsigned char *at = into;
    while (want) {
        ssize_t got = read(fd, at, want);
        if (got <= 0) return false;
        at += got;
        want -= (size_t)got;
    }
    return true;
}

static bool write_all(int fd, const void *from, size_t len) {
    const unsigned char *at = from;
    while (len) {
        ssize_t sent = write(fd, at, len);
        if (sent <= 0) return false;
        at += sent;
        len -= (size_t)sent;
    }
    return true;
}

/* One request, one response, on whatever pair of descriptors carries them.
 *
 * Split from the socket loop so the SAME framing serves a connection and a
 * pipe: a host that speaks this needs one encoder and one decoder, and picks a
 * transport rather than a protocol. A socket passes its fd for both; stdio
 * passes 0 and 1. */
static bool answer(PixyEngine *engine, int in_fd, int out_fd) {
    unsigned char header[4];
    if (!read_exact(in_fd, header, 4)) return false;
    size_t length = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) |
                    ((size_t)header[2] << 8) | header[3];
    if (length > MAX_FRAME) return false;
    char *body = malloc(length + 1);
    if (!body) return false;
    if (!read_exact(in_fd, body, length)) {
        free(body);
        return false;
    }
    body[length] = '\0';

    PixyBuf response = {0};
    PixyJson *request = pixy_json_parse(body, length);
    if (!request) {
        pixy_buf_str(&response,
                     "{\"error\":\"invalid request: not JSON\",\"ok\":false,\"version\":1}");
    } else {
        const PixyJson *select = pixy_json_get(request, "select");
        size_t count = pixy_json_count(select);
        char **names = calloc(count ? count : 1, sizeof(char *));
        size_t found = 0;
        for (size_t i = 0; i < count; i++) {
            size_t len = 0;
            const char *text = pixy_json_string(pixy_json_at(select, i), &len);
            if (text) names[found++] = strndup(text, len);
        }

        const PixyJson *mode = pixy_json_get(request, "mode");
        size_t mode_len = 0;
        const char *mode_text = mode ? pixy_json_string(mode, &mode_len) : NULL;
        PixyRequest render = {0};
        render.select = names;
        render.select_count = found;
        render.mode = PIXY_MODE_RUN;
        if (mode_text && mode_len == 7 && memcmp(mode_text, "surface", 7) == 0)
            render.mode = PIXY_MODE_SURFACE;
        const PixyJson *width = pixy_json_get(request, "width");
        const PixyJson *height = pixy_json_get(request, "height");
        const PixyJson *now = pixy_json_get(request, "now_ms");
        render.width = width ? (uint16_t)pixy_json_number(width) : 80;
        render.height = height ? (uint16_t)pixy_json_number(height) : 1;
        if (now) {
            render.has_now_ms = true;
            render.now_ms = (uint64_t)pixy_json_number(now);
        }

        /* A host flattens its state across the context: the nested `values` map
         * wins over a top-level name of its own, and `env` stays the host's. */
        PixyBuf context = {0};
        const PixyJson *given = pixy_json_get(request, "context");
        pixy_buf_str(&context, "{\"values\":{");
        bool first = true;
        for (size_t i = 0; i < pixy_json_count(given); i++) {
            size_t key_len = 0;
            const char *key = pixy_json_key(given, i, &key_len);
            if ((key_len == 6 && memcmp(key, "values", 6) == 0) ||
                (key_len == 3 && memcmp(key, "env", 3) == 0))
                continue;
            if (!first) pixy_buf_str(&context, ",");
            first = false;
            pixy_buf_json_string(&context, key, key_len);
            pixy_buf_str(&context, ":");
            pixy_json_write(pixy_json_at(given, i), &context);
        }
        const PixyJson *values = given ? pixy_json_get(given, "values") : NULL;
        for (size_t i = 0; i < pixy_json_count(values); i++) {
            size_t key_len = 0;
            const char *key = pixy_json_key(values, i, &key_len);
            if (!first) pixy_buf_str(&context, ",");
            first = false;
            pixy_buf_json_string(&context, key, key_len);
            pixy_buf_str(&context, ":");
            pixy_json_write(pixy_json_at(values, i), &context);
        }
        pixy_buf_str(&context, "}");
        const PixyJson *env = given ? pixy_json_get(given, "env") : NULL;
        if (env) {
            pixy_buf_str(&context, ",\"env\":");
            pixy_json_write(env, &context);
        }
        pixy_buf_str(&context, "}");
        render.context_json = context.data;
        render.context_json_len = context.len;

        PixyOutput output;
        pixy_clear_error();
        if (found == 0) {
            pixy_buf_str(&response,
                         "{\"error\":\"a request names no view\",\"ok\":false,\"version\":1}");
        } else if (pixy_engine_render(engine, &render, &output)) {
            PixyBuf payload = {0};
            pixy_output_json(&output, &payload);
            pixy_buf_str(&response, "{\"ok\":true,\"output\":");
            pixy_buf_add(&response, payload.data, payload.len);
            pixy_buf_str(&response, ",\"version\":1}");
            pixy_buf_free(&payload);
            pixy_output_free(&output);
        } else {
            pixy_buf_str(&response, "{\"error\":");
            pixy_buf_json_string(&response, pixy_error_message(), strlen(pixy_error_message()));
            pixy_buf_str(&response, ",\"ok\":false,\"version\":1}");
            pixy_clear_error();
        }

        for (size_t i = 0; i < found; i++) free(names[i]);
        free(names);
        pixy_buf_free(&context);
        pixy_json_free(request);
    }
    free(body);

    unsigned char out_header[4] = {(unsigned char)(response.len >> 24),
                                   (unsigned char)(response.len >> 16),
                                   (unsigned char)(response.len >> 8), (unsigned char)response.len};
    bool sent = write_all(out_fd, out_header, 4) && write_all(out_fd, response.data, response.len);
    pixy_buf_free(&response);
    return sent;
}

/* The same protocol, on stdin and stdout, for a host that would rather own its
 * painter than share one.
 *
 * A socket painter is a single process every session on the machine talks to:
 * one config to restart, one accept loop serialising everyone, and a stale one
 * outliving the binary that made it. Over a pipe the host spawns its own, keeps
 * it for as long as it wants it, and takes it down with itself -- while sending
 * byte-identical frames, so neither side needs a second code path.
 *
 * Loops until stdin closes, so one child answers many requests and the Lua VM
 * and config are paid for once rather than per frame. */
int pixy_serve_stdio(const char *config_path) {
    PixyPaths paths;
    if (!pixy_paths_discover(&paths)) return pixy_error_code();
    PixyConfigSource source;
    if (!pixy_config_load(config_path, &paths, &source)) return pixy_error_code();
    char watched[4096];
    snprintf(watched, sizeof(watched), "%s", source.path);
    PixyEngine *engine = pixy_engine_load(&source, &paths);
    pixy_config_free(&source);
    if (!engine) return pixy_error_code();

    struct stat info;
    time_t stamp = stat(watched, &info) == 0 ? info.st_mtime : 0;

    for (;;) {
        /* Same reload rule as the socket: saving the file is the procedure. */
        if (watched[0] && stat(watched, &info) == 0 && info.st_mtime != stamp) {
            stamp = info.st_mtime;
            PixyConfigSource next;
            if (pixy_config_load(config_path, &paths, &next)) {
                PixyEngine *reloaded = pixy_engine_load(&next, &paths);
                pixy_config_free(&next);
                if (reloaded) {
                    pixy_engine_free(engine);
                    engine = reloaded;
                } else {
                    fprintf(stderr, "pixy serve --stdio: keeping the last config: %s\n",
                            pixy_error_message());
                    pixy_clear_error();
                }
            }
        }
        if (!answer(engine, 0, 1)) break;
    }
    pixy_engine_free(engine);
    return 0;
}

int pixy_serve(const char *socket_path, const char *config_path, bool force) {
    PixyPaths paths;
    if (!pixy_paths_discover(&paths)) return pixy_error_code();
    PixyConfigSource source;
    if (!pixy_config_load(config_path, &paths, &source)) return pixy_error_code();
    char watched[4096];
    snprintf(watched, sizeof(watched), "%s", source.path);
    PixyEngine *engine = pixy_engine_load(&source, &paths);
    pixy_config_free(&source);
    if (!engine) return pixy_error_code();

    struct stat info;
    time_t stamp = stat(watched, &info) == 0 ? info.st_mtime : 0;

    char path[4096];
    if (!resolve_socket(socket_path, path, sizeof(path))) {
        pixy_engine_free(engine);
        return pixy_error_code();
    }
    /* Binding blind unlinks whatever was there, so a second painter silently
     * takes the first one's place and the host keeps talking to whichever bound
     * last. A socket that answers belongs to someone; only a dead one is ours. */
    if (!force && socket_answers(path)) {
        pixy_fail(PIXY_EXIT_TRANSPORT,
                  "a painter is already listening on %s; --force takes it over", path);
        pixy_engine_free(engine);
        return PIXY_EXIT_TRANSPORT;
    }
    unlink(path);

    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        pixy_fail(PIXY_EXIT_TRANSPORT, "socket: %s", strerror(errno));
        pixy_engine_free(engine);
        return PIXY_EXIT_TRANSPORT;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        pixy_fail(PIXY_EXIT_TRANSPORT, "failed to bind %s: path must be shorter than SUN_LEN",
                  path);
        close(listener);
        pixy_engine_free(engine);
        return PIXY_EXIT_TRANSPORT;
    }
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0) {
        pixy_fail(PIXY_EXIT_TRANSPORT, "failed to bind %s: %s", path, strerror(errno));
        close(listener);
        pixy_engine_free(engine);
        return PIXY_EXIT_TRANSPORT;
    }
    chmod(path, 0600);
    listen(listener, 16);
    fprintf(stderr, "pixy serve: listening on %s\n", path);

    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) continue;
        struct timeval timeout = {IO_TIMEOUT_SEC, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        /* A config edited under a running painter takes effect on the next
         * request, so saving the file is the whole reload procedure. */
        if (watched[0] && stat(watched, &info) == 0 && info.st_mtime != stamp) {
            stamp = info.st_mtime;
            PixyConfigSource next;
            if (pixy_config_load(config_path, &paths, &next)) {
                PixyEngine *reloaded = pixy_engine_load(&next, &paths);
                pixy_config_free(&next);
                if (reloaded) {
                    pixy_engine_free(engine);
                    engine = reloaded;
                } else {
                    fprintf(stderr, "pixy serve: keeping the last config: %s\n",
                            pixy_error_message());
                    pixy_clear_error();
                }
            }
        }

        answer(engine, fd, fd);
        close(fd);
    }
}
