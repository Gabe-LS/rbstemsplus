/* The dismissed list's one writer (dismissed.h). C11 over common/plat.h. */
#include "dismissed.h"

#include "fs_write.h"
#include "plat.h"
#ifndef RBSP_CORE_PLAT_SHIM_H
#  define RBSP_CORE_PLAT_SHIM_H   /* common/plat.h has the shim's names */
#endif
#include "internal.h"   /* the core's keyset_parse, keyset_union_text, utf8_valid */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  define SEP "\\"
#else
#  define SEP "/"      /* the Mac tests: a scratch folder */
#endif

enum { DISMISSED_MAX = 64 * 1024 };   /* the watcher's small-file bound (watch_facts.c) */

size_t watch_r1_dismissed_key(const char *version, char *out, size_t n) {
    int k = snprintf(out, n, "r1:%s", version ? version : "");
    return k < 0 ? 0 : (size_t)k;
}

/* the file's text: 0 with *buf (malloc'd, NUL-terminated) and *n; ENOENT when there is none */
static int read_dismissed(const char *support, char **buf, size_t *n) {
    char p[1100];
    *buf = NULL;
    *n = 0;
    int k = snprintf(p, sizeof p, "%s" SEP "dismissed", support);
    if (k < 0 || (size_t)k >= sizeof p) return ENAMETOOLONG;
    int e = plat_read_file(p, DISMISSED_MAX, buf, n);
    if (!e) (*buf)[*n] = 0;   /* plat_read_file's buffer holds max + 1 bytes */
    return e;
}

int dismissed_has(const char *support, const char *key) {
    char *t;
    size_t n;
    if (!support || !key || read_dismissed(support, &t, &n)) return 0;
    arena a = {0};
    keyset k;
    int r = keyset_parse(&a, t, n, &k) == 0 && keyset_has(&k, key, strlen(key));
    arena_free(&a);
    free(t);
    return r;
}

int dismissed_add(const char *support, const char *lines, size_t n) {
    if (!support || !lines) return EINVAL;
    char *have;
    size_t hn;
    int e = read_dismissed(support, &have, &hn);
    if (e == ENOENT) hn = 0;
    else if (e) return e;   /* not readable now: left as it is, never overwritten blind */
    if (have && !utf8_valid(have, hn)) hn = 0;   /* none, as the readers count it */
    /* one list of both: the file's lines, then the new ones (keyset_parse sorts and drops repeats) */
    size_t all_n = hn + 1 + n;
    char *all = malloc(all_n + 1);
    if (!all) {
        free(have);
        return ENOMEM;
    }
    if (hn) memcpy(all, have, hn);
    all[hn] = '\n';
    memcpy(all + hn + 1, lines, n);
    all[all_n] = 0;
    free(have);
    arena a = {0};
    keyset k;
    e = 0;
    if (!utf8_valid(lines, n)) e = EINVAL;
    else if (keyset_parse(&a, all, all_n, &k)) e = ENOMEM;
    char *out = e ? NULL : malloc(all_n + 2);
    if (!e && !out) e = ENOMEM;
    if (!e) {
        tbuf t;
        tb_init(&t, CORE_OS_WIN, out, all_n + 2);
        keyset_union_text(&t, &k, NULL, 0);
        e = t.err || tb_len(&t) > DISMISSED_MAX ? E2BIG : win_write_atomic(support, "dismissed", out, tb_len(&t));   /* never past the readers' bound: the old file kept */
    }
    free(out);
    arena_free(&a);
    free(all);
    return e;
}

#if defined(_WIN32)
int dismissed_add_locked(const char *support, const char *lines, size_t n) {
    HANDLE m = CreateMutexW(NULL, FALSE, DISMISSED_LOCK_NAME);
    DWORD w = m ? WaitForSingleObject(m, DISMISSED_LOCK_MS) : WAIT_FAILED;
    int held = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;   /* abandoned: a writer died holding it, the file is whole (renamed over) */
    int e = dismissed_add(support, lines, n);
    if (held) ReleaseMutex(m);
    if (m) CloseHandle(m);
    return e;
}
#endif
