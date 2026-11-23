/* The Windows shell's cheap facts (host_facts.h): the reads, and the DLL's export that writes them
 * into the core's sheet (core_win.h's core_win_cheap_facts). Each row is the Windows reader the
 * DLL's steps already use for the same fact, so the view and the flows agree:
 *  - the account: native_user_is_admin (FACT_USER_IS_ADMIN, as BLOCKED's);
 *  - the app in its own place: update.c's update_in_place (FACT_OWN_BUNDLE, UPDATE_CHECK's);
 *  - the choices: support\installed by the core's state_chosen (STATE_LINE's, the watcher's);
 *  - rekordbox: compat.c's current version folder under <Program Files>\rekordbox and its
 *    version, the uninstall entry's else the folder's (BLOCKED's current_folder);
 *  - the lists: the verified saved payload, else the verified installed-release record.
 *  - Stems Cache: every inventory row's verified original, live DLL and receipt.
 *    FACT_CACHE_RECORD_OK means every eligible version is On. FACT_WIN_RB_ROWS
 *    also carries visible repairs, unsupported versions and running EXEs.
 *  - Stems Cache's traces (cacheTraces): Stems Cache in, or anything at <Program Files>\RB Stems
 *    Plus (Pioneer's saved files, ort\'s copy, an empty folder), not following a link; a stat
 *    that fails for any reason but "nothing there" counts as traces (fail closed: Uninstall All
 *    then removes Stems Cache first, or stops); a root folder holding only boot-time deletes (the
 *    uninstall worker's reboot branch, I20, J7) isn't one;
 *  - Demucs v4 in: rekordbox's model file has Demucs v4's size, ours or the list's (the watcher's
 *    rule); its size and time (FACT_MODEL_SIZE, FACT_MODEL_MTIME, the sha256 memo's key);
 *  - the helper there: paths.helper a regular file;
 *  - this machine runs the helper (FACT_APPLE_SILICON's Windows meaning, host_facts.h): always;
 *  - with CORE_FACTS_PREPARE (e->prepare): all covered bridges keep prepared playlists (kept=1
 *    in their memoised cache lines), stemsQuality() from rekordbox3.settings, and the saved
 *    payload's model beside the built-in one (FACT_OUR_MODELS);
 *  - CORE_FACTS_KEPT: the kept sets in paths.kept (host_facts_kept_sets, keptSets());
 *  - config.ini: readConfigState over common/cfg_ini.c's grammar (the bridge's), with
 *    readConfigBytes' reasons for an unreadable file. */
#include "host_facts.h"

#ifndef RBSP_CORE_PLAT_SHIM_H
#  define RBSP_CORE_PLAT_SHIM_H   /* common/plat.h (through compat.h) has the shim's names */
#endif
#include "internal.h"   /* the core's state_chosen (and, in the DLL, fmt_config_number) */

#include "dismissed.h"
#include "cfg_ini.h"
#include "models.h"            /* MODELS_OURS_BUILTIN: ourModel */
#include "native_location.h"   /* native_location_line_says: bridgeSaysLocation's rule */
#include "update.h"
#include "win_rules.h"         /* wr_utf16_to_8 (hf_pending_parse), wr_program_path */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define SEP "\\"
#else
#  define SEP "/"      /* the Mac tests: a scratch tree */
#endif

enum { HF_SMALL_MAX = 64 * 1024 };
/* stemsQuality's file (under 4 MiB) and a .kept file (8 MiB, keptSets') */
enum { HF_SETTINGS_MAX = 4 << 20, HF_KEPT_MAX = 8 << 20 };
/* the Mac's ourModelSize: Demucs v4's hdemucs.onnx (watch.h's WATCH_OUR_MODEL_SIZE) */
enum { HF_OUR_MODEL_SIZE = 308572524 };

static int join(char *out, size_t n, const char *a, const char *b) {
    int k = snprintf(out, n, "%s" SEP "%s", a, b);
    return k > 0 && (size_t)k < n;
}

/* a file's size the way Config.swift's maxGB came: max_bytes is (long long)(gb * 2^30); the
 * shortest decimal that gives the same bytes (the value as typed, for configNumber) */
static double gb_of(long long bytes) {
    double x = (double)bytes / (double)(1LL << 30);
    for (int digits = 1; digits <= 17; digits++) {
        char b[40];
        snprintf(b, sizeof b, "%.*g", digits, x);
        double y = strtod(b, NULL);
        if ((long long)(y * (double)(1LL << 30)) == bytes) return y;
    }
    return x;
}

static void config(const host_facts_env *e, host_facts *o) {
    cfg_config c;
    memset(&c, 0, sizeof c);
    c.enabled = 1;                       /* Config's defaults */
    c.rekordbox_model = 0;
    c.max_bytes = 20LL << 30;
    c.max_days = 60;
    c.location = 0;
    char p[1100];
    char *b = NULL;
    size_t n = 0;
    int r = !e->support || !join(p, sizeof p, e->support, "config.ini") ? ENAMETOOLONG : plat_read_file(p, CFG_MAX, &b, &n);
    if (r == ENOENT) {
        /* no file: the defaults, stems saved on this PC */
    } else if (r) {
        snprintf(o->config_unreadable, sizeof o->config_unreadable, "%s", r == PLAT_EFTYPE ? "over 64 KB" : strerror(r));
    } else if (memchr(b, 0, n)) {
        snprintf(o->config_unreadable, sizeof o->config_unreadable, "a NUL byte");
    } else if (!cfg_utf8_ok((const unsigned char *)b, n) || cfg_parse(b, n, &c, NULL)) {
        snprintf(o->config_unreadable, sizeof o->config_unreadable, "not UTF-8");
    } else {
        o->config_text = b;              /* readable: its bytes */
        o->config_n = n;
        b = NULL;
    }
    free(b);
    o->config_enabled = c.enabled != 0;
    o->config_rb_model = c.rekordbox_model != 0;
    o->config_max_gb = gb_of(c.max_bytes);
    o->config_max_days = c.max_days;
    if (o->config_unreadable[0] || c.location < 0) o->config_location = HF_CONFIG_WRONG;
    else if (c.location == 1) {
        o->config_location = HF_CONFIG_CHOSEN;
        snprintf(o->config_dir, sizeof o->config_dir, "%s", c.dir);
        snprintf(o->config_id, sizeof o->config_id, "%s", c.id);
    } else o->config_location = HF_CONFIG_THIS;
}

void host_facts_config(const char *support, host_facts *out) {
    memset(out, 0, sizeof *out);
    host_facts_env e;
    memset(&e, 0, sizeof e);
    e.support = support;
    config(&e, out);
}

/* ---------------------------------------------------------------- the root's boot-time deletes */

