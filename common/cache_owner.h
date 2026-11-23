#ifndef RBSP_CACHE_OWNER_H
#define RBSP_CACHE_OWNER_H

#include "plat.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CACHE_OWNER_MARKER "this-pc-owner"
#define CACHE_OWNER_IDENTITY_MAX 100

/* The canonical volume GUID (empty in native host tests), followed by the directory ID. */
static inline void cache_owner_identity(char out[CACHE_OWNER_IDENTITY_MAX], size_t root, plat_id id) {
    snprintf(out + root, CACHE_OWNER_IDENTITY_MAX - root, "%016" PRIx64 "%016" PRIx64 "%016" PRIx64,
             id.vol, id.file[0], id.file[1]);
}

static inline int cache_owner_check(plat_dir d, const char *identity) {
    plat_file f;
    int64_t size;
    char marker[CACHE_OWNER_IDENTITY_MAX];
    int err = plat_file_open_at(d, CACHE_OWNER_MARKER, sizeof marker, &size, &f);
    if (err) return err;
    size_t len = strlen(identity);
    err = size == (int64_t)len ? plat_read_all(f, marker, len) : EBUSY;
    if (!err && memcmp(marker, identity, len)) err = EBUSY;
    plat_file_close(f);
    return err;
}

#if defined(_WIN32) || defined(__APPLE__)
#if defined(_WIN32)
#include "plat_win.h"
#include "win_rules.h"
#include <winternl.h>
#else
#include <unistd.h>
#endif

#define CACHE_OWNER_TEMP "." CACHE_OWNER_MARKER ".tmp"

typedef int (*cache_owner_identify_fn)(plat_dir d, const char *path, int created,
                                      char identity[CACHE_OWNER_IDENTITY_MAX]);

/* Host tests can compare the name with the open folder before rmdir. Windows disposes the
 * held directory itself, whose open denies share-delete. */
static inline int cache_owner_dispose(plat_dir parent, const char *name, plat_dir held) {
#if defined(_WIN32)
    (void)parent;
    (void)name;
    return plat_dir_dispose(held);
#else
    plat_stat opened, named;
    int err = plat_dir_stat(held, &opened);
    if (!err) err = plat_stat_at(parent, name, &named);
    if (err) return err;
    if (named.kind != PLAT_DIR || !plat_id_same(opened.id, named.id)) return ESTALE;
    return unlinkat(parent.fd, name, AT_REMOVEDIR) ? errno : 0;
#endif
}

static inline const char *cache_owner_reason(int err) {
    if (err == ENOTEMPTY) return "marker";
    if (err == ENOENT || err == ENOTDIR || err == ELOOP || err == PLAT_EFTYPE) return "folder";
    if (err == EACCES || err == EPERM) return "no-permission";
    if (err == EROFS) return "read-only";
    return "io-error";
}

/* An owner artifact has one of two fixed names. On Windows open it relative to the held
 * cache, with DELETE access and no share-write or share-delete. */
static inline int cache_owner_open_artifact(plat_dir d, const char *name, int create, plat_file *f) {
#if defined(_WIN32)
    const WCHAR *w = !strcmp(name, CACHE_OWNER_MARKER) ? L"" CACHE_OWNER_MARKER : L"" CACHE_OWNER_TEMP;
    USHORT n = (USHORT)(wcslen(w) * sizeof *w);
    UNICODE_STRING nt_name = {n, n, (PWSTR)w};
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &nt_name, OBJ_CASE_INSENSITIVE, d.h, NULL);
    IO_STATUS_BLOCK io;
    ACCESS_MASK access = DELETE | FILE_GENERIC_READ | (create ? FILE_GENERIC_WRITE : 0);
    NTSTATUS status = NtCreateFile(&f->h, access, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL,
                                   FILE_SHARE_READ, create ? FILE_CREATE : FILE_OPEN,
                                   FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    return wr_errno_nt((uint32_t)status, WR_OP_OPEN);
#else
    return create ? plat_file_create_at(d, name, 1, f) : plat_file_open_at(d, name, CACHE_OWNER_IDENTITY_MAX, NULL, f);
#endif
}

