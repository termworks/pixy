/* Sprite packs.
 *
 * Two formats, both unchanged from the Rust build so existing packs keep
 * working: `PIXYPK2` on disk, and the smaller `HXSP` archive compiled into the
 * binary for the Pokemon that ship with pixy.
 */
#include "pixy.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "miniz.h"

#define PACK_MAGIC "PIXYPK2\0"
#define PACK_HEADER 60
#define MAX_PACK_SIZE (16u * 1024 * 1024)
#define MAX_ITEMS 4096
#define MAX_ITEM_SIZE (1024u * 1024)

/* The embedded archive symbols. */
extern const unsigned char _binary_pokemon_hxsp_start[];
extern const unsigned char _binary_pokemon_hxsp_end[];

#define pixy_pokemon_pack _binary_pokemon_hxsp_start
#define pixy_pokemon_pack_len ((size_t)(_binary_pokemon_hxsp_end - _binary_pokemon_hxsp_start))

#define EMBED_MAGIC "HXSP"
#define EMBED_HEADER 16

static uint64_t fnv1a(const unsigned char *bytes, size_t len) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint32_t read_be32(const unsigned char *at) {
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
}

static uint64_t read_be64(const unsigned char *at) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | at[i];
    return value;
}

static uint32_t read_le32(const unsigned char *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
           ((uint32_t)at[3] << 24);
}

static void write_be16(PixyBuf *buf, uint16_t value) {
    char bytes[2] = {(char)(value >> 8), (char)value};
    pixy_buf_add(buf, bytes, 2);
}

static void write_be32(PixyBuf *buf, uint32_t value) {
    char bytes[4] = {(char)(value >> 24), (char)(value >> 16), (char)(value >> 8), (char)value};
    pixy_buf_add(buf, bytes, 4);
}

static void write_be64(PixyBuf *buf, uint64_t value) {
    char bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (char)(value >> (56 - i * 8));
    pixy_buf_add(buf, bytes, 8);
}

static unsigned char *inflate_raw(const unsigned char *bytes, size_t len, size_t raw_size,
                                  bool gzip) {
    if (raw_size > MAX_ITEM_SIZE) return NULL;
    unsigned char *out = malloc(raw_size + 1);
    if (!out) return NULL;
    mz_ulong produced = (mz_ulong)raw_size;
    int flags = gzip ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0;
    size_t written = tinfl_decompress_mem_to_mem(out, raw_size, bytes, len, flags);
    if (written == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || written != raw_size) {
        free(out);
        return NULL;
    }
    (void)produced;
    out[raw_size] = '\0';
    return out;
}

/* ---------------------------------------------------------- pixypack v2 */