/* The uninstall worker's reboot branch (I20, J7: bridge.md's Uninstall row) leaves the root holding
 * only its own renamed image and the emptied bin\<sha12>\, bin\ and the root, each scheduled with
 * MOVEFILE_DELAY_UNTIL_REBOOT, child before parent: "an uninstall that leaves no root folder, or
 * only the disclosed pending delete". Such a root isn't a trace; anything in it that isn't
 * scheduled (or can't be looked at, or is neither a file nor a folder) is, and so is a name whose
 * last delete comes after its folder's (the folder isn't empty when its delete runs, so it stays),
 * and a rename into the root (something arrives there at the restart). Bounded: HF_PENDING_DEPTH
 * folders down (the worker's file is 3), HF_PENDING_NAMES names in all; past either, traces.
 *
 * The order is the queue's: PendingFileRenameOperations, then PendingFileRenameOperations2, the
 * order the Session Manager is taken to run them in. The worker's own entries all go through one
 * MoveFileExW path, so their order among themselves holds either way. */
enum { HF_PENDING_DEPTH = 4, HF_PENDING_NAMES = 256 };

typedef struct {
    const char *list;   /* pending_deletes' entries: a tag byte, a path, a NUL (hf_pending_parse) */
    size_t n;
    int names;          /* names looked at so far */
} hf_pending;

/* a and b's first n bytes are equal, ASCII letters in either case: Windows' names ignore case (a
 * name cased otherwise in other letters doesn't match: fail closed for a delete, which must match) */
