#ifndef PIXY_HOST_H
#define PIXY_HOST_H

#include "pixy.h"

typedef struct lua_State lua_State;

typedef struct {
    char roots[8][4096];
    size_t root_count;
    char data_dir[4096];
    char cache_dir[4096];
    /* The caller's environment view, consulted before the process's own. */
    char **env_names;
    char **env_values;
    size_t env_count;
    long long io_spent_ms;
} PixyHost;

void pixy_host_install(lua_State *L, PixyHost *host);
void pixy_host_begin_render(PixyHost *host);
void pixy_host_set_env(PixyHost *host, char **names, char **values, size_t count);
void pixy_host_free(PixyHost *host);

long long pixy_now_ms(void);
long long pixy_cpu_ms(void);
long long pixy_unix_ms(void);

#endif /* PIXY_HOST_H */
