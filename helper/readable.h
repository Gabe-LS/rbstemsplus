/* readable.c: opening a track without waiting forever (requirement 18). In ~/Documents, ~/Desktop
 * and ~/Downloads macOS asks the user before the first read ("RB Stems Plus would like to access
 * files in your Documents folder"), naming the app, and the read waits until it is answered,
 * however long. Here the stat and open run on their own thread: a read that doesn't return in
 * time is given up (the thread is left to finish on its own) and the folder is remembered, so its
 * other tracks don't wait again. */
#ifndef RBSP_READABLE_H
#define RBSP_READABLE_H
#include <stddef.h>
#include "plat_helper.h"

typedef enum { READ_OK, READ_ABSENT, READ_ASKING, READ_DENIED, READ_FAILED } read_state;

/* whether PATH can be read, waiting at most SECS seconds; *st its kind, size and time when READ_OK,
 * else why (a sentence) and *folder the protected folder concerned ("Documents", "Desktop",
 * "Downloads"; "" elsewhere) */
read_state readable(const char *path, double secs, plat_finfo *st, char *why, size_t n, const char **folder);

/* the protected folder PATH is in ("" when none), from its path alone: nothing is read */
const char *protected_folder(const char *path);

/* this account's home folder, from the user database, never $HOME ("" if there is none): plat_home */
const char *home_folder(void);

#if defined(_WIN32)
/* Windows (helper.md §1, T14): no folder asks before a read, so protected_folder is always "" and
 * READ_DENIED isn't given. READ_ASKING is a read that didn't answer within SECS: a cloud
 * placeholder still downloading (its first read, made inside the wait, brings it to this PC), or
 * a drive that didn't answer (remembered by its root: its other tracks aren't waited for again);
 * its skipped line says which, "folder" is "". For the core: on Windows "asking" means "check
 * again later", never a prompt. */

/* whether even a look at PATH may wait or download (--quick's may_ask): another drive than the
 * system's, a network or removable drive, or a placeholder (its attributes only: nothing is
 * downloaded) */
int readable_may_wait(const char *path);

/* The decode's reads of PATH, bounded (the UNC ruling: metadata, open and decode): from
 * readable_watch_start until readable_watch_stop, a read on the calling thread or the registered
 * stream that stays pending with no byte read by this process for SECS is cancelled (the read fails,
 * and so does the decode). readable_watch_stop(FAILED: whether the watched call failed): 1 when a
 * read was cancelled and the call failed, and PATH's drive is then remembered as one that didn't
 * answer (readable() doesn't wait for it again this run); 0 otherwise (a cancel that didn't take,
 * or a call that worked, remembers nothing). start: 0, or -1 (already watching, or no thread):
 * the caller then doesn't read at all */
int readable_watch_start(const char *path, double secs);
int readable_watch_stop(int failed);

/* Register one open stream handle from the watched thread; read it serially from any thread. */
void readable_watch_stream(plat_file f);
int readable_watch_pread(plat_file f, void *buf, size_t n, int64_t off, size_t *got);
#endif

#endif
