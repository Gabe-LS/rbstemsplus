/* core_native_steps.c: the core DLL's native step entry (core.h "The flows' steps"), through
 * native_steps.c's rules with the real env (update.c over WinHTTP, the payload store, sha_file,
 * compat.c, the native admin's flow bridge) and core_native_report.c's Create Report. Which kinds
 * the shared flows reach on Windows, and which are done here, is win/c/tests/native-steps-reach.py's
 * table (winc-test): a kind not done returns CORE_E_STATE, as a step entry that isn't provided
 * does. Only in the core DLL. Unverified until the VM run (W13a). */
#ifndef RBSP_WIN_CORE_NATIVE_STEPS_H
#define RBSP_WIN_CORE_NATIVE_STEPS_H

#include "core_native_admin.h"
#include "compat.h"
#include "native_location.h"
#include "sha_memo.h"

/* core_native.c's location reads (CORE_SLOW_LOCATION's): LOCATION_NOW's probe of a chosen folder */
extern const native_location_ops nw_location_ops;
/* core_native.c's: a measure as CORE_SLOW_LOCATION's facts (FACT_LOC_*, FACT_REKORDBOX_RUNNING),
 * REPORT_FACTS' too. 0 */
int nw_location_write(const native_location *l, core_facts *out);
/* core_native.c's: the measure now, on CORE_SLOW_LOCATION's own queue and waited for at most its
 * 15 s (a sleeping or lost drive never holds a flow longer), from a worker. Shares an in-flight
 * measure for the same paths (review N3R18). 0 with *out, or -1 no answer */
int nw_location_measure_now(const char *support, const char *cache_default, uint64_t wait_ns, native_location *out);

typedef struct nw_steps nw_steps;

/* on main, inside the backend's open: what the steps need, copied (paths.support, .app, .model,
 * .rekordbox: the current rekordbox version folder; the config's version; the compiled-in keys).
 * admin: the native admin, for CACHE_PREPARE's bridge; shas: the backend's sha256 memo (its own
 * lock), for BRIDGE_SHA and STATE_LINE. NULL: no memory. */
nw_steps *nw_steps_open(core *c, nw_admin *admin, sha_memo *shas);
void nw_steps_close(nw_steps *s);

/* the current rekordbox version folder (compat_find_rekordbox's rule under Windows' own Program
 * Files), into folder (n bytes): CORE_SLOW_R1's "" key (review WR1). 1 when there is one; no
 * verified current folder: 0 (no answer), never paths.rekordbox (review CW1; native_r1_folder) */
int nw_steps_rekordbox_folder(nw_steps *s, char *folder, size_t n);
/* the same with its folder_version ("7.2.19") into version (vn bytes), as the watcher names it */
int nw_steps_rekordbox_current(nw_steps *s, char *folder, size_t n, char *version, size_t vn);
/* 1 when R1's "Don't Ask Again for This Version" is in dismissed for version ("r1:<version>", N10) */
int nw_steps_r1_dismissed(nw_steps *s, const char *version);

/* host->step: any core worker. 0, the step's code, or CORE_E_STATE for a kind not done yet */
int32_t nw_steps_run(nw_steps *s, core_step kind, const char *const *args, int32_t nargs, core_facts *out);

/* Any thread; the last folder a worker verified as current (current_folder: compat_find_rekordbox's
 * current one under <Program Files>\rekordbox; never the core_new snapshot paths.rekordbox), still
 * holding the same rekordbox.exe (one no-follow stat: its file id, size and last write as when
 * verified), into folder and its stored resampler proof bit: 1, or 0 with an empty folder (none, changed,
 * or it doesn't fit). This is the prepare helper's --rekordbox at the spawn (reviews PF1, PF3). */
int nw_steps_rekordbox_known(nw_steps *s, char *folder, size_t n, int *proved);
/* The after-render worker finished the update handoff for this payload store. */
void nw_steps_installed_changed(const char *support, const char *own);

#endif
