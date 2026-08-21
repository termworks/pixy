/* Builds the archive of Pokemon sprites that gets compiled into pixy.
 *
 * Same `HXSP` layout the Rust build produced, so the reader in assets.c is the
 * one that was already there: a little-endian header, an index of
 * (kind, name, raw size, stored size), then gzip members back to back.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "miniz.h"

#define MAX_SPRITES 4096
#define MAX_SPRITE_SIZE (1024 * 1024)

typedef struct {
    unsigned char kind; /* 0 regular, 1 shiny */
    char name[256];
    unsigned char *raw;
    size_t raw_len;
    unsigned char *stored;
    size_t stored_len;
} Sprite;

static int compare(const void *left, const void *right) {
    const Sprite *a = left, *b = right;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    return strcmp(a->name, b->name);
}

static void put_le32(FILE *out, unsigned int value) {
    unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8),
                              (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
    fwrite(bytes, 1, 4, out);
}

static size_t collect(const char *root, const char *variant, unsigned char kind, Sprite *sprites,
                      size_t count) {
    char directory[4096];
    snprintf(directory, sizeof(directory), "%s/%s", root, variant);
    DIR *dir = opendir(directory);
    if (!dir) {
        fprintf(stderr, "pack_sprites: cannot open %s\n", directory);
        exit(1);
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[4400];
        /* A truncated path names a different file; skip rather than pack it. */
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path))
            continue;
        struct stat info;
        if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        if (count >= MAX_SPRITES) break;
        FILE *file = fopen(path, "rb");
        if (!file) continue;
        unsigned char *raw = malloc(MAX_SPRITE_SIZE);
        size_t got = fread(raw, 1, MAX_SPRITE_SIZE, file);
        fclose(file);

        Sprite *sprite = &sprites[count];
        memset(sprite, 0, sizeof(*sprite));
        sprite->kind = kind;
        snprintf(sprite->name, sizeof(sprite->name), "%s", entry->d_name);
        sprite->raw = raw;
        sprite->raw_len = got;

        mz_ulong bound = mz_compressBound((mz_ulong)got) + 32;
        sprite->stored = malloc(bound);
        mz_ulong stored_len = bound;
        /* Level 9, zlib-wrapped: assets.c inflates with the zlib header flag. */
        if (mz_compress2(sprite->stored, &stored_len, raw, (mz_ulong)got, MZ_BEST_COMPRESSION) !=
            MZ_OK) {
            fprintf(stderr, "pack_sprites: failed to compress %s\n", path);
            exit(1);
        }
        sprite->stored_len = stored_len;
        count++;
    }
    closedir(dir);
    return count;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: pack_sprites <docs/assets/pokemon> <output.pack>\n");
        return 1;
    }
    Sprite *sprites = calloc(MAX_SPRITES, sizeof(Sprite));
    size_t count = collect(argv[1], "regular", 0, sprites, 0);
    count = collect(argv[1], "shiny", 1, sprites, count);
    qsort(sprites, count, sizeof(Sprite), compare);

    size_t index_len = 0;
    for (size_t i = 0; i < count; i++) index_len += 10 + strlen(sprites[i].name);

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "pack_sprites: cannot write %s\n", argv[2]);
        return 1;
    }
    fwrite("HXSP", 1, 4, out);
    put_le32(out, 1);
    put_le32(out, (unsigned int)count);
    put_le32(out, (unsigned int)index_len);
    for (size_t i = 0; i < count; i++) {
        unsigned char head[2] = {sprites[i].kind, (unsigned char)strlen(sprites[i].name)};
        fwrite(head, 1, 2, out);
        put_le32(out, (unsigned int)sprites[i].raw_len);
        put_le32(out, (unsigned int)sprites[i].stored_len);
        fwrite(sprites[i].name, 1, strlen(sprites[i].name), out);
    }
    for (size_t i = 0; i < count; i++) {
        fwrite(sprites[i].stored, 1, sprites[i].stored_len, out);
    }
    fclose(out);
    fprintf(stderr, "pack_sprites: %zu sprites\n", count);
    return 0;
}
