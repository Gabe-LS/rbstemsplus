/* dismissed.c: the support folder's dismissed list (State.swift's "Don't ask again for this
 * version": one key a line), its one writer for both processes that answer for it: the watcher's
 * reinstall question (watch.c's remember, through watch_win.c) and the app's R1 "Don't Ask Again
 * for This Version" (N10, the core DLL's CORE_STEP_WIN_R1_DISMISS). Each write reads the file as it
 * is now and adds its lines to it (keyset_union_text: sorted, no repeats), so a line the other
 * process wrote meanwhile is kept; on Windows both take the answer lock for the read and the
 * write (dismissed_add_locked). Over common/plat.h and fs_write.c, so the Mac tests run it on a
 * scratch folder. */
#ifndef RBSP_WIN_DISMISSED_H
#define RBSP_WIN_DISMISSED_H

#include <stddef.h>

/* The line R1's offer in the app writes to dismissed for "Don't Ask Again for This Version"
 * ("r1:7.2.19", the verified current version folder's folder_version; the contract with W-core,
 * app.md §5.7): the watcher doesn't open the app for R1 in that version. Its length. */
size_t watch_r1_dismissed_key(const char *version, char *out, size_t n);

/* 1 when support's dismissed has the line key, 0 when not, or when it can't be read or isn't
 * UTF-8 (the watcher's reader counts such a file as none) */
int dismissed_has(const char *support, const char *key);

/* support's dismissed with the n bytes of lines (one key a line) added, its own lines kept,
 * replaced whole (win_write_atomic). A file that isn't UTF-8 counts as none, as the readers count
 * it; one that can't be read (other than missing) is left as it is. A result past 64 KiB (the
 * readers' bound) is refused (E2BIG), the file as it was. 0, or an errno. */
int dismissed_add(const char *support, const char *lines, size_t n);

#if defined(_WIN32)
/* The answer lock: a named mutex in the session's namespace, held by the app and the watcher for
 * dismissed's read and write. Waited for at most DISMISSED_LOCK_MS; a lock that can't be had (not
 * created, or held past that) doesn't stop the answer: it is written anyway, as before the lock,
 * since losing the user's answer is worse than the rare race the lock closes. */
#  define DISMISSED_LOCK_NAME L"Local\\io.github.rbstemsplus.answer"
enum { DISMISSED_LOCK_MS = 5000 };
/* dismissed_add under the answer lock: 0, or an errno */
int dismissed_add_locked(const char *support, const char *lines, size_t n);
#endif

#endif
