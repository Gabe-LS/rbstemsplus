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

#endif
