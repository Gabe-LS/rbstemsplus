/* The resident watcher's decisions (watch.h): the folders, the schedule and the check, over the
 * core's own watcher decisions. No Win32 here (watch_win.c). */
#include "watch.h"

#ifndef RBSP_CORE_PLAT_SHIM_H
#  define RBSP_CORE_PLAT_SHIM_H   /* common/plat.h (through compat.h) has the shim's names */
#endif
#include "internal.h"   /* the core's watcher_*, state_*, keyset, tbuf, arena */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ names */

static int is_sep(char c) { return c == '\\' || c == '/'; }

static const char *first_sep(const char *s) {
    for (; *s; s++)
        if (is_sep(*s)) return s;
    return NULL;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* a[0..an) and b equal, ASCII case-blind (NTFS names compare case-blind; ours are ASCII) */
static int equal_ci_n(const char *a, size_t an, const char *b) {
    size_t bn = strlen(b);
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (lower(a[i]) != lower(b[i])) return 0;
    return 1;
}

/* the last component of path */
static const char *last_name(const char *path) {
    const char *n = path;
    for (const char *p = path; *p; p++)
        if (is_sep(*p) && p[1]) n = p + 1;
    return n;
}

/* path's parent, in place: 1, or 0 when it has none ("C:\" and "/" have none; "C:\x" -> "C:\") */
static int parent(char *p) {
    size_t n = strlen(p);
    while (n > 1 && is_sep(p[n - 1]) && !(n == 3 && p[1] == ':')) p[--n] = 0;   /* trailing separators */
    if ((n == 3 && p[1] == ':' && is_sep(p[2])) || (n == 1 && is_sep(p[0])) || n == 0) return 0;
    size_t i = n;
    while (i > 0 && !is_sep(p[i - 1])) i--;
    if (i == 0) return 0;
    if (i == 3 && p[1] == ':') p[3] = 0;   /* keep the drive's own separator */
    else if (i == 1) p[1] = 0;              /* "/" */
    else p[i - 1] = 0;
    return 1;
}

/* ------------------------------------------------------------------ what a notification asks for */

int watch_kind_flags(int kind) {
    switch (kind) {
    case WD_REKORDBOX: return WATCH_ROUND | WATCH_R1;
    case WD_ROOT: return WATCH_R1;
    case WD_PIONEER: return WATCH_ROUND;
    default: return 0;
    }
}

/* every folder with its subtree: a handle is held only on the top folder that may be renamed or
 * removed, never below it (NTFS refuses to rename a folder while a handle is open anywhere below
 * it, whatever its share modes; the folder's own handle, shared for delete, doesn't stop it) */
int watch_subtree(int kind) { return kind >= 0 && kind < WD_COUNT; }

/* names and writes (the model replaced in place); the settings folder's writes are dropped by
 * watch_classify, so it is seen by names only */
int watch_filter(int kind) {
    (void)kind;
    return WATCH_F_NAMES | WATCH_F_WRITES;
}

/* a short (8.3) name as NTFS makes them ("REKORD~1", "DEMUCS~1"): a note may carry a folder's
 * short name instead of its long one (FILE_NOTIFY_INFORMATION), so it stands for any name */
static int short_name(const char *s, size_t n) {
    if (n > 12) return 0;
    for (size_t i = 0; i + 1 < n; i++)
        if (s[i] == '~' && s[i + 1] >= '0' && s[i + 1] <= '9') return 1;
    return 0;
}

/* name below prefix ("a\b"), components ASCII case-blind, a short name matching any: -1 not
 * below it, 0 prefix itself, else how many components deeper */
static int depth_below(const char *name, const char *prefix) {
    const char *a = name, *b = prefix;
    for (;;) {
        size_t an = strcspn(a, "\\/"), bn = strcspn(b, "\\/");
        if (!an) return -1;
        int same = short_name(a, an) || short_name(b, bn);
        for (size_t i = 0; !same && an == bn && i <= an; i++)
            if (i == an) same = 1;
            else if (lower(a[i]) != lower(b[i])) break;
        if (!same) return -1;
        a += an, b += bn;
        if (!*b) break;
        if (!*a) return -1;
        a++, b++;
    }
    if (!*a) return 0;
    if (!a[1]) return -1;
    int d = 1;
    for (const char *p = a + 1; *p; p++)
        if (is_sep(*p) && p[1]) d++;
    return d;
}

int watch_classify(int kind, const char *current, int action, const char *name) {
    (void)current;
    if (!name || !*name) return 0;
    const char *sep = first_sep(name);
    size_t c = sep ? (size_t)(sep - name) : strlen(name);
    const char *rest = sep && sep[1] ? sep + 1 : NULL;
    int deeper = rest && first_sep(rest);
    char comp[WATCH_PATH_MAX];
    if (c >= sizeof comp) return 0;
    memcpy(comp, name, c);
    comp[c] = 0;
    switch (kind) {
    case WD_REKORDBOX: {
        char v[COMPAT_VERSION_MAX];
        if (deeper || !compat_folder_version(comp, v, sizeof v)) return 0;
        /* a version folder added, removed or renamed: an update (or an uninstall) */
        if (!rest && action != WATCH_MODIFIED) return WATCH_ROUND | WATCH_R1;
        if (rest && (equal_ci_n(rest, strlen(rest), "rekordbox.exe") ||
                     equal_ci_n(rest, strlen(rest), "onnxruntime.dll"))) return WATCH_ROUND | WATCH_R1;
        return 0;
    }
    case WD_ROOT: return !deeper && equal_ci_n(comp, c, "ort") ? WATCH_R1 : 0;
    case WD_PIONEER: {
        /* the settings folder, the model folder, or a folder on the way to them, added, removed
         * or renamed (its own times don't count) */
        int m = depth_below(WATCH_MODELS_BELOW, name);
        if (m >= 0) return action != WATCH_MODIFIED ? WATCH_ROUND : 0;
        /* a name directly in the settings folder, by names only (rekordbox writes its database
         * there while it runs), as the Mac's WatchPaths sees a folder */
        if (depth_below(name, WATCH_SETTINGS_BELOW) == 1) return action != WATCH_MODIFIED ? WATCH_ROUND : 0;
        /* a name directly in the model folder, writes too */
        return depth_below(name, WATCH_MODELS_BELOW) == 1 ? WATCH_ROUND : 0;
    }
    default: return 0;
    }
}

/* ------------------------------------------------------------------ the folders */

__attribute__((format(printf, 2, 3))) static void elog(const watch_engine *e, const char *fmt, ...) {
    char b[3 * WATCH_PATH_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (e->fs->log) e->fs->log(e->fs->ctx, b);
}

static void close_slot(watch_engine *e, int slot) {
    if (!e->open[slot]) return;
    e->fs->close(e->fs->ctx, slot);
    e->open[slot] = 0;
}

static int open_own(watch_engine *e, int k) {
    if (e->fs->open(e->fs->ctx, k, e->path[k], watch_subtree(k), watch_filter(k))) return 0;
    e->open[k] = 1;
    return 1;
}

/* Opens kind k's folder, or watches its nearest existing ancestor until the next component
 * appears. 1 when the folder itself is open. walk 0 (the tick): an anchor still open and still
 * the folder at its path is kept, and only the folder itself is tried; walk 1 (the anchor saw
 * the next component, or went), or no valid anchor: the deepest ancestor that opens is looked
 * for again. A line is logged only when what is watched changes. */
static int reach(watch_engine *e, int k, int walk) {
    int a = k + WD_COUNT;
    if (!walk && !(e->open[a] && e->fs->same(e->fs->ctx, a, e->anchor[k]))) walk = 1;
    for (int tries = 0; tries < 2; tries++) {
        if (open_own(e, k)) {
            if (e->open[a]) {
                close_slot(e, a);
                elog(e, "watching %s again", e->path[k]);
            }
            e->anchor[k][0] = 0;
            return 1;
        }
        if (!walk || tries) return 0;
        char old[WATCH_PATH_MAX], p[WATCH_PATH_MAX];
        int had = e->open[a];
        snprintf(old, sizeof old, "%s", had ? e->anchor[k] : "");
        close_slot(e, a);
        e->anchor[k][0] = 0;
        snprintf(p, sizeof p, "%s", e->path[k]);
        int found = 0;
        while (parent(p)) {
            if (!e->fs->open(e->fs->ctx, a, p, 0, WATCH_F_NAMES)) {
                e->open[a] = 1;
                snprintf(e->anchor[k], sizeof e->anchor[k], "%s", p);
                if (strcmp(old, p)) elog(e, "%s isn't there: watching %s until it is", e->path[k], p);
                found = 1;
                break;
            }
        }
        if (!found) {
            if (had || !e->seen[k]) elog(e, "%s isn't there, nor any folder above it that can be watched", e->path[k]);
            return 0;
        }
        /* it may have appeared while the ancestor was being opened: once more */
    }
    return 0;
}

void watch_engine_init(watch_engine *e, const watch_fs *fs, const char *const paths[WD_COUNT]) {
    memset(e, 0, sizeof *e);
    e->fs = fs;
    for (int k = 0; k < WD_COUNT; k++) snprintf(e->path[k], sizeof e->path[k], "%s", paths[k] ? paths[k] : "");
}

int watch_engine_tick(watch_engine *e) {
    int flags = 0;
    for (int k = 0; k < WD_COUNT; k++) {
        if (!e->path[k][0]) continue;
        if (e->open[k] && !e->fs->same(e->fs->ctx, k, e->path[k])) {
            close_slot(e, k);
            elog(e, "%s was removed or replaced", e->path[k]);
            flags |= watch_kind_flags(k);
            e->anchor[k][0] = 0;
        }
        /* one that appears after the first look asks for its round */
        if (!e->open[k] && reach(e, k, 0) && e->seen[k]) flags |= watch_kind_flags(k);
        e->seen[k] = 1;
    }
    return flags;
}

int watch_engine_slot_open(const watch_engine *e, int slot) { return slot >= 0 && slot < WATCH_SLOTS && e->open[slot]; }

void watch_engine_set_current(watch_engine *e, const char *folder) {
    snprintf(e->current, sizeof e->current, "%s", folder && *folder ? last_name(folder) : "");
}

int watch_engine_read(watch_engine *e, int slot, int status, const watch_note *notes, int n) {
    if (slot < 0 || slot >= WATCH_SLOTS || !e->open[slot]) return 0;   /* a read of a slot closed since */
    int k = slot % WD_COUNT, flags = 0;
    if (slot < WD_COUNT) {
        if (status == WATCH_READ_OK) {
            for (int i = 0; i < n; i++) flags |= watch_classify(k, e->current, notes[i].action, notes[i].name);
            if (e->fs->same(e->fs->ctx, k, e->path[k])) return flags;
            elog(e, "%s was removed or replaced", e->path[k]);
        } else if (status == WATCH_READ_OVERFLOW) {
            elog(e, "%s: too many changes to list", e->path[k]);
        } else {
            elog(e, "%s went away or can't be watched now", e->path[k]);
        }
        /* closed at once (it may be deleted or replaced: never hold it), then opened again from
         * its path, or watched from its parent */
        close_slot(e, k);
        flags |= watch_kind_flags(k);
        reach(e, k, 1);
        return flags;
    }
    /* an ancestor: does the next component below it appear? */
    int hit = status != WATCH_READ_OK;
    const char *below = e->path[k] + strlen(e->anchor[k]);
    while (is_sep(*below)) below++;
    const char *end = first_sep(below);
    size_t bn = end ? (size_t)(end - below) : strlen(below);
    char next[WATCH_PATH_MAX];
    if (bn >= sizeof next) bn = sizeof next - 1;
    memcpy(next, below, bn);
    next[bn] = 0;
    for (int i = 0; i < n && !hit; i++) {
        const char *nm = notes[i].name;
        if (!nm || (notes[i].action != WATCH_ADDED && notes[i].action != WATCH_RENAMED_NEW)) continue;
        const char *s = first_sep(nm);
        if (equal_ci_n(nm, s ? (size_t)(s - nm) : strlen(nm), next)) hit = 1;
    }
    /* the anchor itself replaced or renamed away: looked for again (an anchor gone, refused or
     * overflowing is a hit too: the walk closes it and looks again) */
    if (!hit && !e->fs->same(e->fs->ctx, slot, e->anchor[k])) hit = 1;
    if (hit && reach(e, k, 1)) flags |= watch_kind_flags(k);
    return flags;
}

/* ------------------------------------------------------------------ the schedule */

void watch_sched_note(watch_sched *s, int flags) { s->pending |= flags; }

void watch_sched_registry(watch_sched *s, uint64_t generation, int periodic) {
    if (generation != s->registry_gen || periodic) watch_sched_note(s, WATCH_REGISTRY);
    s->registry_gen = generation;
}

int watch_sched_poll(watch_sched *s, int64_t now_ms, int64_t *wait_ms) {
    *wait_ms = -1;
    if (s->running || !s->pending) return 0;
    if (s->started && now_ms - s->last_start < WATCH_THROTTLE_MS) {
        *wait_ms = WATCH_THROTTLE_MS - (now_ms - s->last_start);
        return 0;
    }
    s->running = 1;
    s->started = 1;
    s->last_start = now_ms;
    return 1;
}

int watch_sched_take(watch_sched *s) {
    int f = s->pending;
    s->pending = 0;
    return f;
}

void watch_sched_done(watch_sched *s) { s->running = 0; }

/* ------------------------------------------------------------------ the check */

__attribute__((format(printf, 2, 3))) static void olog(const watch_ops *o, const char *fmt, ...) {
    char b[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    o->log(o->ctx, b);
}

static const char *live_word(int live) {
    switch (live) {
    case COMPAT_LIVE_MISSING: return "missing";
    case COMPAT_LIVE_PIONEER: return "pioneer";
    case COMPAT_LIVE_OURS: return "ours";
    case COMPAT_LIVE_FOREIGN: return "foreign";
    case COMPAT_LIVE_OURS_UNLISTED: return "ours-unlisted";
    default: return "unreadable";
    }
}

size_t watch_r1_key(const char *version, const compat_r1 *r, char *out, size_t n) {
    int k = snprintf(out, n, "r1:%s live=%s ort=%d", version ? version : "", live_word(r->live), r->ort_copy ? 1 : 0);
    return k < 0 ? 0 : (size_t)k;
}

/* what R1 found, for the log */
static void r1_words(char *b, size_t n, const char *v, const compat_r1 *r) {
    switch (r->live) {
    case COMPAT_LIVE_MISSING: snprintf(b, n, "rekordbox %s's onnxruntime.dll is missing", v); break;
    case COMPAT_LIVE_FOREIGN: snprintf(b, n, "rekordbox %s's onnxruntime.dll is neither Pioneer's nor RB Stems Plus's", v); break;
    case COMPAT_LIVE_OURS_UNLISTED: snprintf(b, n, "rekordbox %s's onnxruntime.dll is a bridge this version doesn't list", v); break;
    default: snprintf(b, n, "the bridge is in rekordbox %s, but ort\\'s copy of Pioneer's file is missing", v); break;
    }
}

/* key in a newline-separated list */
static int listed_line(const char *list, const char *key) {
    size_t kn = strlen(key);
    for (const char *p = list; *p;) {
        const char *nl = strchr(p, '\n');
        size_t ln = nl ? (size_t)(nl - p) : strlen(p);
        if (ln == kn && !memcmp(p, key, kn)) return 1;
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

/* key added to a newline-separated list of cap bytes; when it is full, the oldest lines go first */
static void add_line(char *list, size_t cap, const char *key) {
    size_t kn = strlen(key);
    if (listed_line(list, key) || kn + 2 > cap) return;
    size_t n = strlen(list);
    while (n + kn + 2 > cap) {
        char *nl = strchr(list, '\n');
        size_t drop = nl ? (size_t)(nl - list) + 1 : n;
        memmove(list, list + drop, n - drop + 1);
        n -= drop;
    }
    memcpy(list + n, key, kn);
    list[n + kn] = '\n';
    list[n + kn + 1] = 0;
}

static int listed_engine(const char *e, const payload_manifest *m) {
    if (!e || !m) return 0;
    for (int i = 0; i < m->n_win_stems_engine; i++)
        if (!strcmp(e, m->win_stems_engine[i])) return 1;
    return 0;
}

/* "a, b" */
static void join_keys(char *b, size_t n, const pstr *k, int32_t nk) {
    b[0] = 0;
    size_t at = 0;
    for (int32_t i = 0; i < nk; i++) {
        int w = snprintf(b + at, n - at, "%s%.*s", i ? ", " : "", (int)k[i].n, k[i].s);
        if (w < 0 || (size_t)w >= n - at) return;
        at += (size_t)w;
    }
}

/* the unsupported key of k (unsupportedKey: the Windows lists, no ORT prefix: Windows lists ORT
 * by sha256, which the key leaves out) into b */
static void unsupported(tbuf *b, const pstr *k, const payload_manifest *m) {
    const char *rb[PAYLOAD_LIST_MAX], *en[PAYLOAD_LIST_MAX];
    int nrb = m ? m->n_win_rekordbox : 0, nen = m ? m->n_win_stems_engine : 0;
    for (int i = 0; i < nrb; i++) rb[i] = m->win_rekordbox[i];
    for (int i = 0; i < nen; i++) en[i] = m->win_stems_engine[i];
    watcher_unsupported_key(b, k->s, k->n, rb, nrb, en, nen, "");
}

static int prefixed(const pstr *k, const char *p) {
    size_t n = strlen(p);
    return k->n >= n && !memcmp(k->s, p, n);
}

/* dismissed's new text with add (keyset_union_text), written through remember */
static void remember(const watch_ops *o, const watch_facts *f, const keyset *have, const pstr *add, int32_t n) {
    size_t cap = f->dismissed_n + 64;
    for (int32_t i = 0; i < n; i++) cap += add[i].n + 1;
    char *b = malloc(cap);
    if (!b) {
        o->log(o->ctx, "couldn't remember the answer: out of memory");
        return;
    }
    tbuf t;
    tb_init(&t, (core_os)o->text_os, b, cap);
    keyset_union_text(&t, have, add, n);
    if (t.err || o->remember(o->ctx, b, t.len)) o->log(o->ctx, "couldn't remember the answer");
    free(b);
}

/* a text in the column os; 0 when it isn't written yet (or doesn't fit) */
#ifdef WATCH_TEST_HOOKS
#ifdef _WIN32
__attribute__((section(".data$rbsp_test_watch_text")))
#endif
int watch_test_unwritten_text = -1;
#endif

static int os_text(int os, core_text_id id, char *b, size_t n) {
#ifdef WATCH_TEST_HOOKS
    if ((int)id == watch_test_unwritten_text) {   /* as a text whose column is "" (core_text: CORE_E_NOTYET) */
        if (n) b[0] = 0;
        return 0;
    }
#endif
    tbuf t;
    tb_init(&t, (core_os)os, b, n);
    TB_TEXT0(&t, id);
    return !t.err && t.len < n;
}

static void sha_text(plat_sha256_ctx *hash, const char *s) {
    plat_sha256_update(hash, s, strlen(s) + 1);
}

static int row_digest(const inv_row_view *r, unsigned char out[32]) {
    plat_sha256_ctx hash;
    if (plat_sha256_init(&hash)) return -1;
    int values[] = {r->state, r->why, r->r1, r->running};
    plat_sha256_update(&hash, values, sizeof values);
    sha_text(&hash, r->version);
    sha_text(&hash, r->name);
    sha_text(&hash, r->display_version);
    return plat_sha256_final(&hash, out);
}

/* A strict subset of the previous rows is a silent removal, even when the candidate changes. */
static int observe_rows(const watch_ops *o, watch_state *st, const watch_facts *f,
                        const unsigned char list[32], int *flags) {
    unsigned char rows[INV_MAX][32], digest[32];
    for (int i = 0; i < f->nrows; i++)
        if (row_digest(&f->rows[i], rows[i])) return 0;
    plat_sha256_ctx hash;
    if (plat_sha256_init(&hash)) return 0;
    plat_sha256_update(&hash, rows, (size_t)f->nrows * sizeof rows[0]);
    plat_sha256_update(&hash, &f->rb_state, sizeof f->rb_state);
    plat_sha256_update(&hash, &f->inv_complete, sizeof f->inv_complete);
    sha_text(&hash, f->folder);
    sha_text(&hash, f->watch_name);
    sha_text(&hash, f->rekordbox_version);
    if (plat_sha256_final(&hash, digest)) return 0;

    /* A disk-only bridge stays visible for cleanup after its registered installation goes. */
    int nrows = 0;
    for (int i = 0; i < f->nrows; i++) {
        const inv_row_view *row = &f->rows[i];
        if (row->why == WHY_DELISTED_BRIDGE && row->r1 == 0) continue;
        if (nrows != i) memcpy(rows[nrows], rows[i], sizeof rows[i]);
        nrows++;
    }
    int same_list = st->observed && !memcmp(st->list_digest, list, sizeof st->list_digest);
    int changed = !st->observed || memcmp(st->rows_digest, digest, sizeof digest);
    int removal = changed && same_list && st->inv_complete && f->inv_complete && nrows < st->nrows;
    for (int i = 0; removal && i < nrows; i++) {
        int j = 0;
        while (j < st->nrows && memcmp(rows[i], st->row_digests[j], sizeof rows[i])) j++;
        if (j == st->nrows) removal = 0;
    }
    st->observed = 1;
    st->nrows = nrows;
    st->inv_complete = f->inv_complete;
    memcpy(st->rows_digest, digest, sizeof digest);
    memcpy(st->row_digests, rows, (size_t)nrows * sizeof rows[0]);
    memcpy(st->list_digest, list, sizeof st->list_digest);
    if (changed) {
        if (o->refresh_app) o->refresh_app(o->ctx);
        *flags |= WATCH_R1;
    }
    return !removal && (changed || !same_list || *flags != WATCH_REGISTRY);
}

static void request_app(const watch_ops *o, const char *name, const char *features) {
    switch (o->request(o->ctx, features)) {
    case 0: break;
    case WATCH_NO_REQUEST: olog(o, "couldn't write the %s request", name); break;
    default: o->log(o->ctx, "couldn't open RB Stems Plus"); break;
    }
}

static int ask_cache(const watch_ops *o, const watch_facts *f, watch_set *s, const keyset *dismissed,
                     int unsupported_row, const char *list_id) {
    char keys[INV_MAX][COMPAT_VERSION_MAX + 80], permanent[INV_MAX][COMPAT_VERSION_MAX + 16];
    pstr versions[INV_MAX], add[INV_MAX], dont_ask[INV_MAX];
    int n = 0;
    for (int i = 0; i < f->nrows; i++) {
        const inv_row_view *row = &f->rows[i];
        if (row->state != (unsupported_row ? INV_ROW_UNSUPPORTED : INV_ROW_READY) || row->r1 != 0) continue;
        size_t vn = strlen(row->version);
        int pn = snprintf(permanent[n], sizeof permanent[n], "%s:%s", unsupported_row ? "unsupported" : "cache", row->version);
        if (keyset_has(dismissed, permanent[n], (size_t)pn)) continue;
        int kn = unsupported_row ? snprintf(keys[n], sizeof keys[n], "unsupported:%s:%s", list_id, row->version)
                                 : snprintf(keys[n], sizeof keys[n], "%s", permanent[n]);
        if (keyset_has(dismissed, keys[n], (size_t)kn) || listed_line(s->put_off, keys[n])) continue;
        versions[n] = (pstr){row->version, vn};
        add[n] = (pstr){keys[n], (size_t)kn};
        dont_ask[n] = (pstr){permanent[n], (size_t)pn};
        n++;
    }
    if (!n) return 1;

    char title[256], labels[3][128], version_list[INV_MAX * (COMPAT_VERSION_MAX + 2)];
    char message[sizeof version_list + 256];
    join_keys(version_list, sizeof version_list, versions, n);
    tbuf b;
    tb_init(&b, CORE_OS_WIN, message, sizeof message);
    core_slot slot = SLOT_S(rekordbox, version_list);
    tb_text(&b, unsupported_row ? T_WIN_W_UNSUPPORTED : T_WIN_W_ADD_CACHE, &slot, 1);
    core_text_id dismiss = n == 1 ? T_W_DONT_ASK : T_WIN_W_DONT_ASK_THESE;
    core_text_id ids[] = {unsupported_row ? T_OK : T_WIN_W_INSTALL_NOW, unsupported_row ? dismiss : T_W_REMIND_LATER, dismiss};
    int nb = unsupported_row ? 2 : 3;
    int ok = !b.err && os_text(o->text_os, T_W_TITLE_UPDATED, title, sizeof title);
    const char *buttons[3];
    for (int i = 0; ok && i < nb; i++) {
        ok = os_text(o->text_os, ids[i], labels[i], sizeof labels[i]);
        buttons[i] = labels[i];
    }
    if (!ok) {
        o->log(o->ctx, "the Stems Cache question's texts aren't written for Windows yet: not asking");
        return 1;
    }

    olog(o, "asking about Stems Cache in rekordbox %s", version_list);
    int pressed = o->ask(o->ctx, title, message, buttons, nb);
    olog(o, "user chose: %s", pressed >= 0 && pressed < nb ? buttons[pressed] : "");
    if (pressed == 0 && !unsupported_row) {
        request_app(o, "cache", "cache");
        return 0;
    }
    if (pressed == nb - 1) remember(o, f, dismissed, dont_ask, n);
    else if (unsupported_row && pressed == 0) remember(o, f, dismissed, add, n);
    for (int i = 0; i < n; i++) add_line(s->put_off, sizeof s->put_off, keys[i]);
    return 1;
}

static int check_cache(const watch_ops *o, watch_set *s, const watch_facts *f,
                       const unsigned char list[32], int flags, int *opened) {
    arena a = {0};
    keyset dismissed = {0};
    if (keyset_parse(&a, f->dismissed, f->dismissed_n, &dismissed)) {
        arena_free(&a);
        return 1;
    }
    if (flags & WATCH_R1) {
        for (int i = 0; i < f->nrows; i++) {
            const inv_row_view *row = &f->rows[i];
            if (row->state != INV_ROW_REPAIR || row->r1 <= 0) continue;
            char key[COMPAT_VERSION_MAX + 32], vkey[COMPAT_VERSION_MAX + 8];
            snprintf(key, sizeof key, "%s:%d:%d", row->version, row->why, row->r1);
            watch_r1_dismissed_key(row->version, vkey, sizeof vkey);
            if (keyset_has(&dismissed, vkey, strlen(vkey)) || listed_line(s->st->r1_opened, key)) continue;
            if (!*opened) {
                olog(o, "rekordbox %s needs repair: opening Manage", row->version);
                if (o->open_app(o->ctx)) {
                    o->log(o->ctx, "couldn't open RB Stems Plus");
                    break;
                }
                *opened = 1;
            }
            add_line(s->st->r1_opened, sizeof s->st->r1_opened, key);
        }
    }
    int next = 1;
    if (f->chosen_cache && f->have_lists) {
        if (f->inv_complete) next = ask_cache(o, f, s, &dismissed, 0, NULL);
        if (next) {
            char list_id[65];
            for (int i = 0; i < 32; i++) snprintf(list_id + i * 2, 3, "%02x", (unsigned)list[i]);
            next = ask_cache(o, f, s, &dismissed, 1, list_id);
        }
    }
    arena_free(&a);
    return next;
}

int watch_check(const watch_ops *o, watch_set *s, int flags) {
    watcher_settle_ops so = {o->ctx, o->updating, o->app_busy, o->rekordbox_open, o->pause, o->half_uninstalled, o->in_session, o->log};
    if (!watcher_settle(&so)) {
        /* busy, or an uninstall left half-done: what came in is dropped, as the Mac's watcher
         * exits here; the next notification starts a set again */
        (void)o->take(o->ctx);
        return 0;
    }
    /* what this check sees starts here: a notification before now is covered by it */
    flags |= o->take(o->ctx);
    watch_facts f;
    memset(&f, 0, sizeof f);
    if (o->facts(o->ctx, &f)) return 1;
    if (f.rb_unsettled) {
        /* An installer can publish the registry entry before its files or finish it after the
         * folder arrives. Give that round one more read, within the same process and busy gates. */
        o->pause(o->ctx, 2);
        if (!watcher_settle(&so)) return 0;
        flags |= o->take(o->ctx);
        memset(&f, 0, sizeof f);
        if (o->facts(o->ctx, &f)) return 1;
    }
    if (o->set_current) o->set_current(o->ctx, f.watch_name);
    int legacy = 0, opened = 0;
#ifdef WATCH_TEST_HOOKS
    legacy = o->text_os == CORE_OS_MAC;
#endif
    int supported = payload_win_rekordbox_supported(f.m, f.rekordbox_version);
    unsigned char compatibility[32];
    if (!legacy) {
        if (inv_list_digest(f.m, compatibility) || !observe_rows(o, s->st, &f, compatibility, &flags)) return 1;
    } else {
        watch_state *st = s->st;
        int changed = !st->observed || st->rb_state != f.rb_state || st->cache_present != f.cache_present ||
                      st->supported != supported || st->live != f.r1.live || st->ort_copy != f.r1.ort_copy ||
                      st->offer_restore != f.r1.offer_restore || strcmp(st->observed_folder, f.folder) ||
                      strcmp(st->observed_version, f.rekordbox_version);
        if (changed) {
            st->observed = 1;
            st->rb_state = f.rb_state;
            st->cache_present = f.cache_present;
            st->supported = supported;
            st->live = f.r1.live;
            st->ort_copy = f.r1.ort_copy;
            st->offer_restore = f.r1.offer_restore;
            snprintf(st->observed_folder, sizeof st->observed_folder, "%s", f.folder);
            snprintf(st->observed_version, sizeof st->observed_version, "%s", f.rekordbox_version);
            if (o->refresh_app) o->refresh_app(o->ctx);
            if (flags & WATCH_REGISTRY) flags |= WATCH_R1;
        } else if (flags == WATCH_REGISTRY) return 1;
    }
    if (!legacy) {
        char states[INV_STATE_TEXT_MAX];
        inv_state_text(f.rows, f.nrows, states, sizeof states);
        olog(o, "state: rekordbox %s; RB Stems Plus Watcher %s, engine %s, Demucs v4 %s; chosen: model=%s cache=%s",
             states, f.version ? f.version : "", f.engine, f.plus_present ? "on" : "off",
             f.chosen_model ? "true" : "false", f.chosen_cache ? "true" : "false");
    } else {
        olog(o, "run: RB Stems Plus Watcher %s; rekordbox %s, engine %s, Demucs v4 %s, Stems Cache %s; chosen: model=%s cache=%s",
             f.version ? f.version : "", f.rekordbox_version, f.engine, f.plus_present ? "on" : "off", f.cache_present ? "on" : "off",
             f.chosen_model ? "true" : "false", f.chosen_cache ? "true" : "false");
    }

    if (!legacy) {
        if (!check_cache(o, s, &f, compatibility, flags, &opened)) return 0;
        f.chosen_cache = 0;
        f.r1_known = 0;
    }
    /* nothing is asked about a rekordbox that isn't there or isn't verified (the app says why) */
    if (f.rb_state == COMPAT_RB_NOT_INSTALLED) {
        o->log(o->ctx, "rekordbox isn't installed: nothing to ask");
        return 1;
    }
    if (f.rb_state != COMPAT_RB_OK) {
        if (!f.rekordbox_version[0]) o->log(o->ctx, "rekordbox couldn't be checked now: not asking");   /* review RE3: no candidate */
        else
            olog(o, f.rb_state == COMPAT_RB_UNVERIFIED ? "couldn't verify rekordbox %s: not asking" : "rekordbox %s couldn't be checked now: not asking",
                 f.rekordbox_version);
        return 1;
    }

    /* R1 holds: Pioneer's file should be put back. A bridge counts as "unlisted" only against a
     * verified list, or when ort\'s copy was looked at and is missing (rekordbox can't start
     * either way) */
    int r1 = f.r1_known && f.r1.offer_restore &&
             (f.have_lists || f.r1.live != COMPAT_LIVE_OURS_UNLISTED || (f.r1.ort_looked && !f.r1.ort_copy));
    /* R1 (I72): the app is opened only in a round a version-folder or ort\ notification asked for */
    if ((flags & WATCH_R1) && r1) {
        char key[256], words[512], vkey[128];
        watch_r1_key(f.rekordbox_version, &f.r1, key, sizeof key);
        watch_r1_dismissed_key(f.rekordbox_version, vkey, sizeof vkey);
        r1_words(words, sizeof words, f.rekordbox_version, &f.r1);
        if (f.dismissed && listed_line(f.dismissed, vkey))
            olog(o, "%s: not opening RB Stems Plus (Don't Ask Again for this version)", words);
        else if (listed_line(s->st->r1_opened, key))
            olog(o, "%s: RB Stems Plus was opened for it already", words);
        else {
            olog(o, "%s: opening RB Stems Plus to put Pioneer's file back", words);
            if (!o->open_app(o->ctx)) {
                add_line(s->st->r1_opened, sizeof s->st->r1_opened, key);
                return 0;
            }
            o->log(o->ctx, "couldn't open RB Stems Plus");
        }
    }

    arena a = {0};
    keyset dismissed = {0}, put = {0};
    pstr keys[2], kept[2];
    int32_t nk = -1, nkept = 0;
    if (keyset_parse(&a, f.dismissed, f.dismissed_n, &dismissed) == 0 && keyset_parse(&a, s->put_off, strlen(s->put_off), &put) == 0)
        nk = state_missing_keys(&a, f.chosen_model, f.chosen_cache, f.cache_present, f.plus_present, f.rekordbox_version, f.engine,
                                &dismissed, keys);
    if (nk < 0) {
        o->log(o->ctx, "not checking: out of memory");
        arena_free(&a);
        return 1;
    }
    /* while R1 holds, the app's own offer is the one: Stems Cache's reinstall question waits (the
     * reinstall wouldn't put Pioneer's file back); Demucs v4's stays */
    if (r1) {
        int32_t w = 0;
        for (int32_t i = 0; i < nk; i++)
            if (prefixed(&keys[i], "cache:")) olog(o, "%.*s isn't asked while Pioneer's file should be put back", (int)keys[i].n, keys[i].s);
            else keys[w++] = keys[i];
        nk = w;
    }
    /* less what the user was already told can't be reinstalled yet */
    char ub[1024];
    for (int32_t i = 0; i < nk; i++) {
        tbuf u;
        tb_init(&u, (core_os)o->text_os, ub, sizeof ub);
        unsupported(&u, &keys[i], f.m);
        if (u.err || !keyset_has(&dismissed, ub, u.len)) kept[nkept++] = keys[i];
    }
    char list[512];
    join_keys(list, sizeof list, kept, nkept);
    int r = 1;
    int32_t q = watcher_ask(kept, nkept, &put);
    if (q == WATCH_NOTHING) o->log(o->ctx, "nothing to ask");
    else if (q == WATCH_PUT_OFF) olog(o, "still missing (%s), put off a moment ago: not asking again now", list);
    else if (!f.have_lists) olog(o, "missing (%s), but no verified payload.json says what can be reinstalled: not asking", list);
    else {
        int model_off = 0, cache_off = 0;
        for (int32_t i = 0; i < nkept; i++) {
            if (prefixed(&kept[i], "model:")) model_off = 1;
            if (prefixed(&kept[i], "cache:")) cache_off = 1;
        }
        char mb[2048], title[256], bt[3][128];
        tbuf m;
        tb_init(&m, (core_os)o->text_os, mb, sizeof mb);
        watcher_dialog d;
        watcher_dialog_build(&m, &d, model_off, cache_off, f.engine, f.rekordbox_version, listed_engine(f.engine, f.m),
                             supported, f.chosen_model && f.plus_present);
        int ok = !m.err && os_text(o->text_os, d.title, title, sizeof title);
        const char *buttons[3];
        for (int32_t i = 0; ok && i < d.nbuttons; i++) {
            ok = os_text(o->text_os, d.buttons[i], bt[i], sizeof bt[i]);
            buttons[i] = bt[i];
        }
        if (!ok) {
            olog(o, "missing (%s), but the question's texts aren't written for Windows yet: not asking", list);
        } else {
            olog(o, "asking: %s", list);
            int pressed = o->ask(o->ctx, title, mb, buttons, d.nbuttons);
            olog(o, "user chose: %s", pressed >= 0 && pressed < d.nbuttons ? buttons[pressed] : "");
            switch (watcher_answer(&d, pressed)) {
            case WANSWER_REINSTALL:
                /* the app does the reinstall: it reads the request at start, or when it is
                 * started again while open */
                request_app(o, "reinstall", d.reinstall);
                r = 0;
                break;
            case WANSWER_REMEMBER:
                remember(o, &f, &dismissed, kept, nkept);
                break;
            case WANSWER_REMEMBER_UNSUPPORTED: {
                /* "not supported yet": nothing to do until a newer version list arrives */
                char uk[2][1024];
                pstr add[2];
                int32_t nadd = 0;
                for (int32_t i = 0; i < nkept; i++) {
                    tbuf u;
                    tb_init(&u, (core_os)o->text_os, uk[nadd], sizeof uk[nadd]);
                    unsupported(&u, &kept[i], f.m);
                    if (!u.err) {
                        add[nadd] = (pstr){uk[nadd], u.len};
                        nadd++;
                    }
                }
                remember(o, &f, &dismissed, add, nadd);
                break;
            }
            default: {
                char pb[sizeof s->put_off];
                tbuf t;
                tb_init(&t, (core_os)o->text_os, pb, sizeof pb);
                keyset_union_text(&t, &put, kept, nkept);
                if (!t.err) memcpy(s->put_off, pb, t.len + 1);
                break;
            }
            }
        }
    }
    arena_free(&a);
    return opened ? 0 : r;
}

typedef struct {
    const watch_ops *o;
    watch_set *s;
} rounds_ctx;

static int32_t r_check(void *c) {
    rounds_ctx *r = c;
    int f = r->s->stash;
    r->s->stash = 0;
    return watch_check(r->o, r->s, f);
}
static void r_release(void *c) { (void)c; }
/* a notification came in during the check: its flags are the next round's */
static int32_t r_again(void *c) {
    rounds_ctx *r = c;
    r->s->stash |= r->o->take(r->o->ctx);
    return r->s->stash != 0;
}
/* one process holds every round: nothing to take again */
static int32_t r_retake(void *c) {
    (void)c;
    return 1;
}
static void r_log(void *c, const char *line) {
    rounds_ctx *r = c;
    r->o->log(r->o->ctx, line);
}

int watch_rounds(const watch_ops *o, watch_state *st) {
    watch_set s;
    memset(&s, 0, sizeof s);
    s.st = st;
    rounds_ctx r = {o, &s};
    watcher_round_ops ro = {&r, r_check, r_release, r_again, r_retake, r_log};
    int n = watcher_rounds(WATCH_MAX_ROUNDS, &ro);
    /* taken for a round that didn't come (the set's last): the next set's (core/watcher.c, which
     * the Mac shares, stays as it is) */
    if (s.stash) o->give_back(o->ctx, s.stash);
    return n;
}

/* ------------------------------------------------------------------ the minimum, small rules */

const char *watch_os_refusal(uint32_t build, uint32_t release) {
    if (build == 0) return "not running: this Windows's version couldn't be read";
    if (build < WATCH_MIN_BUILD) return "not running: RB Stems Plus needs Windows 11, version 22H2 or later";
    if (release < WATCH_MIN_NET_RELEASE) return "not running: RB Stems Plus needs .NET Framework 4.8.1 or later";
    return NULL;
}

size_t watch_log_line(char *out, size_t cap, int y, int mo, int d, int h, int mi, int s, const char *line) {
    int k = snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d  %s\n", y, mo, d, h, mi, s, line ? line : "");
    if (k < 0) return 0;
    return (size_t)k < cap ? (size_t)k : cap ? cap - 1 : 0;
}

size_t watch_stop_event_name(uint32_t pid, char *out, size_t n) {
    int k = snprintf(out, n, "Local\\io.github.rbstemsplus.watcher.stop.%u", (unsigned)pid);
    return k > 0 && (size_t)k < n ? (size_t)k : 0;
}

int watch_stub_path(const char *exe, char *out, size_t n) {
    char p[WATCH_PATH_MAX];
    if (!exe || strlen(exe) >= sizeof p) return 0;
    snprintf(p, sizeof p, "%s", exe);
    /* the exe's folder (the version folder), then its parent (the install's folder) */
    if (!parent(p) || !parent(p)) return 0;
    size_t pn = strlen(p);
    const char *sep = pn && is_sep(p[pn - 1]) ? "" : "\\";
    int k = snprintf(out, n, "%s%sRB Stems Plus.exe", p, sep);
    return k > 0 && (size_t)k < n;
}
