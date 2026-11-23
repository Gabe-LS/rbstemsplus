/* compat.c: rekordbox, its versions and AlphaTheta's files on Windows (app.md §5.2, W2), the
 * facts Compat.swift and Rekordbox.swift give the Mac's gating, plus R1's (the private Windows plan's R1):
 *  - the current rekordbox: the version folder under <Program Files>\rekordbox\ that an
 *    uninstall entry names (rekordbox has no App Paths entry; phase0-probes §2.4), with an
 *    AlphaTheta-signed rekordbox.exe; its folder suffix, the entry's DisplayVersion and the first
 *    three parts of rekordbox.exe's ProductVersion agree (the private Windows plan §2);
 *  - the AlphaTheta pin (the one bridge.md §2 uses) on rekordbox.exe and on Pioneer's ONNX
 *    Runtime, and that ORT's sha256 in windows.compatible.ort_sha256;
 *  - R1's facts: the live onnxruntime.dll missing (a Defender quarantine included), or neither
 *    ours nor AlphaTheta's; `ort\`'s copy missing. Smart App Control is not one of them (I69);
 *  - rekordbox, its updater or its uninstaller running, in any session;
 *  - the resampler check: rekordbox.exe's ProductVersion and sha256 in windows.resampler_proved
 *    (r4:I54, J4);
 *  - rekordbox's process machine against the native one, for the report.
 *
 * File facts (a 17 MB read and a signature check) are memoised by path, size, mtime and file
 * identity (plat_stat's whole seconds: a rewrite in place within the same second at the same
 * size isn't seen; every swap here is a rename, a new file). Reading and pinning run on worker
 * threads (app.md §2 rule 1): the shell's cheap facts on the UI thread (host_facts.c) find them
 * memoised, since core_win_warm_facts reads them on a worker before the first refresh and the
 * steps that change them (the admin worker's runs) read them again on their worker before they
 * return. A miss on the UI thread (compat_main_reads) still reads, so the answer stays right,
 * and says so once per file and stat; there a pin with no answer and an unreadable file are
 * remembered by stat too (a worker's read asks again). Files go
 * through common/plat.h's file layer; what only Windows can answer (the registry, the process
 * list, WinVerifyTrust, the machines) goes through compat_os, which compat_win.c implements and
 * the tests fake. Nothing here prints rekordbox's library key or looks at the library. */
#ifndef RBSP_WIN_COMPAT_H
#define RBSP_WIN_COMPAT_H

#include "payload.h"
#include "plat.h"

#include <stddef.h>
#include <stdint.h>

enum { COMPAT_PATH_MAX = 1024, COMPAT_VERSION_MAX = 64, COMPAT_ENTRIES_MAX = 32, COMPAT_PROCS_MAX = 1024, COMPAT_CACHE_LINE_MAX = 4096 };

/* PE machines (IMAGE_FILE_MACHINE_*) */
enum { COMPAT_MACHINE_UNKNOWN = 0, COMPAT_MACHINE_I386 = 0x014c, COMPAT_MACHINE_AMD64 = 0x8664, COMPAT_MACHINE_ARM64 = 0xaa64 };

/* the AlphaTheta pin's answer */
enum {
    COMPAT_PIN_OK = 1,       /* a valid Authenticode signature whose leaf is O=AlphaTheta Corporation, serialNumber 0104-01-151827 */
    COMPAT_PIN_NO = 0,       /* a real verdict: unsigned, a broken or untrusted signature, or another signer */
    COMPAT_PIN_ERROR = -1,   /* no answer (wintrust.dll didn't load, the file couldn't be read, any other
                              * status): never remembered, and nothing is offered on it */
};

/* One uninstall entry (HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\<key>, both
 * registry views), its values as the registry holds them, in UTF-8. */
typedef struct {
    char key[256];
    char display_name[256];
    char display_version[COMPAT_VERSION_MAX];
    char uninstall_string[COMPAT_PATH_MAX];
    char display_icon[COMPAT_PATH_MAX];
    char install_location[COMPAT_PATH_MAX];
    int view;                /* COMPAT_VIEW_64 or COMPAT_VIEW_32: the registry view it came from
                              * (multi-version contract, mv-spec §2.1; 0 until compat_win.c and
                              * the fakes set it). Diagnostics and inventory.c's dedup only */
} compat_uninstall;
enum { COMPAT_VIEW_64 = 1, COMPAT_VIEW_32 = 2 };