bool pixy_pack_load(const char *path, PixyPack *out) {
    memset(out, 0, sizeof(*out));
    FILE *file = fopen(path, "rb");
    if (!file) {
        pixy_fail(PIXY_EXIT_TRANSPORT, "failed to read %s: %s", path, strerror(errno));
        return false;
    }
    unsigned char *bytes = malloc(MAX_PACK_SIZE);
    if (!bytes) {
        fclose(file);
        pixy_fail(PIXY_EXIT_TRANSPORT, "out of memory");
        return false;
    }
    size_t len = fread(bytes, 1, MAX_PACK_SIZE, file);
    fclose(file);

    if (len < PACK_HEADER || memcmp(bytes, PACK_MAGIC, 8) != 0 || read_be32(bytes + 8) != 2) {
        free(bytes);
        pixy_fail(PIXY_EXIT_TRANSPORT, "%s is not a pixy pack", path);
        return false;
    }
    uint32_t count = read_be32(bytes + 12);
    uint32_t source_len = read_be32(bytes + 16);
    uint32_t license_len = read_be32(bytes + 20);
    uint32_t attribution_len = read_be32(bytes + 24);
    uint64_t index_len = read_be64(bytes + 28);
    uint64_t data_len = read_be64(bytes + 36);
    uint64_t index_hash = read_be64(bytes + 44);

    size_t at = PACK_HEADER;
    if (count > MAX_ITEMS ||
        at + source_len + license_len + attribution_len + index_len + data_len > len) {
        free(bytes);
        pixy_fail(PIXY_EXIT_TRANSPORT, "%s is truncated", path);
        return false;
    }
    memcpy(out->source, bytes + at,
           source_len < sizeof(out->source) - 1 ? source_len : sizeof(out->source) - 1);
    at += source_len;
    memcpy(out->license, bytes + at,
           license_len < sizeof(out->license) - 1 ? license_len : sizeof(out->license) - 1);
    at += license_len;
    memcpy(out->attribution, bytes + at,
           attribution_len < sizeof(out->attribution) - 1 ? attribution_len
                                                          : sizeof(out->attribution) - 1);
    at += attribution_len;

    const unsigned char *index = bytes + at;
    if (fnv1a(index, (size_t)index_len) != index_hash) {
        free(bytes);
        pixy_fail(PIXY_EXIT_TRANSPORT, "%s has a corrupt index", path);
        return false;
    }
    const unsigned char *data = index + index_len;

    out->items = calloc(count ? count : 1, sizeof(PixyPackItem));
    if (!out->items) {
        free(bytes);
        return false;
    }
    size_t cursor = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (cursor + 26 > index_len) break;
        uint16_t name_len = (uint16_t)((index[cursor] << 8) | index[cursor + 1]);
        uint32_t raw_size = read_be32(index + cursor + 2);
        uint32_t stored_size = read_be32(index + cursor + 6);
        uint64_t checksum = read_be64(index + cursor + 10);
        uint64_t offset = read_be64(index + cursor + 18);
        cursor += 26;
        if (cursor + name_len > index_len || offset + stored_size > data_len) break;
        PixyPackItem *item = &out->items[out->count];
        size_t copy = name_len < sizeof(item->name) - 1 ? name_len : sizeof(item->name) - 1;
        memcpy(item->name, index + cursor, copy);
        item->name[copy] = '\0';
        cursor += name_len;
        item->raw_size = raw_size;
        item->checksum = checksum;
        item->bytes = inflate_raw(data + offset, stored_size, raw_size, false);
        item->len = item->bytes ? raw_size : 0;
        if (item->bytes && fnv1a(item->bytes, raw_size) != checksum) {
            free(item->bytes);
            item->bytes = NULL;
            item->len = 0;
        }
        out->count++;
    }
    free(bytes);
    return true;
}

void pixy_pack_free(PixyPack *pack) {
    for (size_t i = 0; i < pack->count; i++) free(pack->items[i].bytes);
    free(pack->items);
    pack->items = NULL;
    pack->count = 0;
}

typedef struct {
    char name[256];
    unsigned char *raw;
    size_t raw_len;
    unsigned char *stored;
    size_t stored_len;
    uint64_t checksum;
} BuildItem;

static int compare_items(const void *left, const void *right) {
    return strcmp(((const BuildItem *)left)->name, ((const BuildItem *)right)->name);
}

static bool collect(const char *root, const char *prefix, BuildItem **items, size_t *count,
                    size_t *cap) {
    char directory[4096];
    snprintf(directory, sizeof(directory), "%s%s%s", root, prefix[0] ? "/" : "", prefix);
    DIR *dir = opendir(directory);
    if (!dir) return false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char relative[512];
        snprintf(relative, sizeof(relative), "%s%s%s", prefix, prefix[0] ? "/" : "", entry->d_name);
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", root, relative);
        struct stat info;
        if (lstat(full, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            collect(root, relative, items, count, cap);
            continue;
        }
        if (!S_ISREG(info.st_mode)) continue; /* symlinks are refused, as before */
        FILE *file = fopen(full, "rb");
        if (!file) continue;
        unsigned char *raw = malloc(MAX_ITEM_SIZE + 1);
        if (!raw) {
            fclose(file);
            continue;
        }
        size_t got = fread(raw, 1, MAX_ITEM_SIZE + 1, file);
        fclose(file);
        if (got > MAX_ITEM_SIZE) {
            free(raw);
            continue;
        }
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 128;
            BuildItem *grown = realloc(*items, *cap * sizeof(BuildItem));
            if (!grown) {
                free(raw);
                closedir(dir);
                return false;
            }
            *items = grown;
        }
        BuildItem *item = &(*items)[*count];
        memset(item, 0, sizeof(*item));
        /* The name is how a render asks for this item again, so a truncated one
         * is an item nobody can name. Leave it out of the pack instead. */
        size_t name_len = strlen(relative);
        if (name_len >= sizeof(item->name)) {
            free(raw);
            continue;
        }
        memcpy(item->name, relative, name_len + 1);
        item->raw = raw;
        item->raw_len = got;
        item->checksum = fnv1a(raw, got);
        mz_ulong bound = mz_compressBound((mz_ulong)got);
        item->stored = malloc(bound ? bound : 1);
        mz_ulong stored_len = bound;
        if (!item->stored || mz_compress2(item->stored, &stored_len, raw, (mz_ulong)got,
                                          MZ_BEST_COMPRESSION) != MZ_OK) {
            free(item->stored);
            item->stored = NULL;
            continue;
        }
        /* The Rust build stored raw deflate; strip the two zlib header bytes and
         * the four-byte adler tail so old readers still understand the file. */
        item->stored_len = stored_len - 6;
        memmove(item->stored, item->stored + 2, item->stored_len);
        (*count)++;
    }
    closedir(dir);
    return true;
}

