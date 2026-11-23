/* stdin_stop.c: the Windows helper's Stop on stdin (stdin_stop.h) */
#include "stdin_stop.h"

#include <string.h>

int stop_scan_feed(stop_scan *s, const void *buf, size_t n) {
    const unsigned char *b = buf;
    for (size_t i = 0; i < n; i++) {
        if (b[i] == '\n') {
            size_t l = s->len;
            if (l && s->line[l - 1] == '\r') l--;
            int hit = !s->over && l == 4 && !memcmp(s->line, "stop", 4);
            s->len = 0;
            s->over = 0;
            if (hit) return 1;
        } else if (s->len < sizeof s->line) s->line[s->len++] = b[i];
        else s->over = 1;
    }
    return 0;
}

#if defined(_WIN32)
#include <windows.h>

static void (*ss_on_stop)(void);

static DWORD WINAPI ss_watch(void *arg) {
    HANDLE h = arg;
    stop_scan s = {0};
    unsigned char b[256];
    for (;;) {
        DWORD got = 0;
        /* the raw handle, never the C library's stdin: nothing else reads it. A pipe whose writer
         * closed gives ERROR_BROKEN_PIPE, NUL and a console's Ctrl+Z 0 bytes: each is the EOF */
        if (!ReadFile(h, b, sizeof b, &got, NULL) || got == 0) break;
        if (stop_scan_feed(&s, b, got)) break;
    }
    ss_on_stop();
    return 0;
}

int stop_watch_start(void (*on_stop)(void)) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (!on_stop || h == NULL || h == INVALID_HANDLE_VALUE) return -1;
    ss_on_stop = on_stop;
    HANDLE t = CreateThread(NULL, 0, ss_watch, h, 0, NULL);
    if (!t) return -1;
    CloseHandle(t);                     /* it runs till the stop or the process's end */
    return 0;
}
#endif