/* One process: its image name ("rekordbox.exe"), its full path when this account can read it
 * ("" otherwise: another account's), and its machine (COMPAT_MACHINE_UNKNOWN when unknown). */
typedef struct {
    char name[260];
    char path[COMPAT_PATH_MAX];
    uint16_t machine;
} compat_proc;

/* What only Windows answers (compat_win.c; the tests' fakes). Every function may run on any
 * worker thread. */
typedef struct {
    /* the uninstall entries that may be rekordbox's (any whose key or DisplayName starts with
     * "Pioneer rekordbox" or "rekordbox"); how many were written (at most max). *incomplete is
     * set to 1 when the enumeration wasn't whole (review RC4: a subkey that couldn't be enumerated
     * or opened, a view's root that is there but couldn't be opened, or max reached), else 0 */
    int (*uninstall_entries)(void *ctx, compat_uninstall *out, int max, int *incomplete);
    /* the AlphaTheta pin on the file at path, open as f (the handle compat.c read it through,
     * which WinVerifyTrust reads too): COMPAT_PIN_*. The callback runs while this path's read is
     * in flight: it may call compat_file_facts on its own thread, but it must never wait on another
     * thread's compat_file_facts call (review SF2). */
    int (*pin)(void *ctx, const char *path, plat_file f);
    /* rekordbox's own processes in every session (image names containing "rekordbox": rekordbox.exe,
     * its updater, its uninstaller); how many were written (at most max), -1 when the list
     * couldn't be had */
    int (*processes)(void *ctx, compat_proc *out, int max);
    /* this PC's native machine (IsWow64Process2), COMPAT_MACHINE_UNKNOWN when unknown */
    uint16_t (*native_machine)(void *ctx);
    void *ctx;
} compat_os;

/* compat_win.c: the real one (Windows builds only) */
const compat_os *compat_os_windows(void);
/* compat_win.c (Windows builds only): a count that moves whenever either Uninstall view (HKLM
 * ...\CurrentVersion\Uninstall, 64- and 32-bit) changes, from RegNotifyChangeKeyValue; 0 when
 * the watch couldn't be set up: then nothing is cached on it (compat_find_rekordbox_cached) */
uint64_t compat_uninstall_generation(void);

/* -- pure rules */

/* shortVersion's first three parts compare equal ("7.2.19.0342" and "7.2.19"); empty: never */
int compat_same_short_version(const char *a, const char *b);
/* the version of a folder named "rekordbox X.Y.Z" (its last component, the name case-blind):
 * 1 with "X.Y.Z" in out, or 0 */
int compat_folder_version(const char *folder, char *out, size_t n);
/* Whether an entry is rekordbox's: its key starts with "Pioneer rekordbox " or its DisplayName
 * with "rekordbox " (case-blind). */
int compat_entry_is_rekordbox(const compat_uninstall *e);
/* The version folder an entry names, a direct child of root named "rekordbox X.Y.Z": from
 * InstallLocation, else UninstallString's folder, else DisplayIcon's (quotes and an icon index
 * dropped, "/" read as "\", compared case-blind). 1 with the folder (spelled root + "\" + its
 * name as the entry has it) in out, or 0. */
int compat_entry_folder(const compat_uninstall *e, const char *root, char *out, size_t n);
/* the AlphaTheta leaf's subject (bridge.md §2): exactly O=AlphaTheta Corporation and
 * serialNumber 0104-01-151827 */
int compat_pin_subject(const char *organization, const char *serial_number);

/* A PE file's headers and its version resource, read as data (no version.dll; the worker's rule,
 * app.md §5.5). read(ctx, off, buf, n) fills buf with n bytes at off: 0, or nonzero past the
 * end or on an error. */
