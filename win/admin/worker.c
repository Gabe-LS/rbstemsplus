/* The elevated worker's runs (worker.h): install, uninstall, restore, install-worker and
 * remove-root-folder, in pure C11 over plat.h, payload.c, compat.c, inventory.c, receipt.c,
 * pe_code.c and admin_os. */
#include "worker.h"

#include "inventory.h"
#include "pe_code.h"
#include "receipt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define SEP "\\"
#else
#  define SEP "/"      /* the Mac tests: a scratch tree */
#endif

static const char LIVE[] = "onnxruntime.dll";
static const char NEW[] = "onnxruntime.dll.rbsp-new";
static const char PROBE[] = "onnxruntime.dll.rbsp-probe";
static const char SUMS[] = "SHA256SUMS";
static const char STAGED_BRIDGE[] = "bridge.dll";
static const char STAGED_WORKER[] = "worker.exe";
static const char UNDO_STAGE[] = "undo-stage.";
static const char MARKER_SYMBOL[] = "rbstems_marker";
static const char ORT_SYMBOL[] = "OrtGetApiBase";

enum { NAMES_MAX = 128, RUN_PATH = ADMIN_PATH_MAX + 64, JOURNAL_MAX = 32 };
enum { HOLD_MOVED = -100 };
enum { MADE_FILE, MADE_DIR, MADE_REPLACED };   /* run.made's kinds */   /* hold_version: the name holds another folder than the inventory saw (Windows' CRT has no ESTALE) */

typedef struct {
    const admin_env *e;
    const admin_os *os;
    const admin_args *a;
    plat_dir root, pioneer, ort, bin, stage, rbroot, vdir, receipts;
    void *lock;
    char self[ADMIN_PATH_MAX], self12[13];
    int self_in_bin;
    char stage_name[32];
    char vname[COMPAT_PATH_MAX], version[COMPAT_VERSION_MAX];   /* the version folder being changed (vdir) */
    payload_manifest *m;
    int have_m;
    unsigned char *pj, *ps;   /* the staged payload.json and its signature as verified (verify_payload), kept until committed */
    size_t pjn, psn;
    const receipt *rc;        /* the receipt of the folder being looked at: its bridge counts as ours */
    char ort_code[65];        /* ort\onnxruntime.dll's PE code digest as last read or written, "" none (receipt.h) */
    inv_list *inv;            /* the fresh inventory (malloc'd) */
    char new_worker12[13];    /* a worker this run put into bin\ */
    int root_made, lock_made; /* this run's own root_open made the root, its lock call the lock file */
    /* what an Install made or replaced in the root until its first target changes (re-review
     * blockers A, B): a refused one takes away the entries it made and puts back the files it
     * replaced, newest first, under the lock */
    struct {
        char in[2][COMPAT_VERSION_MAX];   /* its folder under the root: "" "" the root itself */
        char name[COMPAT_VERSION_MAX];
        int kind;                         /* MADE_* */
        char kept[24];                    /* MADE_REPLACED: the old bytes' copy in the staging folder */
    } made[JOURNAL_MAX];
    int n_made;
    int committed;            /* the payload committed (commit_payload): the root was written */
    int touched;              /* a target's live DLL was changed (repaired or swapped) */
    int verified_change;      /* this run repaired, installed or restored a target and verified it */
    int worker_staged;        /* the app's worker staged and checked (stage_worker), not yet in bin\ */
    int changing;             /* the folder being changed (vdir) was changed and isn't verified yet */
    char auth[65];            /* the original authorized to go back into vdir (its sha256): the rollback's */
    int load_failed;          /* the live bridge (or ort\) failed its load check: never "nothing changed" */
} run;

/* ------------------------------------------------------------------ small things */

