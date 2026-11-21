#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "deck.h"

static FILE *logf;

double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src); }
void log_open(const char *path) { logf = fopen(path, "a"); }

void logmsg(const char *fmt, ...) {
    if (!logf) return;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(logf, "[%5ld.%03ld] ", (long)t.tv_sec, t.tv_nsec / 1000000);
    va_list ap; va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap);
    fputc('\n', logf);
    fflush(logf);
}