typedef int (*compat_read_fn)(void *ctx, uint64_t off, void *buf, size_t n);
typedef struct {
    uint16_t machine;                          /* the COFF header's Machine */
    char product_version[COMPAT_VERSION_MAX];  /* StringFileInfo's ProductVersion, trimmed; "" when none */
    char product_name[COMPAT_VERSION_MAX];     /* its ProductName, the same way */
    uint16_t fixed_product[4];                 /* VS_FIXEDFILEINFO's product version */
    int has_fixed;
} compat_pe;
/* 0 with *out, or -1 when it isn't a PE this reads (out->machine may still be set when only the
 * version resource is missing: then 0 and product_version "") */
int compat_pe_read(compat_read_fn read, void *ctx, compat_pe *out);

/* the resampler check: (ProductVersion, sha256) in windows.resampler_proved; product_version
 * matches an entry written with three parts ("7.2.19") or in full */
int compat_resample_proved(const payload_manifest *m, const char *product_version, const char *exe_sha256);
/* the report's words for rekordbox's process machine: "native", or "x64 under emulation on
 * ARM64" (and so on) when it differs from the native one; "" when either is unknown */
const char *compat_machine_words(uint16_t process, uint16_t native);

/* -- facts (files through plat.h; memoised by path, size, mtime and identity) */

/* compat_file_facts' flags: COMPAT_PIN asks for the AlphaTheta pin too; COMPAT_FRESH reads the
 * file (and pins it) whatever the memo holds, and stores the result.
 *
 * Display, R1's offer and the resampler check (app.md W2) use memoised facts. The key (path,
 * size, whole-second mtime, identity) misses a same-size rewrite in place within its second.
 * W2 accepts that limit to avoid a hash per run: rekordbox updates normally use a new folder,
 * and a stale resampler flag can cost extra resampling or a cache miss. W5's worker reads a
 * verdict FRESH through the handle it holds. */
enum { COMPAT_PIN = 1, COMPAT_FRESH = 2 };

/* one file's facts, from one read: the sha256, the bridge's marker text "rbstems bridge: ",
 * the PE's version and machine, and (COMPAT_PIN) the AlphaTheta pin */
typedef struct {
    int present;             /* something has that name */
    int exists;              /* a regular file (not a link, a reparse point or a folder) */
    int readable;            /* read whole */
    int64_t size, mtime;
    plat_id id;              /* the file read (exists) */
    payload_sha sha256;      /* "" unless readable */
    payload_sha code_sha256; /* pe_code_sha256 (pe_code.h) from the same read, "" unless readable and a
                              * PE that digest takes (mv-spec §2.3; "" until lane B computes it) */
    int marker;              /* holds "rbstems bridge: " (the bridge's own text, Rekordbox.swift) */
    int has_cache_line;      /* the first "rbstems-cache: " is followed by a ';' within 4096 bytes */
    char cache_line[COMPAT_CACHE_LINE_MAX];       /* what's between them (bridgeCacheModels' line, 4095 bytes at most), NUL-ended */
    size_t cache_line_len;   /* its length: the line may hold a NUL (0 without a line) */
    compat_pe pe;
    int pin;                 /* COMPAT_PIN_*; COMPAT_PIN_ERROR when not asked */
} compat_file;
/* 0 with *out (present 0: nothing there); -1 when it couldn't be looked at (its folder or the
 * file refused: an answer for neither), or when the file changed while it was pinned (another
 * file at path, or this one rewritten: the pin isn't this file's; nothing is memoised) */
int compat_file_facts(const compat_os *os, const char *path, int flags, compat_file *out);
/* forgets every memoised file (tests) */
void compat_memo_clear(void);
/* This thread's reads are the UI thread's (on 1) or a worker's (on 0, the default): on the UI
 * thread a memo miss that reads or pins calls miss(ctx, path) (NULL: none), and a COMPAT_PIN_ERROR
 * or an unreadable file is remembered for the UI thread's next reads of that path, size, mtime and
 * identity (never for a worker's, which reads and pins again). */
