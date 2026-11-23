/* host_facts.c: the Windows shell's cheap facts (core.h's host->facts, facts.def; the Mac shell's
 * ShellFacts.cheap), read on main on every refresh and never memoised beyond compat.c's own file
 * memo. The WPF shell's facts entry makes one call, core_win_cheap_facts (core_win.h), which reads
 * the start config's paths and writes the sheet; the reads (host_facts_read) and the mapping
 * (host_facts_write) are here, over common/plat.h, compat.c, update.c's lists and native_facts.c's
 * BLOCKED rules, so the Mac tests run them on a scratch tree and a fake compat_os.
 *
 * CORE_FACTS_REFRESH's rows Windows has a reader for (see host_facts_read), CORE_FACTS_PREPARE's
 * (with e->prepare: host_facts_read's prepare rows) and CORE_FACTS_KEPT's (host_facts_kept_sets).
 * FACT_APPLE_SILICON is "this machine runs the prepare helper and its model": on Windows always 1,
 * written with refresh's rows. The helper is x64 (helper.md finding 5) and runs Pioneer's model on
 * the CPU through Pioneer's own x64 onnxruntime.dll, natively on x64 and under the x64 emulation
 * every ARM64 Windows from the app's minimum (Windows 11 22H2, app.md §5.9) has, so no supported
 * Windows lacks it (pure.h's prep_facts note: T_PREP_NEEDS_ARM, Mac-only, is never composed on
 * Windows). Not answered yet (left unset, read as false or "" by the core): the location snapshot's
 * (FACT_LOC_*: CORE_SLOW_LOCATION's), FACT_CACHE_IS_LINK, FACT_PROTOTYPE_EXISTS and FACT_ORT_PREFIX
 * (Windows uses FACT_ORT_LISTED).
 * FACT_ENGINE, FACT_ENGINE_SUPPORTED, FACT_STEMS_PLUS_PRESENT, FACT_MODEL_SIZE and FACT_MODEL_MTIME
 * read paths.model (the shell's CoreConfig: rekordbox's hdemucs.onnx under roaming AppData); with
 * none they stay empty (not installed, no engine). */
#ifndef RBSP_WIN_HOST_FACTS_H
#define RBSP_WIN_HOST_FACTS_H

#include "compat.h"
#include "native_facts.h"
#include "sig.h"

#include <stdint.h>

typedef struct {
    const compat_os *os;
    const char *programs_rekordbox;   /* <Program Files>\rekordbox: where version folders are found */
    const char *root;                 /* <Program Files>\RB Stems Plus */
    const char *model;                /* paths.model: rekordbox's hdemucs.onnx (NULL: none) */
    const char *support;              /* paths.support: %LOCALAPPDATA%\rbstemsplus (NULL: none) */
    const char *helper;               /* paths.helper (NULL: none) */
    const char *rb_settings;          /* rekordbox's rekordbox3.settings (stemsQuality's file: the Mac's
                                       * rbSettingsDir: the folder holding models\demucs3_model\ under
                                       * paths.model; its Windows place is a W13b row; NULL: none) */
    const payload_manifest *scope_manifest; /* the flow's verified payload, when scoping a swap */
    int fresh_inventory;              /* recheck the EXE pin for a confirmed scope */
    int prepare;                      /* read CORE_FACTS_PREPARE's rows too (the prepare rows below) */
    const char *own;                  /* this app's version (NULL: none) */
    const sig_pubkey *keys;           /* the compiled-in keys the saved lists are checked with */
    size_t nkeys;
    int admin;                        /* native_user_is_admin(): 1, 0, -1 no answer */
    int own_bundle;                   /* update_in_place() == 0: 1, 0; -1 no answer */
    /* the UI thread's read (host->facts): compat_main_reads for its duration, miss(ctx, path) for
     * each file the warm didn't leave memoised; 0: a worker's (the warm) */
    int on_main;
    void (*miss)(void *ctx, const char *path);
    void *miss_ctx;
    /* the boot-time renames and deletes scheduled now (MOVEFILE_DELAY_UNTIL_REBOOT: the Session
     * Manager's PendingFileRenameOperations, then PendingFileRenameOperations2), read, never
     * written: every pair in queue order, as hf_pending_parse's tagged entries ('D' a delete's
     * path, 'R' a rename's new path), in *out (malloc'd, *n bytes; the caller frees it). 0, or
     * nonzero: no answer (the root then counts as it is: fail closed). NULL: none read (the same).
     * A rename counts as into the root when any part of its new path is the root's name or a short
     * name made from it, however the folders above are spelled; an alias of the root folder itself
     * (a junction, a mount point or a subst drive aimed at it, or a short name set by hand) isn't
     * seen */
    int (*pending_deletes)(void *ctx, char **out, size_t *n);
    void *pending_ctx;
} host_facts_env;

enum { HF_CONFIG_THIS = 0, HF_CONFIG_CHOSEN = 1, HF_CONFIG_WRONG = 2 };

