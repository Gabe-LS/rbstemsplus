/* native.h: the "native" dispatch (API-V1-NOTES decision 22, §11; CORE_WIN_POLICY only). The
 * start config's "native" list hands host entries to the DLL's own Windows code: a backend. The
 * core then calls those entries through it instead of the shell, with the backend's own state.
 * Not part of the API: core.h's host table and its rules are unchanged, and a shell sees nothing
 * of this beyond the config key.
 *
 * Includes core.h only (no internal.h, no plat header), so a backend written against
 * common/plat.h (win/c's modules) can include it. */
#ifndef RBSP_CORE_NATIVE_H
#define RBSP_CORE_NATIVE_H

#include "core.h"

#ifdef CORE_WIN_POLICY

/* The DLL's own code for the entries "native" lists. Each entry has core_host's contract (core.h:
 * the same threads, blocking, results and return values), with the backend's state st first
 * instead of the shell's ctx. An entry the backend leaves NULL stays "not provided": the core's
 * paths that need it fail cleanly, as for a NULL host entry.
 *
 * open runs once, on main inside core_new, after the start config is read: c's paths and os are
 * set, and c is valid (core_helper_line, core_helper_ended, core_flow_progress and core_admin_note
 * take it) until close. The backend copies what it keeps: c's paths are freed by core_free, while
 * flows and runs may still call the backend after it. NULL: core_new fails ("no memory").
 * close runs once, from whichever thread drops the core's last reference (after core_free, once
 * every flow, run and job holding the core has ended). It must not wait for a job of its own. */
typedef struct native_backend {
    void *(*open)(core *c);
    void (*close)(void *st);
    void (*post_work)(void *st, core_work kind, core_job *job);
    int32_t (*slow_fact)(void *st, core_slow kind, const char *key, size_t key_len, core_facts *out);
    int32_t (*helper_spawn)(void *st, uint64_t run, const char *const *argv, int32_t argc);
    void (*helper_signal)(void *st, uint64_t run, core_helper_sig sig);
    int32_t (*admin)(void *st, int32_t kind, const char *version, const char *bundle_id, const char *bridge_sha, const char *ort_sha, int32_t force);
    int32_t (*step)(void *st, core_step kind, const char *const *args, int32_t nargs, core_facts *out);
} native_backend;

/* For the backend: the start config's path named key (core_new's "paths": "helper", "support",
 * ...), NULL when not given, and its "version" (NULL when not given); on main, inside open (the
 * backend copies them). And one line for the app's log (host->log), from any thread while the
 * backend's state is open. */
const char *native_core_path(const core *c, const char *key);
const char *native_core_version(const core *c);
/* Windows' own Program Files folder as core_new read it (no trailing '\'; never "" in the DLL,
 * whose core_new fails without it); valid while c is */
const char *native_core_programs(const core *c);
void native_core_log(core *c, const char *line);

/* For the backend's PAYLOAD "check" (manifestDue's blocked part): blockedByVersionList
 * (Compat.swift), whether a feature is off only because a version isn't listed yet, decided by the
 * core's own ordered rules over the native BLOCKED facts (plus_blocked, cache_blocked_win, then
 * feature_why_checks_again on each), so the two can't drift: rekordbox not installed, no engine
 * and a standard account come first and never count. Each part on its own: a part whose facts
 * had no answer (plus_answered, cache_answered 0) counts as 0; the result is the OR of the
 * others. With no verified payload nothing is listed (API-V1-NOTES), so an installed rekordbox
 * with an engine, or on an administrator account, counts. 1 or 0. */
typedef struct {
    int32_t installed;               /* a current rekordbox version folder was found */
    int32_t plus_answered;
    const char *engine;              /* "plus": demucs3_ver.txt's, "" none */
    int32_t engine_supported;
    int32_t cache_answered;          /* "cache"'s facts and the account's were read */
    int32_t admin;                   /* this account is an administrator */
    const char *rekordbox_version;
    int32_t rekordbox_supported, root_ort_exists, lib_exists, lib_signed, ort_listed;
} native_blocked_facts;
int32_t native_blocked_by_list(const native_blocked_facts *f);

/* the DLL's backend (win/c/core_native.c): only in the Windows DLL build that links win/c's
 * modules (CORE_NATIVE_WIN, core/build.sh dll); every other build has none, and each
 * native-listed entry then fails cleanly */
#if defined(_WIN32) && defined(CORE_NATIVE_WIN)
extern const native_backend native_win_backend;
#endif
#if defined(_WIN32)
/* core_new's read of Windows' own Program Files in the DLL (core_native.c: common/plat_win.c's
 * plat_program_files, the one rule the worker, the bridge and the watcher read): 0, or -1 with out
 * "" (unreadable or refused: core_new fails) */
int native_win_programs(char *out, size_t n);
#endif

#ifdef CORE_TEST_HOOKS
/* main, right after core_new, before {"ev":"start"}: the core's backend replaced by b (its state
 * closed, b's opened) for the entries "native" listed. CORE_E_STATE when nothing is native,
 * CORE_E_NOMEM when b->open fails (then no entry is native: each fails cleanly). */
int32_t core_test_native(core *c, const native_backend *b);
/* main, before core_new: the backend the next cores open (NULL: the build's own), and with
 * fail_after_open, a core_new that fails right after the backend's open succeeded (its state is
 * then closed once, with the core) */
void core_test_native_default(const native_backend *b, int fail_after_open);
#endif

#endif /* CORE_WIN_POLICY */
#endif
