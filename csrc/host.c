/* `__pixy_host`: the only door out of Lua.
 *
 * env, read, exec, cell_width and asset. Reads are confined to trusted roots,
 * execution is argv-only with a timeout and a cached result, and every call
 * spends from one per-render I/O budget so a config cannot stall a shell by
 * calling something slow in a loop.
 */
#include "host.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lauxlib.h"
#include "lua.h"

extern char **environ;

static PixyHost *host_of(lua_State *L) {
    return (PixyHost *)lua_touserdata(L, lua_upvalueindex(1));
}

long long pixy_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long long pixy_unix_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void pixy_host_begin_render(PixyHost *host) { host->io_spent_ms = 0; }

static long long io_remaining(const PixyHost *host) {
    long long left = PIXY_MAX_RENDER_IO_MS - host->io_spent_ms;
    return left < 0 ? 0 : left;
}

/* ------------------------------------------------------------------- env */

static bool valid_env_name(const char *name) {
    if (!name || !*name) return false;
    for (const char *at = name; *at; at++) {
        if (!isalnum((unsigned char)*at) && *at != '_') return false;
    }
    return true;
}

static int host_env(lua_State *L) {
    PixyHost *host = host_of(L);
    const char *name = luaL_checkstring(L, 1);
    if (!valid_env_name(name)) {
        lua_pushnil(L);
        return 1;
    }
    /* A caller-supplied context wins over the process environment, which is how
     * a request describes an environment it does not run in. */
    for (size_t i = 0; i < host->env_count; i++) {
        if (strcmp(host->env_names[i], name) == 0) {
            if (host->env_values[i]) {
                lua_pushstring(L, host->env_values[i]);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }
    }
    const char *value = getenv(name);
    if (value) {
        lua_pushstring(L, value);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int host_cell_width(lua_State *L) {
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    lua_pushinteger(L, (lua_Integer)pixy_cell_width(text, len));
    return 1;
}

/* ------------------------------------------------------------------ read */

static bool under_root(const char *resolved, const char *root) {
    size_t len = strlen(root);
    if (len == 0) return false;
    if (strncmp(resolved, root, len) != 0) return false;
    return resolved[len] == '\0' || resolved[len] == '/';
}

static int host_read(lua_State *L) {
    PixyHost *host = host_of(L);
    const char *path = luaL_checkstring(L, 1);
    char joined[4096];
    if (path[0] == '/') {
        snprintf(joined, sizeof(joined), "%s", path);
    } else {
        snprintf(joined, sizeof(joined), "%s/%s", host->roots[0], path);
    }
    char resolved[4096];
    if (!realpath(joined, resolved)) {
        lua_pushnil(L);
        return 1;
    }
    bool allowed = false;
    for (size_t i = 0; i < host->root_count; i++) {
        if (under_root(resolved, host->roots[i])) {
            allowed = true;
            break;
        }
    }
    if (!allowed) return luaL_error(L, "read outside the trusted roots: %s", path);

    struct stat info;
    if (lstat(resolved, &info) != 0 || !S_ISREG(info.st_mode)) {
        lua_pushnil(L);
        return 1;
    }
    long long started = pixy_now_ms();
    FILE *file = fopen(resolved, "rb");
    if (!file) {
        lua_pushnil(L);
        return 1;
    }
    char *bytes = malloc(PIXY_MAX_READ + 1);
    if (!bytes) {
        fclose(file);
        return luaL_error(L, "out of memory");
    }
    size_t got = fread(bytes, 1, PIXY_MAX_READ + 1, file);
    fclose(file);
    host->io_spent_ms += pixy_now_ms() - started;
    if (got > PIXY_MAX_READ) {
        free(bytes);
        return luaL_error(L, "%s exceeds %u bytes", path, PIXY_MAX_READ);
    }
    lua_pushlstring(L, bytes, got);
    free(bytes);
    return 1;
}

/* ------------------------------------------------------------------ exec */

typedef struct {
    int status;
    PixyBuf out;
    PixyBuf err;
    bool timed_out;
    bool truncated;
} ExecResult;

static void exec_result_free(ExecResult *result) {
    pixy_buf_free(&result->out);
    pixy_buf_free(&result->err);
}

/* Results already looked up in this process. A CLI render touches it once, but
 * `serve` renders forever in one process and would otherwise re-read and
 * re-parse the same cache files on every frame. */
typedef struct {
    uint64_t hash;
    long long expires_ms;
    int status;
    char *out;
    size_t out_len;
    char *err;
    size_t err_len;
} MemoEntry;

#define MEMO_SLOTS 64
static MemoEntry memo[MEMO_SLOTS];
static size_t memo_count;

static bool memo_get(uint64_t hash, ExecResult *result) {
    for (size_t i = 0; i < memo_count; i++) {
        if (memo[i].hash != hash) continue;
        if (memo[i].expires_ms <= pixy_unix_ms()) return false;
        result->status = memo[i].status;
        pixy_buf_add(&result->out, memo[i].out, memo[i].out_len);
        pixy_buf_add(&result->err, memo[i].err, memo[i].err_len);
        return true;
    }
    return false;
}

static void memo_put(uint64_t hash, long long ttl_ms, const ExecResult *result) {
    MemoEntry *slot = NULL;
    for (size_t i = 0; i < memo_count; i++) {
        if (memo[i].hash == hash) slot = &memo[i];
    }
    if (!slot) {
        if (memo_count == MEMO_SLOTS) {
            free(memo[0].out);
            free(memo[0].err);
            memmove(memo, memo + 1, (MEMO_SLOTS - 1) * sizeof(MemoEntry));
            memo_count--;
        }
        slot = &memo[memo_count++];
        memset(slot, 0, sizeof(*slot));
    } else {
        free(slot->out);
        free(slot->err);
    }
    slot->hash = hash;
    slot->expires_ms = pixy_unix_ms() + ttl_ms;
    slot->status = result->status;
    slot->out_len = result->out.len;
    slot->err_len = result->err.len;
    slot->out = malloc(slot->out_len + 1);
    slot->err = malloc(slot->err_len + 1);
    if (slot->out) memcpy(slot->out, result->out.data ? result->out.data : "", slot->out_len + 1);
    if (slot->err) memcpy(slot->err, result->err.data ? result->err.data : "", slot->err_len + 1);
}

static uint64_t fnv1a(const char *bytes, size_t len) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* The key covers the command, where it runs, the caller's own overrides, and
 * the variables that change what a program resolves to. Not the whole ambient
 * environment: a shell moves `_` and `PWD` between commands, and keying on
 * those meant nothing was ever reused between renders. */
static void cache_key(PixyBuf *key, char **argv, size_t argc, const char *cwd,
                      const char *env_pairs, long long timeout_ms, long long ttl_ms) {
    for (size_t i = 0; i < argc; i++) {
        pixy_buf_str(key, argv[i]);
        pixy_buf_add(key, "\x1f", 1);
    }
    pixy_buf_fmt(key, "\x1e%s\x1e%s\x1e", cwd ? cwd : "", env_pairs ? env_pairs : "");
    static const char *resolution[] = {"PATH", "HOME", "TZ", "LANG", "LC_ALL"};
    for (size_t i = 0; i < sizeof(resolution) / sizeof(resolution[0]); i++) {
        const char *value = getenv(resolution[i]);
        pixy_buf_fmt(key, "%s=%s\x1f", resolution[i], value ? value : "");
    }
    pixy_buf_fmt(key, "\x1e%lld\x1e%lld", timeout_ms, ttl_ms);
}

static bool cache_path(const PixyHost *host, uint64_t hash, char *out, size_t size) {
    if (!host->cache_dir[0]) return false;
    char version[4096];
    snprintf(version, sizeof(version), "%s/v1", host->cache_dir);
    if (mkdir(host->cache_dir, 0700) != 0 && errno != EEXIST) return false;
    if (mkdir(version, 0700) != 0 && errno != EEXIST) return false;
    snprintf(out, size, "%s/%016llx.json", version, (unsigned long long)hash);
    return true;
}

static bool cache_read(const PixyHost *host, uint64_t hash, ExecResult *result) {
    char path[4200];
    if (!cache_path(host, hash, path, sizeof(path))) return false;
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) return false;
    if (info.st_uid != geteuid() || (info.st_mode & 0077) != 0) return false;
    if (info.st_size <= 0 || info.st_size > 4 * 1024 * 1024) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t size = (size_t)info.st_size;
    char *bytes = malloc(size + 1);
    if (!bytes) {
        fclose(file);
        return false;
    }
    size_t got = fread(bytes, 1, size, file);
    fclose(file);
    PixyJson *value = pixy_json_parse(bytes, got);
    free(bytes);
    if (!value) return false;
    bool ok = false;
    const PixyJson *expires = pixy_json_get(value, "expires_at_ms");
    const PixyJson *inner = pixy_json_get(value, "value");
    if (expires && inner && pixy_json_number(expires) > (double)pixy_unix_ms()) {
        const PixyJson *status = pixy_json_get(inner, "status");
        const PixyJson *out = pixy_json_get(inner, "stdout");
        const PixyJson *err = pixy_json_get(inner, "stderr");
        size_t out_len = 0, err_len = 0;
        const char *out_text = out ? pixy_json_string(out, &out_len) : NULL;
        const char *err_text = err ? pixy_json_string(err, &err_len) : NULL;
        if (status && out_text && err_text) {
            result->status = (int)pixy_json_number(status);
            pixy_buf_add(&result->out, out_text, out_len);
            pixy_buf_add(&result->err, err_text, err_len);
            ok = true;
        }
    }
    pixy_json_free(value);
    return ok;
}

static void cache_write(const PixyHost *host, uint64_t hash, long long ttl_ms,
                        const ExecResult *result) {
    char path[4200];
    if (!cache_path(host, hash, path, sizeof(path))) return;
    PixyBuf body = {0};
    pixy_buf_fmt(&body, "{\"version\":1,\"key_hash\":\"%016llx\",\"created_at_ms\":%lld,",
                 (unsigned long long)hash, pixy_unix_ms());
    pixy_buf_fmt(&body, "\"expires_at_ms\":%lld,\"value\":{\"status\":%d,\"stdout\":",
                 pixy_unix_ms() + ttl_ms, result->status);
    pixy_buf_json_string(&body, result->out.data ? result->out.data : "", result->out.len);
    pixy_buf_str(&body, ",\"stderr\":");
    pixy_buf_json_string(&body, result->err.data ? result->err.data : "", result->err.len);
    pixy_buf_fmt(&body, ",\"timed_out\":%s,\"truncated\":%s}}",
                 result->timed_out ? "true" : "false", result->truncated ? "true" : "false");

    char temp[4300];
    snprintf(temp, sizeof(temp), "%s.tmp%d", path, (int)getpid());
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        ssize_t written = write(fd, body.data, body.len);
        close(fd);
        if (written == (ssize_t)body.len) {
            if (rename(temp, path) != 0) unlink(temp);
        } else {
            unlink(temp);
        }
    }
    pixy_buf_free(&body);
}

static void drain(int fd, PixyBuf *buf, bool *truncated) {
    char chunk[4096];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof(chunk))) > 0) {
        if (buf->len + (size_t)got > PIXY_MAX_EXEC_OUTPUT) {
            size_t room = PIXY_MAX_EXEC_OUTPUT - buf->len;
            if (room) pixy_buf_add(buf, chunk, room);
            *truncated = true;
            continue;
        }
        pixy_buf_add(buf, chunk, (size_t)got);
    }
}

