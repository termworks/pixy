/* A terminal that speaks OSC 1330, so the one round trip in the protocol can be
 * tested against something that actually answers.
 *
 *   tty_probe <have|silent|junk|slow> <command> [argument...]
 *
 * Runs the command on a pty, watches for the `ask` query, and prints the result.
 *
 *   have    reply `have;1330;31`
 *   silent  reply nothing, which the protocol defines as unsupported
 *   junk    an unrelated OSC reply first, then the real one
 *   slow    reply only after 400ms, past the default timeout
 */
#define _GNU_SOURCE
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define REPLY "\033]1330;have;1330;31\033\\"

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *mode = argv[1];
    int master = -1;
    pid_t child = forkpty(&master, NULL, NULL, NULL);
    if (child < 0) return 2;
    if (child == 0) {
        execvp(argv[2], argv + 2);
        _exit(127);
    }

    char seen[4096];
    size_t len = 0;
    int answered = 0;
    for (int round = 0; round < 100 && len + 1 < sizeof(seen); round++) {
        struct pollfd waiting = {.fd = master, .events = POLLIN};
        if (poll(&waiting, 1, 20) > 0) {
            ssize_t got = read(master, seen + len, sizeof(seen) - 1 - len);
            if (got <= 0) break;
            len += (size_t)got;
            seen[len] = '\0';
        }
        if (answered || !strstr(seen, "]1330;ask")) continue;
        if (strcmp(mode, "silent") == 0) {
            answered = 1;
        } else if (strcmp(mode, "slow") == 0) {
            if (round < 20) continue;
            answered = (int)write(master, REPLY, strlen(REPLY));
        } else {
            if (strcmp(mode, "junk") == 0) {
                const char *noise = "\033]11;rgb:1e/1e/2e\033\\";
                if (write(master, noise, strlen(noise)) < 0) return 2;
            }
            answered = (int)write(master, REPLY, strlen(REPLY));
        }
    }

    int status = 0;
    waitpid(child, &status, 0);
    /* The command's own output came back over the same pty, after the query it
     * wrote. Everything past the last terminator is what it printed. */
    const char *answer = seen;
    for (size_t i = 0; i + 1 < len; i++) {
        if (seen[i] == '\033' && seen[i + 1] == '\\') answer = seen + i + 2;
    }
    char trimmed[256];
    size_t out = 0;
    for (const char *at = answer; *at && out + 1 < sizeof(trimmed); at++) {
        if (*at != '\r' && *at != '\n') trimmed[out++] = *at;
    }
    trimmed[out] = '\0';
    printf("exit=%d answer=%s\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1, trimmed);
    return 0;
}
