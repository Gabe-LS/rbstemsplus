/* core_native_steps.c: the core DLL's native step entry (core.h "The flows' steps"): PAYLOAD,
 * CACHE_PREPARE, DOWNLOAD, REKORDBOX, SPACE and BLOCKED so far, through native_steps.c's rules with the real env (update.c over WinHTTP, the payload
 * store, sha_file, compat.c, the native admin's flow bridge). Every other kind returns
 * CORE_E_STATE, as a step entry that isn't provided does. Only in the core DLL. Unverified until
 * the VM run (W13a). */
#ifndef RBSP_WIN_CORE_NATIVE_STEPS_H
#define RBSP_WIN_CORE_NATIVE_STEPS_H

#include "core_native_admin.h"
#include "sha_memo.h"

typedef struct nw_steps nw_steps;

/* on main, inside the backend's open: what the steps need, copied (paths.support, .app, .model,
 * .rekordbox: the current rekordbox version folder; the config's version; the compiled-in keys).
 * admin: the native admin, for CACHE_PREPARE's bridge; shas: the backend's sha256 memo (its own
 * lock), for BRIDGE_SHA and STATE_LINE. NULL: no memory. */
nw_steps *nw_steps_open(core *c, nw_admin *admin, sha_memo *shas);
void nw_steps_close(nw_steps *s);

/* host->step: any core worker. 0, the step's code, or CORE_E_STATE for a kind not done yet */
int32_t nw_steps_run(nw_steps *s, core_step kind, const char *const *args, int32_t nargs, core_facts *out);

/* Any thread; the last folder a worker verified as current (current_folder: compat_find_rekordbox's
 * current one under <Program Files>\rekordbox; never the core_new snapshot paths.rekordbox), still
 * holding the same rekordbox.exe (one no-follow stat: its file id, size and last write as when
 * verified), into folder: 1, or 0 with "" (none, changed, or it doesn't fit). No registry, no hash,
 * no signature check: the prepare helper's --rekordbox at the spawn (reviews PF1, PF3). */
int nw_steps_rekordbox_known(nw_steps *s, char *folder, size_t n);

#endif
