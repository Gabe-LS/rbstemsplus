/* core_native_admin.c: the core DLL's native admin entry (API-V1-NOTES §11, core.h's admin
 * comment, app.md §5.5-5.6): one call is one admin.c client run (one elevation, or two for the
 * install-worker case, I75), with native_admin.c's rules. Only in the core DLL (core/build.sh dll).
 * Unverified until the VM run (W13a). */
#ifndef RBSP_WIN_CORE_NATIVE_ADMIN_H
#define RBSP_WIN_CORE_NATIVE_ADMIN_H

#include "admin.h"
#include "native.h"
#include "sig.h"

typedef struct nw_admin nw_admin;

/* on main, inside the backend's open: what admin needs, copied (paths.support, paths.app, the
 * config's version, the main thread for UAC's owner window, the compiled-in keys). NULL: no
 * memory. */
nw_admin *nw_admin_open(core *c);
void nw_admin_close(nw_admin *a);

/* the flow's checked bridge (CACHE_PREPARE's): the install copies exactly this file into the
 * source folder. path NULL: none. 0, or ENOMEM. Any worker thread. */
int nw_admin_set_bridge(nw_admin *a, const char *path, const char *name);

/* host->admin (core.h): kind (core_admin_kind); version: on Windows the scope token FACT_WIN_SCOPE
 * (multi-version rekordbox), passed as --targets for install and restore (native_admin_targets:
 * an invalid one isn't run, 4), "*" for the other verbs; the bridge's sha256 as the flow checked it
 * (NULL: not checked here); force. --manifest is the copied payload.json's sha256. Flow thread
 * only; blocks for the run. */
int32_t nw_admin_run(nw_admin *a, int32_t kind, const char *version, const char *bridge_sha, int32_t force);

/* CORE_STEP_WIN_ADMIN_TARGETS (multi-version contract, mv-spec §2.4): the last run's @target
 * records, copied into out (at most max, in the order written), and *root_kept its @root-kept.
 * How many, or -1 when no run has ended in this process. Any worker thread. Each nw_admin_run
 * clears them first, so a call that didn't run the worker leaves none. */
int nw_admin_last_targets(nw_admin *a, admin_target_result *out, int max, int *root_kept);

/* CORE_SLOW_R1 for the rekordbox version folder `folder` (native_facts.c's native_r1): our
 * bridges' list from the app's verified saved payload (support\payload, its floor) and verified
 * installed-release record, with the compiled-in keys; our root the worker's. FACT_R1_STATE's
 * value, or -1 (no answer). Any worker thread. */
int nw_admin_r1(nw_admin *a, const char *folder);

/* whether this account is an administrator (FACT_USER_IS_ADMIN's rule; the Mac's userIsAdmin):
 * CheckTokenMembership against BUILTIN\Administrators, on the linked (full) token when this
 * process's is UAC's limited one, else on this one. 1, 0, or -1 when it couldn't be told. Any
 * thread. The shell's FACT_USER_IS_ADMIN uses this same rule. */
int native_user_is_admin(void);

/* core_native.c's: a file's sha256, hex: 0, or nonzero */
int nw_file_sha256(const char *path, char out[65]);

/* the compiled-in keys payload.json is checked with, written by core/build.sh dll from
 * keys/release.pub.pem and keys/backup.pub.pem (as build-app.sh writes Keys.swift) */
extern const sig_key_text native_key_texts[];
extern const int native_n_key_texts;

#endif
