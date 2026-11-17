/* sha_memo.c: a file's sha256, memoised (the core DLL's slow_fact for CORE_SLOW_MODEL_SHA and
 * CORE_SLOW_BRIDGE_SHA, core.h §3.5: "memoised by path, size, mtime"; review NW6). C11 over
 * common/plat.h, so the Mac tests run all of it.
 *
 *  - The key: the path, and the open handle's stamp (size, last-write time, change time and file
 *    id; Windows, through the stat hook: plat_file_stat's size and 128-bit file id, FileBasicInfo's
 *    100 ns LastWriteTime and ChangeTime), read before the hash and again after it on the same
 *    handle. The result is returned and stored only when both stamps match: a file written
 *    while it was hashed (or whose stamp can't be read after it) fails, and its entry is
 *    dropped, so it is never memoised under either stamp.
 *  - A hit is a stamp read on a handle: no byte is hashed. A file replaced, or with a new size,
 *    last-write time, change time or file id, misses. The stamp is a cache key, not proof of
 *    content: a same-size rewrite that keeps or restores the stamp hits (core.h's contract, as on
 *    the Mac). The change time raises the bar (SetFileTime can't set it; NtSetInformationFile can)
 *    but proves nothing. The stamp may also lag a writer that still holds its handle (Windows
 *    updates LastWriteTime for certain only when it closes): best-effort, as above.
 *  - For slow_fact's MODEL_SHA and BRIDGE_SHA only. Anything that gates elevation calls sha_file
 *    (uncached: every byte hashed on every call, no stamp, no memo).
 *  - Thread-safe: one mutex (an SRWLOCK on Windows) guards the entries, never held while a file
 *    is read; two workers hashing the same file at once both hash it, and the later store wins. */
#ifndef RBSP_WIN_SHA_MEMO_H
#define RBSP_WIN_SHA_MEMO_H

#include "plat.h"

#include <stdint.h>

enum { SHA_MEMO_MAX = 8 };

typedef struct {
    int64_t size, mtime;   /* mtime: the finest the stat hook has (plat_stat's seconds by default) */
    int64_t ctime;         /* the change time, when the stat hook has one (Windows); else 0 */
    uint64_t id[3];        /* the volume and the file's number on it */
} sha_memo_stamp;

typedef struct {
    char *path;            /* NULL: free */
    sha_memo_stamp st;
    char sha[65];
} sha_memo_entry;

typedef struct {
    plat_mutex mu;
    sha_memo_entry e[SHA_MEMO_MAX];
    int next;              /* the slot a new path takes when all are used (oldest first) */
    /* the stamp of an open file: 0, or an errno. NULL: plat_file_stat's (seconds) */
    int (*stat)(void *ctx, plat_file f, sha_memo_stamp *out);
    void *ctx;
    /* tests: called between the hash's reads (may be NULL); how many files were hashed */
    void (*between_reads)(void *ctx);
    long hashed;
} sha_memo;

void sha_memo_init(sha_memo *m);
void sha_memo_free(sha_memo *m);
/* path's sha256, hex, into out[65] (from the memo when its stamp still matches): 0, or nonzero
 * (not a regular file, unreadable, changed while hashed, no hash) */
int sha_memo_file(sha_memo *m, const char *path, char out[65]);
/* path's sha256, hex, into out[65], hashed now, never memoised: 0, or nonzero (unreadable, a
 * folder, no hash) */
int sha_file(const char *path, char out[65]);

#endif