/* argv-only: no shell, so nothing a config writes can be re-parsed as one. */
static bool spawn(char **argv, size_t argc, const char *cwd, char **env_names,
                  char **env_values, size_t env_count, long long timeout_ms, ExecResult *result) {
    (void)argc;
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0) return false;
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return false;
    }
    if (child == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        if (cwd && cwd[0] && chdir(cwd) != 0) _exit(127);
        for (size_t i = 0; i < env_count; i++) {
            if (env_values[i]) {
                setenv(env_names[i], env_values[i], 1);
            } else {
                unsetenv(env_names[i]);
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    long long deadline = pixy_now_ms() + timeout_ms;
    struct pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    bool open_fds[2] = {true, true};
    while (open_fds[0] || open_fds[1]) {
        long long left = deadline - pixy_now_ms();
        if (left <= 0) break;
        int ready = poll(fds, 2, (int)left);
        if (ready < 0) break;
        if (ready == 0) break;
        for (int i = 0; i < 2; i++) {
            if (!open_fds[i]) continue;
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                char chunk[4096];
                ssize_t got = read(fds[i].fd, chunk, sizeof(chunk));
                if (got > 0) {
                    PixyBuf *target = i == 0 ? &result->out : &result->err;
                    if (target->len + (size_t)got > PIXY_MAX_EXEC_OUTPUT) {
                        size_t room = PIXY_MAX_EXEC_OUTPUT - target->len;
                        if (room) pixy_buf_add(target, chunk, room);
                        result->truncated = true;
                    } else {
                        pixy_buf_add(target, chunk, (size_t)got);
                    }
                } else if (got == 0) {
                    open_fds[i] = false;
                    fds[i].fd = -1;
                }
            } else if (fds[i].revents & POLLERR) {
                open_fds[i] = false;
                fds[i].fd = -1;
            }
        }
    }

    int status = 0;
    bool reaped = false;
    while (pixy_now_ms() < deadline) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            reaped = true;
            break;
        }
        if (done < 0) break;
        struct timespec nap = {0, 1000000};
        nanosleep(&nap, NULL);
    }
    if (!reaped) {
        /* A command that outlived its timeout is killed rather than waited on:
         * the render has a deadline of its own to keep. */
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        result->timed_out = true;
    }
    if (open_fds[0]) drain(out_pipe[0], &result->out, &result->truncated);
    if (open_fds[1]) drain(err_pipe[0], &result->err, &result->truncated);
    close(out_pipe[0]);
    close(err_pipe[0]);
    result->status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}

