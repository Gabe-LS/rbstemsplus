/* fs_write.h: writeAtomically, win/c's one copy (review steps-3's ruling): the payload store,
 * update.c's records, the watcher's support files and the core DLL's. */
#ifndef RBSP_WIN_FS_WRITE_H
#define RBSP_WIN_FS_WRITE_H

#include <stddef.h>

#include "plat.h"

/* name in the folder d, replaced whole: a new file under a temp name in d
 * (".<name>.<pid>.<ns>.tmp"), its n bytes flushed to the disk, then renamed over name. 0, or an
 * errno (the temp file is removed). d stays open. */
int win_write_at(plat_dir d, const char *name, const void *p, size_t n);
/* The caller controls the final replacement after the temp file is flushed and closed. */
int win_write_with_replace(plat_dir d, const char *name, const void *p, size_t n,
                           int (*replace)(void *, plat_dir, const char *, const char *), void *ctx);
/* The same atomic write, requiring a vacant destination. */
int win_write_new_at(plat_dir d, const char *name, const void *p, size_t n);
/* the same with the folder opened by its path (and closed) */
int win_write_atomic(const char *dir, const char *name, const void *p, size_t n);

#endif
