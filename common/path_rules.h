/* Path rules, as pure strings (bridge.md §0a): a chosen location's path. The Mac form here,
 * then the Windows form (X:\ or \\?\Volume{GUID}\, bridge.md §1, T3c).
 *
 * And the prepare helper's rules for a track's path (helper.md §1, §4): whether it is a file on
 * this computer's file system, whether it is on another drive, and whether two paths are the same
 * file for a kept set's manifest. Each OS's form is selected by the build; the Windows forms of
 * the pure ones are compiled on both, so the Mac tests them too (common/tests/modules.c). */
#ifndef RBSP_PATH_RULES_H
#define RBSP_PATH_RULES_H

#include "api.h"

#include <stddef.h>
#include <string.h>

#define PATHS_CACHE_FOLDER "RB Stems Plus Cache.noindex"   /* a chosen location's folder (the app makes it) */
#define PATHS_LOCATION_MAX 700                             /* bytes in a chosen location's path */

/* a chosen location's path: absolute, at most PATHS_LOCATION_MAX bytes, plain components (none
 * empty, "." or ".."), no '"' or control character, and ending in our folder's name */
COMMON_API int paths_location_ok(const char *p);

/* ---------------------------------------------------------------- a track's path (the helper) */

/* Windows' forms, pure strings: a local file is X:/ or X:\ (one letter) or a UNC path (// or \\);
 * the same file: the same bytes but for the drive letter's case and / against \ */
COMMON_API int paths_local_file_win(const char *p);
COMMON_API int paths_same_win(const char *a, const char *b);

#if defined(_WIN32)
#define paths_local_file paths_local_file_win
#define paths_same paths_same_win
#else
/* A track's path that names a file on this computer's file system (a playlist's streaming tracks
 * don't): Mac, an absolute path. Its inputs, library.c's local rows and the estimate use it. */
static inline int paths_local_file(const char *p) { return p[0] == '/'; }
/* whether a and b name the same track for a kept set's manifest (manifest.c): Mac, the same bytes */
static inline int paths_same(const char *a, const char *b) { return strcmp(a, b) == 0; }
/* Another drive (Mac: under /Volumes/): reading there may wait on macOS's privacy prompt, and a
 * cache there is never made or looked at by --quick. The length of p's drive root at its start
 * ("/Volumes/<name>": what a given-up read is remembered for), 0 when p isn't on another drive.
 * Windows' form (a path not on the %SystemRoot% volume) needs the OS: helper.md T13. */
COMMON_API size_t paths_other_drive(const char *p);
#endif

/* ---------------------------------------------------------------- Windows' form (T3c) */

/* Windows' form of a chosen location's path (bridge.md §1), a pure string compiled on both OSes so
 * the Mac tests it too: a root, "X:\" (one letter) or a volume GUID path "\\?\Volume{GUID}\"
 * (what cache_dir stores: drive letters change between plugs), then components separated by '\':
 * none empty, "." or "..", none ending in '.' or a space, none holding any of <>:"/|?* or a
 * control character (DEL included); at most PATHS_LOCATION_MAX bytes and ending in our folder's
 * name, which may sit at the root (a drive's own folder: Windows' system volume is refused by
 * where it is, check_location, not by its path) */
COMMON_API int paths_location_ok_win(const char *p);
/* the length of a Windows path's root: 3 for "X:\", 49 for "\\?\Volume{GUID}\" (the GUID's hex
 * digits in either case), 0 for anything else (a '/' separator, "\\?\X:\", UNC, relative) */
COMMON_API size_t paths_root_win(const char *p);

/* the build's own form, which cfg_ini.c's reader applies to cache_dir: Windows' on Windows (and
 * with PATHS_WIN_FORM, the Mac's test of that reader with Windows' fixtures), else the Mac's */
#if defined(_WIN32) || defined(PATHS_WIN_FORM)
#define paths_location_ok_os paths_location_ok_win
#else
#define paths_location_ok_os paths_location_ok
#endif

#endif