static int hf_same(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* the index (in queue order, both values') of path's last delete, or -1: none */
static long hf_pending_has(const hf_pending *p, const char *path) {
    size_t len = strlen(path);
    long last = -1, i = 0;
    for (const char *s = p->list, *z = p->list + p->n; s < z; i++) {
        const char *end = memchr(s, 0, (size_t)(z - s));
        size_t l = end ? (size_t)(end - s) : (size_t)(z - s);
        if (l == len + 1 && s[0] == 'D' && hf_same(s + 1, path, len)) last = i;
        s += l + 1;
    }
    return last;
}

/* c's cl bytes could be a short (8.3) name Windows made from nm (no dot in nm, so no extension):
 * t is nm without its spaces ("RBSTEMSPLUS"); no '.', at most 8 bytes, ASCII case ignored, and
 * either a 1-6 character prefix of t, or all 8 bytes as t's first 2 characters and 1-4 hex digits
 * (the hash form, cut as its tail grows), then '~' and only digits to the end ("RBSTEM~1",
 * "RBSTE~10", the hash form "RB1F2A~1" and "RB1F2~10"). It also matches, unavoidably, the basic
 * short name of another folder whose first letters are RB and hex characters ("RB2024~1" for
 * "RB 2024 Backup"): a false trace, the safe side */
static int hf_short_of(const char *c, size_t cl, const char *nm) {
    char t[16];
    size_t tl = 0;
    for (const char *q = nm; *q && tl < sizeof t; q++)
        if (*q != ' ') t[tl++] = *q;
    if (cl < 3 || cl > 8 || memchr(c, '.', cl)) return 0;
    const char *w = memchr(c, '~', cl);
    if (!w) return 0;
    size_t b = (size_t)(w - c);
    if (b + 1 >= cl) return 0;
    for (size_t i = b + 1; i < cl; i++)
        if (c[i] < '0' || c[i] > '9') return 0;
    if (b >= 1 && b <= 6 && b <= tl && hf_same(c, t, b)) return 1;
    if (b >= 3 && b <= 6 && cl == 8 && tl >= 2 && hf_same(c, t, 2)) {
        for (size_t i = 2; i < b; i++) {
            unsigned char x = (unsigned char)c[i];
            if (!((x >= '0' && x <= '9') || (x >= 'a' && x <= 'f') || (x >= 'A' && x <= 'F'))) return 0;
        }
        return 1;
    }
    return 0;
}

/* a rename's new path goes through a folder named like the root, wherever it is in the queue: 1,
 * else 0. A part of the path counts when it's the root's own name (ASCII case ignored) or a short
 * name Windows could make from it (hf_short_of: no extension, at most 8 bytes, a numeric `~` tail),
 * however the folders above are spelled (a volume GUID, a mounted folder, a subst drive, UNC, a
 * short name, non-ASCII letters cased otherwise) and whatever prefix comes before the first
 * separator. Not seen: an alias of the root folder itself (a junction, a
 * mount point or a subst drive aimed at it, or a short name set by hand) */
static int hf_pending_into(const hf_pending *p, const char *root) {
    const char *nm = strrchr(root, SEP[0]);
    nm = nm ? nm + 1 : root;
    size_t nl = strlen(nm);
    for (const char *s = p->list, *z = p->list + p->n; s < z;) {
        const char *end = memchr(s, 0, (size_t)(z - s));
        size_t l = end ? (size_t)(end - s) : (size_t)(z - s);
        if (l && s[0] == 'R')
            for (const char *c = s + 1, *ze = s + l; c <= ze;) {
                const char *q = memchr(c, SEP[0], (size_t)(ze - c));
                if (!q) q = ze;
                size_t cl = (size_t)(q - c);
                if ((cl == nl && hf_same(c, nm, nl)) || hf_short_of(c, cl, nm)) return 1;
                c = q + 1;
            }
        s += l + 1;
    }
    return 0;
}

typedef struct {
    hf_pending *p;
    plat_dir d;
    const char *path;
    int depth;
    long bound;   /* the folder's last delete: every name in it must be deleted (last) before it */
} hf_walk;

static int hf_walk_dir(hf_pending *p, plat_dir d, const char *path, int depth, long bound);

/* one name in a folder: 0 a file whose last delete comes before its folder's, or such a folder
 * holding only such names */
static int hf_walk_one(void *ctx, const char *name, const plat_stat *st) {
    hf_walk *w = ctx;
    char sub[1100];
    if (++w->p->names > HF_PENDING_NAMES || !join(sub, sizeof sub, w->path, name)) return 1;
    long i = hf_pending_has(w->p, sub);
    if (i < 0 || i >= w->bound) return 1;   /* not scheduled, or deleted only after its folder: it stays */
    if (st->kind == PLAT_FILE) return 0;
    if (st->kind != PLAT_DIR || w->depth >= HF_PENDING_DEPTH) return 1;   /* a link, a reparse point: traces */
    plat_dir c;
    if (plat_dir_open_at(w->d, name, &c)) return 1;
    int r = hf_walk_dir(w->p, c, sub, w->depth + 1, i);
    plat_dir_close(c);
    return r;
}

static int hf_walk_dir(hf_pending *p, plat_dir d, const char *path, int depth, long bound) {
    hf_walk w = {p, d, path, depth, bound};
    return plat_dir_each(d, hf_walk_one, &w) ? 1 : 0;   /* a folder not read whole: traces */
}



/* the root folder (there, a folder) holds only boot-time deletes, itself included, each before its
 * folder's, and nothing is renamed into it: 1, else 0 */
static int root_pending_only(const host_facts_env *e) {
    char *list = NULL;
    size_t n = 0;
    if (!e->pending_deletes || e->pending_deletes(e->pending_ctx, &list, &n)) {
        free(list);
        return 0;
    }
    hf_pending p = {list, n, 0};
    plat_dir d;
    int only = 0;
    long lr = list ? hf_pending_has(&p, e->root) : -1;
    if (lr >= 0 && !hf_pending_into(&p, e->root) && !plat_dir_open(e->root, &d)) {
        only = !hf_walk_dir(&p, d, e->root, 1, lr);
        plat_dir_close(d);
    }
    free(list);
    return only;
}

/* one name's prefixes Windows writes taken off (hf_pending_parse): '*' and digits only with the
 * "\??\" after them, then "\??\" */
static void hf_pending_trim(const uint16_t *w, size_t *s0, size_t *sn) {
    size_t a = *s0, n = *sn, q = a + 1;
    if (n >= 2 && w[a] == '*') {
        while (q < a + n && w[q] >= '0' && w[q] <= '9') q++;
        if (q > a + 1 && a + n - q >= 4 && w[q] == '\\' && w[q + 1] == '?' && w[q + 2] == '?' && w[q + 3] == '\\') {
            n -= q - a;
            a = q;
        }
    }
    if (n >= 4 && w[a] == '\\' && w[a + 1] == '?' && w[a + 2] == '?' && w[a + 3] == '\\') {
        a += 4;
        n -= 4;
    }
    *s0 = a;
    *sn = n;
}

int hf_pending_parse(const uint16_t *w, size_t units, char **buf, size_t *len, size_t *cap) {
    size_t at = 0, from = 0, from_n = 0;
    int second = 0;
    while (at < units) {
        size_t end = at;
        while (end < units && w[end]) end++;
        if (!second) {
            from = at;
            from_n = end - at;
        } else if (end > at || from_n) {
            int del = end == at;
            size_t s0 = del ? from : at, sn = del ? from_n : end - at;
            if (!del && w[s0] == '!') {   /* MOVEFILE_REPLACE_EXISTING's mark on the new name */
                s0++;
                sn--;
            }
            hf_pending_trim(w, &s0, &sn);
            if (*cap - *len < sn * 3 + 2) {   /* the tag, at most 3 bytes a unit, the NUL */
                size_t nc = *cap * 2 + sn * 3 + 1024;
                char *nb = realloc(*buf, nc);
                if (!nb) return -1;
                *buf = nb;
                *cap = nc;
            }
            char *o = *buf + *len;
            size_t room = *cap - *len - 1;
            o[0] = del ? 'D' : 'R';
            long u = wr_utf16_to_8(w + s0, sn, o + 1, room);
            if (u < 0 && !del) {   /* a rename's new name kept, an unpaired surrogate as U+FFFD */
                u = 0;
                for (size_t i = 0; i < sn; i++) {
                    size_t k = i + 1 < sn && w[s0 + i] >= 0xD800 && w[s0 + i] <= 0xDBFF && w[s0 + i + 1] >= 0xDC00 && w[s0 + i + 1] <= 0xDFFF ? 2 : 1;
                    long m = wr_utf16_to_8(w + s0 + i, k, o + 1 + u, room - (size_t)u);
                    if (m < 0) {
                        memcpy(o + 1 + u, "\xEF\xBF\xBD", 3);
                        m = 3;
                    }
                    u += m;
                    i += k - 1;
                }
                o[1 + u] = 0;
            }
            if (u >= 0) *len += (size_t)u + 2;   /* a delete's name that isn't UTF-16 matches nothing: traces */
        }
        second = !second;
        at = end + 1;
    }
    return 0;
}

static inv_cache inventory_cache = INV_CACHE_INIT;

int host_facts_inventory(const compat_os *os, const char *root, inv_list *out) {
    uint64_t gen = 0;
#ifdef _WIN32
    gen = compat_uninstall_generation();
#endif
    return inv_read_cached(&inventory_cache, os, gen, root, out);
}

static void read_bridge_models(const native_inv_caps *caps, host_facts *o) {
    if (!caps->live_line) return;
    o->has_bridge_models = 1;
    const char *ln = caps->cache_line, *z = ln + caps->cache_line_len;
    size_t at = 0, n = 0;
    while (at + 8 <= caps->cache_line_len && memcmp(ln + at, "pioneer=", 8)) at++;
    for (const char *s = at + 8 <= caps->cache_line_len ? ln + at + 8 : z; s < z;) {
        const char *end = memchr(s, ',', (size_t)(z - s));
        size_t len = end ? (size_t)(end - s) : (size_t)(z - s);
        char sha[65];
        if (len == 64 && n + 65 < sizeof o->bridge_models) {
            memcpy(sha, s, 64);
            sha[64] = 0;
            if (payload_is_sha256(sha)) {
                if (n) o->bridge_models[n++] = '\n';
                memcpy(o->bridge_models + n, sha, 64);
                n += 64;
            }
        }
        s = end ? end + 1 : z;
    }
    o->bridge_models[n] = 0;
}

static int row_dismissed(const char *support, const char *version) {
#ifdef _WIN32
    char key[COMPAT_VERSION_MAX + sizeof "r1:"];
    int n = snprintf(key, sizeof key, "r1:%s", version);
    return support && n > 0 && (size_t)n < sizeof key && dismissed_has(support, key);
#else
    (void)support;
    (void)version;
    return 0;
#endif
}

static void write_rows(const char *support, const inv_row_view *rows, int count, host_facts *out) {
    size_t at = 0;
    int worst = 0, dismissed = 1, unknown = 0;
    for (int i = 0; i < count; i++) {
        const inv_row_view *r = &rows[i];
        int r1 = r->r1 < 0 ? 0 : r->r1;
        int skip = r1 > 0 && row_dismissed(support, r->version);
        const char *display = strpbrk(r->display_version, "\t\r\n") ? "" : r->display_version;
        size_t room = sizeof out->win_rows - at;
        int n = snprintf(out->win_rows + at, room, "%s\t%d\t%d\t%d\t%d\t%d\t%s\n",
                         r->version, r->state, r->why, r->running, r1, skip, display);
        if (n < 0 || (size_t)n >= room) {
            out->win_rows[0] = 0;
            out->r1_state = -1;
            return;
        }
        at += (size_t)n;
        if (r1 > worst) worst = r1;
        if (r1 && !skip) dismissed = 0;
        if (r->r1 < 0) unknown = 1;
    }
    out->r1_state = !count || (!worst && unknown) ? -1 : worst;
    out->r1_dismissed = worst && dismissed;
}

void hf_covered_capabilities(const native_inv_caps *const *covered, int n, int *keeps, int *location) {
    *keeps = *location = n > 0;
    for (int i = 0; i < n; i++) {
        const native_inv_caps *c = covered[i];
        *keeps &= c->live_line && hf_line_says_kept(c->cache_line, c->cache_line_len);
        *location &= c->live_line && native_location_line_says(c->cache_line, c->cache_line_len);
    }
}

static int read_inventory(const host_facts_env *e, const payload_manifest *m, const payload_manifest *installed, host_facts *out) {
    inv_list *list = malloc(sizeof *list);
    inv_assess_row *assess = malloc(INV_MAX * sizeof *assess);
    inv_row_view *views = malloc(INV_MAX * sizeof *views);
    native_inv_caps *caps = malloc(INV_MAX * sizeof *caps);
    compat_rekordbox *rb = malloc(INV_MAX * sizeof *rb);
    if (!list || !assess || !views || !caps || !rb) {
        free(list);
        free(assess);
        free(views);
        free(caps);
        free(rb);
        return -1;
    }
    if (e->fresh_inventory) inv_read(e->os, e->programs_rekordbox, COMPAT_FRESH, list);
    else host_facts_inventory(e->os, e->programs_rekordbox, list);
    int cur, state;
    inv_current(list, rb, INV_MAX, &cur, &state);
    if (cur >= 0) {
        out->installed = 1;
        strcpy(out->rekordbox_folder, rb[cur].folder);
        strcpy(out->version, rb[cur].display_version[0] ? rb[cur].display_version : rb[cur].folder_version);
    }
    free(rb);

    native_inventory_assess(e->os, list, e->root, m, installed, assess, caps);
    native_inventory_running(e->os, list, assess, out->open_versions, sizeof out->open_versions);
    int count = inv_rows(list, assess, m, views, INV_MAX);
    inv_state_text(views, count, out->cache_states, sizeof out->cache_states);
    write_rows(e->support, views, count, out);

    plat_stat ort;
    char path[COMPAT_PATH_MAX];
    if (e->root && join(path, sizeof path, e->root, "ort" SEP "onnxruntime.dll")) {
        out->blocked.has_root_ort = 1;
        out->blocked.root_ort_exists = !plat_stat_path(path, 0, &ort) && ort.kind == PLAT_FILE;
    }
    const native_inv_caps *bridge = NULL, *covered[INV_MAX];
    int all_done = list->complete, showing = 0;
    out->cache_answered = list->complete;
    out->cache_traces = !list->complete;
    for (int i = 0; i < list->n; i++) {
        const inv_install *r = &list->it[i];
        const inv_assess_row *a = &assess[i];
        const native_inv_caps *c = &caps[i];
        if (r->trust == INV_STALE) continue;
        out->cache_present |= c->bridge;
        out->cache_traces |= c->trace;
        if (!bridge && c->bridge) bridge = c;
        int shown = r->trust != INV_DISK_ONLY || c->bridge;
        int on = shown && views[showing++].state == INV_ROW_ON;
        if (r->trust == INV_VERIFIED && a->listed && a->original_bound) {
            out->blocked.rekordbox_supported = 1;
            out->blocked.has_lib = out->blocked.lib_exists = out->blocked.lib_signed = out->blocked.ort_listed = 1;
            if (!out->blocked.ort_version[0]) strcpy(out->blocked.ort_version, c->ort_version);
            all_done &= on;
        }
        if (!on) continue;
        if (!out->ncovered) bridge = c;
        covered[out->ncovered] = c;
        host_covered_exe *exe = &out->covered[out->ncovered++];
        strcpy(exe->folder, r->folder);
        strcpy(exe->product_version, r->exe.pe.product_version);
        strcpy(exe->sha256, r->exe.sha256);
        exe->id = r->exe.id;
        exe->size = r->exe.size;
        exe->mtime = r->exe.mtime;
    }
    hf_covered_capabilities(covered, out->ncovered, &out->bridge_keeps, &out->bridge_location);
    out->bridge_keeps &= e->prepare;
    out->cache_record_ok = out->ncovered > 0 && all_done;
    if (!out->plus_present && bridge) read_bridge_models(bridge, out);
    out->inventory = list;
    out->assessment = assess;
    free(views);
    free(caps);
    return 0;
}

static int read_facts(const host_facts_env *e, host_facts *o);

int host_facts_read(const host_facts_env *e, host_facts *o) {
    if (e->on_main) compat_main_reads(1, e->miss, e->miss_ctx);
    int r = read_facts(e, o);
    if (e->on_main) compat_main_reads(0, NULL, NULL);
    return r;
}

static int read_facts(const host_facts_env *e, host_facts *o) {
    memset(o, 0, sizeof *o);
    o->has_admin = e->admin >= 0;
    o->admin = e->admin == 1;
    o->has_own_bundle = e->own_bundle >= 0;
    o->own_bundle = e->own_bundle == 1;

    /* the choices */
    char p[1100], *t = NULL;
    size_t tn = 0;
    if (e->support && join(p, sizeof p, e->support, "installed") && !plat_read_file(p, HF_SMALL_MAX, &t, &tn)) {
        int32_t model = 0, cache = 0;
        state_chosen(t, tn, &model, &cache);
        o->chosen_model = model != 0;
        o->chosen_cache = cache != 0;
    }
    free(t);

    /* the lists: payload\'s, else installed-release\'s */
    payload_manifest *saved = calloc(1, sizeof *saved), *inst = calloc(1, sizeof *inst);
    if (!saved || !inst) {
        free(saved);
        free(inst);
        return -1;
    }
    int hs = 0, hi = 0;
    if (e->support && e->own) {
        update_env ue;
        memset(&ue, 0, sizeof ue);
        ue.support = e->support;
        ue.own = e->own;
        ue.keys = e->keys;
        ue.nkeys = e->nkeys;
        update_store st;
        update_store_init(&ue, &st);
        hs = payload_saved(&st.files, saved) == PAYLOAD_OK && saved->has_windows;
        hi = update_installed(&ue, inst) == PAYLOAD_OK && inst->has_windows;
    }
    if (e->scope_manifest) {
        *saved = *e->scope_manifest;
        hs = saved->has_windows;
    }
    const payload_manifest *m = hs ? saved : hi ? inst : NULL;

    native_blocked_plus(e->model, m, o->engine, sizeof o->engine, &o->engine_supported);

    /* Stems Cache's traces: our root folder, whatever is in it (or anything in its place), but a
     * root that holds only boot-time deletes (root_pending_only) */
    plat_stat rst;
    int rs = e->root ? plat_stat_path(e->root, 0, &rst) : ENOENT;
    int root_there = rs != ENOENT && rs != ENOTDIR;
    if (root_there && !rs && rst.kind == PLAT_DIR && root_pending_only(e)) {
        o->root_pending = 1;   /* only the worker's reboot-branch leftovers: gone at the next restart */
        root_there = 0;
    }

    /* rekordbox's model file: the item itself, not a link's target */
    plat_stat ms;
    if (e->model && !plat_stat_path(e->model, 0, &ms) && ms.kind == PLAT_FILE) {
        o->has_model = 1;
        o->model_size = ms.size;
        o->model_mtime_ns = ms.mtime * 1000000000LL;
        o->plus_present = ms.size == HF_OUR_MODEL_SIZE || (m && ms.size == m->model.size);
    }

    if (read_inventory(e, m, hi ? inst : NULL, o)) {
        free(saved);
        free(inst);
        return -1;
    }
    o->cache_traces |= root_there;

    if (e->prepare) {
        o->has_prepare = 1;
        o->stems_quality = -1;
        char *sb = NULL;
        size_t sn = 0;
        if (e->rb_settings && !plat_read_file(e->rb_settings, HF_SETTINGS_MAX, &sb, &sn)) o->stems_quality = hf_stems_quality(sb, sn);
        free(sb);
        if (hs) snprintf(o->saved_model, sizeof o->saved_model, "%s", saved->model.sha256);
        /* prepareFacts' ortThere, read whatever comes first (the Mac's cheap sheet writes it
         * always): where BLOCKED "cache" stopped before the root's copy (rekordbox not listed),
         * the item itself, so Prepare's lines don't add "reinstall Stems Cache" to "not listed" */
        if (!(o->blocked.rekordbox_supported && o->blocked.has_root_ort) && e->root) {
            char op[1100];
            plat_stat ost;
            o->has_ort_there = join(op, sizeof op, e->root, "ort" SEP "onnxruntime.dll");
            o->ort_there = o->has_ort_there && !plat_stat_path(op, 0, &ost) && ost.kind == PLAT_FILE;
        }
    }
    o->cache_manifest = hs ? saved : hi ? inst : NULL;
    if (!hs) free(saved);
    if (hs || !hi) free(inst);

    plat_stat hs_st;
    o->helper_there = e->helper && !plat_stat_path(e->helper, 1, &hs_st) && hs_st.kind == PLAT_FILE;

    config(e, o);
    return 0;
}

int host_facts_live_bridge(const compat_os *os, const char *programs_rekordbox, int *present, int *location) {
    *present = *location = 0;
    if (!programs_rekordbox) return -1;
    inv_list *list = malloc(sizeof *list);
    if (!list) return -1;
    int rc = host_facts_inventory(os, programs_rekordbox, list);
    if (rc || !list->complete) {
        free(list);
        return -1;
    }
    int seen = 0, all_location = 1;
    for (int i = 0; i < list->n; i++) {
        const inv_install *r = &list->it[i];
        if (r->trust != INV_VERIFIED) continue;
        char path[COMPAT_PATH_MAX];
        compat_file file;
        if (!join(path, sizeof path, r->folder, "onnxruntime.dll") || compat_file_facts(os, path, 0, &file)
            || (file.exists && !file.readable)) {
            free(list);
            return -1;
        }
        if (!file.exists || !file.marker) continue;
        seen = 1;
        all_location &= file.has_cache_line && native_location_line_says(file.cache_line, file.cache_line_len);
    }
    free(list);
    *present = seen;
    *location = seen && all_location;
    return 0;
}

/* ---------------------------------------------------------------- prepareFacts' and keptSets()' reads */

static const char *hf_find(const char *s, size_t n, const char *w) {
    size_t k = strlen(w);
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(s + i, w, k)) return s + i;
    return NULL;
}

static int hf_xml_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int hf_quality_value(const char *value, size_t n) {
    size_t i = 0;
    int q = 0;
    for (; i < n && value[i] >= '0' && value[i] <= '9'; i++) {
        int digit = value[i] - '0';
        if (q > (INT_MAX - digit) / 10) return -1;
        q = q * 10 + digit;
    }
    return i ? q : -1;
}

int hf_stems_quality(const char *b, size_t n) {
    static const char target[] = "PartAnalysisQuality";
    if (!b) return -1;
    const char *end = b + n;
    while ((b = hf_find(b, (size_t)(end - b), "<VALUE"))) {
        b += 6;
        const char *p = b, *name = NULL, *value = NULL;
        size_t name_len = 0, value_len = 0;
        for (;;) {
            const char *tail = p;
            while (p < end && hf_xml_space(*p)) p++;
            if (end - p >= 2 && p[0] == '/' && p[1] == '>') {
                if (name_len == sizeof target - 1 && !memcmp(name, target, name_len))
                    return value ? hf_quality_value(value, value_len) : -1;
                b = p + 2;
                break;
            }
            if (p == tail) break;
            const char *key = p;
            while (p < end && !hf_xml_space(*p) && !strchr("=/<>\"'", *p)) p++;
            size_t key_len = (size_t)(p - key);
            while (p < end && hf_xml_space(*p)) p++;
            if (!key_len || p == end || *p++ != '=') break;
            while (p < end && hf_xml_space(*p)) p++;
            if (p == end || (*p != '"' && *p != '\'')) break;
            char quote = *p++;
            const char *attr = p;
            while (p < end && *p != quote && *p != '<') p++;
            if (p == end || *p != quote) break;
            if (!name && key_len == 4 && !memcmp(key, "name", 4)) {
                name = attr;
                name_len = (size_t)(p - attr);
            }
            if (!value && key_len == 3 && !memcmp(key, "val", 3)) {
                value = attr;
                value_len = (size_t)(p - attr);
            }
            p++;
        }
    }
    return -1;
}

int hf_line_says_kept(const char *line, size_t n) {
    if (!line) return 0;
    const char *stop = line + n, *p = hf_find(line, n, "pioneer=");
    if (p) stop = p;
    for (const char *t = line; t < stop;) {
        while (t < stop && *t == ' ') t++;
        const char *e = t;
        while (e < stop && *e != ' ') e++;
        if (e - t == 6 && !memcmp(t, "kept=1", 6)) return 1;
        t = e;
    }
    return 0;
}

/* keptNameOK: 1-64 of A-Z a-z 0-9 - _ */
static int hf_kept_name_ok(const char *s, size_t n) {
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

typedef struct {
    char (*names)[65];
    size_t n, cap;
    int nomem;
} hf_kept_list;

/* one name in the kept folder: <name>.kept, a regular file (the item itself), keptNameOK */
static int hf_kept_one(void *ctx, const char *name, const plat_stat *st) {
    hf_kept_list *l = ctx;
    size_t k = strlen(name);
    if (k <= 5 || name[0] == '.' || memcmp(name + k - 5, ".kept", 5) || !hf_kept_name_ok(name, k - 5) || st->kind != PLAT_FILE) return 0;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        char (*m)[65] = realloc(l->names, cap * sizeof *m);
        if (!m) {
            l->nomem = 1;
            return 1;
        }
        l->names = m;
        l->cap = cap;
    }
    memcpy(l->names[l->n], name, k - 5);
    l->names[l->n++][k - 5] = 0;
    return 0;
}

static int hf_name_cmp(const void *a, const void *b) { return strcmp(a, b); }

static int hf_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/* Validate the entire file as helper/kept.c's parse does before trusting its expiry. */
static int hf_kept_until(const char *b, size_t n, double *until) {
    static const char head[] = "rbstemsplus-kept 1\nuntil ";
    const size_t first = sizeof head - 1;
    if (n < first || memcmp(b, head, first)) return 0;
    size_t i = first;
    int64_t seconds = 0;
    for (; i < n && i - first < 12 && b[i] >= '0' && b[i] <= '9'; i++)
        seconds = seconds * 10 + (b[i] - '0');
    if (i == first || i == n || b[i] != '\n' || (b[first] == '0' && i - first > 1)) return 0;
    i++;
    const size_t row_size = 130;
    if ((n - i) % row_size) return 0;
    for (; i < n; i += row_size) {
        if (b[i + 64] != ' ' || b[i + 129] != '\n') return 0;
        for (size_t j = 0; j < 64; j++)
            if (!hf_lower_hex(b[i + j]) || !hf_lower_hex(b[i + 65 + j])) return 0;
    }
    *until = (double)seconds;
    return 1;
}

/* Count "file " at the start of each line, even when its prefix spans two reads. */
static size_t hf_manifest_files(const char *path) {
    static const char marker[] = "file ";
    const size_t marker_len = sizeof marker - 1;
    char block[64 << 10];
    plat_file f;
    if (plat_file_open_path(path, &f)) return 0;
    size_t got, files = 0, matched = 0;
    int err;
    while (!(err = plat_read(f, block, sizeof block, &got)) && got) {
        for (size_t i = 0; i < got; i++) {
            if (block[i] == '\n') matched = 0;
            else if (matched < marker_len) {
                /* After either a mismatch or a full match, skip to the next newline. */
                if (block[i] != marker[matched]) matched = marker_len;
                else if (++matched == marker_len) files++;
            }
        }
    }
    plat_file_close(f);
    return err ? 0 : files;
}

int host_facts_kept_sets(const char *kept, double now, void (*date)(void *ctx, double t, char *out, size_t n), void *ctx, char **out) {
    *out = NULL;
    hf_kept_list l;
    memset(&l, 0, sizeof l);
    plat_dir d;
    if (kept && !plat_dir_open(kept, &d)) {
        plat_dir_each(d, hf_kept_one, &l);   /* a folder not read whole: the sets read so far */
        plat_dir_close(d);
    }
    if (l.nomem) {
        free(l.names);
        return -1;
    }
    if (l.n) qsort(l.names, l.n, sizeof *l.names, hf_name_cmp);
    size_t cap = 256 + l.n * 128, len = 0;
    char *t = malloc(cap);
    if (!t) {
        free(l.names);
        return -1;
    }
    t[0] = 0;
    for (size_t i = 0; i < l.n; i++) {
        char p[1200], when[256] = "";
        char *b = NULL;
        size_t bn = 0, files = 0;
        double until = 0;
        int has_until = 0;
        if (snprintf(p, sizeof p, "%s" SEP "%s.kept", kept, l.names[i]) >= (int)sizeof p) continue;
        if (!plat_read_file(p, HF_KEPT_MAX, &b, &bn)) has_until = hf_kept_until(b, bn, &until);
        free(b);
        if (has_until && until <= now) continue;   /* its date has passed */
        if (has_until && date) date(ctx, until, when, sizeof when);
        if (snprintf(p, sizeof p, "%s" SEP "%s.manifest", kept, l.names[i]) < (int)sizeof p)
            files = hf_manifest_files(p);
        char line[400];
        int k = snprintf(line, sizeof line, "%s%s\t%s\t%zu", len ? "\n" : "", l.names[i], when, files);
        if (k < 0 || (size_t)k >= sizeof line) continue;
        if (len + (size_t)k + 1 > cap) {
            size_t nc = cap * 2 + (size_t)k;
            char *m = realloc(t, nc);
            if (!m) {
                free(t);
                free(l.names);
                return -1;
            }
            t = m;
            cap = nc;
        }
        memcpy(t + len, line, (size_t)k + 1);
        len += (size_t)k;
    }
    free(l.names);
    *out = t;
    return 0;
}

void host_facts_free(host_facts *f) {
    free(f->inventory);
    free(f->assessment);
    free(f->cache_manifest);
    f->inventory = NULL;
    f->assessment = NULL;
    f->cache_manifest = NULL;
    free(f->config_text);
    f->config_text = NULL;
    f->config_n = 0;
}

/* ---------------------------------------------------------------- the sheet */

static void hf_str(core_facts *out, core_fact f, const char *s) { core_fact_str(out, f, s, CORE_NTS); }

int host_facts_write(const host_facts *f, core_facts *out) {
    if (f->has_admin) core_fact_flag(out, FACT_USER_IS_ADMIN, f->admin);
    if (f->has_own_bundle) core_fact_flag(out, FACT_OWN_BUNDLE, f->own_bundle);
    core_fact_flag(out, FACT_CHOSEN_MODEL, f->chosen_model);
    core_fact_flag(out, FACT_CHOSEN_CACHE, f->chosen_cache);
    core_fact_flag(out, FACT_REKORDBOX_INSTALLED, f->installed);
    hf_str(out, FACT_REKORDBOX_VERSION, f->version);
    hf_str(out, FACT_ENGINE, f->engine);
    core_fact_flag(out, FACT_ENGINE_SUPPORTED, f->engine_supported);
    core_fact_flag(out, FACT_STEMS_PLUS_PRESENT, f->plus_present);
    core_fact_flag(out, FACT_STEMS_CACHE_PRESENT, f->cache_present);
#ifdef CORE_WIN_POLICY   /* a Windows row: the Mac tests' core has no such fact */
    core_fact_flag(out, FACT_CACHE_RECORD_OK, f->cache_record_ok);
    if (f->inventory && !f->inventory->complete) core_fact_flag(out, FACT_LIB_UNCHECKED, 1);
    core_fact_str(out, FACT_WIN_RB_ROWS, f->win_rows, CORE_NTS);
#endif
    core_fact_flag(out, FACT_CACHE_TRACES, f->cache_traces);
    if (f->has_bridge_models) hf_str(out, FACT_BRIDGE_MODELS, f->bridge_models);
    {   /* BLOCKED "cache"'s rows: rekordbox supported always (decided before any file read), the
         * root's ORT copy once it was looked at, Pioneer's ORT only with an answer (HF7) */
        const native_blocked *b = &f->blocked;
        core_fact_flag(out, FACT_REKORDBOX_SUPPORTED, b->rekordbox_supported);
        if (b->rekordbox_supported && b->has_root_ort) core_fact_flag(out, FACT_ROOT_ORT_EXISTS, b->root_ort_exists);
#ifdef CORE_WIN_POLICY   /* a Windows row: the Mac tests' core has no such fact */
        if (b->rekordbox_supported && !f->cache_answered && (!f->inventory || f->inventory->complete))
        core_fact_flag(out, FACT_LIB_UNCHECKED, 1);
#endif
        if (b->rekordbox_supported && f->cache_answered) {
            if (b->has_lib) {
                core_fact_flag(out, FACT_LIB_EXISTS, b->lib_exists);
                core_fact_flag(out, FACT_LIB_SIGNED, b->lib_signed);
#ifdef CORE_WIN_POLICY   /* a Windows row: the Mac tests' core has no such fact */
                core_fact_flag(out, FACT_ORT_LISTED, b->ort_listed);
#endif
                if (b->ort_version[0]) hf_str(out, FACT_ORT_VERSION, b->ort_version);
            }
        }
    }
    if (f->has_model) {
        core_fact_i64(out, FACT_MODEL_SIZE, f->model_size);
        core_fact_i64(out, FACT_MODEL_MTIME, f->model_mtime_ns);
    }
    core_fact_flag(out, FACT_HELPER_THERE, f->helper_there);
    core_fact_flag(out, FACT_APPLE_SILICON, 1);   /* this machine runs the helper: every supported Windows (host_facts.h) */
    if (f->has_prepare) {
        core_fact_flag(out, FACT_BRIDGE_KEEPS, f->bridge_keeps);
        core_fact_i64(out, FACT_STEMS_QUALITY, f->stems_quality);
        char ours[65 * 2 + 1];
        snprintf(ours, sizeof ours, "%s%s%s", MODELS_OURS_BUILTIN, f->saved_model[0] ? "\n" : "", f->saved_model);
        hf_str(out, FACT_OUR_MODELS, ours);
        if (f->has_ort_there) core_fact_flag(out, FACT_ROOT_ORT_EXISTS, f->ort_there);   /* not written above */
    }
    if (f->config_text) core_fact_str(out, FACT_CONFIG_TEXT, f->config_text, f->config_n);
    if (f->config_unreadable[0]) hf_str(out, FACT_CONFIG_UNREADABLE, f->config_unreadable);
    core_fact_flag(out, FACT_CONFIG_ENABLED, f->config_enabled);
    core_fact_flag(out, FACT_CONFIG_RB_MODEL, f->config_rb_model);
    char num[32];
    fmt_config_number(f->config_max_gb, num);
    hf_str(out, FACT_CONFIG_MAX_GB, num);
    fmt_config_number(f->config_max_days, num);
    hf_str(out, FACT_CONFIG_MAX_DAYS, num);
    core_fact_i64(out, FACT_CONFIG_LOCATION, f->config_location);
    if (f->config_location == HF_CONFIG_CHOSEN) hf_str(out, FACT_CONFIG_DIR, f->config_dir);
    hf_str(out, FACT_CHOSEN_LOCATION, f->config_location == HF_CONFIG_CHOSEN ? f->config_dir : "");
    return (f->inventory && !f->inventory->complete) || (f->blocked.rekordbox_supported && !f->cache_answered);
}

#ifdef _WIN32
/* keptSets()' until date as the Status tab shows it: the user's long date format, as DATE_TEXT's
 * (GetDateFormatEx: Windows has no medium style; the long one names the month, as the Mac's
 * medium does), of its local day */
void host_facts_kept_date(void *ctx, double t, char *out, size_t n) {
    (void)ctx;
    out[0] = 0;
    if (!(t > -11644473600.0 && t < 32503680000.0)) return;   /* 1601 to 3000: SYSTEMTIME's range, and sane */
    struct tm tm;
    int64_t secs = (int64_t)t;
    if ((double)secs > t) secs--;   /* floor */
    plat_localtime(secs, &tm);
    SYSTEMTIME s;
    memset(&s, 0, sizeof s);
    s.wYear = (WORD)(tm.tm_year + 1900);
    s.wMonth = (WORD)(tm.tm_mon + 1);
    s.wDay = (WORD)tm.tm_mday;
    wchar_t w[128];
    int k = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &s, NULL, w, 128, NULL);
    if (k < 2 || wr_utf16_to_8((const uint16_t *)w, (size_t)(k - 1), out, n) <= 0) out[0] = 0;
}

