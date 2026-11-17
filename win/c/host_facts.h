/* host_facts.c: the Windows shell's cheap facts (core.h's host->facts, facts.def; the Mac shell's
 * ShellFacts.cheap), read on main on every refresh and never memoised beyond compat.c's own file
 * memo. The WPF shell's facts entry makes one call, core_win_cheap_facts (core_win.h), which reads
 * the start config's paths and writes the sheet; the reads (host_facts_read) and the mapping
 * (host_facts_write) are here, over common/plat.h, compat.c, update.c's lists and native_facts.c's
 * BLOCKED rules, so the Mac tests run them on a scratch tree and a fake compat_os.
 *
 * CORE_FACTS_REFRESH only, the rows Windows has a reader for (see host_facts_read). Not answered
 * yet (left unset, read as false or "" by the core): CORE_FACTS_PREPARE's and CORE_FACTS_KEPT's
 * rows (the Prepare and Status tabs are hidden in the first release), the location snapshot's
 * (FACT_LOC_*: CORE_SLOW_LOCATION's), FACT_CACHE_IS_LINK,
 * FACT_PROTOTYPE_EXISTS, FACT_ORT_PREFIX (Windows uses FACT_ORT_LISTED) and FACT_APPLE_SILICON.
 * FACT_ENGINE, FACT_ENGINE_SUPPORTED, FACT_STEMS_PLUS_PRESENT, FACT_MODEL_SIZE and FACT_MODEL_MTIME
 * read paths.model, which the shell's start config doesn't give yet: they stay empty (not
 * installed, no engine) until the models.c lane supplies "model". */
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
} host_facts_env;

enum { HF_CONFIG_THIS = 0, HF_CONFIG_CHOSEN = 1, HF_CONFIG_WRONG = 2 };

typedef struct {
    /* the account and the app */
    int has_admin, admin;                  /* FACT_USER_IS_ADMIN */
    int has_own_bundle, own_bundle;        /* FACT_OWN_BUNDLE */
    int chosen_model, chosen_cache;        /* FACT_CHOSEN_MODEL, FACT_CHOSEN_CACHE: support\installed */
    /* rekordbox */
    int installed;                         /* FACT_REKORDBOX_INSTALLED: a current version folder */
    char version[COMPAT_VERSION_MAX];      /* FACT_REKORDBOX_VERSION: its uninstall entry's, else the folder's; "" none */
    char engine[64];                       /* FACT_ENGINE: demucs3_ver.txt beside the model, "" none */
    int engine_supported;                  /* FACT_ENGINE_SUPPORTED */
    int plus_present;                      /* FACT_STEMS_PLUS_PRESENT: the model is Demucs v4's (the watcher's size rule) */
    int cache_present;                     /* FACT_STEMS_CACHE_PRESENT: the live onnxruntime.dll is our bridge */
    int has_bridge_models;                 /* FACT_BRIDGE_MODELS is set: Stems Cache in, Demucs v4 not, and the live */
    char bridge_models[COMPAT_CACHE_LINE_MAX + 1];   /* bridge has its cache line (bridgeCacheModels): its pioneer= */
                                           /* sha256s, one a line; "" for an entry-only bridge (no pioneer=) */
    int cache_traces;                      /* FACT_CACHE_TRACES: Stems Cache in, or anything at <Program Files>\RB Stems Plus
                                            * (the item itself, a link not followed; a stat that fails otherwise
                                            * than "nothing there" counts: fail closed) */
    int cache_answered;                    /* native_blocked_cache answered: the rows below are set */
    native_blocked blocked;                /* FACT_REKORDBOX_SUPPORTED, FACT_ROOT_ORT_EXISTS, FACT_LIB_*, FACT_ORT_* */
    int has_model;                         /* rekordbox's model file is there: the two below are set */
    int64_t model_size, model_mtime_ns;    /* FACT_MODEL_SIZE, FACT_MODEL_MTIME (whole seconds, in ns) */
    int helper_there;                      /* FACT_HELPER_THERE */
    /* config.ini (readConfigState) */
    char *config_text;                     /* FACT_CONFIG_TEXT: its bytes (malloc'd), NULL no file or unreadable */
    size_t config_n;
    char config_unreadable[128];           /* FACT_CONFIG_UNREADABLE: why, "" readable or no file */
    int config_enabled, config_rb_model;   /* FACT_CONFIG_ENABLED, FACT_CONFIG_RB_MODEL */
    double config_max_gb, config_max_days; /* FACT_CONFIG_MAX_GB, FACT_CONFIG_MAX_DAYS (configNumber's input) */
    int config_location;                   /* FACT_CONFIG_LOCATION: HF_CONFIG_* */
    char config_dir[1100];                 /* FACT_CONFIG_DIR and FACT_CHOSEN_LOCATION: the chosen folder, "" none */
} host_facts;

/* CORE_FACTS_REFRESH's rows read now into out (zeroed first; host_facts_free after). 0, or -1
 * out of memory (out as far as it got). */
int host_facts_read(const host_facts_env *e, host_facts *out);
void host_facts_free(host_facts *f);

/* stemsCachePresent() and bridgeSaysLocation() for CORE_SLOW_LOCATION (native_location.c), from
 * the read FACT_STEMS_CACHE_PRESENT's comes from: the current rekordbox (compat_find_rekordbox
 * under programs_rekordbox) and its live onnxruntime.dll (compat_file_facts, memoised: no second
 * scan of a file host_facts_read or the warm has read). 0 with *present (the DLL holds the bridge's
 * text, "rbstems bridge: ") and *location (its cache line says location=1, native_location's rule):
 * both 0 with no current rekordbox (none, or one that couldn't be verified: Stems Cache isn't in,
 * as FACT_STEMS_CACHE_PRESENT says), no live DLL, or something else in its place (a link, a
 * folder). -1, no answer (fail closed): no programs_rekordbox, the rekordbox walk had no answer
 * (COMPAT_RB_NO_ANSWER), the DLL couldn't be looked at or read, or out of memory. A worker's call:
 * a miss reads the DLL (17 MB) and pins rekordbox.exe. */
int host_facts_live_bridge(const compat_os *os, const char *programs_rekordbox, int *present, int *location);

/* f's rows into the core's sheet (core_fact_*): the DLL's export, and the Mac tests' fake sheet.
 * 1 when Pioneer's ORT wasn't checked for a supported rekordbox (the DLL's sheet then carries
 * FACT_LIB_UNCHECKED, and the shell warms again on activation), else 0 */
struct core_facts;
int host_facts_write(const host_facts *f, struct core_facts *out);

#endif