void compat_main_reads(int on, void (*miss)(void *ctx, const char *path), void *ctx);
#ifdef COMPAT_TEST_HOOKS
/* test builds only: called with the live DLL's path between R1's unpinned and pinned reads */
extern void (*compat_test_between_reads)(const char *path);
/* test builds only: the bytes the scans have read, all threads */
extern int64_t compat_test_scanned;
/* test builds only: when it returns nonzero for path, that file's read fails after it opened (review RC5) */
extern int (*compat_test_read_fails)(const char *path);
/* test builds only: called before a file's read (outside any lock; review HR5's slow read) */
extern void (*compat_test_before_read)(const char *path);
/* test builds only: how many times a miss waited for another thread's read of its path (under the memo's lock) */
extern int compat_test_flight_waits;
/* test builds only: when it returns nonzero for path, a version folder's stat fails (EACCES) (review RE1) */
extern int (*compat_test_stat_fails)(const char *path);
/* test builds only: when nonzero, find_rekordbox's list of uninstall entries can't be allocated (review NW3) */
extern int compat_test_entries_alloc_fails;
#endif

/* A rekordbox version folder an uninstall entry names. */
typedef struct {
    char folder[COMPAT_PATH_MAX];
    char folder_version[COMPAT_VERSION_MAX];    /* "7.2.19" */
    char display_version[COMPAT_VERSION_MAX];   /* the entry's, "7.2.19.0342" */
    compat_file exe;                            /* its rekordbox.exe, pinned */
    int agree;                                  /* the folder's, the entry's and the exe's versions agree */
    int ok;                                     /* a real folder, rekordbox.exe AlphaTheta's, the versions agree */
    int unknown;                                /* its rekordbox.exe couldn't be looked at, or changed while pinned */
    int candidate;                              /* the one that decides (below) */
} compat_rekordbox;
/* which rekordbox is current (owner decided, 2026-11-11; app.md §5.2) */
enum {
    COMPAT_RB_NOT_INSTALLED,   /* no named folder holds a rekordbox.exe */
    COMPAT_RB_OK,              /* the candidate is ok: *current */
    COMPAT_RB_UNVERIFIED,      /* the candidate's pin is NO or its versions disagree: "couldn't verify
                                * rekordbox X.Y.Z" (its folder_version), no current */
    COMPAT_RB_NO_ANSWER,       /* the candidate couldn't be checked (pin ERROR, unreadable): no answer */
};
/* Every version folder the uninstall entries name under root (<Program Files>\rekordbox),
 * newest first by version, at most max. The candidate is the newest folder holding a
 * rekordbox.exe (a folder without one is passed over; nothing else is): *state says what it
 * is, and *current is its index when it is ok, else -1, with no fallback to an older folder.
 * When the enumeration wasn't whole (uninstall_entries' *incomplete), the entry it missed may be
 * the newest version's: if root holds a "rekordbox X.Y.Z" folder newer than the candidate (or any,
 * with none) that no entry names and that holds a rekordbox.exe, or root couldn't be listed,
 * there is no answer (COMPAT_RB_NO_ANSWER, *current -1): an older folder never becomes current
 * on a partial walk (review RC4, C8). An unlisted newer folder leaves no candidate at all: the
 * older one doesn't decide anything (review RE3). A listed or unlisted folder that couldn't be
 * looked at (a stat error other than ENOENT/ENOTDIR) is no answer, never "no folder" (review
 * RE1). Returns how many. */
int compat_find_rekordbox(const compat_os *os, const char *root, compat_rekordbox *out, int max, int *current, int *state);

/* compat_find_rekordbox's answer kept for the warm path and every refresh after it (review HF6:
 * the registry walk off main): the same answer, from the cache while gen (the caller's
 * compat_uninstall_generation(), read just before) hasn't moved and isn't 0, root and max are the same, and every listed folder and its rekordbox.exe
 * stat as they did (the folder's kind and identity; the exe's existence, size, mtime and identity;
 * neither followed). The stamp is taken after the walk and the answer is kept only when it shows
 * what the walk saw (review RC1, RC2), and only from a whole enumeration (review RC4: a partial
 * one is walked again next call). An answer with nothing checked for sure
 * (COMPAT_RB_NO_ANSWER, an exe that couldn't be looked at, a pin ERROR) is never kept: the next
 * call walks again (C7). The answer is about the file: access lost after a walk isn't checked on a
 * hit, so whatever acts on the exe reads it FRESH (review RC3). Thread-safe: one call at a time per cache (a reader on main
 * waits for a worker's walk rather than walking too). One static cache per process:
 * COMPAT_RB_CACHE_INIT; compat_rb_cache_clear forgets it (tests). */