#endif

/* ---------------------------------------------------------------- the DLL's export */

#if (defined(_WIN32) && defined(CORE_NATIVE_WIN)) || defined(HOST_FACTS_TEST_HOOKS)
#include "core_win.h"

typedef struct {
    host_facts_env e;
    char root[1100], rbs[1100], apps[1100], settings[1100];
} hf_env_buf;

#if defined(_WIN32) && defined(CORE_NATIVE_WIN)
#include "native.h"
#include "core_native_admin.h"   /* native_user_is_admin, native_key_texts */

enum { HF_KEYS_MAX = 4 };
static sig_pubkey hf_keys[HF_KEYS_MAX];
static size_t hf_nkeys;
static int hf_keys_read;
static plat_mutex hf_keys_lock = PLAT_MUTEX_INIT;   /* the warm (a worker) and main may both be first */

enum { HF_PENDING_MAX = 16 << 20 };   /* a value past this: no answer (traces) */

/* the Session Manager's PendingFileRenameOperations(2) (host_facts_env.pending_deletes): the
 * pairs as hf_pending_parse's tagged entries, read only (the worker never edits them, J7). A value
 * that isn't there is no pairs; one that can't be read whole, or isn't REG_MULTI_SZ: no answer. */
static int hf_pending_deletes(void *ctx, char **out, size_t *n) {
    (void)ctx;
    static const wchar_t *const names[2] = {L"PendingFileRenameOperations", L"PendingFileRenameOperations2"};
    *out = NULL;
    *n = 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return -1;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int fail = 0;
    for (int v = 0; v < 2 && !fail; v++) {
        wchar_t *w = NULL;
        DWORD type = 0, size = 0;
        LONG r = ERROR_MORE_DATA;
        for (int tries = 0; r == ERROR_MORE_DATA && tries < 3; tries++) {   /* the value may grow meanwhile */
            r = RegQueryValueExW(k, names[v], NULL, &type, NULL, &size);
            if (r != ERROR_SUCCESS) break;
            if (type != REG_MULTI_SZ || size > HF_PENDING_MAX) {
                r = ERROR_INVALID_DATA;
                break;
            }
            free(w);
            w = malloc((size_t)size + sizeof(wchar_t));
            if (!w) {
                r = ERROR_OUTOFMEMORY;
                break;
            }
            r = RegQueryValueExW(k, names[v], NULL, &type, (BYTE *)w, &size);
            if (r == ERROR_SUCCESS && type != REG_MULTI_SZ) r = ERROR_INVALID_DATA;
        }
        if (r == ERROR_FILE_NOT_FOUND) {
            free(w);
            continue;
        }
        if (r != ERROR_SUCCESS) {
            free(w);
            fail = 1;
            break;
        }
        /* the pairs, in queue order: this value's after the one before (the Session Manager is
         * taken to run PendingFileRenameOperations before PendingFileRenameOperations2) */
        if (hf_pending_parse((const uint16_t *)w, size / sizeof(wchar_t), &buf, &len, &cap)) fail = 1;
        free(w);
    }
    RegCloseKey(k);
    if (fail) {
        free(buf);
        return -1;
    }
    *out = buf;
    *n = len;
    return 0;
}

