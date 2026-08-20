/* A JSON reader for what callers hand pixy: a render context, a painter
 * request. Small on purpose — the shapes are fixed and the input is bounded. */
#include "pixy.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

struct PixyJson {
    PixyJsonKind kind;
    bool boolean;
    double number;
    char *text;
    size_t text_len;
    PixyJson **items;
    char **keys;
    size_t *key_lens;
    size_t count;
    size_t cap;
};

typedef struct {
    const char *at;
    const char *end;
    int depth;
} Reader;

static PixyJson *parse_value(Reader *reader);

static PixyJson *node(PixyJsonKind kind) {
    PixyJson *value = calloc(1, sizeof(PixyJson));
    if (value) value->kind = kind;
    return value;
}

void pixy_json_free(PixyJson *value) {
    if (!value) return;
    for (size_t i = 0; i < value->count; i++) {
        pixy_json_free(value->items[i]);
        if (value->keys) free(value->keys[i]);
    }
    free(value->items);
    free(value->keys);
    free(value->key_lens);
    free(value->text);
    free(value);
}

static bool push(PixyJson *parent, char *key, size_t key_len, PixyJson *child) {
    if (parent->count == parent->cap) {
        size_t want = parent->cap ? parent->cap * 2 : 8;
        PixyJson **items = realloc(parent->items, want * sizeof(*items));
        if (!items) return false;
        parent->items = items;
        if (parent->kind == PIXY_JSON_OBJECT) {
            char **keys = realloc(parent->keys, want * sizeof(*keys));
            if (!keys) return false;
            parent->keys = keys;
            size_t *lens = realloc(parent->key_lens, want * sizeof(*lens));
            if (!lens) return false;
            parent->key_lens = lens;
        }
        parent->cap = want;
    }
    parent->items[parent->count] = child;
    if (parent->kind == PIXY_JSON_OBJECT) {
        parent->keys[parent->count] = key;
        parent->key_lens[parent->count] = key_len;
    }
    parent->count++;
    return true;
}

static void skip_space(Reader *reader) {
    while (reader->at < reader->end && (*reader->at == ' ' || *reader->at == '\t' ||
                                        *reader->at == '\n' || *reader->at == '\r'))
        reader->at++;
}

static bool literal(Reader *reader, const char *word) {
    size_t len = strlen(word);
    if ((size_t)(reader->end - reader->at) < len) return false;
    if (memcmp(reader->at, word, len) != 0) return false;
    reader->at += len;
    return true;
}

