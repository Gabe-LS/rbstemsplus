/* receipt.c: one protected receipt per rekordbox installation that holds our bridge
 * (multi-version contract, .orchestration/mv-spec.md §2.2; decisions D4, D13). The worker writes
 * <root>\receipts\<X.Y.Z> (a regular file in the admin-only root, its DACL inherited, written with
 * write_atomic) as the last step of a target's swap, after the load check, and deletes it before
 * the swap; the app and the watcher only read it. Pure C11 over common/plat.h.
 *
 * The text, in this order, every line ending in "\n", nothing else:
 *
 *   rbsp-receipt 1
 *   folder rekordbox 7.2.19
 *   folder_id <plat_id as hex>
 *   exe_sha256 <64>
 *   original_sha256 <64>
 *   bridge_sha256 <64>
 *   runtime_code_sha256 <64>
 *   payload_version 1.5.0
 *
 * D13: Windows never shipped, so there is no legacy record to adopt. A live bridge with no
 * receipt, or with one that doesn't match, is never treated as installed; a saved set goes back
 * into a folder only when its receipt binds it or it is the signed binding's original
 * (inv_original_bound). A same-version reinstall by Pioneer gives a new folder id or exe sha256,
 * so the old receipt no longer matches: an old backup is never restored on X.Y.Z alone.
 *
 * folder_id is plat_id_hex (common/plat.h) of the folder's identity, read without following a
 * link: on Windows the volume serial and the 128-bit FileId (FILE_ID_INFO).
 *
 * runtime_code_sha256 is ort\onnxruntime.dll's PE code digest (pe_code.h), not its file sha256:
 * the bound originals of two versions differ only in their signature (D2a), so ort\ rewritten
 * from another version's original keeps every receipt matching. That the copy is one of the
 * bound originals at all (pinned, its sha256 bound, its code the payload's) is checked apart, by
 * the worker's runtime_ok and the app's nf_runtime_ok. */
#ifndef RBSP_WIN_RECEIPT_H
#define RBSP_WIN_RECEIPT_H

#include "compat.h"
#include "inventory.h"
#include "payload.h"

#include <stddef.h>

#define RECEIPT_DIR "receipts"   /* under the root: receipts\<X.Y.Z> */
#define RECEIPT_MAGIC "rbsp-receipt 1"
enum { RECEIPT_MAX = 1024 };     /* a receipt's text is at most this many bytes */

typedef struct {
    char folder_name[COMPAT_PATH_MAX];          /* "rekordbox 7.2.19" */
    char version[COMPAT_VERSION_MAX];           /* its X.Y.Z (the file's name) */
    char folder_id[64];                         /* plat_id as hex (common/plat.h plat_id_hex, lane B) */
    char payload_version[PAYLOAD_VERSION_MAX];  /* the release that wrote it */
    payload_sha exe_sha256, original_sha256, bridge_sha256, runtime_code_sha256;
} receipt;

/* the text above for r: its length (NUL not counted), or -1 when a field is invalid or it doesn't fit */
int receipt_format(const receipt *r, char *out, size_t cap);
/* strict: the order above, lowercase hex, no extra line or byte; 0 with *out, or -1 */
int receipt_parse(const char *b, size_t n, receipt *out);
/* root\receipts\<version>, not followed, a regular file of at most RECEIPT_MAX bytes: 0 with
 * *out, ENOENT (none), or EINVAL (anything else: unreadable, malformed, not a regular file) */
int receipt_read(const char *root, const char *version, receipt *out);
/* 1 when every field binds this row now: the folder's name and identity, the exe's sha256, the
 * live DLL's sha256 (live_sha) == bridge_sha256, the saved set's (saved_sha) == original_sha256,
 * ort\'s copy's PE code digest (ort_code) == runtime_code_sha256; else 0 */
int receipt_matches(const receipt *r, const inv_install *row, const char *live_sha, const char *saved_sha, const char *ort_code);

#endif
