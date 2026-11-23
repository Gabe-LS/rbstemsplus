/* fs_write.c: writeAtomically, win/c's one copy (the store, update.c's records, the watcher's and
 * the core DLL's support files; four identical copies before). Over common/plat.h only, so the
 * Mac's tests run it with plat_mac.c; kept in win/c so the Mac's objects stay the same. */
#include "fs_write.h"

#include <stdio.h>
#include <string.h>

static int write_at(plat_dir d, const char *name, const void *p, size_t n, int replace,
                    int (*commit)(void *, plat_dir, const char *, const char *), void *ctx) {
    char tmp[320];   /* a 255-byte temp name and more; the filesystem's 255 limits the final name to ~218 */
    snprintf(tmp, sizeof tmp, ".%s.%d.%llu.tmp", name, plat_pid(), (unsigned long long)plat_now_mono_ns());
    plat_file f;
    int e = plat_file_create_at(d, tmp, 1, &f);
    if (e) return e;
    void *addr = NULL;
    if (n && !(e = plat_map_rw(f, (int64_t)n, &addr))) {
        memcpy(addr, p, n);
        e = plat_map_flush(addr, (int64_t)n);   /* the view written, then the file flushed below */
        plat_unmap(addr, (int64_t)n);
    }
    if (!e) e = plat_fsync(f);
    plat_file_close(f);
    if (!e) {
        if (commit) e = commit(ctx, d, tmp, name);
        else e = replace ? plat_rename_at(d, tmp, d, name) : plat_rename_noreplace_at(d, tmp, d, name);
    }
    if (e) plat_unlink_at(d, tmp);
    return e;
}

int win_write_at(plat_dir d, const char *name, const void *p, size_t n) { return write_at(d, name, p, n, 1, NULL, NULL); }

int win_write_new_at(plat_dir d, const char *name, const void *p, size_t n) { return write_at(d, name, p, n, 0, NULL, NULL); }

int win_write_with_replace(plat_dir d, const char *name, const void *p, size_t n,
                           int (*replace)(void *, plat_dir, const char *, const char *), void *ctx) {
    return write_at(d, name, p, n, 1, replace, ctx);
}

int win_write_atomic(const char *dir, const char *name, const void *p, size_t n) {
    plat_dir d;
    int e = plat_dir_open(dir, &d);
    if (e) return e;
    e = win_write_at(d, name, p, n);
    plat_dir_close(d);
    return e;
}