bool pixy_pack_build(const char *directory, const char *output, const char *source,
                     const char *license, const char *attribution) {
    BuildItem *items = NULL;
    size_t count = 0, cap = 0;
    if (!collect(directory, "", &items, &count, &cap) || count == 0) {
        pixy_fail(PIXY_EXIT_USAGE, "no items under %s", directory);
        return false;
    }
    qsort(items, count, sizeof(BuildItem), compare_items);

    PixyBuf index = {0};
    uint64_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        write_be16(&index, (uint16_t)strlen(items[i].name));
        write_be32(&index, (uint32_t)items[i].raw_len);
        write_be32(&index, (uint32_t)items[i].stored_len);
        write_be64(&index, items[i].checksum);
        write_be64(&index, offset);
        pixy_buf_str(&index, items[i].name);
        offset += items[i].stored_len;
    }

    PixyBuf content = {0};
    for (size_t i = 0; i < count; i++) {
        pixy_buf_str(&content, items[i].name);
        char be[8];
        for (int b = 0; b < 8; b++) be[b] = (char)((uint64_t)items[i].stored_len >> (56 - b * 8));
        pixy_buf_add(&content, be, 8);
        pixy_buf_add(&content, (const char *)items[i].stored, items[i].stored_len);
    }

    PixyBuf out = {0};
    pixy_buf_add(&out, PACK_MAGIC, 8);
    write_be32(&out, 2);
    write_be32(&out, (uint32_t)count);
    write_be32(&out, (uint32_t)strlen(source));
    write_be32(&out, (uint32_t)strlen(license));
    write_be32(&out, (uint32_t)strlen(attribution));
    write_be64(&out, (uint64_t)index.len);
    write_be64(&out, offset);
    write_be64(&out, fnv1a((const unsigned char *)index.data, index.len));
    write_be64(&out, fnv1a((const unsigned char *)content.data, content.len));
    pixy_buf_str(&out, source);
    pixy_buf_str(&out, license);
    pixy_buf_str(&out, attribution);
    pixy_buf_add(&out, index.data, index.len);
    for (size_t i = 0; i < count; i++) {
        pixy_buf_add(&out, (const char *)items[i].stored, items[i].stored_len);
    }

    bool ok = false;
    FILE *file = fopen(output, "wb");
    if (file) {
        ok = fwrite(out.data, 1, out.len, file) == out.len;
        fclose(file);
    }
    if (!ok) pixy_fail(PIXY_EXIT_TRANSPORT, "failed to write %s", output);

    for (size_t i = 0; i < count; i++) {
        free(items[i].raw);
        free(items[i].stored);
    }
    free(items);
    pixy_buf_free(&index);
    pixy_buf_free(&content);
    pixy_buf_free(&out);
    return ok;
}

/* ------------------------------------------------------- embedded archive */

