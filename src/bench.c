#include "pixy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (long long)time.tv_sec * 1000000000 + time.tv_nsec;
}

static int compare_ll(const void *left, const void *right) {
    long long a = *(const long long *)left;
    long long b = *(const long long *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static long long p95(long long *samples, size_t count) {
    qsort(samples, count, sizeof(long long), compare_ll);
    size_t index = count * 95 / 100;
    return samples[index < count ? index : count - 1];
}

static PixyRequest request_for(char *selector) {
    PixyRequest request = {0};
    request.select = &selector;
    request.select_count = 1;
    request.mode = PIXY_MODE_LINE;
    request.has_target = true;
    request.target = PIXY_TARGET_ANSI;
    request.width = 100;
    request.height = 1;
    request.has_now_ms = true;
    request.context_json = "{\"values\":{}}";
    request.context_json_len = 13;
    return request;
}

static PixyEngine *load_engine(const char *config, PixyPaths *paths) {
    PixyConfigSource source;
    if (!pixy_config_load(config, paths, &source)) return NULL;
    PixyEngine *engine = pixy_engine_load(&source, paths);
    pixy_config_free(&source);
    return engine;
}

static int bench_cold(size_t runs, const char *config, const char *selector) {
    PixyPaths paths;
    if (!pixy_paths_discover(&paths)) return 1;
    long long *samples = calloc(runs, sizeof(long long));
    if (!samples) return 1;
    for (size_t i = 0; i < runs; i++) {
        long long started = now_ns();
        PixyEngine *engine = load_engine(config, &paths);
        if (!engine) return 1;
        PixyRequest request = request_for((char *)selector);
        PixyOutput output;
        if (!pixy_engine_render(engine, &request, &output)) return 1;
        pixy_output_free(&output);
        pixy_engine_free(engine);
        samples[i] = now_ns() - started;
    }
    printf("cold_p95_ns=%lld\n", p95(samples, runs));
    free(samples);
    return 0;
}

static int bench_query(size_t runs, const char *config, const char *selector, const char *label) {
    PixyPaths paths;
    if (!pixy_paths_discover(&paths)) return 1;
    PixyEngine *engine = load_engine(config, &paths);
    if (!engine) return 1;
    long long *samples = calloc(runs, sizeof(long long));
    if (!samples) return 1;
    for (size_t i = 0; i < runs; i++) {
        PixyRequest request = request_for((char *)selector);
        long long started = now_ns();
        PixyOutput output;
        if (!pixy_engine_render(engine, &request, &output)) return 1;
        pixy_output_free(&output);
        samples[i] = now_ns() - started;
    }
    printf("%s_p95_ns=%lld\n", label, p95(samples, runs));
    free(samples);
    pixy_engine_free(engine);
    return 0;
}

static int bench_phases(size_t runs, const char *config, const char *selector) {
    long long discover = 0, read = 0, load = 0, render = 0;
    for (size_t i = 0; i < runs; i++) {
        long long a = now_ns();
        PixyPaths paths;
        if (!pixy_paths_discover(&paths)) return 1;
        long long b = now_ns();
        PixyConfigSource source;
        if (!pixy_config_load(config, &paths, &source)) return 1;
        long long c = now_ns();
        PixyEngine *engine = pixy_engine_load(&source, &paths);
        pixy_config_free(&source);
        if (!engine) return 1;
        long long d = now_ns();
        PixyRequest request = request_for((char *)selector);
        PixyOutput output;
        if (!pixy_engine_render(engine, &request, &output)) return 1;
        pixy_output_free(&output);
        long long e = now_ns();
        pixy_engine_free(engine);
        discover += b - a;
        read += c - b;
        load += d - c;
        render += e - d;
    }
    printf("phase_discover_ns=%lld\n", discover / (long long)runs);
    printf("phase_read_config_ns=%lld\n", read / (long long)runs);
    printf("phase_engine_load_ns=%lld\n", load / (long long)runs);
    printf("phase_render_ns=%lld\n", render / (long long)runs);
    return 0;
}

int pixy_bench(int argc, char **argv) {
    if (argc < 1) {
        pixy_fail(PIXY_EXIT_USAGE,
                  "usage: pixy __bench <cold|query|provider|phases> [count] [config]");
        return PIXY_EXIT_USAGE;
    }
    size_t count = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 0;
    const char *config = argc > 2 ? argv[2] : "config/init.lua";
    if (strcmp(argv[0], "cold") == 0)
        return bench_cold(count ? count : 500, config, "prompt.left.directory");
    if (strcmp(argv[0], "query") == 0) {
        int code = bench_query(count ? count : 10000, config, "prompt.left.directory", "query");
        printf("lua_memory_limit_bytes=%u\n", PIXY_MEMORY_LIMIT);
        return code;
    }
    if (strcmp(argv[0], "provider") == 0)
        return bench_query(count ? count : 100, config, "prompt.left.git", "provider_exec");
    if (strcmp(argv[0], "phases") == 0)
        return bench_phases(count ? count : 200, config, "prompt.left.directory");
    pixy_fail(PIXY_EXIT_USAGE, "usage: pixy __bench <cold|query|provider|phases> [count] [config]");
    return PIXY_EXIT_USAGE;
}