static int host_exec(lua_State *L) {
    PixyHost *host = host_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    char *argv[PIXY_MAX_EXEC_ARGS + 1];
    size_t argc = 0;
    size_t argv_bytes = 0;
    for (lua_Integer i = 1;; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        size_t len = 0;
        const char *value = lua_tolstring(L, -1, &len);
        if (!value || argc >= PIXY_MAX_EXEC_ARGS || argv_bytes + len > PIXY_MAX_EXEC_OUTPUT) {
            lua_pop(L, 1);
            for (size_t k = 0; k < argc; k++) free(argv[k]);
            return luaL_error(L, "exec argv exceeds host limits");
        }
        argv[argc] = strdup(value);
        argv_bytes += len;
        argc++;
        lua_pop(L, 1);
    }
    argv[argc] = NULL;
    if (argc == 0 || argv[0][0] == '\0') {
        for (size_t k = 0; k < argc; k++) free(argv[k]);
        return luaL_error(L, "exec requires a non-empty argv");
    }

    long long timeout_ms = 100;
    long long ttl_ms = 0;
    const char *cwd = NULL;
    char *env_names[64];
    char *env_values[64];
    size_t env_count = 0;
    PixyBuf env_pairs = {0};

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "timeout_ms");
        if (lua_isnumber(L, -1)) timeout_ms = (long long)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "ttl_ms");
        if (lua_isnumber(L, -1)) ttl_ms = (long long)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "cwd");
        if (lua_isstring(L, -1)) cwd = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "env");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0 && env_count < 64) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    const char *name = lua_tostring(L, -2);
                    if (valid_env_name(name)) {
                        env_names[env_count] = strdup(name);
                        env_values[env_count] =
                            lua_isstring(L, -1) ? strdup(lua_tostring(L, -1)) : NULL;
                        pixy_buf_fmt(&env_pairs, "%s=%s\x1f", name,
                                     env_values[env_count] ? env_values[env_count] : "");
                        env_count++;
                    }
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    if (timeout_ms > PIXY_MAX_EXEC_TIMEOUT_MS) timeout_ms = PIXY_MAX_EXEC_TIMEOUT_MS;
    long long budget = io_remaining(host);
    if (timeout_ms > budget) timeout_ms = budget;
    if (ttl_ms > (long long)PIXY_MAX_CACHE_TTL_MS) ttl_ms = PIXY_MAX_CACHE_TTL_MS;
    if (timeout_ms <= 0) {
        for (size_t k = 0; k < argc; k++) free(argv[k]);
        pixy_buf_free(&env_pairs);
        return luaL_error(L, "render exhausted its host I/O budget");
    }

    ExecResult result = {0};
    uint64_t hash = 0;
    bool cached = false;
    if (ttl_ms > 0) {
        PixyBuf key = {0};
        cache_key(&key, argv, argc, cwd, env_pairs.data, timeout_ms, ttl_ms);
        hash = fnv1a(key.data ? key.data : "", key.len);
        pixy_buf_free(&key);
        cached = memo_get(hash, &result) || cache_read(host, hash, &result);
        if (cached) memo_put(hash, ttl_ms, &result);
    }

    if (!cached) {
        long long started = pixy_now_ms();
        if (!spawn(argv, argc, cwd, env_names, env_values, env_count, timeout_ms, &result)) {
            for (size_t k = 0; k < argc; k++) free(argv[k]);
            for (size_t k = 0; k < env_count; k++) {
                free(env_names[k]);
                free(env_values[k]);
            }
            pixy_buf_free(&env_pairs);
            exec_result_free(&result);
            return luaL_error(L, "failed to run %s", argv[0]);
        }
        host->io_spent_ms += pixy_now_ms() - started;
        if (ttl_ms > 0) {
            cache_write(host, hash, ttl_ms, &result);
            memo_put(hash, ttl_ms, &result);
        }
    }

    lua_newtable(L);
    lua_pushinteger(L, result.status);
    lua_setfield(L, -2, "status");
    lua_pushlstring(L, result.out.data ? result.out.data : "", result.out.len);
    lua_setfield(L, -2, "stdout");
    lua_pushlstring(L, result.err.data ? result.err.data : "", result.err.len);
    lua_setfield(L, -2, "stderr");
    lua_pushboolean(L, result.timed_out);
    lua_setfield(L, -2, "timed_out");
    lua_pushboolean(L, result.truncated);
    lua_setfield(L, -2, "truncated");

    for (size_t k = 0; k < argc; k++) free(argv[k]);
    for (size_t k = 0; k < env_count; k++) {
        free(env_names[k]);
        free(env_values[k]);
    }
    pixy_buf_free(&env_pairs);
    exec_result_free(&result);
    return 1;
}

