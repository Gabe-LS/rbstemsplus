/* What the prepare helper needs beyond plat.h (helper.md §1, task T4), on top of the shared layer.
 * plat.h itself is left alone (frozen; the Mac builds read it). Since its switch onto common/
 * (T5) the Mac helper is built on plat_mac.c, plat_helper.c and these calls too; it keeps its
 * native fast paths where the Mac has them (getline, big.c's mmap, AudioToolbox, vDSP).
 *
 *  - plat_helper.c, both OSes: the mingw-w64 gaps (strlcat, getline), the recursive walk, a
 *    track's file (on the Mac), and the temporary folder and file (on the Mac: today's big.c calls);
 *  - plat_helper_win.c, Windows only: known folders, the temporary folder and file, the
 *    attributes a read check needs (cloud placeholders, network and removable drives), and the
 *    process's first call (DLL search, binary standard streams).
 *
 * The rest of §1's gaps are in plat.h already, and the helper reaches them only through it:
 * mkdir (plat_mkdir_at, plat_mkdirs), lstat (plat_stat_at, plat_stat_path with follow = 0),
 * opendir (plat_dir_each), the monotonic clock (plat_now_mono_ns: QueryPerformanceCounter, so no
 * clock_gettime and no libwinpthread), and open()'s flags, which have no Windows form: every open
 * there follows no reparse point at its last component (O_NOFOLLOW), a folder is a folder handle
 * (O_DIRECTORY), no handle is inherited (O_CLOEXEC), nothing blocks on a pipe (O_NONBLOCK), and
 * every stdio stream the layer makes is binary (plat_file_stdio adds "b", _open_osfhandle gets
 * _O_BINARY). */
#ifndef RBSP_PLAT_HELPER_H
#define RBSP_PLAT_HELPER_H

#include "plat.h"

/* ---------------------------------------------------------------- the C library's gaps */

/* BSD's strlcat: appends src to dst (n bytes in all, always terminated when n > 0); the length
 * it tried to make */
COMMON_API size_t plat_strlcat(char *dst, const char *src, size_t n);
/* POSIX's getline: the next line of f with its '\n' into *line (grown with realloc, *cap its
 * size); its length, or -1 at the end of the file, on a read error or when out of memory */
COMMON_API long plat_getline(char **line, size_t *cap, FILE *f);

/* A stdio stream's data to its disk: fflush, then Mac fsync(fileno(s)), Windows FlushFileBuffers
 * on the stream's own handle. Once plat_file_stdio has made a stream, the stream owns the file
 * (on Windows the plat_file's handle is closed then): sync through the stream, never through the
 * plat_file. 0 or an errno (EBADF: the stream has no file). */
COMMON_API int plat_stdio_sync(FILE *s);

/* ---------------------------------------------------------------- a folder's files, recursively */

/* fn(ctx, rel, st) for each regular file under the folder at path (prepare.c's fts walk): rel its
 * path below that folder (components joined by the OS's separator), st as plat_dir_each gives it
 * (alloc: the cache's size rule). Folders are entered, at most 64 deep: a deeper one is skipped,
 * with its files, and the walk returns -1; a link, junction or other reparse point is neither
 * followed nor reported. 0 when every folder was read whole; -1 when one couldn't be (the walk
 * goes on with the others) or fn returned nonzero (which stops it). */
typedef int (*plat_walk_fn)(void *ctx, const char *rel, const plat_stat *st);
COMMON_API int plat_walk(const char *path, plat_walk_fn fn, void *ctx);
#if defined(_WIN32) || defined(RBSP_TEST_PREPARE_CACHE_AT)
/* The same walk under an already open folder. The caller owns the handle. */
COMMON_API int plat_walk_at(plat_dir top, plat_walk_fn fn, void *ctx);
#endif

/* ---------------------------------------------------------------- the model file */

/* Identity from the open model or its path (following links), with the mtime in Unix seconds
 * and full subsecond precision (nanoseconds on the Mac, 100 ns steps on Windows). */
typedef struct { plat_id id; int64_t size, mtime; long mtime_nsec; } plat_model_info;
COMMON_API int plat_model_stat_file(plat_file f, plat_model_info *info);
COMMON_API int plat_model_stat_path(const char *path, plat_model_info *info);

/* ---------------------------------------------------------------- a track's file */

