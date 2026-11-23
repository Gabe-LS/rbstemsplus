/* payload.c: payload.json for the Windows app and the worker, the Windows copy of Payload.swift
 * (app.md §5.1, W1; release.md §1.2, §2, §7).
 *
 * The order, at user and admin level alike: size cap -> the signature's DER -> ECDSA over the raw
 * bytes -> only then JSON (plainJSONKeys' check, then the core's strict reader) -> the floor.
 *
 * What a payload.json must be, as the Mac's Manifest.valid for its part, plus the Windows block:
 *  - payload_version: x.y.z (each part 1-9 digits); a suffix is refused, as on the Mac (there is
 *    no beta channel, release.md §2).
 *  - The Mac part (app, bridge, compatible): required, and the Mac's rules hold exactly (so a
 *    payload the Mac refuses is refused here too).
 *  - model {file, sha256, size > 0} and previous_models (optional), as on the Mac.
 *  - windows (release.md §1.2's keys, J26), optional: absent is "no news" for the Windows app
 *    (has_windows 0: payload_accept saves nothing and the floor stays). When present, every key
 *    there is checked: app {version <= payload_version, x.y.z, zip,
 *    sha256, admin_sha256, stub_sha256 {x64, arm64}}, bridge {file, sha256}, compatible
 *    {rekordbox, ort_sha256, stems_engine, optional ort and ort_code_sha256}, and
 *    resampler_proved (optional) [{product_version, rekordbox_sha256}].
 *  - Unknown keys anywhere are ignored, as on the Mac; a key twice, or with an escape or outside
 *    printable ASCII, is refused (plainJSONKeys).
 *
 * Stricter than the Mac only past any real payload.json (recorded here so a review can weigh it):
 * over 64 KiB (the size cap and the core reader's limit), nesting over 16 or more than 256 items
 * in one array or object (the core reader), "\u0000" or a lone surrogate escape in a value (the
 * core reader), a list over PAYLOAD_LIST_MAX or a string over its buffer below, and an integer
 * written with a fraction or exponent that a double can't hold exactly ("9468852530000000000e-10":
 * only plain integers are exact; release.sh writes plain integers). */
#ifndef RBSP_WIN_PAYLOAD_H
#define RBSP_WIN_PAYLOAD_H

#include "sig.h"

#include <stddef.h>
#include <stdint.h>

enum {
    PAYLOAD_MAX = 64 * 1024,   /* the size cap, checked first */
    PAYLOAD_LIST_MAX = 64,     /* entries kept per list (previous_models, the windows lists) */
    PAYLOAD_VERSION_MAX = 48,  /* "999999999.999999999.999999999" fits */
    PAYLOAD_NAME_MAX = 128,    /* file names; rekordbox and engine versions too */
};

/* the outcome of a check; PAYLOAD_OK is 0 */
enum {
    PAYLOAD_OK = 0,
    PAYLOAD_NO_SIGNATURE,      /* none, or empty (.noSignature) */
    PAYLOAD_BAD_SIGNATURE,     /* not by a trusted key, malformed or truncated (.badSignature) */
    PAYLOAD_NOT_VALID,         /* not a valid payload description (.notValid) */
    PAYLOAD_OLDER,             /* older than the floor (.older) */
    PAYLOAD_TOO_BIG,           /* over PAYLOAD_MAX: refused before anything else */
    PAYLOAD_NO_CRYPTO,         /* the system's crypto couldn't be used (bcrypt.dll) */
    PAYLOAD_NOT_SAVED,         /* payload_accept: couldn't be saved (.notSaved) */
    PAYLOAD_NO_NEWS,           /* payload_accept: valid, but without a windows block: nothing saved */
    PAYLOAD_NONE,              /* payload_saved: none saved (nothing to report) */
};

typedef char payload_sha[65];   /* 64 lowercase hex digits */

typedef struct {
    char payload_version[PAYLOAD_VERSION_MAX];
    const char *signed_by;                   /* the key's name (the caller's keys own it) */
    struct { char file[PAYLOAD_NAME_MAX]; payload_sha sha256; int64_t size; } model;
    payload_sha previous_models[PAYLOAD_LIST_MAX];
    int n_previous_models;

    int has_windows;                         /* 0: no news for the Windows app; nothing below is set */
    struct {
        char version[PAYLOAD_VERSION_MAX];
        char zip[PAYLOAD_NAME_MAX];
        payload_sha sha256, admin_sha256, stub_x64, stub_arm64;
    } win_app;
    struct { char file[PAYLOAD_NAME_MAX]; payload_sha sha256; } win_bridge;
    char win_rekordbox[PAYLOAD_LIST_MAX][PAYLOAD_NAME_MAX];
    int n_win_rekordbox;
    payload_sha win_ort_sha256[PAYLOAD_LIST_MAX];
    int n_win_ort_sha256;
    char win_stems_engine[PAYLOAD_LIST_MAX][PAYLOAD_NAME_MAX];
    int n_win_stems_engine;
    struct { char product_version[PAYLOAD_NAME_MAX]; payload_sha rekordbox_sha256; } resampler_proved[PAYLOAD_LIST_MAX];
    int n_resampler_proved;
    /* windows.compatible.ort and ort_code_sha256 (mv-spec §2.3): each listed rekordbox's original
     * onnxruntime.dll sha256 (one binding per listed version, each sha256 also in ort_sha256), and the PE code digest
     * (pe_code.h) every bound original shares. Optional for old payloads: none means install and
     * reinstall are refused by the worker; ort_code_sha256 is required with ort */
    struct { char rekordbox[PAYLOAD_NAME_MAX]; payload_sha sha256; } win_ort_bind[PAYLOAD_LIST_MAX];
    int n_win_ort_bind;
    payload_sha win_ort_code_sha256;
    /* not in the JSON: the sha256 of the payload.json bytes this manifest was verified from, when
     * payload_store.c's payload_saved or payload_accept gave it ("" from payload_check alone). The
     * app's install scope carries it to the worker's --manifest (re-review finding 6) */
    payload_sha json_sha256;
} payload_manifest;