/* the start config's paths as host_facts reads them; valid while c is */
static void hf_env(core *c, hf_env_buf *b) {
    plat_mutex_lock(&hf_keys_lock);
    if (!hf_keys_read) {   /* once */
        hf_keys_read = 1;
        for (int i = 0; i < native_n_key_texts && hf_nkeys < HF_KEYS_MAX; i++)
            if (!sig_key_from_text(&native_key_texts[i], &hf_keys[hf_nkeys])) hf_nkeys++;
    }
    plat_mutex_unlock(&hf_keys_lock);
    const char *pf = native_core_programs(c), *app = native_core_path(c, "app");
    int have_pf = pf && wr_program_path(pf, WR_PF_ROOT, b->root, sizeof b->root) >= 0
               && wr_program_path(pf, WR_PF_REKORDBOX, b->rbs, sizeof b->rbs) >= 0;
    host_facts_env *e = &b->e;
    memset(e, 0, sizeof *e);
    e->os = compat_os_windows();
    e->programs_rekordbox = have_pf ? b->rbs : NULL;
    e->root = have_pf ? b->root : NULL;
    e->model = native_core_path(c, "model");
    e->support = native_core_path(c, "support");
    e->helper = native_core_path(c, "helper");
    /* rekordbox3.settings: in the folder that holds models\demucs3_model\hdemucs.onnx (the Mac's
     * rbSettingsDir, whose modelDir is rbSettingsDir + "/models/demucs3_model") */
    if (e->model && snprintf(b->settings, sizeof b->settings, "%s", e->model) < (int)sizeof b->settings) {
        char *cut = b->settings;
        for (int up = 0; up < 3 && cut; up++) {
            cut = strrchr(b->settings, '\\');
            if (cut) *cut = 0;
        }
        if (cut && cut != b->settings && strlen(b->settings) + sizeof "\\rekordbox3.settings" <= sizeof b->settings) {
            strcat(b->settings, "\\rekordbox3.settings");
            e->rb_settings = b->settings;
        }
    }
    e->own = native_core_version(c);
    e->keys = hf_keys;
    e->nkeys = hf_nkeys;
    e->admin = native_user_is_admin();
    e->own_bundle = -1;
    e->pending_deletes = hf_pending_deletes;
    if (app && e->own && e->support && snprintf(b->apps, sizeof b->apps, "%s", app) < (int)sizeof b->apps) {
        char *cut = strrchr(b->apps, '\\');   /* the app's own folder is <apps>\<version> */
        if (cut && cut != b->apps) {
            *cut = 0;
            update_env ue;
            memset(&ue, 0, sizeof ue);
            ue.support = e->support;
            ue.apps = b->apps;
            ue.own = e->own;
            ue.os = update_os_windows();
            e->own_bundle = update_in_place(&ue) == 0;
        }
    }
}