/* What a kept set's manifest records of a track (manifest.c) and readable.c checks: its kind
 * (PLAT_FILE, PLAT_DIR, PLAT_OTHER), size and modification time to the nanosecond since 1970.
 * plat_stat's mtime is whole seconds. Following links: Mac, stat(2) (its errno as it is);
 * Windows, GetFileAttributesExW, which reads the attributes only, so a cloud placeholder isn't
 * hydrated (a link is the link's own entry there: helper.md T14 decides links and placeholders),
 * LastWriteTime as Unix ns. 0, or an errno. */
typedef struct { int kind; int64_t size, mtime_ns; } plat_finfo;
COMMON_API int plat_finfo_path(const char *path, plat_finfo *fi);
#if defined(_WIN32) || defined(RBSP_TEST_WIN_RATE)
/* The same metadata from the handle the decoder reads. */
COMMON_API int plat_finfo_file(plat_file f, plat_finfo *fi);
#endif

/* A track opened for reading (the decoders', readable.c's). Windows: by the track path kind
 * (win_rules.h's wr_track_nt_path: wr_nt_path's paths and validated UNC, "\\server\share\..." or
 * "\\?\UNC\...", as "\\?\UNC\..."; never a relative, dotted or device path), links followed,
 * sharing everything, synchronous; plat_finfo_path and plat_path_where take the same paths. UNC is
 * for track input only: a cache location never takes it (helper.md T13, the UNC ruling). Mac:
 * plat_file_open_path. 0 or an errno (EINVAL: not a track path) */
#if defined(_WIN32)
COMMON_API int plat_track_open(const char *path, plat_file *out);
#else
static inline int plat_track_open(const char *path, plat_file *out) { return plat_file_open_path(path, out); }
#endif

/* ---------------------------------------------------------------- temporary files */

/* The account's own temporary folder, never from the environment (app.md §5.6 sets the helper's):
 * Windows: the LocalAppData known folder + "\Temp" (not GetTempPath2W, which reads TMP and TEMP);
 * Mac: confstr(_CS_DARWIN_USER_TEMP_DIR). 0, or -1 when there is none or it doesn't fit. */
COMMON_API int plat_temp_dir(char *out, size_t n);
/* A new file there that only this process can reach and that is gone once closed (Windows:
 * FILE_ATTRIBUTE_TEMPORARY and FILE_FLAG_DELETE_ON_CLOSE, sharing nothing; Mac: mkstemp, then
 * unlinked), read-write, not inherited. 0 or an errno. */
COMMON_API int plat_temp_file(plat_file *out);

#if defined(_WIN32)
/* ---------------------------------------------------------------- Windows only */

/* An account folder from the shell (SHGetKnownFolderPath), never from the environment: Local
 * (%LOCALAPPDATA%'s: kept sets, the temporary folder) or Roaming (%APPDATA%'s: rekordbox's
 * master.db). 0, or -1 when there is none, it isn't an absolute "X:\" path or it doesn't fit. */
enum { PLAT_FOLDER_LOCAL_APPDATA, PLAT_FOLDER_ROAMING_APPDATA };
COMMON_API int plat_known_folder(int which, char *out, size_t n);

/* What reading a track may cost before it is read, from its attributes and its drive only
 * (GetFileAttributesExW and GetDriveTypeW: neither opens the file's data, so a cloud placeholder
 * isn't hydrated): readable.c's and may_ask's checks (helper.md §1, T14). */
typedef struct {
    int exists;                 /* the attributes could be read: the name is there */
    int dir;
    int placeholder;            /* RECALL_ON_DATA_ACCESS or RECALL_ON_OPEN: reading it downloads it (OneDrive) */
    int offline;                /* FILE_ATTRIBUTE_OFFLINE */
    int pinned, unpinned;       /* the cloud's "always keep on this device" / "free up space" marks */
    int remote, removable;      /* its drive: a network share, a removable disk */
    uint32_t attributes;        /* the raw FILE_ATTRIBUTE_* bits, for the log */
} plat_where;
/* 0 and *w; ENOENT when the name isn't there (w->exists 0); another errno */
COMMON_API int plat_path_where(const char *path, plat_where *w);

/* The helper's first call, before anything else runs: SetDefaultDllDirectories with System32
 * only, so no DLL is ever loaded from the current folder, the exe's folder or PATH (the helper
 * loads rekordbox's DLLs by full path: plat_lib_open); then the three standard streams in binary
 * mode (_setmode), so the protocol and the files written through them get no CR. 0, or -1 when
 * either failed (the caller stops: it must not run with an unsafe DLL search). */
COMMON_API int plat_exe_start(void);
#endif

#endif