static bool encode_utf8(unsigned int code, PixyBuf *out) {
    char bytes[4];
    size_t len;
    if (code < 0x80) {
        bytes[0] = (char)code;
        len = 1;
    } else if (code < 0x800) {
        bytes[0] = (char)(0xC0 | (code >> 6));
        bytes[1] = (char)(0x80 | (code & 0x3F));
        len = 2;
    } else if (code < 0x10000) {
        bytes[0] = (char)(0xE0 | (code >> 12));
        bytes[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (code & 0x3F));
        len = 3;
    } else {
        bytes[0] = (char)(0xF0 | (code >> 18));
        bytes[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((code >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (code & 0x3F));
        len = 4;
    }
    return pixy_buf_add(out, bytes, len);
}

static bool hex4(Reader *reader, unsigned int *out) {
    if (reader->end - reader->at < 4) return false;
    unsigned int value = 0;
    for (int i = 0; i < 4; i++) {
        char ch = reader->at[i];
        value <<= 4;
        if (ch >= '0' && ch <= '9') value |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') value |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') value |= (unsigned)(ch - 'A' + 10);
        else return false;
    }
    reader->at += 4;
    *out = value;
    return true;
}

static char *parse_string(Reader *reader, size_t *len) {
    if (reader->at >= reader->end || *reader->at != '"') return NULL;
    reader->at++;
    PixyBuf buf = {0};
    while (reader->at < reader->end) {
        unsigned char ch = (unsigned char)*reader->at;
        if (ch == '"') {
            reader->at++;
            if (!buf.data && !pixy_buf_add(&buf, "", 0)) return NULL;
            *len = buf.len;
            return buf.data;
        }
        if (ch == '\\') {
            reader->at++;
            if (reader->at >= reader->end) break;
            char esc = *reader->at++;
            bool ok = true;
            switch (esc) {
            case '"':
                ok = pixy_buf_add(&buf, "\"", 1);
                break;
            case '\\':
                ok = pixy_buf_add(&buf, "\\", 1);
                break;
            case '/':
                ok = pixy_buf_add(&buf, "/", 1);
                break;
            case 'b':
                ok = pixy_buf_add(&buf, "\b", 1);
                break;
            case 'f':
                ok = pixy_buf_add(&buf, "\f", 1);
                break;
            case 'n':
                ok = pixy_buf_add(&buf, "\n", 1);
                break;
            case 'r':
                ok = pixy_buf_add(&buf, "\r", 1);
                break;
            case 't':
                ok = pixy_buf_add(&buf, "\t", 1);
                break;
            case 'u': {
                unsigned int code;
                if (!hex4(reader, &code)) {
                    ok = false;
                    break;
                }
                /* A surrogate pair is two escapes; join them or the UTF-8
                 * comes out as replacement soup. */
                if (code >= 0xD800 && code <= 0xDBFF && reader->end - reader->at >= 6 &&
                    reader->at[0] == '\\' && reader->at[1] == 'u') {
                    reader->at += 2;
                    unsigned int low;
                    if (hex4(reader, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                }
                ok = encode_utf8(code, &buf);
                break;
            }
            default:
                ok = false;
            }
            if (!ok) break;
            continue;
        }
        if (ch < 0x20) break;
        if (!pixy_buf_add(&buf, reader->at, 1)) break;
        reader->at++;
    }
    pixy_buf_free(&buf);
    return NULL;
}

static PixyJson *parse_value(Reader *reader) {
    if (reader->depth > MAX_DEPTH) return NULL;
    skip_space(reader);
    if (reader->at >= reader->end) return NULL;
    char ch = *reader->at;

    if (ch == 'n') return literal(reader, "null") ? node(PIXY_JSON_NULL) : NULL;
    if (ch == 't' || ch == 'f') {
        bool value = ch == 't';
        if (!literal(reader, value ? "true" : "false")) return NULL;
        PixyJson *out = node(PIXY_JSON_BOOL);
        if (out) out->boolean = value;
        return out;
    }
    if (ch == '"') {
        size_t len = 0;
        char *text = parse_string(reader, &len);
        if (!text) return NULL;
        PixyJson *out = node(PIXY_JSON_STRING);
        if (!out) {
            free(text);
            return NULL;
        }
        out->text = text;
        out->text_len = len;
        return out;
    }
    if (ch == '[' || ch == '{') {
        bool object = ch == '{';
        reader->at++;
        reader->depth++;
        PixyJson *out = node(object ? PIXY_JSON_OBJECT : PIXY_JSON_ARRAY);
        if (!out) return NULL;
        skip_space(reader);
        if (reader->at < reader->end && *reader->at == (object ? '}' : ']')) {
            reader->at++;
            reader->depth--;
            return out;
        }
        while (reader->at < reader->end) {
            char *key = NULL;
            size_t key_len = 0;
            if (object) {
                skip_space(reader);
                key = parse_string(reader, &key_len);
                if (!key) break;
                skip_space(reader);
                if (reader->at >= reader->end || *reader->at != ':') {
                    free(key);
                    break;
                }
                reader->at++;
            }
            PixyJson *child = parse_value(reader);
            if (!child || !push(out, key, key_len, child)) {
                free(key);
                pixy_json_free(child);
                break;
            }
            skip_space(reader);
            if (reader->at < reader->end && *reader->at == ',') {
                reader->at++;
                continue;
            }
            if (reader->at < reader->end && *reader->at == (object ? '}' : ']')) {
                reader->at++;
                reader->depth--;
                return out;
            }
            break;
        }
        pixy_json_free(out);
        return NULL;
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        char *stop = NULL;
        double value = strtod(reader->at, &stop);
        if (stop == reader->at || stop > reader->end) return NULL;
        reader->at = stop;
        PixyJson *out = node(PIXY_JSON_NUMBER);
        if (out) out->number = value;
        return out;
    }
    return NULL;
}

PixyJson *pixy_json_parse(const char *text, size_t len) {
    Reader reader = {text, text + len, 0};
    PixyJson *value = parse_value(&reader);
    if (!value) return NULL;
    skip_space(&reader);
    if (reader.at != reader.end) {
        pixy_json_free(value);
        return NULL;
    }
    return value;
}

PixyJsonKind pixy_json_kind(const PixyJson *value) {
    return value ? value->kind : PIXY_JSON_NULL;
}
bool pixy_json_bool(const PixyJson *value) {
    return value && value->boolean;
}
double pixy_json_number(const PixyJson *value) {
    return value ? value->number : 0;
}
const char *pixy_json_string(const PixyJson *value, size_t *len) {
    if (!value || value->kind != PIXY_JSON_STRING) return NULL;
    if (len) *len = value->text_len;
    return value->text;
}
size_t pixy_json_count(const PixyJson *value) {
    return value ? value->count : 0;
}
const PixyJson *pixy_json_at(const PixyJson *value, size_t index) {
    if (!value || index >= value->count) return NULL;
    return value->items[index];
}
const char *pixy_json_key(const PixyJson *value, size_t index, size_t *len) {
    if (!value || value->kind != PIXY_JSON_OBJECT || index >= value->count) return NULL;
    if (len) *len = value->key_lens[index];
    return value->keys[index];
}
const PixyJson *pixy_json_get(const PixyJson *value, const char *key) {
    if (!value || value->kind != PIXY_JSON_OBJECT) return NULL;
    size_t want = strlen(key);
    for (size_t i = 0; i < value->count; i++) {
        if (value->key_lens[i] == want && memcmp(value->keys[i], key, want) == 0) {
            return value->items[i];
        }
    }
    return NULL;
}

bool pixy_json_write(const PixyJson *value, PixyBuf *out) {
    if (!value) return pixy_buf_str(out, "null");
    switch (value->kind) {
    case PIXY_JSON_NULL:
        return pixy_buf_str(out, "null");
    case PIXY_JSON_BOOL:
        return pixy_buf_str(out, value->boolean ? "true" : "false");
    case PIXY_JSON_NUMBER: {
        if (value->number == (double)(long long)value->number) {
            return pixy_buf_fmt(out, "%lld", (long long)value->number);
        }
        return pixy_buf_fmt(out, "%.17g", value->number);
    }
    case PIXY_JSON_STRING:
        return pixy_buf_json_string(out, value->text, value->text_len);
    case PIXY_JSON_ARRAY:
    case PIXY_JSON_OBJECT: {
        bool object = value->kind == PIXY_JSON_OBJECT;
        if (!pixy_buf_str(out, object ? "{" : "[")) return false;
        for (size_t i = 0; i < value->count; i++) {
            if (i && !pixy_buf_str(out, ",")) return false;
            if (object) {
                if (!pixy_buf_json_string(out, value->keys[i], value->key_lens[i])) return false;
                if (!pixy_buf_str(out, ":")) return false;
            }
            if (!pixy_json_write(value->items[i], out)) return false;
        }
        return pixy_buf_str(out, object ? "}" : "]");
    }
    }
    return false;
}