/* ----------------------------------------------------------------- asset */

static int host_asset(lua_State *L) {
    PixyHost *host = host_of(L);
    const char *pack = luaL_checkstring(L, 1);
    const char *name = luaL_checkstring(L, 2);
    if (strstr(name, "..")) {
        lua_pushnil(L);
        return 1;
    }
    size_t len = 0;
    unsigned char *bytes = pixy_embedded_item(pack, name, &len);
    if (bytes) {
        lua_pushlstring(L, (const char *)bytes, len);
        free(bytes);
        return 1;
    }
    if (host->data_dir[0]) {
        char path[4300];
        snprintf(path, sizeof(path), "%s/%s.pixypack", host->data_dir, pack);
        PixyPack loaded;
        if (pixy_pack_load(path, &loaded)) {
            for (size_t i = 0; i < loaded.count; i++) {
                if (strcmp(loaded.items[i].name, name) == 0) {
                    lua_pushlstring(L, (const char *)loaded.items[i].bytes, loaded.items[i].len);
                    pixy_pack_free(&loaded);
                    return 1;
                }
            }
            pixy_pack_free(&loaded);
        }
        pixy_clear_error();
    }
    lua_pushnil(L);
    return 1;
}

void pixy_host_install(lua_State *L, PixyHost *host) {
    lua_newtable(L);

    lua_pushstring(L, "linux");
    lua_setfield(L, -2, "platform");

    struct {
        const char *name;
        lua_CFunction fn;
    } entries[] = {
        {"env", host_env},
        {"cell_width", host_cell_width},
        {"read", host_read},
        {"exec", host_exec},
        {"asset", host_asset},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        lua_pushlightuserdata(L, host);
        lua_pushcclosure(L, entries[i].fn, 1);
        lua_setfield(L, -2, entries[i].name);
    }
    lua_setglobal(L, "__pixy_host");
}

void pixy_host_set_env(PixyHost *host, char **names, char **values, size_t count) {
    for (size_t i = 0; i < host->env_count; i++) {
        free(host->env_names[i]);
        free(host->env_values[i]);
    }
    free(host->env_names);
    free(host->env_values);
    host->env_names = names;
    host->env_values = values;
    host->env_count = count;
}

void pixy_host_free(PixyHost *host) {
    pixy_host_set_env(host, NULL, NULL, 0);
}