static inline int cache_owner_remove_artifact(plat_dir d, const char *name, plat_file f) {
#if defined(_WIN32)
    (void)d;
    (void)name;
    FILE_DISPOSITION_INFO info = {TRUE};
    return SetFileInformationByHandle(f.h, FileDispositionInfo, &info, sizeof info) ? 0
        : wr_errno_win32(GetLastError(), WR_OP_RENAME_DELETE);
#else
    plat_stat opened, named;
    int err = plat_file_stat(f, &opened);
    if (!err) err = plat_stat_at(d, name, &named);
    if (err) return err;
    return plat_id_same(opened.id, named.id) ? plat_unlink_at(d, name) : ESTALE;
#endif
}

static inline int cache_owner_publish(plat_dir d, plat_file f) {
#if defined(_WIN32)
    (void)d;
    /* FileRenameInformation (10), with ReplaceIfExists false and a leaf in the same folder. */
    struct {
        ULONG flags;
        HANDLE root;
        ULONG length;
        WCHAR name[sizeof CACHE_OWNER_MARKER];
    } info = {0, NULL, (sizeof CACHE_OWNER_MARKER - 1) * sizeof(WCHAR), L"" CACHE_OWNER_MARKER};
    IO_STATUS_BLOCK io;
    NTSTATUS status = NtSetInformationFile(f.h, &io, &info, sizeof info, (FILE_INFORMATION_CLASS)10);
    return wr_errno_nt((uint32_t)status, WR_OP_RENAME_DELETE);
#else
    (void)f;
    return plat_rename_noreplace_at(d, CACHE_OWNER_TEMP, d, CACHE_OWNER_MARKER);
#endif
}

static inline int cache_owner_write(plat_dir d, const char *identity) {
    plat_file f;
    int err = cache_owner_open_artifact(d, CACHE_OWNER_TEMP, 1, &f);
    if (err) return err;
    size_t n = strlen(identity);
    void *addr;
    err = plat_map_rw(f, (int64_t)n, &addr);
    if (!err) {
        memcpy(addr, identity, n);
        err = plat_map_flush(addr, (int64_t)n);
        plat_unmap(addr, (int64_t)n);
    }
    if (!err) err = plat_fsync(f);
    if (!err) err = cache_owner_publish(d, f);
    if (err) cache_owner_remove_artifact(d, CACHE_OWNER_TEMP, f);
    plat_file_close(f);
    return err;
}

typedef struct {
    plat_stat marker, temp;
} cache_owner_files;

static inline int cache_owner_list(void *ctx, const char *name, const plat_stat *st) {
    cache_owner_files *files = ctx;
    if (st->kind == PLAT_FILE && st->size >= 0 && st->size < CACHE_OWNER_IDENTITY_MAX) {
        if (!strcmp(name, CACHE_OWNER_MARKER)) { files->marker = *st; return 0; }
        if (!strcmp(name, CACHE_OWNER_TEMP)) { files->temp = *st; return 0; }
    }
    errno = ENOTEMPTY;
    return -1;
}

static inline int cache_owner_artifact_matches(plat_file f, const plat_stat *listed, const char *identity, int temp) {
    plat_stat st;
    int err = plat_file_stat(f, &st);
    if (err) return err;
    if (st.kind != PLAT_FILE || !plat_id_same(st.id, listed->id) || st.size != listed->size || st.size > (int64_t)strlen(identity))
        return ENOTEMPTY;
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(f.h, &info)) return wr_errno_win32(GetLastError(), WR_OP_OPEN);
    if (info.nNumberOfLinks != 1) return ENOTEMPTY;
