/* lifecycle.h's decisions, pure C. */
#include "lifecycle.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ one copy */

int life_one_copy(const life_one_ops *o, uint32_t wait_ms) {
    int c = o->create(o->ctx);
    if (c == LIFE_MUTEX_OWNED) return LIFE_ONE_HELD;
    if (c != LIFE_MUTEX_EXISTS) return LIFE_ONE_FAILED;
    uint64_t start = o->now_ms(o->ctx);
    for (;;) {
        uint32_t pid = o->find(o->ctx);
        if (pid && o->hand_off(o->ctx, pid) == 0) return LIFE_ONE_HANDED;
        uint64_t now = o->now_ms(o->ctx), spent = now > start ? now - start : 0;
        if (spent >= wait_ms) return LIFE_ONE_BUSY;
        uint64_t left = wait_ms - spent;
        int w = o->wait(o->ctx, left < 100 ? (uint32_t)left : 100);   /* the window may appear meanwhile */
        if (w == LIFE_MUTEX_OWNED) return LIFE_ONE_HELD;
        if (w != LIFE_MUTEX_TIMEOUT) return LIFE_ONE_FAILED;
    }
}

int life_one_acked(int sent, uintptr_t answer) { return sent && answer == LIFE_ONE_ACK; }

/* ------------------------------------------------------------------ the minimum, the token */

int life_os_verdict(uint32_t major, uint32_t build, int net, int64_t release) {
    if (major < 10 || (major == 10 && build < LIFE_MIN_BUILD)) return LIFE_OS_WINDOWS_OLD;
    if (net && release < LIFE_MIN_NET_RELEASE) return LIFE_OS_NET_OLD;
    return LIFE_OS_OK;
}

int life_token_verdict(int elevation_type, int elevated, int service) {
    if (service) return LIFE_TOKEN_SERVICE;
    switch (elevation_type) {
    case 2: return LIFE_TOKEN_ELEVATED;                     /* TokenElevationTypeFull: "Run as administrator" */
    case 3: return LIFE_TOKEN_OK;                           /* TokenElevationTypeLimited: an admin's normal start */
    case 1:                                                 /* TokenElevationTypeDefault: a standard user, or UAC off */
        return elevated == 1 ? LIFE_TOKEN_UAC_OFF : elevated == 0 ? LIFE_TOKEN_OK : LIFE_TOKEN_UNKNOWN;
    default: return LIFE_TOKEN_UNKNOWN;
    }
}

/* ------------------------------------------------------------------ command lines */

/* "\"<program>\" <arg>" */
static int quoted(const char *program, const char *arg, char *out, size_t n) {
    if (!program[0] || strchr(program, '"')) return -1;
    int r = snprintf(out, n, "\"%s\" %s", program, arg);
    return r < 0 || (size_t)r >= n ? -1 : 0;
}

int life_run_value(const char *stub, char *out, size_t n) { return quoted(stub, "--watcher", out, n); }
int life_uninstall_string(const char *stub, char *out, size_t n) { return quoted(stub, "--uninstall", out, n); }

static char fold(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c == '/' ? '\\' : c;
}

int life_run_names(const char *value, const char *stub) {
    const char *p = value, *prog, *end;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        prog = ++p;
        end = strchr(p, '"');
        if (!end) return 0;
        p = end + 1;
    } else {
        prog = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        end = p;
    }
    size_t l = (size_t)(end - prog);
    if (l == 0 || l != strlen(stub)) return 0;
    for (size_t i = 0; i < l; i++)
        if (fold(prog[i]) != fold(stub[i])) return 0;
    if (*p != ' ' && *p != '\t') return 0;                  /* the program and its argument are apart */
    while (*p == ' ' || *p == '\t') p++;
    static const char arg[] = "--watcher";
    if (strncmp(p, arg, sizeof arg - 1)) return 0;
    for (p += sizeof arg - 1; *p; p++)
        if (*p != ' ' && *p != '\t') return 0;
    return 1;
}

int life_approved_off(const unsigned char *b, size_t n) { return b && n >= 1 && (b[0] & 1) != 0; }

uint32_t life_size_kb(uint64_t bytes) {
    uint64_t kb = bytes / 1024 + (bytes % 1024 != 0);
    return kb > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)kb;
}
