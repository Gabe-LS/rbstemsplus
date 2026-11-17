/* manifest.c: a kept set's manifest, <kept dir>/<name>.manifest, next to its <name>.kept: each
 * file the set was prepared from (its size and modification time then) and its entries. The
 * helper alone reads it (the bridge reads only .kept files): the estimate uses it to tell which
 * files are still prepared (the file unchanged, every entry still in the cache), and so does the
 * quick check when rekordbox opens. Text, renamed into place whole:
 *
 *   rbstemsplus-manifest 1
 *   helper <helper version>
 *   model <64 hex: the model folder>
 *   playlist <rekordbox's playlist ID, or ->
 *   rate <model runs> <their seconds> <threads>
 *   file <size> <mtime in ns> <complete 0|1> <entries> <path>
 *   <64 hex: an entry's key>              (one line per entry)
 *
 * A path with a newline isn't recorded. Paths are compared with common/path_rules.h's paths_same
 * (helper.md §4); the folder is opened, and the file written and renamed, through common/'s file
 * layer. */
#ifndef RBSP_MANIFEST_H
#define RBSP_MANIFEST_H
#include <stddef.h>
#include "plat_helper.h"

typedef struct {
    char *path;
    long long size, mtime_ns;
    int complete;                       /* every chunk was done (not stopped in the middle) */
    size_t nkeys, cap;
    char (*keys)[65];
} mfile;

typedef struct {
    char helper[16], model[65], playlist[65];
    long long runs;                     /* the model runs of the run that wrote it, and their time */
    double run_s;
    int threads;
    mfile *f;
    size_t n, cap;
} manifest;

/* reads DIR/NAME.manifest into *m (zeroed first); 0, or -1 when missing or not the format (*m is
 * then empty) */
int manifest_read(const char *dir, const char *name, manifest *m);
void manifest_free(manifest *m);

/* a new file record (its size and time from FI), or NULL when PATH can't be recorded */
mfile *manifest_file(manifest *m, const char *path, const plat_finfo *fi);
void mfile_key(mfile *f, const char *key);       /* once per key */

/* removes the records of files that aren't among PATHS (N of them) */
void manifest_keep_only(manifest *m, char **paths, size_t n);
void manifest_drop_last(manifest *m);            /* takes the newest record out again */

/* the record for PATH, whatever its size and time (the last one), or NULL */
const mfile *manifest_by_path(const manifest *m, const char *path);

/* the record for PATH whose size and time are FI's, or NULL */
const mfile *manifest_find(const manifest *m, const char *path, const plat_finfo *fi);

/* writes M as DIR/NAME.manifest, with EARLIER's records for paths M hasn't (EARLIER may be NULL).
 * 0, or -1 and *why */
int manifest_write(const char *dir, const char *name, const manifest *m, const manifest *earlier, char *why, size_t n);

#endif