#endif
    char bytes[CACHE_OWNER_IDENTITY_MAX];
    size_t n = (size_t)st.size;
    err = plat_read_all(f, bytes, n);
    if (err) return err;
    /* The temporary file may have been extended by mapping before the complete write. */
    if (temp) while (n && !bytes[n - 1]) --n;
    return memcmp(bytes, identity, n) ? ENOTEMPTY : 0;
}

static inline int cache_owner_remove_leftovers(plat_dir d, const char *identity) {
    cache_owner_files files = {0};
    if (plat_dir_each(d, cache_owner_list, &files)) return errno ? errno : EIO;
    const char *names[] = {CACHE_OWNER_MARKER, CACHE_OWNER_TEMP};
    const plat_stat *listed[] = {&files.marker, &files.temp};
    plat_file held[2];
    size_t owned = 0;
    int err = 0;
    /* Check all candidates before changing anything. */
    for (size_t i = 0; i < 2 && !err; ++i) {
        if (!listed[i]->kind) continue;
        err = cache_owner_open_artifact(d, names[i], 0, &held[i]);
        if (!err) {
            owned |= (size_t)1 << i;
            err = cache_owner_artifact_matches(held[i], listed[i], identity, i == 1);
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        if (!(owned & ((size_t)1 << i))) continue;
        if (!err) err = cache_owner_remove_artifact(d, names[i], held[i]);
        plat_file_close(held[i]);
    }
    return err;
}

/* Only reserved marker files can be left over from a failed initialization. Refuse an
 * incomplete listing, a different spelling or a real cache tree without matching ownership. */
static inline int cache_owner_recover(plat_dir parent, const char *name, const char *path,
                                      cache_owner_identify_fn identify, int *removed) {
    plat_dir d;
#if defined(_WIN32)
    int err = plat_dir_open_at_for_removal(parent, name, &d);
#else
    int err = plat_dir_open_at(parent, name, &d);
#endif
    if (err) return err;
    char identity[CACHE_OWNER_IDENTITY_MAX];
    if (!plat_dir_name_is(d, name)) err = PLAT_EFTYPE;
    else if (!(err = identify(d, path, 0, identity))) {
        err = cache_owner_check(d, identity);
        if (!err) err = plat_dir_writable(d);
        else if (err == ENOENT || err == EBUSY) {
            err = cache_owner_remove_leftovers(d, identity);
            if (!err) {
                err = cache_owner_dispose(parent, name, d);
                *removed = !err;
            }
        }
    }
    plat_dir_close(d);
    return err;
}

static inline int cache_owner_init(plat_dir parent, const char *name, const char *path,
                                   cache_owner_identify_fn identify,
                                   int (*mkdir_open)(plat_dir, const char *, int, plat_dir *)) {
    plat_dir d;
    char identity[CACHE_OWNER_IDENTITY_MAX];
    int err = plat_dir_open_at(parent, name, &d);
    if (!err) {
        if (!plat_dir_name_is(d, name)) {
            plat_dir_close(d);
            return PLAT_EFTYPE;
        }
        err = identify(d, path, 0, identity);
        if (err) {
            plat_dir_close(d);
            return err;
        }
        err = cache_owner_check(d, identity);
        if (!err) {
            err = plat_dir_writable(d);
            plat_dir_close(d);
            return err;
        }
        plat_dir_close(d);
        if (err != ENOENT && err != EBUSY) return err;
        int removed = 0;
        err = cache_owner_recover(parent, name, path, identify, &removed);
        if (err || !removed) return err;
    } else if (err != ENOENT) return err;

    err = mkdir_open(parent, name, 0, &d);
    if (err) return err;
    err = identify(d, path, 1, identity);
    if (!err) err = cache_owner_write(d, identity);
    if (err) cache_owner_dispose(parent, name, d);   /* FILE_CREATE proved it was ours; dispose only if empty */
    plat_dir_close(d);
    return err;
}
#endif

#endif