/* -- the pure rules */

/* plainJSONKeys: one JSON object in which no object at any level has a key twice, and every key
 * is non-empty printable ASCII without a backslash. Values are only skipped. Byte for byte the
 * Mac's (Payload.swift), its depth limit (32) included. */
int payload_plain_keys(const void *data, size_t n);

/* isReleaseVersion: x.y.z, each part 1 to 9 ASCII digits */
int payload_is_release_version(const char *s);
/* The order of two release versions: -1, 0 or 1; 2 when either isn't one. Parts compare as
 * numbers (so "1.0.9" < "1.0.10" and "08.1.1" == "8.1.1", as the Mac's olderPayload). */
int payload_version_cmp(const char *a, const char *b);
/* isSHA256: 64 lowercase hex digits */
int payload_is_sha256(const char *s);

/* Windows rekordbox compatibility accepts a listed version or its dotted build suffix. */
int payload_win_rekordbox_supported(const payload_manifest *m, const char *version);
/* The original onnxruntime.dll sha256 the payload binds to the rekordbox of this ProductVersion,
 * by payload_win_rekordbox_supported's rule (the longest listed version, optionally followed by
 * a dotted build suffix), or NULL when none is bound (mv-spec §2.3). */
const char *payload_win_ort_for(const payload_manifest *m, const char *product_version);

/* decodeManifest with the rules above (no signature): PAYLOAD_OK with *out filled, or
 * PAYLOAD_NOT_VALID (PAYLOAD_TOO_BIG over the cap). out->signed_by is NULL. */
int payload_decode(const void *data, size_t n, payload_manifest *out);

/* checkManifest: the size cap, the signature (one of keys), payload_decode, and not older than
 * floor (NULL or "": none; one that isn't a version refuses as PAYLOAD_OLDER). PAYLOAD_OK with
 * *out filled (signed_by set), or why not. */
int payload_check(const void *data, size_t n, const void *sig, size_t sig_n, const sig_pubkey *keys, size_t nkeys,
                  const char *floor, payload_manifest *out);

/* The floor record's text (payloadFloor): trimmed of spaces, tabs and line ends; non-empty, under
 * 64 characters, letters, digits, dots, dashes and underscores only. 1 with the version in out,
 * or 0 (no usable record). */
int payload_floor_parse(const char *text, size_t n, char out[64]);
/* acceptedFloor: the newer of the saved record and the minimum (either may be NULL or empty), into
 * out. 1, or 0 when there is neither; -1 when a non-empty minimum isn't a version (it fails
 * closed: the store then refuses every payload). A record that isn't a version (by
 * payload_version_cmp) is ignored; payload_check refuses against a floor that isn't one. */
int payload_accepted_floor(const char *saved, const char *minimum, char out[64]);

/* PayloadRefusal's description, for the log ("it has no signature"...), into out (n bytes,
 * always NUL-terminated); version and floor name the two for PAYLOAD_OLDER. Returns out. */
const char *payload_refusal_text(int why, const char *version, const char *floor, char *out, size_t n);

/* -- the saved copy and the floor (payload_store.c, over common/plat.h's file layer)
 *
 * The last good payload.json is kept with its signature next to it (payload.json.sig) and
 * checked again on every read; the newest payload_version accepted is kept in payload-version.
 * The app keeps them under %LOCALAPPDATA%\rbstemsplus\ (payload\ and the support folder), the
 * worker its own under the admin-only root (app.md §5.10). Files are replaced atomically (a temp
 * name in the same folder, flushed, renamed over). */
typedef struct {
    const char *payload_dir;   /* holds payload.json and payload.json.sig */
    const char *floor_dir;     /* holds payload-version */
    const sig_pubkey *keys;
    size_t nkeys;
    const char *minimum;       /* the oldest payload_version accepted even with no record: the
                                * app's own version (payloadMinimum), the worker's compiled one */
} payload_files;

/* payloadFloor: the record's version (1, into out), or 0 when there is none or it isn't one */
int payload_read_floor(const payload_files *f, char out[64]);
/* savedManifest: the saved payload.json checked again now (signature, rules, not older than the
 * accepted floor). PAYLOAD_OK with *out; PAYLOAD_NONE when none was saved; else why it is ignored
 * (the caller logs it, as "saved payload.json ignored: <payload_refusal_text>"). */
int payload_saved(const payload_files *f, payload_manifest *out);
/* acceptManifest: checks a downloaded payload.json and its signature; when they count and carry
 * the windows block, raises the floor when newer (first, its write checked), then saves the pair
 * (the signature first: if this stops between the two, the saved pair doesn't verify and is
 * ignored until the next download). A failure after the floor's write leaves the old pair under a
 * newer floor, refused as older: it fails closed. PAYLOAD_OK; PAYLOAD_NO_NEWS (valid, no windows
 * block: nothing saved, floor unchanged); PAYLOAD_NOT_SAVED (the floor's or the pair's write
 * failed); or the check's refusal (nothing changed). (The Mac writes the pair first and ignores
 * the floor's write: an owner question.) Accepts are serialised by a lock in the process: each
 * store has one process writing it (the app per account, the worker in its own root), so no lock
 * across processes is needed. */
int payload_accept(const payload_files *f, const void *data, size_t n, const void *sig, size_t sig_n, payload_manifest *out);

#endif
