/* native_facts.c: the core DLL's slow facts beside the sha256s (core.h's core_slow; the Mac
 * shell's CoreShell.slowFact), over common/plat.h, volumes.c and compat.c, so the Mac tests run
 * them with a fake compat_os.
 *
 *  - CORE_SLOW_DRIVE (key: the location): FACT_FREE_BYTES, what this account may still write on
 *    its volume (GetDiskFreeSpaceEx's caller bytes: quotas respected). A location that isn't
 *    there, or whose volume can't be asked, couldn't be measured. FACT_DRIVE_STATE stays unset,
 *    as on the Mac (its values are M4's seam). A network location (no volume GUID) couldn't be
 *    measured; the stems folder is never one (vol_refusal).
 *  - CORE_SLOW_SIZE_WALK (key: the cache root): FACT_SIZE_BYTES, entriesSize (vol_entries_size:
 *    0 when the folder isn't there; a folder that can't be read couldn't be measured).
 *  - CORE_SLOW_ALPHATHETA (key: a file): FACT_SIGNATURE_OK, the AlphaTheta pin (compat.c, the
 *    memoised read: core.h's "Memoised"; whatever acts on a verdict reads it again, as the
 *    worker does). Nothing there, or not a regular file, is a verdict: not signed. No answer
 *    from the pin (wintrust didn't load, the file couldn't be read or changed while pinned)
 *    couldn't be measured. */
#ifndef RBSP_WIN_NATIVE_FACTS_H
#define RBSP_WIN_NATIVE_FACTS_H

#include "compat.h"
#include "inventory.h"

#include <stdint.h>

/* core.h's core_slow values */
enum { NF_SLOW_DRIVE = 2, NF_SLOW_SIZE_WALK = 3, NF_SLOW_ALPHATHETA = 4 };

typedef struct {
    int has_free, has_size, has_signature;   /* which facts the answer sets */
    int64_t free_bytes, size_bytes;
    int signature_ok;
} native_fact;

/* one slow fact of kind for key: 0 with *out, or nonzero (couldn't be measured; an unknown kind) */
int native_fact_read(const compat_os *os, int kind, const char *key, native_fact *out);

/* CORE_SLOW_R1's FACT_R1_STATE (facts.def) from compat_r1_facts' answer: 0 nothing to put back,
 * 1 the live onnxruntime.dll is missing, 2 it is neither Pioneer's nor ours (a foreign file, or
 * the bridge's text with an unlisted sha256), 3 ours with ort\'s copy missing; -1 no answer (the
 * live DLL unreadable, or ours with ort\'s copy not looked at), never an offer. Without a verified
 * list (have_lists 0) an unlisted bridge is judged as ours, as watch.c's R1 rule. The answer
 * itself needs the verified payloads' bridge lists, so the wiring waits for the compiled-in keys. */
enum { NF_R1_NOTHING = 0, NF_R1_LIVE_MISSING = 1, NF_R1_FOREIGN = 2, NF_R1_ORT_MISSING = 3 };
int native_r1_state(const compat_r1 *r, int have_lists);

/* CORE_SLOW_R1 for the version folder `folder`: compat_r1_facts against our bridges' list, the
 * verified saved payload's and the verified installed-release record's windows.bridge.sha256
 * (either may be NULL; watch.c's rule), its ORT list from the saved payload, else the record;
 * then native_r1_state with have_lists when either verified. root: our root (<Program
 * Files>\RB Stems Plus, from Windows' own Program Files: plat_program_files, the rule the watcher
 * reads too, so R1 here and the watcher's agree on whichever drive). FACT_R1_STATE's value, or
 * -1 (no answer). */
int native_r1(const compat_os *os, const char *folder, const char *root, const payload_manifest *saved, const payload_manifest *installed);

/* CORE_SLOW_R1 as the core DLL reads it (core_native_admin.c's nw_admin_r1, review R14): the
 * lists from support's saved payload (support\payload, verified with keys under support's floor
 * and the app's own minimum) and from its installed-release record (update_installed: verified,
 * naming this app's version own, or its prev\), each counted only with its windows block; then
 * native_r1. FACT_R1_STATE's value, or -1 (no answer; no support or own version). */
