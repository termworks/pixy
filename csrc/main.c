#include <signal.h>
#include <stdio.h>

#include "pixy.h"

const char *pixy_error_prefix(void);

int main(int argc, char **argv) {
    /* `pixy names | head` closes the pipe early; the default disposition makes
     * that a quiet exit rather than a write error on every later line. */
    signal(SIGPIPE, SIG_DFL);
    int code = pixy_main(argc, argv);
    if (pixy_failed()) {
        fprintf(stderr, "%s: %s\n", pixy_error_prefix(), pixy_error_message());
        return code ? code : pixy_error_code();
    }
    return code;
}