typedef struct {
    char folder[COMPAT_PATH_MAX];
    char product_version[COMPAT_VERSION_MAX];
    payload_sha sha256;
    plat_id id;
    int64_t size, mtime;
} host_covered_exe;

typedef struct {
    /* the account and the app */
    int has_admin, admin;                  /* FACT_USER_IS_ADMIN */
    int has_own_bundle, own_bundle;        /* FACT_OWN_BUNDLE */
    int chosen_model, chosen_cache;        /* FACT_CHOSEN_MODEL, FACT_CHOSEN_CACHE: support\installed */
    /* rekordbox */
    int installed;                         /* FACT_REKORDBOX_INSTALLED: a current version folder */
    char rekordbox_folder[COMPAT_PATH_MAX]; /* its verified folder, for the watcher's helper */
    char version[COMPAT_VERSION_MAX];      /* FACT_REKORDBOX_VERSION: its uninstall entry's, else the folder's; "" none */
    char engine[64];                       /* FACT_ENGINE: demucs3_ver.txt beside the model, "" none */
    int engine_supported;                  /* FACT_ENGINE_SUPPORTED */
    int plus_present;                      /* FACT_STEMS_PLUS_PRESENT: the model is Demucs v4's (the watcher's size rule) */
    int cache_present;                     /* FACT_STEMS_CACHE_PRESENT: the live onnxruntime.dll is our bridge */
    int cache_record_ok;                   /* FACT_CACHE_RECORD_OK: every eligible version is On */
    int has_bridge_models;                 /* FACT_BRIDGE_MODELS is set: Stems Cache in, Demucs v4 not, and the live */
    char bridge_models[COMPAT_CACHE_LINE_MAX + 1];   /* bridge has its cache line (bridgeCacheModels): its pioneer= */
                                           /* sha256s, one a line; "" for an entry-only bridge (no pioneer=) */
    int cache_traces;                      /* FACT_CACHE_TRACES: Stems Cache in, or anything at <Program Files>\RB Stems Plus
                                            * (the item itself, a link not followed; a stat that fails otherwise
                                            * than "nothing there" counts: fail closed), except a root folder
                                            * that holds only boot-time deletes (root_pending) */
    int root_pending;                      /* the root folder is there, but it and every name in it is a
                                            * boot-time delete (pending_deletes): the uninstall worker's
                                            * reboot branch (I20, J7), "removed at the next restart" */
    int cache_answered;                    /* native_blocked_cache answered: the rows below are set */
    native_blocked blocked;                /* FACT_REKORDBOX_SUPPORTED, FACT_ROOT_ORT_EXISTS, FACT_LIB_*, FACT_ORT_* */
    int has_model;                         /* rekordbox's model file is there: the two below are set */
    int64_t model_size, model_mtime_ns;    /* FACT_MODEL_SIZE, FACT_MODEL_MTIME (whole seconds, in ns) */
    int helper_there;                      /* FACT_HELPER_THERE */
    /* CORE_FACTS_PREPARE's (prepareFacts' own reads), only with e->prepare */
    int has_prepare;                       /* read: the three rows below are written */
    int bridge_keeps;                      /* FACT_BRIDGE_KEEPS: every covered bridge has kept=1 */
    int stems_quality;                     /* FACT_STEMS_QUALITY: stemsQuality(), -1 unknown */
    char saved_model[65];                  /* FACT_OUR_MODELS' second line: the verified saved payload's model
                                            * sha256 (savedManifest()'s), "" none */
    int has_ort_there, ort_there;          /* FACT_ROOT_ORT_EXISTS where BLOCKED "cache" didn't write it (rekordbox
                                            * not listed): prepareFacts' ortThere, root\ort\onnxruntime.dll a file */
    /* The inventory and its assessment are owned by this read. */
    inv_list *inventory;
    inv_assess_row *assessment;
    payload_manifest *cache_manifest;
    char win_rows[INV_MAX * (COMPAT_VERSION_MAX * 2 + 32)];
    char cache_states[INV_STATE_TEXT_MAX];   /* all displayed rows, for app.log */
    char open_versions[INV_MAX * COMPAT_VERSION_MAX + 1];
    int r1_state, r1_dismissed;
    int ncovered, bridge_location;
    host_covered_exe covered[INV_MAX];
    /* config.ini (readConfigState) */
    char *config_text;                     /* FACT_CONFIG_TEXT: its bytes (malloc'd), NULL no file or unreadable */
    size_t config_n;
    char config_unreadable[128];           /* FACT_CONFIG_UNREADABLE: why, "" readable or no file */
    int config_enabled, config_rb_model;   /* FACT_CONFIG_ENABLED, FACT_CONFIG_RB_MODEL */
    double config_max_gb, config_max_days; /* FACT_CONFIG_MAX_GB, FACT_CONFIG_MAX_DAYS (configNumber's input) */
    int config_location;                   /* FACT_CONFIG_LOCATION: HF_CONFIG_* */
    char config_dir[1100];                 /* FACT_CONFIG_DIR and FACT_CHOSEN_LOCATION: the chosen folder, "" none */
    char config_id[37];                    /* ... its marker's id (cache_id), "" none */
} host_facts;

