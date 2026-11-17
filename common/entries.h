/* The cache's layout and its size rule (bridge.md §0a), moved from the bridge's layout(),
 * is_pid_tmp, is_memo_tmp, is_hex and entry_size: which names a walk of the cache counts, and the
 * disk space an entry takes. Used by the bridge, and (on their switch) the helper and the core.
 *
 * Cache folder: <model sha256>/<key[0:2]>/<key>.flac and <key>.desc, each 64 lowercase hex
 * digits; their writers' temporary files <name>.<pid>.tmp; this Mac's folder also holds the
 * model-hash memos' temporary files model-<inode>.sha256.<pid>.tmp. exFAT and FAT32 add an
 * AppleDouble companion "._<name>" beside each of those (a drive shared with a Mac has them on
 * Windows too). Everything else (another app's files, a link, a folder someone made, the marker
 * and its companion) is ENT_OTHER: never read, deleted or entered. Companions are never opened. */
#ifndef RBSP_ENTRIES_H
#define RBSP_ENTRIES_H

#include "api.h"
#include "plat.h"

#include <stddef.h>

enum { ENT_OTHER, ENT_MODEL_DIR, ENT_PREFIX_DIR, ENT_FLAC, ENT_DESC, ENT_TMP, ENT_MEMO_TMP, ENT_COMP_DIR, ENT_COMP_FLAC,
       ENT_COMP_DESC, ENT_COMP_TMP };

/* n lowercase hex digits (as the cache's names are written) */
COMMON_API int entries_is_hex(const char *s, size_t n);
/* ".<pid>.tmp", the whole rest of the name */
COMMON_API int entries_is_pid_tmp(const char *s);
/* model-<inode>.sha256.<pid>.tmp */
COMMON_API int entries_is_memo_tmp(const char *n);
/* What name n is, found at `level` below the cache folder (1: in it; 3: in the prefix folder
 * `parent`), a folder (dir) or a regular file (file), never following a link. memos: the folder
 * is this computer's own (the memos' temporary files are ours there; in a chosen location they
 * are ENT_OTHER). */
COMMON_API int entries_layout(const char *n, int level, int dir, int file, const char *parent, int memos);

/* An entry's files, which its disk space and its removal cover: <key>.flac, <key>.desc and their
 * companions, in that order (i < ENTRIES_FILES); out holds at least 80 bytes */
#define ENTRIES_FILES 4
COMMON_API void entries_file(char *out, size_t n, const char *key, int i);
/* The disk space entry key takes in its prefix folder x (the size limit counts it): the allocated
 * size of each of its files that is a regular file; a missing one counts 0. */
COMMON_API long long entries_size_at(plat_dir x, const char *key);

#endif
