/* worker.c: the elevated worker's decisions and steps (app.md §5.5, bridge.md §2 T9), in pure C11
 * over common/plat.h's file layer, win/c's payload.c (verification and the floor) and compat.c
 * (rekordbox, the AlphaTheta pin, processes). What only Windows does (held handles and their
 * share modes, the admin-only ACL on a handle, the root's protected DACL, the lock, the
 * mapped-image check, the load check, the worker's own removal, the pipe) goes through
 * admin_os, which worker_win.c implements and the tests fake (on the Mac, over plat_mac.c, in a
 * scratch tree). The build name is rbsp-admin.exe; every shipped or installed copy is named
 * "RB Stems Plus.exe" (I78).
 *
 * Layout (app.md §5.10, the root admin-only): <root> = <Program Files>\RB Stems Plus\, where
 * <Program Files> is Windows' own on whichever drive (plat_program_files, HKLM's ProgramFilesDir)
 *   lock                      the lock (an exclusive handle, never a Global\ mutex)
 *   payload-version           the floor (payload_store.c's record); payload.json(.sig): the pair
 *                             that raised it
 *   receipts\<v>              one per installation holding our bridge (receipt.h): the folder's name
 *                             and identity, rekordbox.exe's, the bridge's, the original's and the
 *                             runtime's sha256; deleted before a swap, written after its checks
 *   pioneer\<v>\              Pioneer's onnxruntime.dll of rekordbox <v>, SHA256SUMS last
 *   ort\onnxruntime.dll       Pioneer's ORT, which every bridge loads: one copy, kept while it is a
 *                             bound original's bytes (mv-spec §2.3)
 *   bin\<sha12>\RB Stems Plus.exe   the installed workers (I78)
 *   stage.<hex>\              a run's staging folder, with an Install's copies of the root files it
 *                             replaces until its first target changes (undo.<n>: a refused run
 *                             puts them back); removed at its end, leftovers by a later run (an
 *                             Install's once a target changed)
 * and in each rekordbox version folder (<Program Files>\rekordbox\rekordbox X.Y.Z\):
 *   onnxruntime.dll           Pioneer's, or the bridge
 *   onnxruntime.dll.rbsp-new  the one temp name of every swap and put-back (bridge.md §2, I36)
 *   onnxruntime.dll.rbsp-probe  the write probe (removed at once)
 *
 * Multi-version rekordbox (.orchestration/mv-spec.md §2.2, decisions D1, D4-D7, D13): Stems Cache
 * is one machine-wide installation over every verified, supported rekordbox folder. The worker
 * reads the inventory again (inventory.c, the app's walk) under the lock, assesses each folder
 * through a held handle and computes the scope with the app's rule (inv_targets): a scope other
 * than --targets is 15, nothing changed. Install and Reinstall run each target in version order
 * (a failure rolls that one back; the ones before it stay installed: 16); Uninstall and Restore
 * take each folder by itself. A saved set goes back into a folder only when its receipt binds it
 * or the signed binding vouches for it, never on X.Y.Z alone; a bridge with no receipt is never
 * "done" and never adopted (D13: Put Back Pioneer's File repairs it).
 *
 * Order of every run (the Mac's, Scripts.swift): the root (created with its protected DACL, or
 * an existing one exactly so), the lock (ADMIN_EXIT_ALREADY_RUNNING), leftovers, Pioneer's
 * installer or updater (8) and rekordbox open in any session, with the linger allowed (5: the
 * system-wide gate, D6), the payload and --manifest (install), the inventory and each folder's
 * checks (9 none; 10 not Pioneer's or not admin-only; 4), then the verb. The classification comes
 * before anything is touched; the system-wide gate (8, 5) is asked again before the shared writes
 * and before each folder's change; after a swap, any failure is a rollback through the one
 * put-back routine (6, 4 or 11). */
#ifndef RBSP_WIN_ADMIN_WORKER_H
#define RBSP_WIN_ADMIN_WORKER_H

#include "admin_proto.h"
#include "compat.h"
#include "payload.h"
#include "plat.h"

#include <stddef.h>
#include <stdint.h>

/* caps (bridge.md §2, Scripts.swift:310-311); ADMIN_BRIDGE_MAX is admin_proto.h's */
#define ADMIN_ORT_MAX ((int64_t)128 * 1024 * 1024)
#define ADMIN_WORKER_MAX ((int64_t)16 * 1024 * 1024)
#define ADMIN_SIG_MAX 4096

enum { ADMIN_GONE_AT_RESTART = 1, ADMIN_FOLDERS_STAY = 2 };   /* delete_self's *reboot bits */

