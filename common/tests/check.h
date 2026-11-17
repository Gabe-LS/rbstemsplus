/* A few lines shared by common/'s unit tests: CHECK and the fixtures' escapes. */
#ifndef RBSP_COMMON_CHECK_H
#define RBSP_COMMON_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;   /* CHECK counts both */
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

/* tests/config-fixtures.txt and marker-fixtures.txt's content escapes into out (cap bytes): \n \r \t \\ \xHH and \*COUNT.C
 * (C repeated COUNT times); the length, or -1 when it doesn't fit or an escape is bad */
__attribute__((unused)) static long fixture_decode(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = s; *p;) {
        if (*p != '\\') { if (n >= cap) return -1; out[n++] = *p++; continue; }
        char e = p[1];
        if (e == 'n' || e == 'r' || e == 't' || e == '\\') {
            if (n >= cap) return -1;
            out[n++] = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : '\\';
            p += 2;
        } else if (e == 'x') {
            char h[3] = {p[2], p[3], 0};
            if (!h[0] || !h[1] || n >= cap) return -1;
            out[n++] = (char)strtol(h, NULL, 16);
            p += 4;
        } else if (e == '*') {
            char *dot;
            long k = strtol(p + 2, &dot, 10);
            if (*dot != '.' || !dot[1] || k < 0 || n + (size_t)k > cap) return -1;
            memset(out + n, dot[1], (size_t)k);
            n += (size_t)k;
            p = dot + 2;
        } else return -1;
    }
    return (long)n;
}

#endif
