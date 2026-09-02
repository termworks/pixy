/* Palette namespaces: a private 256-colour table per region of output. */
#include "palette.h"

#include "host.h"

#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* Sender-side advice from the spec: other terminals may cap an OSC payload. */
#define ENTRIES_PER_SET 32

unsigned pixy_palette_osc(void) {
    const char *from_env = getenv("PIXY_PALETTE_OSC");
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

bool pixy_palette_set(PixyBuf *out, const char *slot, const PixyPaletteEntry *entries, size_t count,
                      PixyTarget target) {
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

/* Reads `have;<osc>;<max>` out of whatever the terminal sent back. Other replies
 * and stray keystrokes land in the same buffer, so the answer is looked for
 * rather than assumed to be at the front. */
static bool find_have(const char *data, size_t len, unsigned *osc_out, long *max_out) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] != '\033' || data[i + 1] != ']') continue;
        const char *at = data + i + 2;
        const char *end = data + len;
        unsigned long introducer = 0;
        if (!isdigit((unsigned char)*at)) continue;
        while (at < end && isdigit((unsigned char)*at))
            introducer = introducer * 10 + (unsigned)(*at++ - '0');
        if (at >= end || *at != ';') continue;
        at++;
        if ((size_t)(end - at) < 5 || strncmp(at, "have;", 5) != 0) continue;
        at += 5;
        unsigned long osc = 0, max = 0;
        if (!isdigit((unsigned char)*at)) continue;
        while (at < end && isdigit((unsigned char)*at)) osc = osc * 10 + (unsigned)(*at++ - '0');
        if (at >= end || *at != ';') continue;
        at++;
        if (!isdigit((unsigned char)*at)) continue;
        while (at < end && isdigit((unsigned char)*at)) max = max * 10 + (unsigned)(*at++ - '0');
        /* The reply has to be terminated, or a truncated read would be taken
         * for an answer. */
        bool terminated =
            at < end && (*at == '\a' || (*at == '\033' && at + 1 < end && at[1] == '\\'));
        if (!terminated) continue;
        (void)introducer;
        *osc_out = (unsigned)osc;
        *max_out = (long)max;
        return true;
    }
    return false;
}

/* The one round trip in the protocol, and the only place pixy reads from a
 * terminal. Silence is the documented answer for "unsupported", so this always
 * times out rather than waiting, and it talks to /dev/tty rather than stdout,
 * which for a prompt is usually a pipe. */
bool pixy_palette_query(long timeout_ms, unsigned *osc_out, long *max_out) {
    int tty = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (tty < 0) return false;
    if (!isatty(tty)) {
        close(tty);
        return false;
    }
    struct termios saved;
    if (tcgetattr(tty, &saved) != 0) {
        close(tty);
        return false;
    }
    /* Held off for the length of the round trip: being killed between the mode
     * change and the restore would leave the terminal raw. */
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGQUIT);
    sigaddset(&blocked, SIGTERM);
    sigprocmask(SIG_BLOCK, &blocked, &previous);

    struct termios raw = saved;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    bool answered = false;
    if (tcsetattr(tty, TCSANOW, &raw) == 0) {
        PixyBuf query = {0};
        if (pixy_palette_ask(&query, PIXY_TARGET_ANSI) &&
            write(tty, query.data, query.len) == (ssize_t)query.len) {
            char buffer[512];
            size_t len = 0;
            long long deadline = pixy_now_ms() + (timeout_ms > 0 ? timeout_ms : 100);
            while (len + 1 < sizeof(buffer)) {
                long long left = deadline - pixy_now_ms();
                if (left <= 0) break;
                struct pollfd waiting = {.fd = tty, .events = POLLIN};
                int ready = poll(&waiting, 1, (int)left);
                if (ready <= 0) break;
                ssize_t got = read(tty, buffer + len, sizeof(buffer) - 1 - len);
                if (got <= 0) break;
                len += (size_t)got;
                if (find_have(buffer, len, osc_out, max_out)) {
                    answered = true;
                    break;
                }
            }
        }
        pixy_buf_free(&query);
        tcsetattr(tty, TCSANOW, &saved);
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
    close(tty);
    return answered;
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