/* CORE_FACTS_REFRESH's rows read now into out (zeroed first; host_facts_free after). 0, or -1
 * out of memory (out as far as it got). */
int host_facts_read(const host_facts_env *e, host_facts *out);
void host_facts_free(host_facts *f);
/* config.ini's rows alone (readConfigState: the config_* fields; the rest zero), from support's
 * config.ini (support NULL: none, the defaults); host_facts_free after */
void host_facts_config(const char *support, host_facts *out);
/* The core DLL's: host_facts_read with the start config's paths of c (core_win_cheap_facts's env),
 * on a worker (Create Report's REPORT_FACTS). 0, or -1 (no memory) */
struct core;
int host_facts_read_core(struct core *c, host_facts *out);
int host_facts_read_scope(struct core *c, const payload_manifest *manifest, host_facts *out);
int host_facts_inventory(const compat_os *os, const char *root, inv_list *out);
void nw_steps_refresh_inventory(struct core *c);
struct nw_steps;
int nw_steps_cache_covers_prepare(struct nw_steps *s);
#ifdef HOST_FACTS_TEST_HOOKS
extern int host_facts_test_env_alloc_fails;
#endif

/* The detached location measure has the installation root but no payload context.
 * A readable bridge in a verified version counts as present and every such
 * bridge must advertise location support. An unchecked inventory or DLL is -1. */
int host_facts_live_bridge(const compat_os *os, const char *programs_rekordbox, int *present, int *location);

/* one PendingFileRenameOperations(2) value (units UTF-16 units: the names one after another, each
 * NUL-ended, in pairs (old name, new name); a lone trailing terminator is ignored) appended to *buf
 * (*len bytes used of *cap, grown with realloc; the caller frees it), one entry per pair in queue
 * order, each NUL-ended and starting with a tag byte:
 *  - 'D' + the old name's DOS path, for a delete (an empty new name; a pair with both names empty
 *    is nothing);
 *  - 'R' + the new name's DOS path, for a rename (a leading '!', MOVEFILE_REPLACE_EXISTING's,
 *    taken off first).
 * Windows 11's '*' and digits before the NT prefix ("*1\??\C:\...", only with the "\??\" after
 * it), then "\??\", are taken off each path. A delete's name that isn't UTF-16 is left out
 * (matches nothing: traces); a rename's is kept, an unpaired surrogate as U+FFFD (it still names
 * its folder: traces). 0, or -1 out of memory (*buf as far as it got). Pure: the Mac tests run it */
int hf_pending_parse(const uint16_t *w, size_t units, char **buf, size_t *len, size_t *cap);

/* stemsQuality() over a settings file's bytes: the first <VALUE .../> named PartAnalysisQuality,
 * with val in either attribute order. Returns its leading decimal digits, or -1 if unknown. */
int hf_stems_quality(const char *b, size_t n);
/* bridgeSaysKept()'s rule on the cache line itself (compat_file's cache_line): its tokens before
 * "pioneer=", split at spaces, include "kept=1". Pure */
int hf_line_says_kept(const char *line, size_t n);

/* The capabilities common to the covered bridges' cache lines. */
void hf_covered_capabilities(const native_inv_caps *const *covered, int n, int *keeps, int *location);

/* keptSets() as FACT_KEPT_SETS (ShellFacts.keptSetsText): the kept sets in the folder kept whose
 * date hasn't passed (now, seconds since 1970), by name, one a line: the name, a tab, its until
 * date as date(ctx, seconds) writes it ("" when the file isn't the bridge's format), a tab, the
 * "file " lines of its manifest. A set is <name>.kept, a regular file (not followed) whose name
 * passes keptNameOK (1-64 of A-Z a-z 0-9 - _), read whole up to 8 MiB and validated as in the
 * helper's kept.c. Its manifest <name>.manifest is read in blocks (unreadable: 0 files).
 * No folder, or one that can't be listed: no sets. *out malloc'd, NUL-ended ("" none;
 * the caller frees it). 0, or -1 out of memory */
int host_facts_kept_sets(const char *kept, double now, void (*date)(void *ctx, double t, char *out, size_t n), void *ctx, char **out);
#ifdef _WIN32
void host_facts_kept_date(void *ctx, double t, char *out, size_t n);
#endif

/* f's rows into the core's sheet (core_fact_*): the DLL's export, and the Mac tests' fake sheet.
 * 1 when Pioneer's ORT wasn't checked for a supported rekordbox (the DLL's sheet then carries
 * FACT_LIB_UNCHECKED, and the shell warms again on activation), else 0 */
struct core_facts;
int host_facts_write(const host_facts *f, struct core_facts *out);

#endif