/* What only Windows does. Every call is on the worker's one thread. */
typedef struct admin_os {
    const compat_os *compat;   /* the uninstall entries, the AlphaTheta pin, the process list */
    void *ctx;
    /* The root folder at the absolute path: created when missing (create) with an explicit
     * protected DACL (SYSTEM and Administrators full, Users read and execute, inherited by what
     * is made in it) and owner Administrators; an existing one must have exactly that owner and
     * DACL. Held for the run as hold_dir holds. *made: whether this call made it (one another run
     * made meanwhile is opened, never counted as made: re-review blocker A). 0 with *out; ENOENT
     * (missing, create 0); EACCES (an existing one isn't exactly that); ELOOP (a reparse point);
     * another errno. */
    int (*root_open)(void *ctx, const char *path, int create, plat_dir *out, int *made);
    /* Opens folder `name` in parent (parent not open: name is an absolute path) without following
     * a reparse point, held (READ_CONTROL | FILE_LIST_DIRECTORY | SYNCHRONIZE, share READ|WRITE,
     * never DELETE: it can't be renamed or removed while held), and checks its descriptor on that
     * handle: the shared fail-closed admin-only rule (common/plat_win.c). 0 with *out; ENOENT;
     * ENOTDIR; ELOOP (a reparse point); EACCES (the ACL check failed); another errno. */
    int (*hold_dir)(void *ctx, plat_dir parent, const char *name, plat_dir *out);
    /* The file `name` in d: no reparse point, and the same admin-only rule on its descriptor.
     * 0; ENOENT; ELOOP; EACCES; another errno. */
    int (*file_admin_only)(void *ctx, plat_dir d, const char *name);
    /* Makes folder `name` in d (inheriting d's DACL, owner Administrators): 0 or EEXIST or an errno */
    int (*mkdir_at)(void *ctx, plat_dir d, const char *name);
    /* Removes the empty folder `name` in d (a reparse point there: the point itself) */
    int (*rmdir_at)(void *ctx, plat_dir d, const char *name);
    /* Renames from to `to` in d without replacing: EEXIST when something has that name */
    int (*rename_noreplace)(void *ctx, plat_dir d, const char *from, const char *to);
    /* The lock: an exclusive handle (share 0) on `lock` in root, made when missing (*made: by this
     * call). 0 with *token; EBUSY when another run holds it; another errno. */
    int (*lock)(void *ctx, plat_dir root, void **token, int *made);
    void (*unlock)(void *ctx, void *token);
    /* Whether `name` in d is mapped (opened for FILE_WRITE_DATA sharing READ|WRITE|DELETE: a
     * sharing violation means mapped): 1, 0, or -1 when it couldn't be told */
    int (*mapped)(void *ctx, plat_dir d, const char *name);
    /* How many visible top-level windows rekordbox's processes have (PLAN R1's linger is a
     * process with none left); -1 when it can't be told */
    int (*rekordbox_windows)(void *ctx);
    /* The load check: the library at path loaded (its folder and System32 only), symbol looked
     * up, the library freed again. 0 when both worked. */
    int (*load_check)(void *ctx, const char *path, const char *symbol);
    /* The worker's own image's absolute path */
    int (*self_path)(void *ctx, char *out, size_t n);
    /* The last step of removing the root (I77, J7): the worker's own file self, inside
     * folders[0] (bin\<sha12>\), folders[1] (bin\), folders[2] (the root), everything else
     * already gone and no handle held. POSIX delete of self, then the folders removed, child
     * first, each one that can't be removed now scheduled with MOVEFILE_DELAY_UNTIL_REBOOT; or
     * the reboot branch: self renamed to "<self>.<random>.del" and that name and the folders
     * scheduled, child first. *reboot: ADMIN_GONE_AT_RESTART when something waits for the next
     * restart, plus ADMIN_FOLDERS_STAY when a folder could be neither removed nor scheduled. 0,
     * or an errno when the file itself could be neither deleted nor scheduled. */
    int (*delete_self)(void *ctx, const char *self, const char *const folders[3], int *reboot);
    /* one line for the app (the pipe); never blocks long, never fails the run */
    void (*say)(void *ctx, const char *line);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    uint64_t (*now_ms)(void *ctx);
    /* n random lowercase hex digits and a NUL into out (n + 1 bytes) */
    void (*random_hex)(void *ctx, char *out, size_t n);
    /* Holds the file `name` in d against writes, renames and removal until release_file: opened
     * for reading, never following a reparse point, sharing READ only (another open for writing
     * or deleting it fails, so it can't be written, replaced or removed while held). 0 with
     * *token; ENOENT; ELOOP (a reparse point); EBUSY (another handle writes or may delete it);
     * another errno. */
    int (*hold_file)(void *ctx, plat_dir d, const char *name, void **token);
    void (*release_file)(void *ctx, void *token);
} admin_os;