static void say(run *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(run *r, const char *fmt, ...) {
    char line[ADMIN_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (r->os->say) r->os->say(r->os->ctx, line);
}

static int join(char *out, size_t cap, const char *a, const char *b) {
    int k = snprintf(out, cap, "%s" SEP "%s", a, b);
    return k > 0 && (size_t)k < cap;
}
static int join3(char *out, size_t cap, const char *a, const char *b, const char *c) {
    int k = snprintf(out, cap, "%s" SEP "%s" SEP "%s", a, b, c);
    return k > 0 && (size_t)k < cap;
}

static int join4(char *out, size_t cap, const char *a, const char *b, const char *c, const char *d) {
    int k = snprintf(out, cap, "%s" SEP "%s" SEP "%s" SEP "%s", a, b, c, d);
    return k > 0 && (size_t)k < cap;
}

static void hex(const unsigned char *d, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) out[2 * i] = h[d[i] >> 4], out[2 * i + 1] = h[d[i] & 15];
    out[2 * n] = 0;
}

static int sha_hex(const void *p, size_t n, char out[65]) {
    unsigned char d[32];
    if (plat_sha256(p, n, d)) {
        out[0] = 0;
        return -1;
    }
    hex(d, 32, out);
    return 0;
}

static void close_dir(plat_dir *d) {
    if (plat_dir_ok(*d)) plat_dir_close(*d);
    *d = plat_dir_cwd();
}

int admin_sums_line(const char *sha, char *out, size_t cap) {
    if (!payload_is_sha256(sha)) return 0;
    int k = snprintf(out, cap, "%s  %s\n", sha, LIVE);
    return k > 0 && (size_t)k < cap;
}

int admin_classify(int present, int exists, int readable, int marker, int pin, int listed, int complete) {
    if (!present) return complete ? ADMIN_LIVE_ABSENT : ADMIN_LIVE_NOTHING;
    if (!exists) return ADMIN_LIVE_REFUSED;   /* a folder, a link or a reparse point in its place */
    if (!readable) return ADMIN_LIVE_NO_ANSWER;
    if (!marker) {
        if (pin == COMPAT_PIN_OK) return ADMIN_LIVE_PIONEER;
        if (pin != COMPAT_PIN_NO) return ADMIN_LIVE_NO_ANSWER;
    } else if (listed && complete)
        return ADMIN_LIVE_OURS;
    return complete ? ADMIN_LIVE_HALF_RUN : ADMIN_LIVE_REFUSED;
}

int admin_root_acl_exact(int owner, int is_protected, int present, const admin_ace *a, size_t n) {
    static const struct { int sid; uint32_t mask; } want[3] = {
        {ADMIN_SID_SYSTEM, ADMIN_ROOT_FULL}, {ADMIN_SID_ADMINS, ADMIN_ROOT_FULL}, {ADMIN_SID_USERS, ADMIN_ROOT_READ}};
    if (owner != ADMIN_SID_ADMINS || !is_protected || !present || n != 3) return 0;
    for (size_t i = 0; i < 3; i++)
        if (a[i].type != 0 || a[i].flags != ADMIN_ROOT_INHERIT || a[i].sid != want[i].sid || a[i].mask != want[i].mask) return 0;
    return 1;
}

int admin_lock_acl_exact(int owner, int is_protected, int present, const admin_ace *a, size_t n) {
    static const int want[2] = {ADMIN_SID_SYSTEM, ADMIN_SID_ADMINS};
    if (owner != ADMIN_SID_ADMINS || !is_protected || !present || n != 2) return 0;
    for (size_t i = 0; i < 2; i++)
        if (a[i].type != 0 || a[i].flags != 0 || a[i].sid != want[i] || a[i].mask != ADMIN_ROOT_FULL) return 0;
    return 1;
}

/* a whole file `name` in d, at most max bytes: 0 with *buf (malloc'd) and *n; ENOENT; PLAT_EFTYPE
 * (not a regular file, or over max); another errno */
static int read_whole(plat_dir d, const char *name, int64_t max, unsigned char **buf, size_t *n) {
    plat_file f;
    int64_t size = 0;
    *buf = NULL;
    *n = 0;
    int e = plat_file_open_at(d, name, max, &size, &f);
    if (e) return e;
    unsigned char *b = malloc(size ? (size_t)size : 1);
    if (!b) e = ENOMEM;
    else if (size && (e = plat_read_all(f, b, (size_t)size))) {
        free(b);
        b = NULL;
    }
    plat_file_close(f);
    if (e) return e;
    *buf = b;
    *n = (size_t)size;
    return 0;
}

/* a new file `name` in d (which must not exist) with data, flushed to the disk: 0 or an errno (the
 * file removed again) */
static int write_new(plat_dir d, const char *name, const void *data, size_t n) {
    plat_file f;
    int e = plat_file_create_at(d, name, 1, &f);
    if (e) return e;
    void *addr = NULL;
    if (n && !(e = plat_map_rw(f, (int64_t)n, &addr))) {
        memcpy(addr, data, n);
        e = plat_map_flush(addr, (int64_t)n);   /* the view written, then the file flushed below (review WF4) */
        plat_unmap(addr, (int64_t)n);
    }
    if (!e) e = plat_fsync(f);
    plat_file_close(f);
    if (e) plat_unlink_at(d, name);
    return e;
}

/* name replaced atomically: a temp name in d, flushed, renamed over it */
static int write_atomic(run *r, plat_dir d, const char *name, const void *data, size_t n) {
    char tmp[96], rnd[9];
    r->os->random_hex(r->os->ctx, rnd, 8);
    snprintf(tmp, sizeof tmp, "%s.%s.part", name, rnd);
    int e = write_new(d, tmp, data, n);
    if (!e && (e = plat_rename_at(d, tmp, d, name))) plat_unlink_at(d, tmp);
    return e;
}

static int unlink_quiet(plat_dir d, const char *name) {
    int e = plat_unlink_at(d, name);
    return e == ENOENT ? 0 : e;
}

/* the names in d (at most NAMES_MAX, each < 256 bytes), with their kinds */
typedef struct {
    char name[NAMES_MAX][256];
    int kind[NAMES_MAX];
    int n, over;
} names;
static int collect(void *ctx, const char *name, const plat_stat *st) {
    names *l = ctx;
    if (l->n == NAMES_MAX || strlen(name) >= 256) {
        l->over = 1;
        return 0;
    }
    memcpy(l->name[l->n], name, strlen(name) + 1);
    l->kind[l->n++] = st->kind;
    return 0;
}
static int list_dir(plat_dir d, names *l) {
    memset(l, 0, sizeof *l);
    return plat_dir_each(d, collect, l) ? EIO : 0;
}

/* removes `name` in d and everything under it, never following a link or a reparse point (one
 * there is removed itself): 0, or the first errno */
static int rm_tree(run *r, plat_dir d, const char *name, int depth) {
    plat_stat st;
    int e = plat_stat_at(d, name, &st);
    if (e) return e == ENOENT ? 0 : e;
    if (st.kind != PLAT_DIR) {
        e = plat_unlink_at(d, name);
        if (e && e != ENOENT) e = r->os->rmdir_at(r->os->ctx, d, name);   /* a folder's reparse point */
        return e == ENOENT ? 0 : e;
    }
    if (depth > 8) return ELOOP;
    plat_dir sub;
    if ((e = plat_dir_open_at(d, name, &sub))) return e;
    names *l = malloc(sizeof *l);
    if (!l) {
        plat_dir_close(sub);
        return ENOMEM;
    }
    int first = list_dir(sub, l);
    if (!first && l->over) first = E2BIG;
    for (int i = 0; i < l->n; i++) {
        int x = rm_tree(r, sub, l->name[i], depth + 1);
        if (x && !first) first = x;
    }
    free(l);
    plat_dir_close(sub);
    if (first) return first;
    e = r->os->rmdir_at(r->os->ctx, d, name);
    return e == ENOENT ? 0 : e;
}

/* ------------------------------------------------------------------ what an Install made (re-review blocker A) */

/* An Install notes each entry it makes in the root (its own mkdir or file creation, never one
 * that was there) and keeps a copy of each file it replaces there, until its first target
 * changes; a refused one takes away exactly what it made and puts back what it replaced, newest
 * first, under the lock (admin_run): it never removes another run's work (re-review blocker A),
 * and an existing root ends as it was (blocker B) */
static int noting(const run *r) { return r->a->verb == ADMIN_INSTALL && !r->touched; }

/* room to note one more: 0, or ENOSPC (the caller makes nothing) */
static int note_room(run *r) {
    if (!noting(r) || r->n_made < JOURNAL_MAX) return 0;
    say(r, "too many changes to undo in one run: nothing changed");
    return ENOSPC;
}

/* entry name in in1\in2 under the root ("" "": the root itself) noted (kind MADE_*) */
static void note_made(run *r, const char *in1, const char *in2, const char *name, int kind) {
    if (!noting(r) || r->n_made == JOURNAL_MAX) return;
    snprintf(r->made[r->n_made].in[0], sizeof r->made[0].in[0], "%s", in1);
    snprintf(r->made[r->n_made].in[1], sizeof r->made[0].in[1], "%s", in2);
    snprintf(r->made[r->n_made].name, sizeof r->made[0].name, "%s", name);
    snprintf(r->made[r->n_made].kept, sizeof r->made[0].kept, "undo.%d", r->n_made);
    r->made[r->n_made++].kind = kind;
}

/* the file name in d (in1\in2 under the root), about to be written or removed: noted as made by
 * this run when it isn't there yet, else its bytes copied into the staging folder first, flushed
 * (once per name: the first note is the state before the run). 0, or an errno (nothing is
 * changed then) */
static int note_file(run *r, plat_dir d, const char *in1, const char *in2, const char *name) {
    if (!noting(r)) return 0;
    for (int i = 0; i < r->n_made; i++)
        if (!strcmp(r->made[i].in[0], in1) && !strcmp(r->made[i].in[1], in2) && !strcmp(r->made[i].name, name)) return 0;
    if (note_room(r)) return ENOSPC;
    plat_stat st;
    int e = plat_stat_at(d, name, &st);
    if (e == ENOENT) {
        note_made(r, in1, in2, name, MADE_FILE);
        return 0;
    }
    unsigned char *b = NULL;
    size_t n = 0;
    char kept[24];
    snprintf(kept, sizeof kept, "undo.%d", r->n_made);
    if (!e && !plat_dir_ok(r->stage)) e = EIO;
    if (!e && !(e = read_whole(d, name, ADMIN_ORT_MAX, &b, &n)) && !(e = unlink_quiet(r->stage, kept))) e = write_new(r->stage, kept, b, n);
    free(b);
    if (e) {
        say(r, "couldn't keep a copy of %s (error %d): nothing changed", name, e);
        return e;
    }
    note_made(r, in1, in2, name, MADE_REPLACED);
    return 0;
}

/* in1\in2 under the root, opened: 0 with *d (*own: the caller closes it), or an errno */
static int open_in(run *r, const char *in1, const char *in2, plat_dir *d, int *own) {
    *own = 0;
    *d = r->root;
    if (!in1[0]) return 0;
    plat_dir a;
    int e = plat_dir_open_at(r->root, in1, &a);
    if (e || !in2[0]) {
        if (!e) *d = a, *own = 1;
        return e;
    }
    e = plat_dir_open_at(a, in2, d);
    plat_dir_close(a);
    if (!e) *own = 1;
    return e;
}

/* a file this run replaced, put back from its copy when it differs now: the staged copy renamed
 * over it (the same volume, flushed when it was kept), else written there from its bytes; the
 * copy stays in the staging folder until it is back. 0 or an errno */
static int put_kept(run *r, plat_dir d, const char *name, const char *kept) {
    unsigned char *old = NULL, *now = NULL;
    size_t on = 0, nn = 0;
    int e = read_whole(r->stage, kept, ADMIN_ORT_MAX, &old, &on);
    if (!e && (read_whole(d, name, ADMIN_ORT_MAX, &now, &nn) || nn != on || memcmp(now, old, on)) && plat_rename_at(r->stage, kept, d, name))
        e = write_atomic(r, d, name, old, on);
    free(old);
    free(now);
    return e;
}

/* what this run made in the root (note_made) taken away again and what it replaced put back,
 * newest first: 0, or -1 when an entry stayed (admin_run keeps the staging folder with the saved
 * copies then). The subfolders' held handles are closed first, the staging folder still there */
static int undo_made(run *r) {
    int failed = 0;
    for (int i = r->n_made - 1; i >= 0; i--) {
        plat_dir d;
        int kind = r->made[i].kind;
        int own, e = open_in(r, r->made[i].in[0], r->made[i].in[1], &d, &own);
        if (!e) {
            const char *name = r->made[i].name;
            e = kind == MADE_DIR  ? r->os->rmdir_at(r->os->ctx, d, name)
              : kind == MADE_FILE ? plat_unlink_at(d, name)
                                  : put_kept(r, d, name, r->made[i].kept);
            if (own) plat_dir_close(d);
        }
        if (e && (e != ENOENT || kind == MADE_REPLACED)) {
            say(r, kind == MADE_REPLACED ? "couldn't restore %s (error %d)" : "couldn't take %s away again (error %d)", r->made[i].name, e);
            failed = 1;
        }
    }
    r->n_made = 0;
    return failed ? -1 : 0;
}

/* a held folder `name` in parent, made when missing and create is set (in: the parent's place
 * under the root for note_made, "" the root, NULL not noted): 0 with *out; ENOENT; or
 * ADMIN_EXIT_CHECK_FAILED after saying why */
static int sub_dir(run *r, plat_dir parent, const char *in, const char *name, int create, plat_dir *out) {
    *out = plat_dir_cwd();
    int e = r->os->hold_dir(r->os->ctx, parent, name, out);
    if (e == ENOENT && create) {
        if (in && note_room(r)) return ADMIN_EXIT_CHECK_FAILED;
        e = r->os->mkdir_at(r->os->ctx, parent, name);
        if (!e && in) note_made(r, in, "", name, MADE_DIR);   /* made by this call: one there meanwhile (EEXIST) isn't this run's */
        if (!e || e == EEXIST) e = r->os->hold_dir(r->os->ctx, parent, name, out);
    }
    if (!e) return 0;
    if (e == ENOENT) return ENOENT;
    if (e == ELOOP || e == ENOTDIR || e == EACCES) say(r, "%s isn't a folder of the administrators' alone: nothing changed", name);
    else say(r, "couldn't open %s (error %d): nothing changed", name, e);
    return ADMIN_EXIT_CHECK_FAILED;
}

/* ------------------------------------------------------------------ facts */

static int path_in_version(run *r, const char *vname, const char *file, char *out, size_t cap) {
    return join3(out, cap, r->e->rekordbox, vname, file);
}

/* a file's facts read now (never the memo), pinned when asked */
static int facts(run *r, const char *path, int pin, compat_file *f) {
    return compat_file_facts(r->os->compat, path, COMPAT_FRESH | (pin ? COMPAT_PIN : 0), f);
}

/* ours: this payload's bridge, or the one the folder's own receipt names (an older build) */
static int listed_bridge(run *r, const char *sha) {
    if (!sha[0]) return 0;
    if (r->have_m && !strcmp(sha, r->m->win_bridge.sha256)) return 1;
    return r->rc && !strcmp(sha, r->rc->bridge_sha256);
}

/* a memory compat_read_fn (pe_code_sha256 over bytes read once) */
typedef struct {
    const unsigned char *b;
    size_t n;
} mem_reader;
static int mem_read(void *ctx, uint64_t off, void *buf, size_t n) {
    const mem_reader *m = ctx;
    if (off > m->n || n > m->n - off) return -1;
    memcpy(buf, m->b + (size_t)off, n);
    return 0;
}

/* a file's facts, pinned, read now (facts), with its PE code digest (pe_code.h) from one more
 * read of the same bytes (their sha256 must be the facts'): 0, or -1 (unreadable, or changed in
 * between). f->code_sha256 is "" when the digest doesn't take it */
static int facts_code(run *r, const char *path, compat_file *f) {
    if (facts(r, path, 1, f)) return -1;
    f->code_sha256[0] = 0;
    if (!f->exists || !f->readable) return 0;
    char *b = NULL, got[65];
    size_t n = 0;
    if (plat_read_file(path, (size_t)ADMIN_ORT_MAX, &b, &n) || sha_hex(b, n, got) || strcmp(got, f->sha256)) {
        free(b);
        return -1;
    }
    mem_reader m = {(const unsigned char *)b, n};
    payload_sha code;
    if (!pe_code_sha256(mem_read, &m, n, code)) memcpy(f->code_sha256, code, sizeof code);
    free(b);
    return 0;
}

/* the payload binds originals to versions (mv-spec §2.3): without, no install */
static int bindings_ok(const run *r) { return r->have_m && r->m->n_win_ort_bind > 0 && payload_is_sha256(r->m->win_ort_code_sha256); }

/* ort\'s copy as the shared runtime (mv-spec §2.3): AlphaTheta's, one of the bound originals'
 * bytes, the bound code */
static int runtime_ok(const run *r, const compat_file *o) {
    if (!bindings_ok(r) || !o->exists || !o->readable || o->marker || o->pin != COMPAT_PIN_OK || strcmp(o->code_sha256, r->m->win_ort_code_sha256))
        return 0;
    for (int i = 0; i < r->m->n_win_ort_bind; i++)
        if (!strcmp(o->sha256, r->m->win_ort_bind[i].sha256)) return 1;
    return 0;
}

/* the saved set of version v: complete when pioneer\<v>\onnxruntime.dll and SHA256SUMS are
 * there and agree; its sha in sha (65 bytes) */
static int set_complete(run *r, const char *v, char sha[65]) {
    sha[0] = 0;
    if (!plat_dir_ok(r->pioneer)) return 0;
    plat_dir vs;
    if (r->os->hold_dir(r->os->ctx, r->pioneer, v, &vs)) return 0;
    unsigned char *sums = NULL, *lib = NULL;
    size_t sn = 0, ln = 0;
    int ok = 0;
    char want[160], got[65];
    if (!read_whole(vs, SUMS, 256, &sums, &sn) && !read_whole(vs, LIVE, ADMIN_ORT_MAX, &lib, &ln) && !sha_hex(lib, ln, got)
        && admin_sums_line(got, want, sizeof want) && sn == strlen(want) && !memcmp(sums, want, sn)) {
        ok = 1;
        memcpy(sha, got, 65);
    }
    free(sums);
    free(lib);
    plat_dir_close(vs);
    return ok;
}

/* the live file of version folder vname, classified (bridge.md §2's order); *f its facts */
static int classify(run *r, const char *vname, const char *v, compat_file *f, char set_sha[65]) {
    char live[RUN_PATH];
    if (!path_in_version(r, vname, LIVE, live, sizeof live) || facts(r, live, 0, f)) return ADMIN_LIVE_NO_ANSWER;
    /* the pin goes on Pioneer's file only, never on the live bridge (I6) */
    if (f->exists && f->readable && !f->marker && facts(r, live, 1, f)) return ADMIN_LIVE_NO_ANSWER;
    int complete = set_complete(r, v, set_sha);
    return admin_classify(f->present, f->exists, f->readable, f->marker, f->pin, f->marker && listed_bridge(r, f->sha256), complete);
}

/* rekordbox.exe in version folder vname (version v), pinned now, its ProductVersion agreeing
 * with the folder's and (listed) in this release's signed list (G8): 0 with *f, 4, or 10 */
static int exe_check(run *r, const char *vname, const char *v, int listed, compat_file *f) {
    char exe[RUN_PATH];
    if (!path_in_version(r, vname, "rekordbox.exe", exe, sizeof exe) || facts(r, exe, 1, f) || !f->readable) {
        say(r, "couldn't check rekordbox %s's rekordbox.exe: nothing changed", v);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (f->pin == COMPAT_PIN_ERROR) {
        say(r, "couldn't check rekordbox %s's signature: nothing changed", v);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (f->pin != COMPAT_PIN_OK || !compat_same_short_version(f->pe.product_version, v)) {
        say(r, "rekordbox.exe isn't AlphaTheta's rekordbox %s: reinstall rekordbox with Pioneer's installer. Nothing changed", v);
        return ADMIN_EXIT_NOT_PIONEERS;
    }
    if (listed && (!r->have_m || !payload_win_rekordbox_supported(r->m, f->pe.product_version))) {
        say(r, "rekordbox %s isn't listed by this release: nothing changed", v);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    return 0;
}

/* ------------------------------------------------------------------ the put-back (I49, I71) */

enum { PB_OK = 0, PB_UNCHANGED = -1, PB_CHANGED = -2 };

/* whether vname's path in rekordbox's folder is still the held folder vdir (its identity) */
static int same_folder(run *r, plat_dir vdir, const char *vname) {
    char p[RUN_PATH];
    plat_stat a, b;
    return join(p, sizeof p, r->e->rekordbox, vname) && !plat_dir_stat(vdir, &a) && !plat_stat_path(p, 0, &b) && b.kind == PLAT_DIR && plat_id_same(a.id, b.id);
}

/* Pioneer's file of version v back into the held version folder vdir (named vname), from
 * pioneer\<v>\, and only the original authorized for it (want: its sha256, review finding 3): the
 * saved set's sha256 against SHA256SUMS and want, its pin; copied to onnxruntime.dll.rbsp-new,
 * flushed, its sha256 (want) and pin checked; the folder's path checked to be vdir still; renamed
 * over the live file, or, when there is none, into its place by a rename that fails if one
 * appeared meanwhile; then the result verified again against want. Made in place, the new file
 * has the folder's inherited security. The one routine of the rollback, uninstall and restore
 * (I49). */
static int put_back(run *r, plat_dir vdir, const char *vname, const char *v, const char *want) {
    char sha[65], saved[RUN_PATH], newp[RUN_PATH], live[RUN_PATH];
    if (!set_complete(r, v, sha)) {
        say(r, "no complete saved copy of rekordbox %s's file", v);
        return PB_UNCHANGED;
    }
    if (!want || !payload_is_sha256(want) || strcmp(sha, want)) {
        say(r, "the saved copy of rekordbox %s's file isn't the one checked", v);
        return PB_UNCHANGED;
    }
    if (!join4(saved, sizeof saved, r->e->root, "pioneer", v, LIVE) || !path_in_version(r, vname, NEW, newp, sizeof newp)
        || !path_in_version(r, vname, LIVE, live, sizeof live))
        return PB_UNCHANGED;
    compat_file f;
    if (facts(r, saved, 1, &f) || !f.readable || strcmp(f.sha256, sha) || f.pin != COMPAT_PIN_OK || f.marker) {
        say(r, "the saved copy of rekordbox %s's file isn't AlphaTheta's", v);
        return PB_UNCHANGED;
    }
    plat_dir vs;
    unsigned char *b = NULL;
    size_t n = 0;
    if (r->os->hold_dir(r->os->ctx, r->pioneer, v, &vs)) return PB_UNCHANGED;
    int e = read_whole(vs, LIVE, ADMIN_ORT_MAX, &b, &n);
    plat_dir_close(vs);
    char got[65];
    if (e || sha_hex(b, n, got) || strcmp(got, sha)) {
        free(b);
        say(r, "couldn't read the saved copy of rekordbox %s's file", v);
        return PB_UNCHANGED;
    }
    if (unlink_quiet(vdir, NEW) || (e = write_new(vdir, NEW, b, n))) {
        free(b);
        say(r, "couldn't write into rekordbox %s (error %d)", v, e);
        return PB_UNCHANGED;
    }
    free(b);
    if (facts(r, newp, 1, &f) || strcmp(f.sha256, sha) || f.pin != COMPAT_PIN_OK || f.marker) {
        unlink_quiet(vdir, NEW);
        say(r, "the copy of Pioneer's file in rekordbox %s doesn't verify", v);
        return PB_UNCHANGED;
    }
    if (!same_folder(r, vdir, vname)) {
        unlink_quiet(vdir, NEW);
        say(r, "rekordbox %s's folder changed meanwhile", v);
        return PB_UNCHANGED;
    }
    plat_stat st;
    int present = plat_stat_at(vdir, LIVE, &st) != ENOENT;
    e = present ? plat_rename_at(vdir, NEW, vdir, LIVE) : r->os->rename_noreplace(r->os->ctx, vdir, NEW, LIVE);
    if (e) {
        unlink_quiet(vdir, NEW);
        if (!present && e == EEXIST) say(r, "a file appeared in rekordbox %s's place meanwhile", v);
        else say(r, "couldn't put Pioneer's file back into rekordbox %s (error %d)", v, e);
        return PB_UNCHANGED;
    }
    if (facts(r, live, 1, &f) || strcmp(f.sha256, sha) || f.pin != COMPAT_PIN_OK || f.marker) {
        say(r, "rekordbox %s's file doesn't verify as Pioneer's after putting it back", v);
        return PB_CHANGED;
    }
    say(r, "Pioneer's file is back in rekordbox %s", v);
    return PB_OK;
}

/* whether the live file of vname is now the original authorized for it (want: its sha256),
 * pinned: never just any AlphaTheta-signed file (re-review finding 3) */
static int is_pioneers(run *r, const char *vname, const char *want) {
    char live[RUN_PATH];
    compat_file f;
    if (!want || !payload_is_sha256(want) || !path_in_version(r, vname, LIVE, live, sizeof live) || facts(r, live, 0, &f) || !f.readable || f.marker) return 0;
    return !facts(r, live, 1, &f) && f.pin == COMPAT_PIN_OK && !f.marker && !strcmp(f.sha256, want);
}

/* the rollback after the current folder was changed (Scripts.swift's rollback): 6, 4 or 11 */
static int rollback(run *r) {
    say(r, "failed after rekordbox was changed: putting Pioneer's file back");
    put_back(r, r->vdir, r->vname, r->version, r->auth);
    r->changing = 0;
    if (is_pioneers(r, r->vname, r->auth)) {
        say(r, "rolled back: rekordbox has Pioneer's original file again");
        return ADMIN_EXIT_ROLLED_BACK;
    }
    char live[RUN_PATH];
    compat_file f;
    /* "nothing changed" only when the bridge still there is one that loads: never after its load
     * check failed */
    if (!r->load_failed && path_in_version(r, r->vname, LIVE, live, sizeof live) && !facts(r, live, 0, &f) && f.marker
        && listed_bridge(r, f.sha256)) {
        say(r, "Pioneer's file couldn't be put back: rekordbox still has Stems Cache");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    say(r, "ROLLBACK FAILED: reinstall rekordbox from Pioneer's installer");
    return ADMIN_EXIT_ROLLBACK_FAILED;
}

/* a put-back that changed the folder and didn't verify: tried once more; then Pioneer's (0), or
 * 11 when either try changed it, or 4 when neither did */
static int put_back_hard(run *r, plat_dir d, const char *vname, const char *v, const char *want) {
    int pb = put_back(r, d, vname, v, want), changed = pb == PB_CHANGED;
    if (changed) pb = put_back(r, d, vname, v, want);
    if (pb == PB_OK || is_pioneers(r, vname, want)) {
        if (pb == PB_OK || changed) r->verified_change = 1;
        return 0;
    }
    if (changed || pb == PB_CHANGED) {
        say(r, "ROLLBACK FAILED: reinstall rekordbox %s from Pioneer's installer", v);
        return ADMIN_EXIT_ROLLBACK_FAILED;
    }
    return ADMIN_EXIT_CHECK_FAILED;
}

/* ------------------------------------------------------------------ the common start */

/* Pioneer's installer, updater or uninstaller (8); rekordbox open (5), its linger allowed: a
 * rekordbox process with no window left is waited for, the app told once that it is still
 * closing (PLAN R1) */
static int processes_ok(run *r) {
    uint64_t start = r->os->now_ms(r->os->ctx);
    int told = 0;
    for (;;) {
        compat_procs p;
        if (compat_processes(r->os->compat, NULL, &p)) {
            say(r, "couldn't list the running programs: nothing changed");
            return ADMIN_EXIT_CHECK_FAILED;
        }
        if (p.updating) {
            say(r, "Pioneer's installer or updater is running: nothing changed");
            return ADMIN_EXIT_INSTALLER_RUNNING;
        }
        if (!p.running) {
            if (told) say(r, "@closed");   /* the app's "still closing" line comes down */
            return 0;
        }
        int w = r->os->rekordbox_windows(r->os->ctx);
        if (w > 0 || r->os->now_ms(r->os->ctx) - start >= r->e->linger_ms) {
            say(r, "rekordbox is open: nothing changed");
            return ADMIN_EXIT_REKORDBOX_OPEN;
        }
        if (!told) {
            say(r, "@still-closing");
            say(r, "rekordbox is still closing: waiting for it");
            told = 1;
        }
        r->os->sleep_ms(r->os->ctx, r->e->poll_ms);
    }
}

/* the live file mapped in any session, just before a change (5) */
static int not_mapped(run *r, plat_dir vdir, const char *v) {
    if (r->os->mapped(r->os->ctx, vdir, LIVE) == 1) {
        say(r, "rekordbox %s's ONNX Runtime is in use (rekordbox is open): nothing changed", v);
        return ADMIN_EXIT_REKORDBOX_OPEN;
    }
    return 0;
}

/* the root (made with create), the lock; leftovers of a stopped run. 0; ENOENT (no root, create
 * 0); or an exit code */
static int open_root(run *r, int create) {
    int made = 0, e = r->os->root_open(r->os->ctx, r->e->root, create, &r->root, &made);
    if (e == ENOENT && !create) return ENOENT;
    if (e == EACCES || e == ELOOP || e == ENOTDIR) {
        say(r, "%s isn't the administrators' alone: nothing changed", r->e->root);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (e) {
        say(r, "couldn't open %s (error %d): nothing changed", r->e->root, e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    r->root_made = made;   /* this call's own: never one another run made meanwhile (re-review blocker A) */
    e = r->os->lock(r->os->ctx, r->root, &r->lock, &made);
    if (!e) r->lock_made = made;
    if (e == EBUSY) {
        say(r, "another change to rekordbox is running: nothing changed");
        return ADMIN_EXIT_ALREADY_RUNNING;
    }
    if (e) {
        say(r, "couldn't take the lock (error %d): nothing changed", e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    int x;
    if ((x = sub_dir(r, r->root, NULL, "pioneer", 0, &r->pioneer)) && x != ENOENT) return x;
    if ((x = sub_dir(r, r->root, NULL, "ort", 0, &r->ort)) && x != ENOENT) return x;
    if ((x = sub_dir(r, r->root, NULL, "bin", 0, &r->bin)) && x != ENOENT) return x;
    if ((x = sub_dir(r, r->root, NULL, RECEIPT_DIR, 0, &r->receipts)) && x != ENOENT) return x;
    return 0;
}

static int has_undo_copy(void *ctx, const char *name, const plat_stat *st) {
    (void)ctx;
    (void)st;
    return !strncmp(name, "undo.", 5);
}

/* Install staging is named separately from the start so it survives even an older worker's
 * ordinary cleanup. For older stage.* journals, keep any with saved copies (or an unreadable
 * directory whose contents couldn't be checked). */
static int kept_stage(run *r, const char *name, int kind) {
    if (!strncmp(name, UNDO_STAGE, sizeof UNDO_STAGE - 1)) return 1;
    if (kind != PLAT_DIR || strncmp(name, "stage.", 6)) return 0;
    plat_dir d;
    if (plat_dir_open_at(r->root, name, &d)) return 1;
    int kept = plat_dir_each(d, has_undo_copy, NULL) != 0;
    plat_dir_close(d);
    return kept;
}

/* Leftover staging (never this run's own) and renamed workers (*.del) the boot-time delete left
 * (J7), once the run is authorized to write. Undo journals need a verified target change. */
static void clean_leftovers(run *r) {
    names *l = malloc(sizeof *l);
    if (!l || !plat_dir_ok(r->root)) {
        free(l);
        return;
    }
    if (!list_dir(r->root, l))
        for (int i = 0; i < l->n; i++) {
            const char *name = l->name[i];
            if (strcmp(name, r->stage_name) && (!strncmp(name, "stage.", 6) || !strncmp(name, UNDO_STAGE, sizeof UNDO_STAGE - 1))
                && (r->verified_change || !kept_stage(r, name, l->kind[i])) && rm_tree(r, r->root, name, 0))
                say(r, "couldn't remove the leftover %s", name);
        }
    if (plat_dir_ok(r->bin) && !list_dir(r->bin, l)) {
        names *k = malloc(sizeof *k);
        for (int i = 0; k && i < l->n; i++) {
            plat_dir w;
            if (l->kind[i] != PLAT_DIR || plat_dir_open_at(r->bin, l->name[i], &w)) continue;
            if (!list_dir(w, k))
                for (int j = 0; j < k->n; j++) {
                    size_t nl = strlen(k->name[j]);
                    if (nl > 4 && !strcmp(k->name[j] + nl - 4, ".del")) plat_unlink_at(w, k->name[j]);   /* a mapped one stays */
                }
            plat_dir_close(w);
        }
        free(k);
    }
    free(l);
}

/* ------------------------------------------------------------------ staging and the payload */

/* `name` in from (at most max bytes, a regular file, no reparse point) copied into the staging
 * folder as to, flushed, read back there: its sha256 (of the staged bytes) in sha */
static int stage_copy(run *r, plat_dir from, const char *name, int64_t max, const char *to, char sha[65]) {
    unsigned char *b = NULL;
    size_t n = 0;
    int e = read_whole(from, name, max, &b, &n);
    if (!e) e = write_new(r->stage, to, b, n);
    free(b);
    if (e) return e;
    if ((e = read_whole(r->stage, to, max, &b, &n))) return e;
    e = sha_hex(b, n, sha) ? EIO : 0;
    free(b);
    return e;
}

static int make_stage(run *r) {
    const char *prefix = r->a->verb == ADMIN_INSTALL ? UNDO_STAGE : "stage.";
    for (int i = 0; i < 16; i++) {
        char rnd[17], name[sizeof r->stage_name];
        r->os->random_hex(r->os->ctx, rnd, 16);
        snprintf(name, sizeof name, "%s%s", prefix, rnd);
        int e = r->os->mkdir_at(r->os->ctx, r->root, name);
        if (e == EEXIST) continue;   /* a retry must never reuse a retained journal */
        if (!e) {
            memcpy(r->stage_name, name, strlen(name) + 1);
            return sub_dir(r, r->root, NULL, name, 0, &r->stage) ? ADMIN_EXIT_CHECK_FAILED : 0;
        }
        say(r, "couldn't make %s (error %d): nothing changed", name, e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    say(r, "couldn't make a new staging folder: nothing changed");
    return ADMIN_EXIT_CHECK_FAILED;
}

static void drop_stage(run *r) {
    close_dir(&r->stage);
    if (r->stage_name[0] && rm_tree(r, r->root, r->stage_name, 0)) say(r, "couldn't remove %s", r->stage_name);
    r->stage_name[0] = 0;
}

/* payload.json and its signature copied from --source into the staging folder and verified there
 * (size cap -> DER -> ECDSA over the raw bytes -> JSON -> against the floor), nothing written in
 * the root: the verified bytes kept in r->pj and r->ps for manifest_ok and commit_payload (review
 * finding 1). 0, ADMIN_EXIT_OLDER or ADMIN_EXIT_CHECK_FAILED. */
static int verify_payload(run *r, plat_dir src) {
    char sha[65];
    int e = stage_copy(r, src, "payload.json", PAYLOAD_MAX, "payload.json", sha);
    if (e == PLAT_EFTYPE) {
        say(r, "payload.json is over %d bytes or not a file: nothing changed", PAYLOAD_MAX);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (!e) e = stage_copy(r, src, "payload.json.sig", ADMIN_SIG_MAX, "payload.json.sig", sha);
    if (e) {
        say(r, "couldn't copy the payload from the app's folder (error %d): nothing changed", e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    unsigned char *data = NULL, *sig = NULL;
    size_t n = 0, sn = 0;
    if (read_whole(r->stage, "payload.json", PAYLOAD_MAX, &data, &n) || read_whole(r->stage, "payload.json.sig", ADMIN_SIG_MAX, &sig, &sn)) {
        free(data);
        free(sig);
        say(r, "couldn't read the staged payload: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    payload_files pf = {r->e->root, r->e->root, r->e->keys, r->e->nkeys, r->e->minimum};
    /* payload_accept's check (payload_store.c) without its writes: the floor as it is now */
    char floor[64], why[160], rec[64];
    int a = payload_accepted_floor(payload_read_floor(&pf, rec) ? rec : NULL, r->e->minimum, floor);
    int pr = a < 0 ? PAYLOAD_NOT_VALID : payload_check(data, n, sig, sn, r->e->keys, r->e->nkeys, a ? floor : NULL, r->m);
    if (pr == PAYLOAD_OK && !r->m->has_windows) pr = PAYLOAD_NO_NEWS;
    if (pr == PAYLOAD_OK) {
        r->pj = data, r->pjn = n, r->ps = sig, r->psn = sn;
    } else {
        free(data);
        free(sig);
    }
    if (pr == PAYLOAD_OLDER) {
        char saved[64];
        if (!payload_read_floor(&pf, saved)) saved[0] = 0;
        payload_accepted_floor(saved, r->e->minimum, floor);
        payload_manifest *m = r->m;
        char v[PAYLOAD_VERSION_MAX] = "";
        unsigned char *d2 = NULL;
        size_t n2 = 0;
        if (!read_whole(r->stage, "payload.json", PAYLOAD_MAX, &d2, &n2) && payload_decode(d2, n2, m) == PAYLOAD_OK)
            snprintf(v, sizeof v, "%s", m->payload_version);
        free(d2);
        say(r, "@floor %s", floor);
        say(r, "payload.json %s is older than %s, which this computer has seen: update the app first. Nothing changed", v, floor);
        return ADMIN_EXIT_OLDER;
    }
    if (pr == PAYLOAD_NO_NEWS) {
        say(r, "payload.json has no Windows part: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (pr != PAYLOAD_OK) {
        say(r, "payload.json refused: %s. Nothing changed", payload_refusal_text(pr, NULL, NULL, why, sizeof why));
        return ADMIN_EXIT_CHECK_FAILED;
    }
    r->have_m = 1;
    say(r, "payload.json %s verified (signed by the %s key)", r->m->payload_version, r->m->signed_by ? r->m->signed_by : "?");
    return 0;
}

/* the undo journal's copies of the files commit_payload replaces, made before the last close
 * gate so no disk I/O is left between that gate and the floor's write (re-review 2, finding 8):
 * 0 or ADMIN_EXIT_CHECK_FAILED */
static int keep_payload_copies(run *r) {
    static const char *const kept[3] = {"payload-version", "payload.json.sig", "payload.json"};   /* payload_store.c's */
    for (int i = 0; i < 3; i++)
        if (note_file(r, r->root, "", "", kept[i])) {
            say(r, "couldn't keep the verified payload.json: nothing changed");
            return ADMIN_EXIT_CHECK_FAILED;
        }
    return 0;
}

/* the verified payload accepted, once the run is authorized (the scope and --manifest agree, every
 * target checked) and its files' copies kept (keep_payload_copies): the floor raised to it and
 * the pair kept in the root (payload_accept checks the same bytes again). 0 or
 * ADMIN_EXIT_CHECK_FAILED */
static int commit_payload(run *r) {
    payload_files pf = {r->e->root, r->e->root, r->e->keys, r->e->nkeys, r->e->minimum};
    int pr = r->pj ? payload_accept(&pf, r->pj, r->pjn, r->ps, r->psn, r->m) : PAYLOAD_NOT_VALID;
    if (pr != PAYLOAD_OK) {
        char why[160];
        say(r, "couldn't keep the verified payload.json: %s. Nothing changed", payload_refusal_text(pr, NULL, NULL, why, sizeof why));
        return ADMIN_EXIT_CHECK_FAILED;
    }
    r->committed = 1;
    char floor[64];
    if (payload_read_floor(&pf, floor)) say(r, "@floor %s", floor);
    return 0;
}

/* ------------------------------------------------------------------ bin\ (I75, I78, J7) */

/* whether bin\ holds a worker at all: a folder named exactly 12 hex digits holding a regular
 * file named exactly RB Stems Plus.exe (a renamed *.del doesn't count) */
static int bin_has_worker(run *r) {
    if (!plat_dir_ok(r->bin)) return 0;
    names *l = malloc(sizeof *l);
    int found = 0;
    if (l && !list_dir(r->bin, l))
        for (int i = 0; i < l->n && !found; i++) {
            plat_dir w;
            plat_stat st;
            if (l->kind[i] != PLAT_DIR || !admin_sha12_ok(l->name[i]) || plat_dir_open_at(r->bin, l->name[i], &w)) continue;
            found = !plat_stat_at(w, ADMIN_WORKER_NAME, &st) && st.kind == PLAT_FILE;
            plat_dir_close(w);
        }
    free(l);
    return found;
}

/* the worker in --source's admin\ folder, staged and checked against the verified payload's
 * admin_sha256 and its protocol, nothing written outside the staging folder: 0,
 * ADMIN_EXIT_PROTOCOL or ADMIN_EXIT_CHECK_FAILED */
static int stage_worker(run *r, plat_dir src) {
    plat_dir adm;
    char sha[65];
    int e = plat_dir_open_at(src, ADMIN_APP_COPY_DIR, &adm);
    if (!e) {
        e = stage_copy(r, adm, ADMIN_WORKER_NAME, ADMIN_WORKER_MAX, STAGED_WORKER, sha);
        plat_dir_close(adm);
    }
    if (e) {
        say(r, "couldn't copy the app's worker (error %d): nothing changed", e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (strcmp(sha, r->m->win_app.admin_sha256)) {
        say(r, "the app's worker isn't the one payload.json names: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    {
        /* never an older protocol's worker in bin\ (D5): an older app updates first */
        unsigned char *wb = NULL;
        size_t wn = 0;
        int proto = -1;
        char min[ADMIN_VERSION_MAX];
        if (read_whole(r->stage, STAGED_WORKER, ADMIN_WORKER_MAX, &wb, &wn) || !admin_marker_find(wb, wn, &proto, min)) proto = -1;
        free(wb);
        if (proto < ADMIN_PROTO) {
            say(r, "the app's worker speaks an older protocol (%d) than this one (%d): update the app first. Nothing changed", proto, ADMIN_PROTO);
            return ADMIN_EXIT_PROTOCOL;
        }
    }
    r->worker_staged = 1;
    return 0;
}

/* the staged worker (stage_worker) moved into bin\<sha12>\ (its sha12 the payload's admin_sha256):
 * 0 or ADMIN_EXIT_CHECK_FAILED */
static int place_worker(run *r) {
    const char *sha = r->m->win_app.admin_sha256;
    int x, e;
    if (!r->worker_staged) return ADMIN_EXIT_CHECK_FAILED;
    if (!plat_dir_ok(r->bin) && sub_dir(r, r->root, "", "bin", 1, &r->bin)) return ADMIN_EXIT_CHECK_FAILED;
    char d12[13];
    memcpy(d12, sha, 12);
    d12[12] = 0;
    plat_dir w;
    if ((x = sub_dir(r, r->bin, "bin", d12, 1, &w))) return ADMIN_EXIT_CHECK_FAILED;
    unsigned char *b = NULL;
    size_t n = 0;
    char got[65] = "";
    if (!read_whole(w, ADMIN_WORKER_NAME, ADMIN_WORKER_MAX, &b, &n)) sha_hex(b, n, got);
    free(b);
    b = NULL;
    if (strcmp(got, sha)) {
        /* missing (or not that file: never the case for a 12-hex name it was made under) */
        e = note_file(r, w, "bin", d12, ADMIN_WORKER_NAME);
        if (!e) e = plat_rename_at(r->stage, STAGED_WORKER, w, ADMIN_WORKER_NAME);
        got[0] = 0;
        if (!e && !read_whole(w, ADMIN_WORKER_NAME, ADMIN_WORKER_MAX, &b, &n)) sha_hex(b, n, got);
        free(b);
        b = NULL;
        if (e || strcmp(got, sha)) {
            plat_dir_close(w);
            say(r, "couldn't put the worker into bin\\%s (error %d): nothing changed", d12, e);
            return ADMIN_EXIT_CHECK_FAILED;
        }
        say(r, "the worker %s is installed", d12);
    }
    plat_dir_close(w);
    memcpy(r->new_worker12, d12, 13);
    return 0;
}

/* older workers: every bin\<12 hex>\ but this one's and the one just installed (a mapped one
 * stays, for a later run) */
static void drop_old_workers(run *r) {
    /* a run from the app folder's copy that installed none keeps bin\ as it is: never the last one */
    if (!plat_dir_ok(r->bin) || (!r->self_in_bin && !r->new_worker12[0])) return;
    names *l = malloc(sizeof *l);
    if (l && !list_dir(r->bin, l))
        for (int i = 0; i < l->n; i++) {
            if (l->kind[i] != PLAT_DIR || !admin_sha12_ok(l->name[i])) continue;
            if ((r->self_in_bin && !strcmp(l->name[i], r->self12)) || !strcmp(l->name[i], r->new_worker12)) continue;
            if (rm_tree(r, r->bin, l->name[i], 0)) say(r, "the older worker %s stays for now", l->name[i]);
        }
    free(l);
}

/* ------------------------------------------------------------------ the inventory (mv-spec §2.1, §2.2) */

/* "@target <v> <code> <what>" (code -1: "-") */
static void say_target(run *r, const char *v, int code, const char *what) {
    if (code < 0) say(r, "@target %s - %s", v, what);
    else say(r, "@target %s %d %s", v, code, what);
#ifdef ADMIN_TEST_HOLD
    /* The client consumes the protocol record. Also log it as text for VM assertions. */
    if (code < 0) say(r, "test build: @target %s - %s", v, what);
    else say(r, "test build: @target %s %d %s", v, code, what);
#endif
}

/* every installed rekordbox, read now (inventory.c, the app's and the watcher's walk): 0 with
 * r->inv; whole: an incomplete one refuses (4) */
static int inventory(run *r, int whole) {
    compat_memo_clear();   /* every fact here read now */
    if (!r->inv && !(r->inv = malloc(sizeof *r->inv))) return ADMIN_EXIT_FAILED;
    if (inv_read(r->os->compat, r->e->rekordbox, COMPAT_FRESH, r->inv)) {
        say(r, "couldn't list the installed versions of rekordbox: nothing changed");
        return ADMIN_EXIT_FAILED;
    }
    if (whole && !r->inv->complete) {
        say(r, "couldn't check every installed version of rekordbox: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    return 0;
}

/* the row of version v, STALE rows never: its index, or -1 */
static int row_of(const run *r, const char *v) {
    for (int i = 0; i < r->inv->n; i++)
        if (r->inv->it[i].trust != INV_STALE && !strcmp(r->inv->it[i].version, v)) return i;
    return -1;
}

/* rekordbox's folder (<Program Files>\rekordbox), held: 0, 9 (missing), 10 */
static int hold_rbroot(run *r) {
    int e = r->os->hold_dir(r->os->ctx, plat_dir_cwd(), r->e->rekordbox, &r->rbroot);
    if (!e) return 0;
    if (e == ENOENT) {
        say(r, "rekordbox isn't installed: nothing changed");
        return ADMIN_EXIT_REKORDBOX_MISSING;
    }
    say(r, "rekordbox's folder isn't the administrators' alone: reinstall rekordbox with Pioneer's installer. Nothing changed");
    return e == ELOOP || e == EACCES || e == ENOTDIR ? ADMIN_EXIT_NOT_PIONEERS : ADMIN_EXIT_CHECK_FAILED;
}

/* the version folder name in rekordbox's folder, held, still the folder the inventory saw
 * (id): 0 with *d; ENOENT; HOLD_MOVED (another folder now); or hold_dir's errno */
static int hold_version(run *r, const char *name, plat_id id, plat_dir *d) {
    int e = r->os->hold_dir(r->os->ctx, r->rbroot, name, d);
    if (e) return e;
    plat_stat st;
    if (plat_dir_stat(*d, &st) || !plat_id_same(st.id, id)) {
        close_dir(d);
        return HOLD_MOVED;
    }
    return 0;
}

/* the live DLL as the inventory's rule reads it (COMPAT_LIVE_*): ours is this payload's bridge,
 * an older bridge of ours is OURS_UNLISTED (its receipt says whether it is ours to replace) */
static int live_kind(const run *r, const compat_file *f) {
    if (!f->present) return COMPAT_LIVE_MISSING;
    if (!f->exists) return COMPAT_LIVE_FOREIGN;
    if (!f->readable) return COMPAT_LIVE_UNREADABLE;
    if (f->marker) return r->have_m && !strcmp(f->sha256, r->m->win_bridge.sha256) ? COMPAT_LIVE_OURS : COMPAT_LIVE_OURS_UNLISTED;
    if (f->pin == COMPAT_PIN_OK) return COMPAT_LIVE_PIONEER;
    return f->pin == COMPAT_PIN_NO ? COMPAT_LIVE_FOREIGN : COMPAT_LIVE_UNREADABLE;
}

/* one row's state, besides inv_assess_row's */
typedef struct {
    int hold_err;         /* its folder couldn't be held (hold_version's errno) */
    int cls;              /* ADMIN_LIVE_* */
    compat_file live;
    char set[65];         /* the complete saved set's sha256, "" none */
    int have_rc;
    receipt rc;
} row_x;

/* a row assessed now, through a held handle (mv-spec §2.2 step 3): no write */
static void assess(run *r, const inv_install *row, row_x *x, inv_assess_row *a) {
    memset(x, 0, sizeof *x);
    memset(a, 0, sizeof *a);
    a->live = COMPAT_LIVE_UNREADABLE;
    x->cls = ADMIN_LIVE_NO_ANSWER;
    if (row->trust == INV_STALE) return;
    plat_dir d;
    if ((x->hold_err = hold_version(r, row->name, row->dir_id, &d))) return;
    plat_dir_close(d);
    x->have_rc = !receipt_read(r->e->root, row->version, &x->rc);
    r->rc = x->have_rc ? &x->rc : NULL;
    x->cls = classify(r, row->name, row->version, &x->live, x->set);
    r->rc = NULL;
    a->live = live_kind(r, &x->live);
    a->saved_complete = x->set[0] != 0;
    memcpy(a->saved_sha, x->set, sizeof a->saved_sha);
    /* the original: the live DLL when it is Pioneer's, else the complete saved set */
    char p[RUN_PATH];
    int have = a->live == COMPAT_LIVE_PIONEER ? path_in_version(r, row->name, LIVE, p, sizeof p)
                                              : a->saved_complete && join4(p, sizeof p, r->e->root, "pioneer", row->version, LIVE);
    compat_file o;
    if (have && !facts_code(r, p, &o) && o.readable && (a->live == COMPAT_LIVE_PIONEER || !strcmp(o.sha256, x->set))) {
        memcpy(a->original_sha, o.sha256, sizeof a->original_sha);
        memcpy(a->original_code, o.code_sha256, sizeof a->original_code);
        a->original_bound = r->have_m && inv_original_bound(r->m, row->exe.pe.product_version, &o);
    }
    a->listed = r->have_m && payload_win_rekordbox_supported(r->m, row->exe.pe.product_version);
    a->receipt_ok = x->have_rc && receipt_matches(&x->rc, row, x->live.sha256, x->set, r->ort_code);
}

/* every row assessed now (assess); *bridges: whether any folder may have a bridge live, so may
 * load ort\'s runtime: a bridge read there, or a folder that couldn't be checked (its hold
 * refused, its live DLL unreadable or without an answer: review finding 5); 1 when some row isn't
 * STALE or DISK_ONLY (rekordbox is installed) */
static int assess_all(run *r, row_x *xs, inv_assess_row *as, int *bridges) {
    int any = 0;
    *bridges = 0;
    for (int i = 0; i < r->inv->n; i++) {
        assess(r, &r->inv->it[i], &xs[i], &as[i]);
        int unchecked = r->inv->it[i].trust != INV_STALE && (xs[i].hold_err || xs[i].cls == ADMIN_LIVE_NO_ANSWER || as[i].live == COMPAT_LIVE_UNREADABLE);
        *bridges |= xs[i].live.marker || unchecked;
        any |= r->inv->it[i].trust != INV_STALE && r->inv->it[i].trust != INV_DISK_ONLY;
    }
    return any;
}

/* --targets against the fresh scope: 0 when they are the same; else 10 when a named folder isn't
 * the administrators' alone or its rekordbox.exe isn't AlphaTheta's, or 15, nothing changed */
static int same_scope(run *r, const admin_target *s, int ns, const row_x *xs) {
    const admin_args *a = r->a;
    int same = ns == a->nt;
    for (int i = 0; same && i < ns; i++) same = !strcmp(s[i].version, a->t[i].version) && !strcmp(s[i].exe16, a->t[i].exe16);
    if (same) return 0;
    for (int i = 0; i < a->nt; i++) {
        int k = row_of(r, a->t[i].version);
        if (k < 0) continue;   /* a refusal of the folder itself, whatever its rekordbox.exe is now */
        int e = xs[k].hold_err;
        if (e == EACCES || e == ELOOP || e == ENOTDIR) {
            say(r, "rekordbox %s's folder isn't the administrators' alone: reinstall rekordbox %s with Pioneer's installer. Nothing changed", a->t[i].version,
                a->t[i].version);
            say_target(r, a->t[i].version, ADMIN_EXIT_NOT_PIONEERS, ADMIN_TARGET_WHAT_REFUSED);
            return ADMIN_EXIT_NOT_PIONEERS;
        }
        if (r->inv->it[k].trust == INV_UNVERIFIED) {
            say(r, "rekordbox %s isn't installed the way Pioneer's installer leaves it: nothing changed", a->t[i].version);
            say_target(r, a->t[i].version, ADMIN_EXIT_NOT_PIONEERS, ADMIN_TARGET_WHAT_REFUSED);
            return ADMIN_EXIT_NOT_PIONEERS;
        }
    }
    char asked[ADMIN_TARGETS_ARG_MAX], found[ADMIN_TARGETS_ARG_MAX];
    if (admin_targets_format(a->t, a->nt, asked, sizeof asked) < 0) snprintf(asked, sizeof asked, "?");
    if (!ns || admin_targets_format(s, ns, found, sizeof found) < 0) snprintf(found, sizeof found, "none");
    say(r, "the rekordbox versions to change aren't the ones checked (checked %s, now %s): nothing changed", asked, found);
    return ADMIN_EXIT_SCOPE_CHANGED;
}

/* the verified payload.json's bytes are the ones the app verified (--manifest): 0 or 15 */
static int manifest_ok(run *r) {
    char sha[65] = "";
    if (r->pj) sha_hex(r->pj, r->pjn, sha);
    if (sha[0] && !strcmp(sha, r->a->manifest)) return 0;
    say(r, "payload.json isn't the one the app checked: nothing changed");
    return ADMIN_EXIT_SCOPE_CHANGED;
}

/* the bridge from --source, staged and checked against the verified payload: 0 or 4 */
static int stage_bridge(run *r, plat_dir src) {
    char bsha[65];
    int e = admin_plain_name_ok(r->m->win_bridge.file) ? stage_copy(r, src, r->m->win_bridge.file, ADMIN_BRIDGE_MAX, STAGED_BRIDGE, bsha) : EINVAL;
    if (e == PLAT_EFTYPE) say(r, "the bridge is over 32 MB or not a file: nothing changed");
    else if (e) say(r, "couldn't copy the bridge (error %d): nothing changed", e);
    else if (strcmp(bsha, r->m->win_bridge.sha256)) say(r, "the bridge's checksum doesn't match payload.json: nothing changed");
    return e || strcmp(bsha, r->m->win_bridge.sha256) ? ADMIN_EXIT_CHECK_FAILED : 0;
}

/* ort\onnxruntime.dll (mv-spec §2.3); dry: decided only, nothing written (the batch asks first,
 * before any write, then again to write). Kept when it passes as the shared runtime; written from the
 * original at path (sha256 sha: the first target's) when missing, or when it fails and no bridge
 * may be live anywhere (bridges: an unchecked folder counts as one, assess_all); else 4.
 * r->ort_code its PE code digest then */
static int shared_runtime(run *r, const char *path, const char *sha, int bridges, int dry) {
    char ortp[RUN_PATH];
    compat_file o;
    if (!join3(ortp, sizeof ortp, r->e->root, "ort", LIVE)) return ADMIN_EXIT_CHECK_FAILED;
    int read = !facts_code(r, ortp, &o);
    if (read && runtime_ok(r, &o)) {
        memcpy(r->ort_code, o.code_sha256, sizeof r->ort_code);
        return 0;
    }
    if (!read || (o.present && bridges)) {
        say(r, "Stems Cache's ONNX Runtime copy isn't one this release supports: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    char *b = NULL, got[65];
    size_t n = 0;
    int e = plat_read_file(path, (size_t)ADMIN_ORT_MAX, &b, &n);
    if (!e && (sha_hex(b, n, got) || strcmp(got, sha))) e = EIO;
    if (dry) {
        /* the decision alone, before any write: the copy to be made is the original checked */
        free(b);
        if (!e) return 0;
        say(r, "couldn't read Pioneer's ONNX Runtime at %s (error %d): nothing changed", path, e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (!e) e = note_file(r, r->ort, "ort", "", LIVE);
    if (!e) e = write_atomic(r, r->ort, LIVE, b, n);
    free(b);
    if (e || facts_code(r, ortp, &o) || !runtime_ok(r, &o) || strcmp(o.sha256, sha)) {
        say(r, "couldn't copy Pioneer's ONNX Runtime to %s (error %d): nothing changed", ortp, e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    memcpy(r->ort_code, o.code_sha256, sizeof r->ort_code);
    say(r, "Pioneer's ONNX Runtime copied to ort\\");
    return 0;
}

/* Pioneer's file of the held folder (live, sha256 sha) saved into pioneer\<v>\, once per identity
 * (again when Pioneer re-shipped it), SHA256SUMS last: 0 or 4 */
static int save_original(run *r, const char *v, const char *sha, const char *set) {
    unsigned char *b = NULL;
    size_t n = 0;
    char got[65];
    if (read_whole(r->vdir, LIVE, ADMIN_ORT_MAX, &b, &n) || sha_hex(b, n, got) || strcmp(got, sha)) {
        free(b);
        say(r, "rekordbox %s's ONNX Runtime changed after it was checked: nothing changed", v);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (!strcmp(set, sha)) {
        free(b);
        say(r, "Pioneer's file of rekordbox %s is saved already", v);
        return 0;
    }
    plat_dir vs;
    char line[160], sp[RUN_PATH];
    compat_file s;
    say(r, "saving Pioneer's file of rekordbox %s", v);
    if (sub_dir(r, r->pioneer, "pioneer", v, 1, &vs)) {
        free(b);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    int e = note_file(r, vs, "pioneer", v, SUMS);
    if (!e) e = note_file(r, vs, "pioneer", v, LIVE);
    if (!e) e = unlink_quiet(vs, SUMS);
    if (!e) e = unlink_quiet(vs, LIVE);
    if (!e) e = write_atomic(r, vs, LIVE, b, n);
    free(b);
    if (!e && (!join4(sp, sizeof sp, r->e->root, "pioneer", v, LIVE) || facts(r, sp, 1, &s) || strcmp(s.sha256, sha) || s.pin != COMPAT_PIN_OK))
        e = EIO;
    if (!e && admin_sums_line(sha, line, sizeof line)) e = write_atomic(r, vs, SUMS, line, strlen(line));
    plat_dir_close(vs);
    if (e) {
        say(r, "couldn't save Pioneer's file of rekordbox %s (error %d): nothing changed", v, e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    return 0;
}

/* One target of the batch (mv-spec §2.2 step 6), its folder held in r->vdir: the live DLL repaired
 * from the bound saved set when missing, Pioneer's file saved, the bridge written beside it and
 * verified, the receipt deleted, the swap, the checks (the bridge's and ort\'s load, rekordbox.exe
 * again), then the receipt. 0; or an exit code with *what the folder's state (kept as it was, or
 * put back to Pioneer's file by the rollback) */
static int install_one(run *r, const inv_install *row, const inv_assess_row *as, row_x *x, const char **what) {
    char live[RUN_PATH], newp[RUN_PATH], ortp[RUN_PATH];
    const char *v = row->version;
    *what = ADMIN_TARGET_WHAT_KEPT;
    path_in_version(r, row->name, LIVE, live, sizeof live);
    path_in_version(r, row->name, NEW, newp, sizeof newp);
    join3(ortp, sizeof ortp, r->e->root, "ort", LIVE);
    r->rc = x->have_rc ? &x->rc : NULL;
    int xc;
    if (as->live == COMPAT_LIVE_MISSING || as->live == COMPAT_LIVE_FOREIGN) {
        /* the scope took it with its bound saved set (D4; a stranger's file only in a Reinstall's):
         * Pioneer's file back first */
        if ((xc = not_mapped(r, r->vdir, v))) return xc;
        say(r, as->live == COMPAT_LIVE_MISSING ? "rekordbox %s's ONNX Runtime is missing: putting Pioneer's file back first"
                                               : "rekordbox %s's ONNX Runtime isn't Pioneer's (a run was stopped half-way): putting Pioneer's file back first",
            v);
        r->changing = 1;
        memcpy(r->auth, as->saved_sha, sizeof r->auth);   /* the assessed, bound saved set */
        int pb = put_back(r, r->vdir, row->name, v, r->auth);
        if (pb == PB_UNCHANGED) {
            /* the live DLL as it was: still a refusal before the first change, so a first target's
             * undo journal runs (admin_run) */
            r->changing = 0;
            return ADMIN_EXIT_CHECK_FAILED;
        }
        r->touched = 1;   /* PB_OK or PB_CHANGED: the folder changed */
        if (pb != PB_OK || !is_pioneers(r, row->name, r->auth)) {
            xc = rollback(r);
            *what = xc == ADMIN_EXIT_ROLLED_BACK ? ADMIN_TARGET_WHAT_PUT_BACK : ADMIN_TARGET_WHAT_KEPT;
            return xc;
        }
        r->verified_change = 1;
        r->changing = 0;
    }
    compat_file f;
    char set[65];
    int c = classify(r, row->name, v, &f, set);
    if (c == ADMIN_LIVE_PIONEER) {
        if (strcmp(f.sha256, as->original_sha)) {
            say(r, "rekordbox %s's ONNX Runtime changed after it was checked: nothing changed", v);
            return ADMIN_EXIT_CHECK_FAILED;
        }
        if ((xc = save_original(r, v, f.sha256, set))) return xc;
        memcpy(set, f.sha256, sizeof set);
    } else if (c != ADMIN_LIVE_OURS || strcmp(set, as->saved_sha)) {
        /* ours, as its receipt binds it (the scope's rule), or nothing is done */
        say(r, "rekordbox %s's ONNX Runtime changed after it was checked: nothing changed", v);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    /* the bridge next to its place, flushed and verified; the folder is unchanged so far */
    unsigned char *b = NULL;
    size_t n = 0;
    compat_file nf;
    int e = read_whole(r->stage, STAGED_BRIDGE, ADMIN_BRIDGE_MAX, &b, &n);
    if (!e) e = unlink_quiet(r->vdir, NEW);
    if (!e) e = write_new(r->vdir, NEW, b, n);
    free(b);
    if (e || facts(r, newp, 0, &nf) || strcmp(nf.sha256, r->m->win_bridge.sha256) || !nf.marker) {
        unlink_quiet(r->vdir, NEW);
        say(r, "couldn't write the bridge into rekordbox %s (error %d): nothing changed", v, e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if ((xc = not_mapped(r, r->vdir, v))) {
        unlink_quiet(r->vdir, NEW);
        return xc;
    }
    memcpy(r->auth, set, sizeof r->auth);   /* the original verified above: the only one a rollback puts back */
    /* the receipt goes before the swap: a run cut off from here on leaves none, so the folder
     * reads "needs repair", never "on", until a run checks it (review OR1); the old one comes
     * back when the swap doesn't happen. A first target's is journaled first (note_file: a
     * refusal's undo brings it back even when the write-back below fails); no copy, no deletion */
    unsigned char *old = NULL;
    size_t on = 0;
    if (plat_dir_ok(r->receipts) && (e = note_file(r, r->receipts, RECEIPT_DIR, "", v))) {
        unlink_quiet(r->vdir, NEW);
        say(r, "couldn't keep a copy of rekordbox %s's receipt (error %d): nothing changed", v, e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    int had = plat_dir_ok(r->receipts) && !read_whole(r->receipts, v, RECEIPT_MAX, &old, &on);
    if (plat_dir_ok(r->receipts) && (e = unlink_quiet(r->receipts, v))) {
        free(old);
        unlink_quiet(r->vdir, NEW);
        say(r, "couldn't remove rekordbox %s's receipt (error %d): nothing changed", v, e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    /* the swap (bridge.md §2): atomic on NTFS; a refusal before anything changed is rekordbox open
     * only when the live file is mapped (I83) */
    if ((e = plat_rename_at(r->vdir, NEW, r->vdir, LIVE))) {
        unlink_quiet(r->vdir, NEW);
        if (had && write_atomic(r, r->receipts, v, old, on)) say(r, "couldn't write rekordbox %s's receipt back", v);
        free(old);
        if (r->os->mapped(r->os->ctx, r->vdir, LIVE) == 1) {
            say(r, "rekordbox %s's ONNX Runtime is in use (rekordbox is open): nothing changed", v);
            return ADMIN_EXIT_REKORDBOX_OPEN;
        }
        say(r, "couldn't swap the bridge into rekordbox %s (error %d): nothing changed", v, e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    free(old);
    r->changing = r->touched = 1;
    r->load_failed = 0;
#ifdef ADMIN_TEST_HOLD
    char where[ADMIN_LINE_MAX];
    snprintf(where, sizeof where, "the bridge swapped into rekordbox %s, not yet verified", v);
    admin_test_mid_hold(r->os, where, v);
#endif
    /* verified, then the load check of the bridge and of ort\ (each freed before any rollback),
     * then rekordbox.exe again */
    compat_file ef;
    if (facts(r, live, 0, &nf) || strcmp(nf.sha256, r->m->win_bridge.sha256) || !nf.marker) say(r, "rekordbox %s's file isn't the bridge after the swap", v), xc = -1;
    else if (
#ifdef ADMIN_TEST_HOLD
        admin_test_fail_target(r->os, v) ||
#endif
        r->os->load_check(r->os->ctx, live, MARKER_SYMBOL) || r->os->load_check(r->os->ctx, ortp, ORT_SYMBOL)) {
        say(r, "the bridge or Pioneer's ONNX Runtime doesn't load");
        r->load_failed = 1;
        xc = -1;
    } else if ((xc = exe_check(r, row->name, v, 1, &ef)) == 0 && strcmp(ef.sha256, row->exe.sha256)) {
        say(r, "rekordbox %s's rekordbox.exe changed during the run", v);
        xc = -1;
    }
    if (!xc) {
        /* the receipt (D4): this folder, its rekordbox.exe, the bridge, its original and the runtime */
        receipt rc;
        char text[RECEIPT_MAX + 1];
        memset(&rc, 0, sizeof rc);
        snprintf(rc.folder_name, sizeof rc.folder_name, "%s", row->name);
        snprintf(rc.version, sizeof rc.version, "%s", v);
        plat_id_hex(row->dir_id, rc.folder_id);
        memcpy(rc.exe_sha256, row->exe.sha256, sizeof rc.exe_sha256);
        memcpy(rc.original_sha256, set, sizeof rc.original_sha256);
        memcpy(rc.bridge_sha256, r->m->win_bridge.sha256, sizeof rc.bridge_sha256);
        memcpy(rc.runtime_code_sha256, r->ort_code, sizeof rc.runtime_code_sha256);
        snprintf(rc.payload_version, sizeof rc.payload_version, "%s", r->m->payload_version);
        int tn = receipt_format(&rc, text, sizeof text);
        if (tn < 0 || !plat_dir_ok(r->receipts) || (e = write_atomic(r, r->receipts, v, text, (size_t)tn))) {
            say(r, "couldn't write rekordbox %s's receipt", v);
            xc = -1;
        }
    }
    if (xc) {
        xc = rollback(r);
        *what = xc == ADMIN_EXIT_ROLLED_BACK ? ADMIN_TARGET_WHAT_PUT_BACK : ADMIN_TARGET_WHAT_KEPT;
        return xc;
    }
    r->changing = 0;
    r->verified_change = 1;
    r->rc = NULL;
    *what = ADMIN_TARGET_WHAT_INSTALLED;
    return 0;
}

/* Install and Reinstall (force) over every verified, supported rekordbox folder in one run
 * (mv-spec §2.2, D1, D4, D5, D7): the common start; the fresh inventory and its scope against
 * --targets (15 when they differ); each target's checks before any write; the shared writes once;
 * then each target in version order. A failure in target k rolls k back and leaves 1..k-1
 * installed, each with its receipt (16 when one was done); the targets after it aren't started.
 * Retrying is idempotent: done targets drop out of the next scope. */
static int do_install(run *r) {
    /* the root made only when missing, and taken away again by a refused run (admin_run) */
    int x = open_root(r, 0);
    if (x == ENOENT) {
        close_dir(&r->root);
        x = open_root(r, 1);
    }
    if (x || (x = processes_ok(r)) || (x = make_stage(r))) return x;
    plat_dir src;
    if (plat_dir_open(r->a->source, &src)) {
        say(r, "couldn't open the app's payload folder: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    x = verify_payload(r, src);
    if (!x) x = manifest_ok(r);
    if (!x && !bindings_ok(r)) {
        say(r, "this release's list has no ONNX Runtime bindings: nothing changed");
        x = ADMIN_EXIT_CHECK_FAILED;
    }
    if (!x) x = stage_bridge(r, src);
    plat_dir_close(src);
    if (x || (x = inventory(r, 1)) || (x = hold_rbroot(r))) return x;
    int n = r->inv->n, ns = 0;
    row_x *xs = calloc((size_t)(n ? n : 1), sizeof *xs);
    inv_assess_row *as = calloc((size_t)(n ? n : 1), sizeof *as);
    plat_dir *held = calloc(ADMIN_TARGETS_MAX, sizeof *held);
    admin_target scope[ADMIN_TARGETS_MAX];
    int rows[ADMIN_TARGETS_MAX];
    if (!xs || !as || !held) {
        x = ADMIN_EXIT_FAILED;
        goto out;
    }
    for (int t = 0; t < ADMIN_TARGETS_MAX; t++) held[t] = plat_dir_cwd();
    {
        char ortp[RUN_PATH];
        compat_file o;
        r->ort_code[0] = 0;
        if (join3(ortp, sizeof ortp, r->e->root, "ort", LIVE) && !facts_code(r, ortp, &o) && o.exists && o.readable)
            memcpy(r->ort_code, o.code_sha256, sizeof r->ort_code);
    }
    int bridges = 0;
    if (!assess_all(r, xs, as, &bridges)) {
        say(r, "rekordbox isn't installed: nothing changed");
        x = ADMIN_EXIT_REKORDBOX_MISSING;
        goto out;
    }
    ns = inv_targets(r->a->force ? INV_VERB_REINSTALL : INV_VERB_INSTALL, r->inv, as, r->m, scope, ADMIN_TARGETS_MAX);
    if (ns < 0) {
        say(r, "couldn't check every installed version of rekordbox: nothing changed");
        x = ADMIN_EXIT_CHECK_FAILED;
        goto out;
    }
    if ((x = same_scope(r, scope, ns, xs))) goto out;
    /* each target's checks, before any write (step 4): its folder held for the run */
    for (int t = 0; t < ns && !x; t++) {
        int k = rows[t] = row_of(r, scope[t].version);
        const inv_install *row = &r->inv->it[k];
        compat_file ef;
        int e = hold_version(r, row->name, row->dir_id, &held[t]);
        if (e == HOLD_MOVED || e == ENOENT) {
            say(r, "rekordbox %s's folder changed after it was checked: nothing changed", row->version);
            x = ADMIN_EXIT_SCOPE_CHANGED;
        } else if (e) {
            say(r, "rekordbox %s's folder isn't the administrators' alone: reinstall rekordbox %s with Pioneer's installer. Nothing changed", row->version,
                row->version);
            x = ADMIN_EXIT_NOT_PIONEERS;
        } else if ((x = exe_check(r, row->name, row->version, 1, &ef)) == 0 && strcmp(ef.sha256, row->exe.sha256)) {
            say(r, "rekordbox %s's rekordbox.exe changed after it was checked: nothing changed", row->version);
            x = ADMIN_EXIT_SCOPE_CHANGED;
        }
        if (!x) {
            /* only the live DLL may be missing here (re-review finding 4) */
            e = r->os->file_admin_only(r->os->ctx, held[t], "rekordbox.exe");
            if (e == ENOENT) {
                say(r, "rekordbox %s's rekordbox.exe changed after it was checked: nothing changed", row->version);
                x = ADMIN_EXIT_SCOPE_CHANGED;
            } else if (!e) e = r->os->file_admin_only(r->os->ctx, held[t], LIVE);
            if (!x && e && e != ENOENT) {
                say(r, "rekordbox %s's files can be changed by other accounts: reinstall rekordbox %s with Pioneer's installer. Nothing changed", row->version,
                    row->version);
                x = ADMIN_EXIT_NOT_PIONEERS;
            }
        }
        if (!x) x = not_mapped(r, held[t], row->version);
        if (x) say_target(r, row->version, x, ADMIN_TARGET_WHAT_REFUSED);
    }
    /* every row assessed again, the targets' folders held (their live DLL, saved set and receipt
     * read under the hold), and the scope compared again before any write (review finding 2) */
    if (!x) {
        admin_target again[ADMIN_TARGETS_MAX];
        assess_all(r, xs, as, &bridges);
        int na = inv_targets(r->a->force ? INV_VERB_REINSTALL : INV_VERB_INSTALL, r->inv, as, r->m, again, ADMIN_TARGETS_MAX);
        if (na < 0) {
            say(r, "couldn't check every installed version of rekordbox: nothing changed");
            x = ADMIN_EXIT_CHECK_FAILED;
        } else
            x = same_scope(r, again, na, xs);
    }
    /* each folder takes a write (Controlled Folder Access refuses it before anything else
     * changes); a stopped run's temp files stay until that target's own change (re-review
     * blocker B): the probe takes another name when one is there */
    for (int t = 0; t < ns && !x; t++) {
        char probe[sizeof PROBE + 16], rnd[9];
        plat_stat st;
        snprintf(probe, sizeof probe, "%s", PROBE);
        if (plat_stat_at(held[t], probe, &st) != ENOENT) {
            r->os->random_hex(r->os->ctx, rnd, 8);
            snprintf(probe, sizeof probe, "%s.%s", PROBE, rnd);
        }
        if (write_new(held[t], probe, NULL, 0) || plat_unlink_at(held[t], probe)) {
            unlink_quiet(held[t], probe);
            say(r, "couldn't write into rekordbox %s: nothing changed", scope[t].version);
            say_target(r, scope[t].version, ADMIN_EXIT_CHECK_FAILED, ADMIN_TARGET_WHAT_REFUSED);
            x = ADMIN_EXIT_CHECK_FAILED;
        }
    }
    if (x) goto out;
    /* the shared writes' checks, still before any write: the first install's worker staged and
     * checked (I75), ort\'s copy decided */
    int need_worker = !bin_has_worker(r);
    if (need_worker) {
        if (plat_dir_open(r->a->source, &src)) {
            say(r, "couldn't open the app's payload folder: nothing changed");
            x = ADMIN_EXIT_CHECK_FAILED;
            goto out;
        }
        x = stage_worker(r, src);
        plat_dir_close(src);
        if (x) goto out;
    }
    const inv_install *first_row = &r->inv->it[rows[0]];
    char first_orig[RUN_PATH];
    if (!(as[rows[0]].live == COMPAT_LIVE_PIONEER ? path_in_version(r, first_row->name, LIVE, first_orig, sizeof first_orig)
                                                  : join4(first_orig, sizeof first_orig, r->e->root, "pioneer", first_row->version, LIVE))) {
        x = ADMIN_EXIT_CHECK_FAILED;
        goto out;
    }
    if ((x = shared_runtime(r, first_orig, as[rows[0]].original_sha, bridges, 1))) goto out;
    /* the system-wide close gate again, immediately before the shared writes: after the staging,
     * the dry run and the payload files' copies (review finding 8) */
    if ((x = keep_payload_copies(r)) || (x = processes_ok(r)) || (x = commit_payload(r))) goto out;
    /* the shared writes, once (step 5) */
    if ((!plat_dir_ok(r->pioneer) && sub_dir(r, r->root, "", "pioneer", 1, &r->pioneer)) || (!plat_dir_ok(r->ort) && sub_dir(r, r->root, "", "ort", 1, &r->ort))
        || (!plat_dir_ok(r->receipts) && sub_dir(r, r->root, "", RECEIPT_DIR, 1, &r->receipts))) {
        x = ADMIN_EXIT_CHECK_FAILED;
        goto out;
    }
    /* the first install: the staged, hash-checked worker goes into bin\ too (I75) */
    if ((need_worker && (x = place_worker(r))) || (x = shared_runtime(r, first_orig, as[rows[0]].original_sha, bridges, 0))) goto out;
    x = 0;
    /* each target in version order (steps 6, 7) */
    int done = 0;
    for (int t = 0; t < ns; t++) {
        int k = rows[t];
        r->vdir = held[t];
        snprintf(r->vname, sizeof r->vname, "%s", r->inv->it[k].name);
        snprintf(r->version, sizeof r->version, "%s", r->inv->it[k].version);
        const char *what = ADMIN_TARGET_WHAT_KEPT;
        /* and before each target: a rekordbox started meanwhile in any session stops the batch
         * here, the targets before it staying done (review finding 8) */
        int c = processes_ok(r);
        if (!c) c = install_one(r, &r->inv->it[k], &as[k], &xs[k], &what);
        r->vdir = plat_dir_cwd();
        say_target(r, scope[t].version, c, what);
        if (c) {
            for (int u = t + 1; u < ns; u++) say_target(r, scope[u].version, -1, ADMIN_TARGET_WHAT_NOT_STARTED);
            x = done ? ADMIN_EXIT_PARTIAL : c;
            break;
        }
        done++;
#ifdef ADMIN_TEST_HOLD
        if (t + 1 < ns) admin_test_mid_hold(r->os, "batch: a target done, the next not yet swapped", NULL);
#endif
    }
    if (done) {
        say(r, "@bridge %s", r->m->win_bridge.sha256);
        drop_old_workers(r);
    }
    /* a stopped run's leftovers in the root go once a target changed: a refused run keeps them */
    if (r->touched) clean_leftovers(r);
    if (!x) say(r, "Stems Cache installed");
out:
    for (int t = 0; held && t < ADMIN_TARGETS_MAX; t++) close_dir(&held[t]);
    free(held);
    free(xs);
    free(as);
    return x;
}

static int do_install_worker(run *r) {
    int x;
    if ((x = open_root(r, 1)) || (x = make_stage(r))) return x;
    plat_dir src;
    if (plat_dir_open(r->a->source, &src)) {
        say(r, "couldn't open the app's payload folder: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (!(x = verify_payload(r, src)) && !(x = manifest_ok(r)) && !(x = stage_worker(r, src)) && !(x = commit_payload(r))) {
        clean_leftovers(r);
        x = place_worker(r);
    }
    plat_dir_close(src);
    if (x) return x;
    drop_old_workers(r);
    say(r, "run the app's own worker now");
    return ADMIN_EXIT_RUN_AGAIN;
}

/* the root folder itself, empty and no longer held: 0, or -1 (it stays) */
static int rmdir_root(run *r) {
    char parent[RUN_PATH];
    snprintf(parent, sizeof parent, "%s", r->e->root);
    char *s = strrchr(parent, SEP[0]);
    plat_dir p;
    if (!s || s == parent) return -1;
    *s = 0;
    if (plat_dir_open(parent, &p)) return -1;
    int e = r->os->rmdir_at(r->os->ctx, p, s + 1);
    plat_dir_close(p);
    return e && e != ENOENT ? -1 : 0;
}

/* everything in the root but the worker's own file, then the lock, then the worker's file and
 * its folders (I77, J7): 0, or ADMIN_EXIT_FAILED when something stayed */
static int remove_root(run *r) {
    close_dir(&r->receipts);
    close_dir(&r->pioneer);
    close_dir(&r->ort);
    close_dir(&r->bin);
    close_dir(&r->stage);
    int failed = 0;
    int kept[NAMES_MAX] = {0};
    names *l = malloc(sizeof *l), *k = malloc(sizeof *k);
    if (!l || !k || list_dir(r->root, l)) failed = 1;
    for (int i = 0; !failed && i < l->n; i++) {
        if (!strcmp(l->name[i], "lock")) continue;
        if (!r->verified_change && kept_stage(r, l->name[i], l->kind[i])) {
            kept[i] = 1;
            continue;
        }
        if (r->self_in_bin && !strcmp(l->name[i], "bin")) {
            plat_dir b;
            if (plat_dir_open_at(r->root, "bin", &b)) {
                failed = 1;
                continue;
            }
            if (list_dir(b, k)) {
                plat_dir_close(b);
                failed = 1;
                continue;
            }
            for (int j = 0; j < k->n; j++) {
                if (strcmp(k->name[j], r->self12)) {
                    if (rm_tree(r, b, k->name[j], 0)) failed = 1;
                    continue;
                }
                plat_dir w;
                names *own = malloc(sizeof *own);
                if (!own || plat_dir_open_at(b, r->self12, &w)) {
                    free(own);
                    failed = 1;
                    continue;
                }
                if (list_dir(w, own)) failed = 1;
                else
                    for (int q = 0; q < own->n; q++)
                        if (strcmp(own->name[q], ADMIN_WORKER_NAME) && rm_tree(r, w, own->name[q], 0)) failed = 1;
                free(own);
                plat_dir_close(w);
            }
            plat_dir_close(b);
            continue;
        }
        if (rm_tree(r, r->root, l->name[i], 0)) {
            say(r, "couldn't remove %s", l->name[i]);
            failed = 1;
        }
    }
    /* With no verified target change, remove journals only after the rest of the root cleanup. */
    for (int i = 0; !failed && i < l->n; i++) {
        if (kept[i] && rm_tree(r, r->root, l->name[i], 0)) {
            say(r, "couldn't remove %s", l->name[i]);
            failed = 1;
        }
    }
    free(l);
    free(k);
    if (r->lock) r->os->unlock(r->os->ctx, r->lock);
    r->lock = NULL;
    if (!failed && unlink_quiet(r->root, "lock")) failed = 1;
    close_dir(&r->root);
    if (failed) {
        say(r, "some of %s's files couldn't be removed", r->e->root);
        return ADMIN_EXIT_FAILED;
    }
    if (r->self_in_bin) {
        char d12[RUN_PATH], b[RUN_PATH];
        int reboot = 0;
        join(b, sizeof b, r->e->root, "bin");
        join(d12, sizeof d12, b, r->self12);
        const char *folders[3] = {d12, b, r->e->root};
        if (r->os->delete_self(r->os->ctx, r->self, folders, &reboot)) {
            say(r, "couldn't remove the worker's own file");
            return ADMIN_EXIT_FAILED;
        }
        if (reboot & ADMIN_FOLDERS_STAY) say(r, "some empty folders of %s stay", r->e->root);
        if (reboot & ADMIN_GONE_AT_RESTART) {
            say(r, "@restart");
            say(r, "the last file of %s is removed at the next restart", r->e->root);
            return ADMIN_EXIT_OK;
        }
        if (reboot & ADMIN_FOLDERS_STAY) return ADMIN_EXIT_OK;
    } else if (rmdir_root(r))   /* not run from bin\ (a test, or a root without any worker) */
        return ADMIN_EXIT_FAILED;
    say(r, "removed %s", r->e->root);
    return ADMIN_EXIT_OK;
}

/* the verified payload the root keeps (payload_saved: the pair that raised the floor), for the
 * verbs without a source: its bindings are the identity rule's (b). 0 when there is none */
static int saved_payload(run *r) {
    payload_files pf = {r->e->root, r->e->root, r->e->keys, r->e->nkeys, r->e->minimum};
    r->have_m = payload_saved(&pf, r->m) == PAYLOAD_OK && r->m->has_windows;
    return r->have_m;
}

/* Whether pioneer\<v>\ (its complete set's sha256 set) may go back into the held folder vd named
 * vname (D4, D13; never on X.Y.Z alone): (a) the folder's receipt rc names this folder (its name
 * and identity), its rekordbox.exe and that set; or (b) the set is the signed binding's original
 * for this folder's rekordbox (the caller's live DLL is ours, half-run or absent, never stock).
 * receipt_only: (a) alone (an Uninstall over an incomplete inventory: restore-only work on the
 * folders our receipts bind, the D3 ruling of 2026-11-20) */
static int set_may_return(run *r, plat_dir vd, const char *vname, const char *v, const char *set, const receipt *rc, int receipt_only) {
    char p[RUN_PATH];
    compat_file exe;
    plat_stat st;
    if (!set[0] || plat_dir_stat(vd, &st) || !path_in_version(r, vname, "rekordbox.exe", p, sizeof p) || facts(r, p, 1, &exe) || !exe.readable) return 0;
    if (rc) {
        char id[PLAT_ID_HEX_MAX], fv[COMPAT_VERSION_MAX];
        plat_id_hex(st.id, id);
        if (compat_folder_version(vname, fv, sizeof fv) && !strcmp(fv, rc->version) && !strcmp(rc->version, v) && !strcmp(rc->folder_id, id)
            && !strcmp(rc->exe_sha256, exe.sha256) && !strcmp(rc->original_sha256, set))
            return 1;
    }
    compat_file o;
    return !receipt_only && r->have_m && exe.pin == COMPAT_PIN_OK && join4(p, sizeof p, r->e->root, "pioneer", v, LIVE) && !facts_code(r, p, &o) && !strcmp(o.sha256, set)
           && inv_original_bound(r->m, exe.pe.product_version, &o);
}

/* Pioneer's file back into one held folder whose live DLL is ours, half-run or absent: the
 * identity rule (3 when the set may not go back), rekordbox.exe (there: 15) and the live DLL
 * (which may be missing) admin-only with no reparse point (10, review finding 4), rekordbox closed
 * everywhere and the DLL not mapped (5), rekordbox.exe still the confirmed one (exe: its sha256,
 * Restore's; NULL for Uninstall) at the replacement (15, re-review finding 4) and held against
 * writes and replacement until the put-back and its retry are done, then the one put-back
 * routine; its receipt goes. 0 or the folder's code */
static int restore_one(run *r, plat_dir vd, const char *vname, const char *v, int cls, const char *set, const receipt *rc, int receipt_only, const char *exe) {
    int x;
    if (!set_may_return(r, vd, vname, v, set, rc, receipt_only)) {
        say(r, "the saved copy of rekordbox %s's file isn't one for this installation: reinstall rekordbox %s using Pioneer's installer. Nothing changed", v,
            v);
        return ADMIN_EXIT_NO_ORIGINALS;
    }
    int e = r->os->file_admin_only(r->os->ctx, vd, "rekordbox.exe");
    if (e == ENOENT) {
        say(r, "rekordbox %s's rekordbox.exe changed after it was checked: nothing changed", v);
        return ADMIN_EXIT_SCOPE_CHANGED;
    }
    if (!e) e = r->os->file_admin_only(r->os->ctx, vd, LIVE);
    if (e && e != ENOENT) {
        say(r, "rekordbox %s's files can be changed by other accounts: reinstall rekordbox %s with Pioneer's installer. Nothing changed", v, v);
        return ADMIN_EXIT_NOT_PIONEERS;
    }
    /* the system-wide close gate again before each folder's change (review finding 8) */
    if ((x = processes_ok(r))) return x;
    if (cls != ADMIN_LIVE_ABSENT && (x = not_mapped(r, vd, v))) return x;
    void *exe_hold = NULL;
    if (exe) {
        /* held against writes and replacement first, then read: the put-back and its retry run
         * under the checked rekordbox.exe (re-review 2, finding 4); the held folder keeps its path */
        char p[RUN_PATH];
        compat_file f;
        int he = r->os->hold_file(r->os->ctx, vd, "rekordbox.exe", &exe_hold);
        if (he && he != ENOENT) {
            say(r, "couldn't hold rekordbox %s's rekordbox.exe (error %d): nothing changed", v, he);
            return ADMIN_EXIT_CHECK_FAILED;
        }
        if (he || !path_in_version(r, vname, "rekordbox.exe", p, sizeof p) || facts(r, p, 0, &f) || !f.readable || strcmp(f.sha256, exe)) {
            if (exe_hold) r->os->release_file(r->os->ctx, exe_hold);
            say(r, "rekordbox %s isn't the installation that was checked: nothing changed", v);
            return ADMIN_EXIT_SCOPE_CHANGED;
        }
    }
    say(r, cls == ADMIN_LIVE_ABSENT ? "rekordbox %s's ONNX Runtime is missing: putting Pioneer's file back" : "putting Pioneer's file back into rekordbox %s", v);
    x = put_back_hard(r, vd, vname, v, set);
    if (exe_hold) r->os->release_file(r->os->ctx, exe_hold);
    if (x) return x;
    if (plat_dir_ok(r->receipts) && unlink_quiet(r->receipts, v)) say(r, "couldn't remove rekordbox %s's receipt", v);
    return 0;
}

/* Uninstall (mv-spec §2.2): every "rekordbox X.Y.Z" folder on disk checked by itself, and
 * Pioneer's file put back wherever ours is, a failure in one folder not stopping the others
 * (16, or its code); the root goes only when every folder ended Pioneer's or with nothing of ours
 * and the inventory and the disk list were whole (D3), else @root-kept */
static int do_uninstall(run *r) {
    int x = open_root(r, 0), have_root = x == 0;
    if (x && x != ENOENT) return x;
    if ((x = processes_ok(r)) || (x = inventory(r, 0)) || (x = hold_rbroot(r))) return x;
    saved_payload(r);
    clean_leftovers(r);
    names *l = malloc(sizeof *l);
    if (!l || list_dir(r->rbroot, l) || l->over) {
        free(l);
        say(r, "couldn't list rekordbox's folders: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    int folders = 0, put = 0, failed = 0, first = 0, unchecked = 0;
    for (int i = 0; i < l->n; i++) {
        char v[COMPAT_VERSION_MAX], set[65];
        if (!compat_folder_version(l->name[i], v, sizeof v)) continue;
        folders++;
        plat_dir d;
        int code = 0;
        const char *what = ADMIN_TARGET_WHAT_KEPT;
        if (l->kind[i] != PLAT_DIR || r->os->hold_dir(r->os->ctx, r->rbroot, l->name[i], &d)) {
            say(r, "rekordbox %s's folder isn't the administrators' alone: left as it is", v);
            code = ADMIN_EXIT_NOT_PIONEERS, unchecked++;
            what = ADMIN_TARGET_WHAT_REFUSED;
        } else {
            /* a stopped install's temp files (the bridge's bytes in .rbsp-new) go, whatever the state */
            unlink_quiet(d, NEW);
            unlink_quiet(d, PROBE);
            receipt rc;
            int have_rc = !receipt_read(r->e->root, v, &rc);
            r->rc = have_rc ? &rc : NULL;
            compat_file f;
            int c = classify(r, l->name[i], v, &f, set);
            if (c == ADMIN_LIVE_NO_ANSWER) say(r, "couldn't check rekordbox %s's ONNX Runtime: left as it is", v), code = ADMIN_EXIT_CHECK_FAILED;
            else if (c == ADMIN_LIVE_REFUSED && f.marker) {
                say(r, "no complete saved copy of rekordbox %s's file: left as it is (reinstall rekordbox %s from Pioneer's installer)", v, v);
                code = ADMIN_EXIT_NO_ORIGINALS;
            } else if (c == ADMIN_LIVE_OURS || c == ADMIN_LIVE_HALF_RUN || c == ADMIN_LIVE_ABSENT) {
                if (!(code = restore_one(r, d, l->name[i], v, c, set, have_rc ? &rc : NULL, !r->inv->complete, NULL))) {
                    what = ADMIN_TARGET_WHAT_PUT_BACK;
                    put++;
                }
            } else if (c == ADMIN_LIVE_REFUSED)
                say(r, "rekordbox %s's ONNX Runtime is neither AlphaTheta's nor ours: left as it is", v);   /* nothing of ours there */
            r->rc = NULL;
            plat_dir_close(d);
            if (!code && strcmp(what, ADMIN_TARGET_WHAT_PUT_BACK)) continue;   /* Pioneer's, or nothing of ours: no record */
        }
        say_target(r, v, code, what);
        if (code && !first) first = code;
        failed += code != 0;
    }
    free(l);
    if (!folders) {
        say(r, "rekordbox isn't installed: nothing changed");
        return ADMIN_EXIT_REKORDBOX_MISSING;
    }
#ifdef ADMIN_TEST_HOLD
    if (put) admin_test_mid_hold(r->os, "Pioneer's file put back into rekordbox, our folder not yet removed", NULL);
#endif
    x = failed ? (put ? ADMIN_EXIT_PARTIAL : first) : 0;
    if (!have_root) {
        if (!failed) say(r, "there is nothing of ours to remove");
        return x;
    }
    if (failed || !r->inv->complete) {
        say(r, "@root-kept %s", failed > unchecked ? "bridge-remains" : failed ? "folder-unchecked" : "inventory-incomplete");
        say(r, "%s stays: %s", r->e->root, failed ? "a version still has Stems Cache" : "not every installed rekordbox could be checked");
        return x;
    }
    close_dir(&r->rbroot);
    if ((x = remove_root(r))) return x;
    say(r, "Stems Cache removed: rekordbox has Pioneer's file again");
    return ADMIN_EXIT_OK;
}

/* Put Back Pioneer's File (mv-spec §2.2) for each --targets folder: first every folder held and
 * its rekordbox.exe read again against the app's checked identity (exe16; 15 for the run when one
 * isn't, nothing changed), then Pioneer's file back where ours, a half-run or nothing is, under
 * the identity rule; one target's failure doesn't stop the others (16, or its code). The root,
 * ort\ and the other folders' bridges stay. */
static int do_restore(run *r) {
    int x = open_root(r, 0);
    if (x && x != ENOENT) return x;
    if ((x = processes_ok(r)) || (x = inventory(r, 0)) || (x = hold_rbroot(r))) return x;
    saved_payload(r);
    plat_dir held[ADMIN_TARGETS_MAX];
    int code[ADMIN_TARGETS_MAX], stale = 0;
    /* every requested identity first, each folder held and its rekordbox.exe read now: one that
     * isn't the installation the app checked is 15 for the run, nothing changed anywhere (review
     * finding 4) */
    for (int t = 0; t < r->a->nt; t++) {
        const char *v = r->a->t[t].version;
        int k = row_of(r, v);
        held[t] = plat_dir_cwd();
        code[t] = 0;
        int e = k < 0 ? ENOENT : hold_version(r, r->inv->it[k].name, r->inv->it[k].dir_id, &held[t]);
        char p[RUN_PATH];
        compat_file f;
        memset(&f, 0, sizeof f);
        if (!e && (!path_in_version(r, r->inv->it[k].name, "rekordbox.exe", p, sizeof p) || facts(r, p, 0, &f) || !f.readable)) e = ENOENT;
        if (k < 0 || e == ENOENT || e == HOLD_MOVED || strncmp(r->inv->it[k].exe.sha256, r->a->t[t].exe16, 16)
            || (!e && strcmp(f.sha256, r->inv->it[k].exe.sha256))) {
            say(r, "rekordbox %s isn't the installation that was checked: nothing changed", v);
            say_target(r, v, ADMIN_EXIT_SCOPE_CHANGED, ADMIN_TARGET_WHAT_REFUSED);
            code[t] = ADMIN_EXIT_SCOPE_CHANGED;
            stale = 1;
        } else if (e) {
            say(r, "rekordbox %s's folder isn't the administrators' alone: nothing changed there", v);
            code[t] = ADMIN_EXIT_NOT_PIONEERS;
        }
    }
    if (stale) {
        for (int t = 0; t < r->a->nt; t++) {
            if (!code[t]) say_target(r, r->a->t[t].version, -1, ADMIN_TARGET_WHAT_NOT_STARTED);
            close_dir(&held[t]);
        }
        return ADMIN_EXIT_SCOPE_CHANGED;
    }
    int ok = 0, first = 0;
    for (int t = 0; t < r->a->nt; t++) {
        const char *v = r->a->t[t].version;
        const char *what = code[t] ? ADMIN_TARGET_WHAT_REFUSED : ADMIN_TARGET_WHAT_KEPT;
        plat_dir d = held[t];
        if (!code[t]) {
            int k = row_of(r, v);
            const char *vname = r->inv->it[k].name;
            unlink_quiet(d, NEW);
            unlink_quiet(d, PROBE);
            receipt rc;
            int have_rc = !receipt_read(r->e->root, v, &rc);
            r->rc = have_rc ? &rc : NULL;
            compat_file f;
            char set[65];
            int c = classify(r, vname, v, &f, set);
            if (c == ADMIN_LIVE_PIONEER) {
                say(r, "rekordbox %s has Pioneer's file already", v);
                what = ADMIN_TARGET_WHAT_ALREADY;
            } else if (c == ADMIN_LIVE_NO_ANSWER) {
                say(r, "couldn't check rekordbox %s's ONNX Runtime: nothing changed there", v);
                code[t] = ADMIN_EXIT_CHECK_FAILED;
            } else if (c == ADMIN_LIVE_NOTHING || c == ADMIN_LIVE_REFUSED) {
                say(r, "no complete saved copy of rekordbox %s's file: nothing changed there (reinstall rekordbox %s from Pioneer's installer)", v, v);
                code[t] = ADMIN_EXIT_NO_ORIGINALS;
            } else if (!(code[t] = restore_one(r, d, vname, v, c, set, have_rc ? &rc : NULL, 0, r->inv->it[k].exe.sha256)))
                what = ADMIN_TARGET_WHAT_PUT_BACK;
            r->rc = NULL;
        }
        close_dir(&held[t]);
        say_target(r, v, code[t], what);
        if (!code[t]) ok++;
        else if (!first) first = code[t];
    }
    clean_leftovers(r);
    if (ok) drop_old_workers(r);
    return first ? (ok ? ADMIN_EXIT_PARTIAL : first) : ADMIN_EXIT_OK;
}

static int do_remove_root_folder(run *r) {
    int x = open_root(r, 0);
    if (x == ENOENT) {
        say(r, "%s isn't there: nothing to remove", r->e->root);
        return ADMIN_EXIT_OK;
    }
    if (x || (x = processes_ok(r)) || (x = inventory(r, 1))) return x;
    /* only behind a fresh, whole sweep that finds no rekordbox folder at all */
    for (int i = 0; i < r->inv->n; i++)
        if (r->inv->it[i].trust != INV_STALE) {
            say(r, "rekordbox is installed: %s stays", r->e->root);
            return ADMIN_EXIT_CHECK_FAILED;
        }
    return remove_root(r);
}

int admin_run(const admin_env *e, const admin_os *os, const admin_args *a) {
    run *r = calloc(1, sizeof *r);
    payload_manifest *m = calloc(1, sizeof *m);
    if (!r || !m) {
        free(r);
        free(m);
        return ADMIN_EXIT_FAILED;
    }
    r->e = e;
    r->os = os;
    r->a = a;
    r->m = m;
    r->root = r->pioneer = r->ort = r->bin = r->stage = r->rbroot = r->vdir = r->receipts = plat_dir_cwd();
    if (!os->self_path(os->ctx, r->self, sizeof r->self)) r->self_in_bin = admin_self_in_bin(e->root, r->self, r->self12);
    int x;
    switch (a->verb) {
    case ADMIN_INSTALL: x = do_install(r); break;
    case ADMIN_UNINSTALL: x = do_uninstall(r); break;
    case ADMIN_RESTORE: x = do_restore(r); break;
    case ADMIN_INSTALL_WORKER: x = do_install_worker(r); break;
    case ADMIN_REMOVE_ROOT_FOLDER: x = do_remove_root_folder(r); break;
    default:
        say(r, "unknown action: nothing changed");
        x = ADMIN_EXIT_CHECK_FAILED;
    }
    /* finish: a change not verified is rolled back (never reached: every path after a change
     * returns through rollback), then the staging folder, the leftovers, the lock */
    if (r->changing) x = rollback(r);
    if (plat_dir_ok(r->vdir) && a->verb != ADMIN_INSTALL) {
        unlink_quiet(r->vdir, NEW);
        unlink_quiet(r->vdir, PROBE);
    }
    close_dir(&r->vdir);
    close_dir(&r->rbroot);
    close_dir(&r->pioneer);
    close_dir(&r->ort);
    close_dir(&r->bin);
    close_dir(&r->receipts);
    /* an Install that changed no rekordbox folder takes away what it made in the root, and only
     * that, under the lock (the owner's PC: a refused run's root reads as Stems Cache traces;
     * re-review blocker A: never another run's work), then a root and a lock it made itself */
    int undo_failed = 0, undone = 0;
    if (a->verb == ADMIN_INSTALL && x != ADMIN_EXIT_OK && !r->touched && r->lock) {
        undo_failed = undo_made(r) != 0;
        undone = !undo_failed;
    }
    /* an undo that left an entry keeps the staging folder: the saved copies of what it couldn't
     * put back stay there */
    if (undo_failed) {
        x = ADMIN_EXIT_UNDO_INCOMPLETE;   /* never "nothing changed" */
        if (r->stage_name[0]) {
            close_dir(&r->stage);
            say(r, "rollback incomplete: the saved copies stay in %s\\%s", r->e->root, r->stage_name);
        } else
            say(r, "rollback incomplete");
    } else if (plat_dir_ok(r->root))
        drop_stage(r);
    if (r->lock) os->unlock(os->ctx, r->lock);
    /* a lock file this run made in a root it left as it was goes */
    if (r->lock && r->lock_made && !r->touched && (!r->committed || undone)) unlink_quiet(r->root, "lock");
    close_dir(&r->root);
    if (undone && r->root_made && rmdir_root(r)) say(r, "couldn't remove %s again", r->e->root);
    free(r->inv);
    free(r->pj);
    free(r->ps);
    free(m);
    free(r);
    return x;
}