int native_r1_read(const compat_os *os, const char *support, const char *own, const sig_pubkey *keys, size_t nkeys, const char *folder,
                   const char *root);

/* Read and memoise an ORT's code digest through the same file handle as its
 * ordinary facts. The caller supplies the PE digest implementation. */
int compat_ort_file_facts(const compat_os *os, const char *path, int flags,
                         int (*digest)(compat_read_fn, void *, uint64_t, payload_sha), compat_file *out);

/* App-side observations for each inventory row, sharing the worker's scope inputs. */
typedef struct {
    int bridge, trace, original_signed;
    char ort_version[COMPAT_VERSION_MAX];
    int live_line;
    char cache_line[COMPAT_CACHE_LINE_MAX];
    size_t cache_line_len;
} native_inv_caps;
void native_inventory_assess(const compat_os *os, const inv_list *list, const char *root, const payload_manifest *m,
                             const payload_manifest *installed, inv_assess_row *out, native_inv_caps *caps);

void native_inventory_running(const compat_os *os, const inv_list *list, inv_assess_row *out, char *versions, size_t cap);

/* The repair state for an explicitly requested folder in this inventory. */
int native_r1_assessed(const inv_list *list, const inv_assess_row *rows, const char *folder);

/* -- BLOCKED (core.h's step; Compat.swift's stemsPlusBlocked and stemsCacheBlocked, their facts
 * only: the core decides, cache_blocked_win on Windows). The compatible list is the verified
 * saved payload's windows.compatible, else the verified installed-release record's; Windows has
 * no built-in list, so with neither nothing is listed ("not supported yet, checks again"). */

/* Compat.swift's listed(): v is an entry, or starts with an entry and a '.' ("7.2.19.0342" by
 * "7.2.19") */
/* engineSupported's contains: v is exactly an entry (the STEMS Engine; watch.c's listed_engine) */
int native_listed_exact(const char *v, const char (*list)[PAYLOAD_NAME_MAX], int n);
/* "plus": engineVersion(), demucs3_ver.txt beside rekordbox's model file (model), trimmed ("" none),
 * into engine (n bytes); *supported: m (NULL: no list) lists it exactly (native_listed_exact) */
void native_blocked_plus(const char *model, const payload_manifest *m, char *engine, size_t n, int *supported);

typedef struct {
    int rekordbox_supported;   /* FACT_REKORDBOX_SUPPORTED; when 0 nothing below is set (the Mac stops there) */
    int root_ort_exists;       /* FACT_ROOT_ORT_EXISTS: our root's ort\onnxruntime.dll */
    int has_root_ort;          /* root_ort_exists was read (a supported rekordbox's folder and the root known) */
    int lib_exists, lib_signed;/* FACT_LIB_EXISTS, FACT_LIB_SIGNED: Pioneer's ORT (the live DLL, or ort\'s copy with our bridge in) */
    int has_lib;               /* the two above are set */
    char ort_version[64];      /* FACT_ORT_VERSION: its ProductVersion, "" none */
    int ort_listed;            /* FACT_ORT_LISTED: its sha256 in the list's windows.compatible.ort_sha256 */
} native_blocked;
/* "cache" for the current version folder (rb_version: its version, "" none): 0 with *out, or -1
 * when the live DLL or ort\'s copy couldn't be looked at (no answer: the core stops). r1: R1's
 * facts for folder and root against saved's and installed's bridges, already read (the cheap
 * facts read them once for both uses), or NULL: read here (BLOCKED's step) */
int native_blocked_cache(const compat_os *os, const char *folder, const char *rb_version, const char *root, const payload_manifest *saved,
                         const payload_manifest *installed, const compat_r1 *r1, native_blocked *out);
/* blockedByVersionList itself is the core's (native.h's native_blocked_by_list, its ordered
 * rules over these facts) */

#endif
