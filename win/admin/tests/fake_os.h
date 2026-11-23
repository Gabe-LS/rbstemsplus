/* The worker's tests on the Mac: a fake admin_os and compat_os over plat_mac.c in a scratch tree.
 * The fake's ACL verdicts, processes, windows, mapped files, load check and self removal are set
 * by each case; files are real. */
#ifndef RBSP_ADMIN_TEST_FAKE_OS_H
#define RBSP_ADMIN_TEST_FAKE_OS_H

#include "../worker.h"

enum { FAKE_MAX = 16 };

typedef struct fake {
    /* compat_os */
    compat_uninstall entries[FAKE_MAX];
    int n_entries;
    int entries_partial;
    compat_proc procs[FAKE_MAX];
    int n_procs;
    int procs_fail;
    int pin_error;              /* every pin answers ERROR */
    int pin_calls;
    /* admin_os */
    const char *bad_acl[FAKE_MAX];   /* paths whose ACL check fails (folders and files) */
    int n_bad_acl;
    int root_not_exact;              /* an existing root isn't exactly our DACL */
    int windows;                     /* rekordbox_windows' answer */
    int mapped;                      /* mapped's answer */
    int rekordbox_exits_after;       /* sleeps until the processes are gone (0: never) */
    int sleeps;
    uint64_t clock;
    int load_fail;                   /* load_check fails */
    const char *load_fail_sub;       /* load_check fails for the paths holding this (one version folder's) */
    const char *pin_no_sub;          /* every pin of a path ending in this answers NO */
    int load_calls;
    void (*on_load)(struct fake *f); /* called in load_check (a case's hook) */
    char self[1024];
    int reboot;                      /* delete_self takes the reboot branch */
    int rmdir_fail;                  /* the POSIX branch's folders can't be removed now */
    int schedule_fail;               /* nor scheduled for the next restart */
    int pin_ok_left;                 /* > 0: that many more pins answer as usual, then every one NO */
    char scheduled[4][1024];         /* what the reboot branch scheduled */
    int n_scheduled;
    int rnd;
    int mid_holds;                   /* admin_test_mid_hold's calls (an ADMIN_TEST_HOLD build) */
    const char *fail_target;         /* only this version fails its post-swap load check */
    const char *mid_target;          /* only hold after swapping this version */
    char mid_where[256];             /* the last one's where */
    void (*on_mid_hold)(struct fake *f);   /* called in it (a case's hook: the files as a cut-off run leaves them) */
    void (*on_admin_only)(struct fake *f, const char *path);   /* called in file_admin_only with the file's path (a case's hook) */
    void (*on_mapped)(struct fake *f);   /* called in mapped (a case's hook: just before a change) */
    void (*on_pin)(struct fake *f, const char *path);   /* called in the pin with the file's path (a case's hook) */
    void (*on_root_create)(struct fake *f);   /* called in root_open just before it makes a missing root (another run's moment) */
    int procs_seen;                  /* the process list's calls (the run's: the case resets it) */
    void (*on_procs)(struct fake *f, int call);   /* called in the process list with procs_seen (a case's hook) */
    void (*on_random)(struct fake *f);   /* called in random_hex (a case's hook: just before a temp name is used) */
    char held_file[4][1024];         /* the files hold_file holds now ("" a free slot) */
    int file_holds;                  /* hold_file's successful calls */
    int procs_from_call;             /* > 0: the process list is empty until its procs_from_call-th call (rekordbox starts mid-run) */
    int proc_calls;
    char said[64 * 1024];            /* every line, one per line */
    size_t said_n;
    int verbose;
    compat_os compat;
    admin_os os;
} fake;

void fake_init(fake *f);
/* whether a line said since the last fake_clear_said contains s */
int fake_said(const fake *f, const char *s);
void fake_clear_said(fake *f);
/* a rekordbox uninstall entry for folder (its DisplayVersion dv) */
void fake_entry(fake *f, const char *folder, const char *dv);
void fake_proc(fake *f, const char *name, const char *path);
/* whether the file at path (its real path) is held by hold_file now: a write, rename or removal
 * of it is refused on Windows then, and a case's hook that stands for another program skips it */
int fake_file_held(const fake *f, const char *path);

enum { FAKE_REG_64 = 1, FAKE_REG_32 = 2 };
enum { FAKE_RB_STOCK, FAKE_RB_BRIDGED, FAKE_RB_INTERRUPTED };
enum { FAKE_RB_SIGNED, FAKE_RB_UNSIGNED, FAKE_RB_UNVERIFIABLE };

typedef struct {
    const char *root, *stems_root;
    const unsigned char *exe_7219;
    size_t exe_size;
    const char *bridge;
} fake_inventory;

typedef struct {
    const char *version;
    int views;        /* zero: disk only, both flags: duplicate rows from both views */
    int registry_only;
    int trust;
    int live;
} fake_install;

/* Add any combination of version folders and registrations. The EXE template has ProductVersion
 * 7.2.19; use same-length versions. Both modified states get a saved original, and the bridged
 * state also seeds the shared runtime if absent and writes the folder's receipt (receipt.h: the
 * completed install, payload_version 1.5.0). */
void fake_inventory_add(fake *f, const fake_inventory *inventory, fake_install install);
/* Pioneer's onnxruntime.dll of a version: "signed", one per version, every version's the same
 * "code" (fake_pe_code.c: the bytes before " #cert "): AlphaTheta re-signs one ORT per release (D2a) */
void fake_stock_dll(const char *version, char *out, size_t n);
/* the code digest every fake_stock_dll shares (64 hex) */
void fake_stock_code(char out[65]);

#endif
