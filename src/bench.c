/* `pixy __bench`: the numbers scripts/bench.sh holds to a budget.
 *
 * In-process on purpose. A CLI invocation is dominated by starting a process
 * and by whatever the config shells out to; these measure the engine, so a
 * regression in rendering is visible rather than buried under fork and exec.
 */
#include "pixy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "host.h"

extern const char PIXY_HEXE_OSLO_CONFIG[];

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int compare_ll(const void *left, const void *right) {
    long long a = *(const long long *)left, b = *(const long long *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static long long p95(long long *samples, size_t count) {
    if (count == 0) return 0;
    qsort(samples, count, sizeof(long long), compare_ll);
    size_t index = (count * 95) / 100;
    if (index >= count) index = count - 1;
    return samples[index];
}

static bool write_temp_config(char *path, size_t size, const char *source) {
    snprintf(path, size, "/tmp/pixy-bench-%d.lua", (int)getpid());
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    fputs(source, file);
    fclose(file);
    return true;
}

static PixyRequest request_for(char **selectors, size_t count, uint16_t width) {
    PixyRequest request = {0};
    request.select = selectors;
    request.select_count = count;
    request.mode = PIXY_MODE_LINE;
    request.has_target = true;
    request.target = PIXY_TARGET_ANSI;
    request.width = width;
    request.height = 1;
    request.has_now_ms = true;
    request.now_ms = 0;
    request.context_json = "{\"values\":{}}";
    request.context_json_len = 13;
    return request;
}

static int bench_cold(size_t runs, const char *config_source, const char *prefix,
                      const char *selector) {
    char path[256];
    if (config_source && !write_temp_config(path, sizeof(path), config_source)) return 1;
    PixyPaths paths;
    if (!pixy_paths_discover(&paths)) return 1;

    long long *samples = calloc(runs, sizeof(long long));
    char *selectors[1] = {(char *)selector};
    for (size_t i = 0; i < runs; i++) {
        long long started = now_ns();
        PixyConfigSource source;
        if (!pixy_config_load(config_source ? path : NULL, &paths, &source)) return 1;
        PixyEngine *engine = pixy_engine_load(&source, &paths);
        pixy_config_free(&source);
        if (!engine) return 1;
        PixyRequest request = request_for(selectors, 1, 100);
        PixyOutput output;
        if (pixy_engine_render(engine, &request, &output)) pixy_output_free(&output);
        pixy_engine_free(engine);
        samples[i] = now_ns() - started;
    }
    printf("%s_p95_ns=%lld\n", prefix, p95(samples, runs));
    free(samples);
    if (config_source) unlink(path);
    return 0;
}

static int bench_query(size_t runs, const char *config_source, const char *prefix,
                       const char *selector, const char *segment) {
    char path[256];
    if (config_source && !write_temp_config(path, sizeof(path), config_source)) return 1;
    PixyPaths paths;
    if (!pixy_paths_discover(&paths)) return 1;
    PixyConfigSource source;
    if (!pixy_config_load(config_source ? path : NULL, &paths, &source)) return 1;
    PixyEngine *engine = pixy_engine_load(&source, &paths);
    pixy_config_free(&source);
    if (!engine) return 1;

    char *selectors[1] = {(char *)selector};
    long long *samples = calloc(runs, sizeof(long long));
    for (size_t i = 0; i < runs; i++) {
        PixyRequest request = request_for(selectors, 1, 100);
        long long started = now_ns();
        PixyOutput output;
        if (pixy_engine_render(engine, &request, &output)) pixy_output_free(&output);
        samples[i] = now_ns() - started;
    }
    printf("%s_p95_ns=%lld\n", prefix, p95(samples, runs));

    if (segment) {
        char *one[1] = {(char *)segment};
        for (size_t i = 0; i < runs; i++) {
            PixyRequest request = request_for(one, 1, 100);
            long long started = now_ns();
            PixyOutput output;
            if (pixy_engine_render(engine, &request, &output)) pixy_output_free(&output);
            samples[i] = now_ns() - started;
        }
        printf("compat_segment_p95_ns=%lld\n", p95(samples, runs));
    }
    free(samples);
    pixy_engine_free(engine);
    if (config_source) unlink(path);
    return 0;
}

static const char PROVIDER_CONFIG[] =
    "local pixy = require(\"pixy\")\n"
    "return pixy.config({zones = {x = pixy.zone({pixy.segment(\"v\", function()\n"
    "  local result = pixy.host.exec({\"/bin/true\"}, {timeout_ms = 200, ttl_ms = 0})\n"
    "  return tostring(result.status)\n"
    "end)})}})\n";

static const char SIMPLE_CONFIG[] =
    "local pixy = require(\"pixy\")\n"
    "return pixy.config({zones = {x = pixy.zone({pixy.segment(\"v\", function(ctx)\n"
    "  return \"pixy \" .. tostring(ctx.values.status or 0)\n"
    "end)})}})\n";

int pixy_bench(int argc, char **argv) {
    if (argc < 1) {
        pixy_fail(PIXY_EXIT_USAGE, "usage: pixy __bench <cold|query|provider|compat> [count]");
        return PIXY_EXIT_USAGE;
    }
    size_t count = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 0;

    if (strcmp(argv[0], "cold") == 0) {
        return bench_cold(count ? count : 500, SIMPLE_CONFIG, "cold", "x");
    }
    if (strcmp(argv[0], "query") == 0) {
        int code = bench_query(count ? count : 10000, SIMPLE_CONFIG, "query", "x", NULL);
        printf("lua_memory_limit_bytes=%u\n", PIXY_MEMORY_LIMIT);
        return code;
    }
    if (strcmp(argv[0], "provider") == 0) {
        return bench_query(count ? count : 100, PROVIDER_CONFIG, "provider_exec", "x", NULL);
    }
    if (strcmp(argv[0], "compat") == 0) {
        int code =
            bench_cold(count ? count : 500, PIXY_HEXE_OSLO_CONFIG, "compat_cold", "prompt.left");
        if (code) return code;
        return bench_query(count ? count : 500, PIXY_HEXE_OSLO_CONFIG, "compat_query",
                           "prompt.left", "prompt.left.hostname");
    }
    pixy_fail(PIXY_EXIT_USAGE, "usage: pixy __bench <cold|query|provider|compat> [count]");
    return PIXY_EXIT_USAGE;
}