/* CORE_FACTS_KEPT: keptSets() from paths.kept, now */
static void hf_write_kept(core *c, core_facts *out) {
    char *t = NULL;
    if (host_facts_kept_sets(native_core_path(c, "kept"), (double)plat_now_wall_ns() / 1e9, host_facts_kept_date, NULL, &t)) {
        native_core_log(c, "facts: out of memory reading the kept sets: none shown");
        return;
    }
    core_fact_str(out, FACT_KEPT_SETS, t, CORE_NTS);
    free(t);
}
#else
/* The native tests exercise the export's OOM path before the Windows readers run. */
static void hf_env(core *c, hf_env_buf *b) {
    (void)c;
    (void)b;
    abort();
}
static void hf_write_kept(core *c, core_facts *out) {
    (void)c;
    (void)out;
    abort();
}
static void native_core_log(core *c, const char *line) {
    (void)c;
    (void)line;
}
#endif

#ifdef HOST_FACTS_TEST_HOOKS
int host_facts_test_env_alloc_fails;
#endif

/* a file the warm didn't leave memoised, read on the UI thread: said in the log (once per file
 * and stat: compat.c remembers the answer) */
static void hf_miss(void *ctx, const char *path) {
    char line[1200];
    snprintf(line, sizeof line, "facts: %s read on the UI thread (not warm)", path);
    native_core_log((core *)ctx, line);
}