typedef struct {
    plat_mutex lock;
    int valid, max, n, current, state;
    uint64_t gen;
    char root[COMPAT_PATH_MAX];
    compat_rekordbox *list;   /* malloc'd, COMPAT_ENTRIES_MAX */
    struct {
        int err, dir_err;     /* plat_stat_path's answers: 0, or the errno (ENOENT: no exe) */
        plat_stat st, dir;    /* <folder>\\rekordbox.exe and <folder>, neither followed */
    } exe[COMPAT_ENTRIES_MAX];
    int walks;                /* how many times it walked (tests) */
} compat_rb_cache;
#define COMPAT_RB_CACHE_INIT {.lock = PLAT_MUTEX_INIT}
int compat_find_rekordbox_cached(compat_rb_cache *c, const compat_os *os, uint64_t gen, const char *root, compat_rekordbox *out, int max,
                                 int *current, int *state);
void compat_rb_cache_clear(compat_rb_cache *c);

/* R1's facts and Pioneer's ORT, for one rekordbox version folder */
/* OURS: a bridge whose sha256 is listed (bridges below); OURS_UNLISTED: the bridge's text with an
 * unlisted sha256 (an older build, or a file only claiming to be ours): offered, the worker's
 * restore handles it (app.md §5.5's "marker and sha") */
enum { COMPAT_LIVE_MISSING, COMPAT_LIVE_PIONEER, COMPAT_LIVE_OURS, COMPAT_LIVE_FOREIGN, COMPAT_LIVE_UNREADABLE, COMPAT_LIVE_OURS_UNLISTED };
enum { COMPAT_ORT_OK, COMPAT_ORT_NONE, COMPAT_ORT_NOT_ORIGINAL, COMPAT_ORT_UNLISTED };
typedef struct {
    int live;                /* COMPAT_LIVE_*: the live onnxruntime.dll in the version folder */
    int ort_copy;            /* our root's ort\onnxruntime.dll is there (asked only when live is ours;
                              * when it can't be looked at, there is no answer and no offer) */
    int offer_restore;       /* R1: offer to put Pioneer's file back (live missing, foreign or ours
                              * unlisted, or ours with ort\'s copy missing) */
    int ort;                 /* COMPAT_ORT_*: Pioneer's ORT (the live DLL, or with our bridge in, the
                              * saved original or ort\'s copy) pinned and listed */
    payload_sha ort_sha256;  /* that ORT's sha256, "" when none was read */
    int ort_looked;          /* ort\'s copy was looked at (added for rbsp-watch.exe): ort_copy 0
                              * then means it is missing, not that it couldn't be looked at */
} compat_r1;
/* folder: the version folder; root: <Program Files>\RB Stems Plus; bridges: the sha256s known
 * as ours (the payload's windows.bridge.sha256, the installed record's); m: the verified payload
 * for windows.compatible.ort_sha256 (NULL: none listed) */
void compat_r1_facts(const compat_os *os, const char *folder, const char *root, const payload_sha *bridges, int nbridges,
                     const payload_manifest *m, compat_r1 *out);

/* rekordbox's processes (Rekordbox.swift's rekordboxOpen, pioneerUpdating) */
typedef struct {
    int running;             /* a rekordbox.exe in any session */
    int running_here;        /* one from this version folder (or one whose path can't be read) */
    int updating;            /* its updater (Upmgr rekordbox.exe) or uninstaller (Uninstall rekordbox.exe); open:
                              * VM question (app.md:1100): the installer and NSIS's temp uninstaller
                              * (~nsu*.tmp) aren't matched yet (review C9) */
    uint16_t machine;        /* the first rekordbox.exe's machine, COMPAT_MACHINE_UNKNOWN when unknown */
} compat_procs;
/* from a process list (pure); folder may be NULL */
void compat_procs_classify(const compat_proc *list, int n, const char *folder, compat_procs *out);
/* from the OS: 0, or -1 when the process list couldn't be had (then *out says nothing runs) */
int compat_processes(const compat_os *os, const char *folder, compat_procs *out);

#endif
