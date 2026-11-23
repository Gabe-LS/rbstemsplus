/* models.c: rekordbox's stems model and Pioneer's saved originals on Windows (app.md §5.3), the
 * port of Models.swift: which models are ours (by sha256, from signed facts only), saving
 * Pioneer's model before ours replaces it, choosing and putting back the right one, installing
 * ours, and what is left behind. C11 over common/plat.h, so the Mac tests run it in a scratch
 * folder.
 *
 * The folders: rekordbox's model folder (%APPDATA%\Pioneer\rekordbox6\models\demucs3_model\:
 * hdemucs.onnx, demucs3_ver.txt) and the support folder's originals\ (<sha256>.onnx, each with
 * <sha256>.engine, the STEMS Engine version it came with). Each is opened once by its path without
 * following a link there, and every name in it is used through that handle, never followed
 * (a link where a model is expected is "can't be read"). Nothing is deleted but the temporary
 * files this code makes (.hdemucs.new, .hdemucs.restore and <sha>.onnx.part, each with a per-run
 * .<pid>.<ns> suffix, and those names left by stopped runs) and a damaged saved
 * copy it set aside itself (<sha>.onnx.damaged); rekordbox's model is replaced only by a rename
 * over it, after Pioneer's is saved and verified (or it is ours), and only if it hasn't changed
 * since it was hashed. Every copy is flushed to the disk and checked before its rename. */
#ifndef RBSP_WIN_MODELS_H
#define RBSP_WIN_MODELS_H

#include "shell.h"   /* critical_work (review MC3) */

#include <stdint.h>

/* the Demucs v4 model every release has installed, compiled in (Paths.swift's ourModel; run.sh
 * checks they agree) */
#define MODELS_OURS_BUILTIN "0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772"
enum { MODELS_OURS_MAX = 80 };

/* core.h's core_model_outcome (core_native_steps.c asserts they agree); MODELS_NO_CODE: a failure
 * Swift's modelOutcomeCode has no code for */
enum {
    MODELS_OK = 0, MODELS_UNKNOWN = 1, MODELS_UNREADABLE = 2, MODELS_NOT_A_FILE = 3, MODELS_NO_SPACE_SAVE = 4,
    MODELS_NOT_SAVED = 5, MODELS_NO_SPACE_COPY = 6, MODELS_NOT_COPIED_WHY = 7, MODELS_COPY_BAD = 8, MODELS_CHANGED = 9,
    MODELS_NOT_PLACED_WHY = 10, MODELS_NO_SAVED_COPY = 11, MODELS_NO_SPACE_BACK = 12, MODELS_NOT_BACK = 13,
    MODELS_NOT_BACK_WHY = 14, MODELS_NOT_VERIFIED = 15, MODELS_SAVED_DAMAGED = 16, MODELS_NO_CODE = -1,
    /* Windows-only, no core code: CriticalWork's admission is closed (the app is quitting), so the
     * copy or restore wasn't begun and rekordbox's model is as it was (review MC3) */
    MODELS_STOPPED = -2
};

typedef struct {
    const char *dir;         /* rekordbox's model folder */
    const char *originals;   /* support\originals */
    char ours[MODELS_OURS_MAX][65];   /* ourModelChecksums: the built-in one, the verified payload's model and previous_models */
    int nours;
    void (*log)(void *ctx, const char *line);
    void (*will_replace)(void *ctx);  /* right before rekordbox's model is hashed again and replaced (the tests'); NULL */
    void *ctx;
    void (*will_save)(void *ctx);     /* the tests': between Pioneer's model's hash and its copy (NULL) */
    void (*after_rename)(void *ctx);  /* the tests': right after restore's rename, before its check (NULL) */
    /* the support folder: originals\ is opened through it, neither followed if a link (review
     * MD5, S31's rule: a linked support or originals is refused, nothing read or written there);
     * `originals` above names the same folder, for the log and the tests only */
    const char *support;
    /* CriticalWork for the copy through the rename (install, restore): quitting waits for it, and
     * once its admission is closed neither begins (MODELS_STOPPED). The save and the prompts stay
     * outside, as Models.swift's critical.run (review MC3/MC4). NULL: the process's one
     * (critical_process) */
    critical_work *critical;
} models;

#ifdef MODELS_TEST_HOOKS
/* test builds only: asked before each look at a name in rekordbox's model folder (stat); a nonzero
 * errno is that look's answer */
extern int (*models_test_stat_fault)(const char *name);
/* asked after each listing of originals\ (call: its number, from 1): nonzero makes it a listing
 * that couldn't be read whole; asked before each open of originals\ (call: its number): a nonzero
 * errno is that open's answer. The counters count every listing and open (review VQ5, VQ2) */
extern int (*models_test_each_fault)(int call);
extern int (*models_test_originals_fault)(int call);
/* asked before restore removes a candidate's copy that didn't verify (its name): nonzero leaves it
 * there, as an unlink that failed would (review WF5); called just before a saved copy is renamed in
 * (review WF7); asked before install's final rename: a nonzero errno is that rename's answer (WF11) */
extern int (*models_test_unlink_fault)(const char *name);
extern void (*models_test_before_place)(void);
extern int (*models_test_rename_fault)(void);
/* asked before the save writes a saved model's <sha>.engine: a nonzero errno is that write's
 * answer (review b-a2a-F1) */
extern int (*models_test_engine_fault)(void);
extern int models_test_each_calls, models_test_originals_calls;
#endif

/* whether s is 64 lowercase hex digits */
int models_is_sha256(const char *s);
/* adds sha to m's ours (a sha256; once; MODELS_OURS_MAX at most) */
void models_add_ours(models *m, const char *sha);

/* MODEL_COPY (installOurs): our model, the file source_dir\source_name with this sha256, in
 * rekordbox's place. MODELS_OK, or the outcome (why: the OS error for the *_WHY ones) */
int models_install_ours(const models *m, const char *source_dir, const char *source_name, const char *sha, char why[128]);
/* MODEL_RESTORE (restore): Pioneer's model back over ours, or where rekordbox's model is missing
 * (its folder there). MODELS_OK, or the outcome */
int models_restore(const models *m, char why[128]);

/* MODELS_INFO: savedOriginals non-empty (no hashing); verifiedOriginals non-empty, a saved copy
 * that can't be read, or isn't a regular file, counting as one (fails closed: kept by "Remove
 * Anyway", review J1, VU2); what
 * modelLeftBehind finds (0 no model, 1 unreadable, 2 Demucs v4, 3 one RB Stems Plus doesn't know) */
int models_any_saved(const models *m);
/* savedOriginals' checksums, best first, into shas (at most max): the count */
int models_saved_list(const models *m, char (*shas)[65], int max);
int models_any_verified(const models *m);
/* both from one look at originals\ (review MP1): what models_any_saved and models_any_verified
 * answer; verified NULL: saved alone, nothing hashed */
void models_originals_info(const models *m, int *saved, int *verified);
int models_left_behind(const models *m);

/* REMOVE_LEFTOVERS (removeLeftovers' model part; the caller checks appBusy): the temporary copies
 * a stopped run left, .hdemucs.new and .hdemucs.restore in rekordbox's model folder and
 * <sha256>.onnx.part in originals\, each only when it is a regular file (never a link or a
 * folder of that name), each logged "leftover <name> removed: ok|FAILED". The count removed */
int models_remove_leftovers(const models *m);

#endif