/* Where things are and what is trusted. main.c passes the places under Windows' own Program
 * Files (plat_program_files; a test build's may be RBSP_TEST_PROGRAMS's fake tree, one name under
 * a drive root, app.md §7). */
typedef struct {
    const char *root;          /* <Program Files>\RB Stems Plus */
    const char *rekordbox;     /* <Program Files>\rekordbox */
    const sig_pubkey *keys;    /* the compiled-in keys */
    size_t nkeys;
    const char *minimum;       /* the compiled minimum (J18): the floor before the floor file exists */
    uint32_t linger_ms;        /* how long a rekordbox with no window left may linger (PLAN R1: ~75 s) */
    uint32_t poll_ms;          /* how often the linger is looked at */
} admin_env;

/* One run: the verb in a (parsed, protocol checked), its exit code (admin_proto.h). Lines go to
 * os->say as it goes. */
int admin_run(const admin_env *e, const admin_os *os, const admin_args *a);

#ifdef ADMIN_TEST_HOLD
/* Test builds only (win/admin/build.sh --test-key; a release build has neither the call nor the
 * function): admin_run calls it once its work has begun changing files, at the one point of a
 * verb where a run cut off leaves rekordbox mid-change (where: what is done and what isn't yet):
 *   install    the bridge swapped into a version folder, not yet verified (its receipt deleted, none
 *              written yet); with several targets, also each target done (its receipt written)
 *              before the next one's swap
 *   uninstall  Pioneer's file put back into rekordbox, our folder not yet removed
 * Other verbs, and runs that change nothing there (already in place, nothing to put back), never
 * call it. main.c's holds the worker RBSP_TEST_WORKER_MIDHOLD seconds there (the VM's sign-out
 * cases WC3 and WC10: the session's end lands mid-work); the tests' fake records the call. */
/* version is set only for a swap. Setting RBSP_TEST_WORKER_MIDHOLD_TARGET selects that swap. */
void admin_test_mid_hold(const admin_os *os, const char *where, const char *version);
/* Fail a selected target in the existing load-check and rollback path. */
int admin_test_fail_target(const admin_os *os, const char *version);
#endif

/* -- the pure rules (worker.c), exported for the tests */

/* SHA256SUMS of the saved set: exactly "<sha256>  onnxruntime.dll\n" */
int admin_sums_line(const char *sha, char *out, size_t cap);
/* the classification of a version folder's live onnxruntime.dll (bridge.md §2, I6) */
enum {
    ADMIN_LIVE_PIONEER,    /* no marker, and the pin passes */
    ADMIN_LIVE_OURS,       /* the marker, its sha listed (shipped or the installed record), the saved set complete */
    ADMIN_LIVE_HALF_RUN,   /* neither, the saved set complete: put it back, then check again as Pioneer's */
    ADMIN_LIVE_ABSENT,     /* no live file, the saved set complete: put it back (I71) */
    ADMIN_LIVE_REFUSED,    /* anything else */
    ADMIN_LIVE_NO_ANSWER,  /* couldn't be looked at (an error, the pin ERROR) */
    ADMIN_LIVE_NOTHING,    /* no live file and no saved set: nothing of ours, nothing to put back */
};
/* present/exists/marker/pin as compat_file gives them (pin asked only without the marker); listed:
 * its sha256 is a shipped one or the installed record's; complete: the saved set */
int admin_classify(int present, int exists, int readable, int marker, int pin, int listed, int complete);

/* The root's descriptor, exactly (app.md §5.5): owner Administrators; a protected DACL holding
 * exactly, in this order, ALLOW SYSTEM full, ALLOW Administrators full, ALLOW Users read and
 * execute, each inherited by files and folders (OI|CI) and none inherited itself. */
enum { ADMIN_SID_OTHER, ADMIN_SID_SYSTEM, ADMIN_SID_ADMINS, ADMIN_SID_USERS };
#define ADMIN_ROOT_FULL 0x001F01FFu   /* FILE_ALL_ACCESS */
#define ADMIN_ROOT_READ 0x001200A9u   /* FILE_GENERIC_READ | FILE_GENERIC_EXECUTE */
#define ADMIN_ROOT_INHERIT 0x03u      /* OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE */
typedef struct { unsigned type, flags; uint32_t mask; int sid; } admin_ace;
/* owner: ADMIN_SID_*; is_protected: SE_DACL_PROTECTED; present: a DACL that isn't NULL */
int admin_root_acl_exact(int owner, int is_protected, int present, const admin_ace *a, size_t n);
/* The lock's descriptor, exactly (admin-only: no other account may open it first and hold it):
 * owner Administrators; a protected DACL holding exactly ALLOW SYSTEM full, ALLOW Administrators
 * full, neither inherited nor inheritable. An existing lock that isn't so is made again. */
int admin_lock_acl_exact(int owner, int is_protected, int present, const admin_ace *a, size_t n);

#endif