static bool embed_header(size_t *data_start, size_t *count, size_t *data_len) {
    if (pixy_pokemon_pack_len < EMBED_HEADER || memcmp(pixy_pokemon_pack, EMBED_MAGIC, 4) != 0 ||
        read_le32(pixy_pokemon_pack + 4) != 1)
        return false;
    size_t items = read_le32(pixy_pokemon_pack + 8);
    size_t index_len = read_le32(pixy_pokemon_pack + 12);
    size_t start = EMBED_HEADER + index_len;
    if (items == 0 || items > MAX_ITEMS || start > pixy_pokemon_pack_len) return false;
    *data_start = start;
    *count = items;
    *data_len = pixy_pokemon_pack_len - start;
    return true;
}

unsigned char *pixy_embedded_item(const char *pack, const char *name, size_t *len) {
    if (strcmp(pack, "pokemon") != 0) return NULL;
    unsigned char want_kind;
    const char *wanted;
    if (strncmp(name, "regular/", 8) == 0) {
        want_kind = 0;
        wanted = name + 8;
    } else if (strncmp(name, "shiny/", 6) == 0) {
        want_kind = 1;
        wanted = name + 6;
    } else {
        return NULL;
    }
    if (!*wanted || strchr(wanted, '/')) return NULL;

    size_t data_start, count, data_len;
    if (!embed_header(&data_start, &count, &data_len)) return NULL;
    size_t cursor = EMBED_HEADER, offset = 0;
    for (size_t i = 0; i < count; i++) {
        if (cursor + 10 > data_start) return NULL;
        unsigned char kind = pixy_pokemon_pack[cursor];
        size_t name_len = pixy_pokemon_pack[cursor + 1];
        size_t raw_len = read_le32(pixy_pokemon_pack + cursor + 2);
        size_t stored_len = read_le32(pixy_pokemon_pack + cursor + 6);
        cursor += 10;
        if (cursor + name_len > data_start || offset + stored_len > data_len) return NULL;
        if (kind == want_kind && strlen(wanted) == name_len &&
            memcmp(pixy_pokemon_pack + cursor, wanted, name_len) == 0) {
            unsigned char *bytes =
                inflate_raw(pixy_pokemon_pack + data_start + offset, stored_len, raw_len, true);
            if (bytes && len) *len = raw_len;
            return bytes;
        }
        cursor += name_len;
        offset += stored_len;
    }
    return NULL;
}

static int compare_strings(const void *left, const void *right) {
    return strcmp(*(const char **)left, *(const char **)right);
}

bool pixy_embedded_names(const char *pack, char ***names_out, size_t *count_out) {
    if (strcmp(pack, "pokemon") != 0) return false;
    size_t data_start, count, data_len;
    if (!embed_header(&data_start, &count, &data_len)) return false;
    char **names = calloc(count, sizeof(char *));
    if (!names) return false;
    size_t found = 0, cursor = EMBED_HEADER;
    for (size_t i = 0; i < count; i++) {
        if (cursor + 10 > data_start) break;
        size_t name_len = pixy_pokemon_pack[cursor + 1];
        cursor += 10;
        if (cursor + name_len > data_start) break;
        names[found] = strndup((const char *)pixy_pokemon_pack + cursor, name_len);
        cursor += name_len;
        found++;
    }
    qsort(names, found, sizeof(char *), compare_strings);
    /* One id per creature: a name appears once however many variants carry it. */
    size_t unique = 0;
    for (size_t i = 0; i < found; i++) {
        if (unique && strcmp(names[unique - 1], names[i]) == 0) {
            free(names[i]);
            continue;
        }
        names[unique++] = names[i];
    }
    *names_out = names;
    *count_out = unique;
    return true;
}

size_t pixy_embedded_count(void) {
    size_t data_start, count, data_len;
    if (!embed_header(&data_start, &count, &data_len)) return 0;
    return count;
}

const char *pixy_embedded_source(void) {
    return "krabby / PokéSprite";
}
const char *pixy_embedded_license(void) {
    return "GPL-3.0-only; sprite images © Nintendo/Creatures Inc./GAME FREAK Inc.";
}
const char *pixy_embedded_attribution(void) {
    return "yannjor/krabby, msikma/PokéSprite, pokemon-generator-scripts, PokéAPI";
}
