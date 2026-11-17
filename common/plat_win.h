/* What plat_win.c adds to plat.h v1 on Windows only. plat.h itself is left alone: it is frozen,
 * and the Mac builds read it. */
#ifndef RBSP_PLAT_WIN_H
#define RBSP_PLAT_WIN_H

#include "plat.h"

#if defined(_WIN32)
/* plat_owner_check's rule held through the load (owner decided 2026-11-11, review B1; bridge.md
 * §1): the file's `levels` parent folders are opened top-down, the top one by its path and each
 * one below relative to the one above, then the file relative to its folder; each without
 * following a reparse point, for reading with READ_CONTROL, sharing READ only (so no one can
 * write, rename or delete what was checked, and a handle someone holds for that refuses it); each
 * descriptor checked on its own handle. LoadLibraryExW (plat_lib_open) runs while the handles are
 * held; they are closed after. The verdict as plat_owner_check gives it (a sharing violation is
 * FILE or FOLDER, a missing folder MISSING); on PLAT_OWNER_OK, *out is the library, or NULL with
 * *err when the loader refused it. levels: at most 8. The Mac keeps plat_owner_check and
 * plat_lib_open. */
COMMON_API int plat_lib_open_checked(const char *path, int levels, int accept_self, char *bad, size_t n, plat_lib **out,
                                     const char **err);

/* plat_lib_open_checked's check and hold without the load (review NT1): the file's `levels`
 * parent folders opened top-down and the file relative to its folder, each without following a
 * reparse point, for reading (FILE_LIST_DIRECTORY, FILE_READ_DATA) with READ_CONTROL, sharing
 * READ only, each descriptor checked on its own handle. The verdict as plat_lib_open_checked's (a
 * sharing violation is FILE or FOLDER, a missing folder MISSING). On PLAT_OWNER_OK, *out holds
 * all of them open until plat_owner_release: no one can write, rename or delete what was
 * checked, and a write or delete handle someone already held refused it. Otherwise *out is NULL
 * and nothing is held. A read-only open (plat_read_file) and the loader's and CreateProcess's
 * read/execute opens share with it. For the core's elevation of the admin worker (admin.c:
 * held through ShellExecuteExW). plat_owner_release(NULL) does nothing. */
typedef struct plat_owner_held plat_owner_held;
COMMON_API int plat_owner_hold(const char *path, int levels, int accept_self, char *bad, size_t n, plat_owner_held **out);
COMMON_API void plat_owner_release(plat_owner_held *h);

/* Deletes `name` in d and, when it is a real folder, everything under it, by handle (T3b): each
 * entry opened relative to its folder's handle with FILE_OPEN_REPARSE_POINT, folders listed by
 * handle, every entry deleted by handle (POSIX delete, read-only ignored, on NTFS/ReFS; on
 * FAT/exFAT a read-only entry stays and gives EBUSY), and a reparse point (a link, a junction, a
 * mount point) deleted as itself, never followed, even one swapped in during the walk
 * (win_rules.h's wr_remove_tree). 0; ENOENT when nothing is there; else the first errno (EBUSY:
 * in use, as a running exe); what could be deleted is. */
COMMON_API int plat_remove_tree_at(plat_dir d, const char *name);
/* Deletes the folder `name` in d only when it is the folder `expect` names (its id, taken through
 * the caller's own handle on it) and empty, through one handle opened without following a
 * reparse point: 0; ELOOP a reparse point; ENOTDIR not a folder (a file symlink too); ESTALE
 * another folder; ENOTEMPTY not empty; ENOENT; or another errno (review MT1: never by name, never
 * recursive). On FAT32 and exFAT the id is the entry's position, so an empty folder made in the
 * freed slot can pass as `expect`: at worst an empty replacement goes, as the Mac's
 * unlinkat(AT_REMOVEDIR) (review MQ1) */
#ifndef ESTALE
#  define ESTALE 151   /* not in the UCRT's errno.h */
#endif
COMMON_API int plat_remove_empty_dir_at(plat_dir d, const char *name, plat_id expect);
/* the same for an absolute path: its parent folder opened by path (the caller's path is trusted:
 * a junction among its ancestors is resolved as usual; none at its last component), then
 * plat_remove_tree_at */
COMMON_API int plat_remove_tree(const char *path);
/* plat_owner_check's rule on one handle the caller opened (with READ_CONTROL and
 * FILE_READ_ATTRIBUTES, without following a reparse point): not a reparse point, and the
 * descriptor read from that handle (NtQuerySecurityObject, never GetSecurityInfo) owned by
 * Administrators, SYSTEM or TrustedInstaller with every write-granting ACE theirs; fails closed
 * (a null or absent DACL, a failed query, an unknown ACE type). 1 ok, 0 not. For the admin
 * worker's held folders and files (app.md §5.5). accept_self as plat_owner_check's. */
COMMON_API int plat_win_handle_admin_only(void *handle, int accept_self);

/* Windows' own Program Files folder (win_rules.h's wr_program_files: HKLM's ProgramFilesDir in
 * the 64-bit view, REG_SZ, read through ADVAPI32), joined with sub (NULL or "": the folder
 * itself; WR_PF_ROOT, WR_PF_REKORDBOX, WR_PF_REAL_ORT), into out (n bytes, NUL included): 0, or -1
 * with out "" (unreadable, refused, or too long): the caller fails closed. A test build
 * (PLAT_TEST_PROGRAMS) takes RBSP_TEST_PROGRAMS instead when it is set, through the same rule
 * (the VM's fake trees, app.md §7); a release build never reads the environment here. */
COMMON_API int plat_program_files(const char *sub, char *out, size_t n);

/* Whether path's drive letter is a network drive (GetDriveTypeW's DRIVE_REMOTE, mapped and
 * connected or not), which the table of volumes (plat_volume_holding) doesn't list: a chosen
 * location there is "not-local", not "not-connected" (common/location.c, T3c). A volume GUID
 * path never is. */
COMMON_API int plat_win_drive_remote(const char *path);

/* The name of the volume holding path, as driveName shows it (review NL1, NL5): its label
 * (GetVolumeInformationW) into label, and its first drive letter (GetVolumePathNamesForVolumeNameW)
 * as "E:" into letter; either "" when it has none. The root goes through the same lookup as
 * plat_volume_info (a long drive path prefixed, a volume GUID path as it is). 0 when the path's own
 * volume answered (the same GUID for a volume GUID path, the same letter for a drive path); another
 * volume answering is ENOENT (review NR1); an errno when it isn't there (unplugged) or can't be
 * read. */
COMMON_API int plat_volume_name(const char *path, char *label, size_t ln, char *letter, size_t lt);

#ifdef PLAT_TEST_FLUSH
/* test builds only (common/tests/win.sh's plat-win-test): plat_map_flush's next n flushes report
 * err (0: ERROR_LOCK_VIOLATION) without flushing (n <= 0: none), and the sleeps' record restarts;
 * _left is how many are still to come; _sleeps gives each sleep's ms since (*ms, the first 16) and
 * returns how many there were */
COMMON_API void plat_test_flush_faults(int n, unsigned err);
COMMON_API int plat_test_flush_faults_left(void);
COMMON_API int plat_test_flush_sleeps(const unsigned **ms);
#endif
#endif

#endif