CORE_API int32_t CORE_CALL core_win_cheap_facts(core *c, core_facts *out) {
    if (!c || !out) return -1;
    uint32_t parts = core_facts_parts(out);
    if (parts & CORE_FACTS_KEPT) hf_write_kept(c, out);
    if (!(parts & CORE_FACTS_REFRESH)) return -1;   /* the kept sets alone: FACT_LIB_UNCHECKED not read */
    /* Fail closed until host_facts_write answers: assume Stems Cache's traces (HR2), and tell
     * Prepare that this machine can run the helper so it can report the missing helper on OOM. */
    core_fact_flag(out, FACT_CACHE_TRACES, 1);
    core_fact_flag(out, FACT_APPLE_SILICON, 1);
#ifdef HOST_FACTS_TEST_HOOKS
    hf_env_buf *b = host_facts_test_env_alloc_fails ? NULL : malloc(sizeof *b);
#else
    hf_env_buf *b = malloc(sizeof *b);
#endif
    if (!b) {
        native_core_log(c, "facts: out of memory: the cheap facts are unset, Stems Cache's traces assumed");
        return 0;
    }
    hf_env(c, b);
    b->e.prepare = (parts & CORE_FACTS_PREPARE) != 0;
    b->e.on_main = 1;
    b->e.miss = hf_miss;
    b->e.miss_ctx = c;
    host_facts f;
    int unchecked = 0;
    if (!host_facts_read(&b->e, &f)) unchecked = host_facts_write(&f, out);
    else native_core_log(c, "facts: out of memory reading the cheap facts: the rows read so far are unset, Stems Cache's traces assumed");
    host_facts_free(&f);
    free(b);
    return unchecked;
}
#endif

#if defined(_WIN32) && defined(CORE_NATIVE_WIN)
static int host_read_core(struct core *c, int fresh, const payload_manifest *manifest, host_facts *out) {
    memset(out, 0, sizeof *out);
    hf_env_buf *b = c ? malloc(sizeof *b) : NULL;
    if (!b) return -1;
    hf_env(c, b);
    b->e.fresh_inventory = fresh;
    b->e.scope_manifest = manifest;
    int r = host_facts_read(&b->e, out);
    free(b);
    return r;
}

int host_facts_read_core(struct core *c, host_facts *out) { return host_read_core(c, 0, NULL, out); }

int host_facts_read_scope(struct core *c, const payload_manifest *manifest, host_facts *out) {
    return host_read_core(c, 1, manifest, out);
}

CORE_API void CORE_CALL core_win_warm_facts(core *c) {
    if (!c) return;
    hf_env_buf *b = malloc(sizeof *b);
    if (!b) return;
    hf_env(c, b);
    host_facts f;
    host_facts_read(&b->e, &f);   /* compat.c's memo keeps the files' facts; the rows go */
    host_facts_free(&f);
    free(b);
    nw_steps_refresh_inventory(c);
}
#endif
