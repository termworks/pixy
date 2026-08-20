/* Palette namespaces: a private 256-colour table per region of output.
 *
 * The protocol is hexe's (docs/palette.md). pixy is a client of it: a prompt
 * claims a slot, prints indexed colours, and releases it, after which the
 * colours can be repainted without pixy rendering anything again.
 *
 * Every failure here is benign by design. A terminal without support discards
 * the sequence and the indexed colours render exactly as before, so pixy emits
 * optimistically and never asks first.
 */
#include "palette.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sender-side advice from the spec: other terminals may cap an OSC payload. */
#define ENTRIES_PER_SET 32

unsigned pixy_palette_osc(void) {
    /* hexe exports its OSC number into every pane when it is not the default,
     * so a client builds sequences on the right one without being told. */
    const char *from_env = getenv("PIXY_PALETTE_OSC");
    if (!from_env || !*from_env) from_env = getenv("HEXE_PALETTE_OSC");
    if (from_env && *from_env) {
        char *stop = NULL;
        long value = strtol(from_env, &stop, 10);
        if (stop && *stop == '\0' && value > 0 && value < 65536) return (unsigned)value;
    }
    return PIXY_PALETTE_OSC_DEFAULT;
}

bool pixy_palette_valid_slot(long slot, bool for_use) {
    /* 0 is the ordinary palette and 1 belongs to the terminal's own chrome;
     * neither may be selected. A slot outside the range selects nothing rather
     * than folding onto a live one, which would paint someone else's cells. */
    if (slot < 0 || slot > PIXY_PALETTE_MAX_SLOT) return false;
    if (for_use && slot < 2) return false;
    return true;
}

/* A key is an index 0-255, or one of the three names. */
bool pixy_palette_valid_key(const char *key) {
    if (!key || !*key) return false;
    if (strcmp(key, "fg") == 0 || strcmp(key, "bg") == 0 || strcmp(key, "cursor") == 0) return true;
    for (const char *at = key; *at; at++) {
        if (!isdigit((unsigned char)*at)) return false;
    }
    long value = strtol(key, NULL, 10);
    return value >= 0 && value <= 255;
}

/* `#rrggbb`, `rrggbb` or `rgb:rr/gg/bb` with 1-4 hex digits a component. A
 * colour may never contain a `;`, which is the sequence separator. */
bool pixy_palette_valid_colour(const char *colour) {
    if (!colour || !*colour) return false;
    if (strchr(colour, ';')) return false;
    if (strncmp(colour, "rgb:", 4) == 0) {
        const char *at = colour + 4;
        for (int component = 0; component < 3; component++) {
            int digits = 0;
            while (isxdigit((unsigned char)*at)) {
                at++;
                digits++;
            }
            if (digits < 1 || digits > 4) return false;
            if (component < 2) {
                if (*at != '/') return false;
                at++;
            }
        }
        return *at == '\0';
    }
    if (*colour == '#') colour++;
    size_t len = strlen(colour);
    if (len != 6) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)colour[i])) return false;
    }
    return true;
}

static bool begin(PixyBuf *out, const char *verb) {
    return pixy_buf_fmt(out, "\033]%u;%s", pixy_palette_osc(), verb);
}

/* Both terminators are the protocol's. Bash is the reason there is a choice:
 * inside `\[ … \]` it reads the backslash of ST as an escape, swallowing the
 * closing marker and printing a stray `]` into the prompt. BEL has no
 * backslash, so it survives the shell that has to count the line. */
static bool finish(PixyBuf *out, PixyTarget target) {
    return pixy_buf_str(out, target == PIXY_TARGET_BASH ? "\a" : "\033\\");
}

bool pixy_palette_use(PixyBuf *out, long slot, PixyTarget target) {
    if (!pixy_palette_valid_slot(slot, true)) return false;
    return begin(out, "use") && pixy_buf_fmt(out, ";%ld", slot) && finish(out, target);
}

bool pixy_palette_end(PixyBuf *out, PixyTarget target) {
    return begin(out, "end") && finish(out, target);
}

bool pixy_palette_reset(PixyBuf *out, const char *slot, PixyTarget target) {
    return begin(out, "reset") && pixy_buf_fmt(out, ";%s", slot) && finish(out, target);
}

bool pixy_palette_ask(PixyBuf *out, PixyTarget target) {
    return begin(out, "ask") && finish(out, target);
}

bool pixy_palette_set(PixyBuf *out, const char *slot, const PixyPaletteEntry *entries,
                      size_t count, PixyTarget target) {
    if (count == 0) return true;
    /* `set` is a patch: indexes left unnamed keep passing through to the
     * terminal's own theme, so one entry does not blacken the other 255. */
    for (size_t at = 0; at < count; at += ENTRIES_PER_SET) {
        size_t chunk = count - at < ENTRIES_PER_SET ? count - at : ENTRIES_PER_SET;
        if (!begin(out, "set") || !pixy_buf_fmt(out, ";%s", slot)) return false;
        for (size_t i = 0; i < chunk; i++) {
            if (!pixy_buf_fmt(out, ";%s=%s", entries[at + i].key, entries[at + i].colour))
                return false;
        }
        if (!finish(out, target)) return false;
    }
    return true;
}

/* A prompt counts printable width, so the sequences have to be marked
 * invisible in exactly the way each shell expects. */
bool pixy_palette_wrap(PixyBuf *out, const char *sequence, PixyTarget target) {
    switch (target) {
        case PIXY_TARGET_BASH:
            return pixy_buf_fmt(out, "\\[%s\\]", sequence);
        case PIXY_TARGET_ZSH:
            return pixy_buf_fmt(out, "%%{%s%%}", sequence);
        default:
            return pixy_buf_str(out, sequence);
    }
}
