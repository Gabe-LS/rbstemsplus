/* big.h on plat.h's file mappings: the Windows helper's big.c (helper.md §1, T7). The same rule as
 * big.c's: from 32 MB a buffer's pages belong to a private temporary file, so the system writes
 * them out and drops them under memory pressure instead of swapping; smaller ones, or when no file
 * can be made, are malloc's. On Windows the file is made in LocalAppData\Temp (plat_temp_file:
 * FILE_ATTRIBUTE_TEMPORARY, FILE_FLAG_DELETE_ON_CLOSE, shared with no one) and mapped with
 * CreateFileMappingW/MapViewOfFile (plat_map_rw). The Mac helper keeps big.c; this file builds on
 * the Mac only for its tests (helper/tests/win-plat.sh). As big.c: one thread at a time. */
#include "big.h"
#include "plat_helper.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <malloc.h>
#define big_msize _msize
#else
#include <malloc/malloc.h>
#define big_msize malloc_size
#endif

#define BIG_MIN ((size_t)32 << 20)
#define BIG_MAX 64                      /* buffers alive at once: MAX_TRACKS x a few, with room */

typedef struct { void *p; size_t n; plat_file f; } ent;
static ent tab[BIG_MAX];

static ent *find(const void *p) {
    for (int i = 0; p && i < BIG_MAX; i++) if (tab[i].p == p) return &tab[i];
    return NULL;
}

static void *mapped(size_t n) {
    ent *e = NULL;
    for (int i = 0; !e && i < BIG_MAX; i++) if (!tab[i].p) e = &tab[i];
    plat_file f;
    void *p = NULL;
    if (!e || n > INT64_MAX || plat_temp_file(&f)) return NULL;
    if (plat_map_rw(f, (int64_t)n, &p) || !p) { plat_file_close(f); return NULL; }
    *e = (ent){p, n, f};
    return p;
}

void *big_alloc(size_t n) {
    void *p = n >= BIG_MIN ? mapped(n) : NULL;
    return p ? p : malloc(n ? n : 1);
}

void *big_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    size_t n = count * size;
    void *p = n >= BIG_MIN ? mapped(n) : NULL;     /* a new file reads as zeros */
    return p ? p : calloc(count ? count : 1, size ? size : 1);
}

void *big_realloc(void *p, size_t n) {
    ent *e = find(p);
    if (!e) {
        if (n < BIG_MIN) return realloc(p, n);
        void *q = mapped(n);
        if (!q) return realloc(p, n);
        if (p) { size_t old = big_msize(p); memcpy(q, p, old < n ? old : n); free(p); }
        return q;
    }
    /* the file grows to n with the old view still mapped, then a new view of it; on any failure
     * the old one stays (Windows can't shrink a file under a view: that fails, P kept) */
    void *q = NULL;
    if (n > INT64_MAX || plat_map_rw(e->f, (int64_t)n, &q) || !q) return NULL;
    plat_unmap(e->p, (int64_t)e->n);
    e->p = q;
    e->n = n;
    return q;
}

void big_free(void *p) {
    ent *e = find(p);
    if (!e) { free(p); return; }
    plat_unmap(e->p, (int64_t)e->n);
    plat_file_close(e->f);
    *e = (ent){0};
}
