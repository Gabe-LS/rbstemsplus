/* The elevated worker's runs (worker.h): install, uninstall, restore, install-worker and
 * remove-root-folder, in pure C11 over plat.h, payload.c, compat.c and admin_os. */
#include "worker.h"

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
static const char RECORD[] = "installed-bridge";
static const char STAGED_BRIDGE[] = "bridge.dll";
static const char STAGED_WORKER[] = "worker.exe";
static const char MARKER_SYMBOL[] = "rbstems_marker";
static const char ORT_SYMBOL[] = "OrtGetApiBase";

enum { NAMES_MAX = 128, RUN_PATH = ADMIN_PATH_MAX + 64 };

typedef struct {
    const admin_env *e;
    const admin_os *os;
    const admin_args *a;
    plat_dir root, pioneer, ort, bin, stage, rbroot, vdir;
    void *lock;
    char self[ADMIN_PATH_MAX], self12[13];
    int self_in_bin;
    char stage_name[32];
    char vname[COMPAT_PATH_MAX], version[COMPAT_VERSION_MAX];   /* the current version folder */
    payload_manifest *m;
    int have_m;
    char record[65];          /* the installed-bridge record, "" when none */
    char new_worker12[13];    /* a worker this run put into bin\ */
    int changing;             /* rekordbox's current folder was changed and isn't verified yet */
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

/* a held folder `name` in parent, made when missing and create is set: 0 with *out; ENOENT; or
 * ADMIN_EXIT_CHECK_FAILED after saying why */
static int sub_dir(run *r, plat_dir parent, const char *name, int create, plat_dir *out) {
    *out = plat_dir_cwd();
    int e = r->os->hold_dir(r->os->ctx, parent, name, out);
    if (e == ENOENT && create) {
        e = r->os->mkdir_at(r->os->ctx, parent, name);
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

static int listed_bridge(run *r, const char *sha) {
    if (!sha[0]) return 0;
    if (r->have_m && !strcmp(sha, r->m->win_bridge.sha256)) return 1;
    return r->record[0] && !strcmp(sha, r->record);
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

/* rekordbox.exe in the held version folder, pinned now, its version agreeing with the folder's */
static int exe_ok(run *r) {
    char exe[RUN_PATH];
    compat_file f;
    if (!path_in_version(r, r->vname, "rekordbox.exe", exe, sizeof exe) || facts(r, exe, 1, &f) || !f.readable) {
        say(r, "couldn't check rekordbox.exe: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (f.pin == COMPAT_PIN_ERROR) {
        say(r, "couldn't check rekordbox.exe's signature: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (f.pin != COMPAT_PIN_OK || !compat_same_short_version(f.pe.product_version, r->version)) {
        say(r, "rekordbox.exe isn't AlphaTheta's rekordbox %s: reinstall rekordbox with Pioneer's installer. Nothing changed", r->version);
        return ADMIN_EXIT_NOT_PIONEERS;
    }
    return 0;
}

/* ------------------------------------------------------------------ the put-back (I49, I71) */

enum { PB_OK = 0, PB_UNCHANGED = -1, PB_CHANGED = -2 };

/* Pioneer's file of version v back into the held version folder vdir (named vname), from
 * pioneer\<v>\: the saved original's sha256 against SHA256SUMS and its pin; copied to
 * onnxruntime.dll.rbsp-new, flushed, its sha256 and pin checked; renamed over the live file, or,
 * when there is none, into its place by a rename that fails if one appeared meanwhile; then the
 * result verified again. Made in place, the new file has the folder's inherited security. The
 * one routine of the rollback, uninstall and restore (I49). */
static int put_back(run *r, plat_dir vdir, const char *vname, const char *v) {
    char sha[65], saved[RUN_PATH], newp[RUN_PATH], live[RUN_PATH];
    if (!set_complete(r, v, sha)) {
        say(r, "no complete saved copy of rekordbox %s's file", v);
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

/* whether the live file of vname is Pioneer's now (pinned) */
static int is_pioneers(run *r, const char *vname) {
    char live[RUN_PATH];
    compat_file f;
    if (!path_in_version(r, vname, LIVE, live, sizeof live) || facts(r, live, 0, &f) || !f.readable || f.marker) return 0;
    return !facts(r, live, 1, &f) && f.pin == COMPAT_PIN_OK && !f.marker;
}

/* the rollback after the current folder was changed (Scripts.swift's rollback): 6, 4 or 11 */
static int rollback(run *r) {
    say(r, "failed after rekordbox was changed: putting Pioneer's file back");
    put_back(r, r->vdir, r->vname, r->version);
    r->changing = 0;
    if (is_pioneers(r, r->vname)) {
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
static int put_back_hard(run *r, plat_dir d, const char *vname, const char *v) {
    int pb = put_back(r, d, vname, v), changed = pb == PB_CHANGED;
    if (changed) pb = put_back(r, d, vname, v);
    if (pb == PB_OK || is_pioneers(r, vname)) return 0;
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
    int e = r->os->root_open(r->os->ctx, r->e->root, create, &r->root);
    if (e == ENOENT && !create) return ENOENT;
    if (e == EACCES || e == ELOOP || e == ENOTDIR) {
        say(r, "%s isn't the administrators' alone: nothing changed", r->e->root);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (e) {
        say(r, "couldn't open %s (error %d): nothing changed", r->e->root, e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    e = r->os->lock(r->os->ctx, r->root, &r->lock);
    if (e == EBUSY) {
        say(r, "another change to rekordbox is running: nothing changed");
        return ADMIN_EXIT_ALREADY_RUNNING;
    }
    if (e) {
        say(r, "couldn't take the lock (error %d): nothing changed", e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    int x;
    if ((x = sub_dir(r, r->root, "pioneer", 0, &r->pioneer)) && x != ENOENT) return x;
    if ((x = sub_dir(r, r->root, "ort", 0, &r->ort)) && x != ENOENT) return x;
    if ((x = sub_dir(r, r->root, "bin", 0, &r->bin)) && x != ENOENT) return x;
    /* leftovers: staging folders, renamed workers (*.del) the boot-time delete left (J7) */
    names *l = malloc(sizeof *l);
    if (!l) return ADMIN_EXIT_FAILED;
    if (!list_dir(r->root, l))
        for (int i = 0; i < l->n; i++)
            if (!strncmp(l->name[i], "stage.", 6) && rm_tree(r, r->root, l->name[i], 0))
                say(r, "couldn't remove the leftover %s", l->name[i]);
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
    {
        char rec[80];
        unsigned char *b = NULL;
        size_t n = 0;
        r->record[0] = 0;
        if (!read_whole(r->root, RECORD, 80, &b, &n) && n == 65 && b[64] == '\n') {
            memcpy(rec, b, 64);
            rec[64] = 0;
            if (payload_is_sha256(rec)) memcpy(r->record, rec, 65);
        }
        free(b);
    }
    return 0;
}

/* rekordbox's current version folder, from its uninstall entries: 9, 10, 4, or 0 with it held
 * (rbroot and vdir), rekordbox.exe pinned now, the version as the app checked it */
static int rekordbox_folder(run *r, int *installed) {
    compat_rekordbox *list = calloc(COMPAT_ENTRIES_MAX, sizeof *list);
    if (!list) return ADMIN_EXIT_FAILED;
    int cur = -1, st = COMPAT_RB_NOT_INSTALLED;
    compat_memo_clear();   /* every fact here read now */
    compat_find_rekordbox(r->os->compat, r->e->rekordbox, list, COMPAT_ENTRIES_MAX, &cur, &st);
    if (installed) *installed = st != COMPAT_RB_NOT_INSTALLED;
    int x = 0;
    if (st == COMPAT_RB_NOT_INSTALLED) {
        say(r, "rekordbox isn't installed: nothing changed");
        x = ADMIN_EXIT_REKORDBOX_MISSING;
    } else if (st == COMPAT_RB_UNVERIFIED) {
        say(r, "rekordbox isn't installed the way Pioneer's installer leaves it: nothing changed");
        x = ADMIN_EXIT_NOT_PIONEERS;
    } else if (st != COMPAT_RB_OK || cur < 0) {
        say(r, "couldn't check rekordbox: nothing changed");
        x = ADMIN_EXIT_CHECK_FAILED;
    } else if (r->a->version[0] && strcmp(list[cur].folder_version, r->a->version)) {
        say(r, "rekordbox is %s now, not %s as checked: nothing changed", list[cur].folder_version, r->a->version);
        x = ADMIN_EXIT_CHECK_FAILED;
    } else {
        const char *s = strrchr(list[cur].folder, SEP[0]);
        snprintf(r->vname, sizeof r->vname, "%s", s ? s + 1 : list[cur].folder);
        snprintf(r->version, sizeof r->version, "%s", list[cur].folder_version);
    }
    free(list);
    if (x) return x;
    int e = r->os->hold_dir(r->os->ctx, plat_dir_cwd(), r->e->rekordbox, &r->rbroot);
    if (!e) e = r->os->hold_dir(r->os->ctx, r->rbroot, r->vname, &r->vdir);
    if (e == ENOENT) {
        say(r, "rekordbox's folder is missing: nothing changed");
        return ADMIN_EXIT_REKORDBOX_MISSING;
    }
    if (e) {
        say(r, "rekordbox's folder isn't the administrators' alone: reinstall rekordbox with Pioneer's installer. Nothing changed");
        return e == ELOOP || e == EACCES || e == ENOTDIR ? ADMIN_EXIT_NOT_PIONEERS : ADMIN_EXIT_CHECK_FAILED;
    }
    if ((x = exe_ok(r))) return x;
    e = r->os->file_admin_only(r->os->ctx, r->vdir, "rekordbox.exe");
    if (!e) e = r->os->file_admin_only(r->os->ctx, r->vdir, LIVE);
    if (e && e != ENOENT) {
        say(r, "rekordbox's files can be changed by other accounts: reinstall rekordbox with Pioneer's installer. Nothing changed");
        return ADMIN_EXIT_NOT_PIONEERS;
    }
    say(r, "rekordbox %s checked", r->version);
    return 0;
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
    char rnd[17];
    r->os->random_hex(r->os->ctx, rnd, 16);
    snprintf(r->stage_name, sizeof r->stage_name, "stage.%s", rnd);
    return sub_dir(r, r->root, r->stage_name, 1, &r->stage) ? ADMIN_EXIT_CHECK_FAILED : 0;
}

static void drop_stage(run *r) {
    close_dir(&r->stage);
    if (r->stage_name[0] && rm_tree(r, r->root, r->stage_name, 0)) say(r, "couldn't remove %s", r->stage_name);
    r->stage_name[0] = 0;
}

/* payload.json and its signature copied from --source into the staging folder and verified there
 * (size cap -> DER -> ECDSA over the raw bytes -> JSON -> the floor), then accepted: the floor
 * raised to it and the pair kept in the root. 0, ADMIN_EXIT_OLDER or ADMIN_EXIT_CHECK_FAILED. */
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
    int pr = payload_accept(&pf, data, n, sig, sn, r->m);
    free(data);
    free(sig);
    char floor[64], why[160];
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
    if (payload_read_floor(&pf, floor)) say(r, "@floor %s", floor);
    say(r, "payload.json %s verified (signed by the %s key)", r->m->payload_version, r->m->signed_by ? r->m->signed_by : "?");
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
 * admin_sha256, then moved into bin\<sha12>\. 0 or ADMIN_EXIT_CHECK_FAILED. */
static int install_worker_file(run *r, plat_dir src) {
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
    int x;
    if (!plat_dir_ok(r->bin) && sub_dir(r, r->root, "bin", 1, &r->bin)) return ADMIN_EXIT_CHECK_FAILED;
    char d12[13];
    memcpy(d12, sha, 12);
    d12[12] = 0;
    plat_dir w;
    if ((x = sub_dir(r, r->bin, d12, 1, &w))) return ADMIN_EXIT_CHECK_FAILED;
    unsigned char *b = NULL;
    size_t n = 0;
    char got[65] = "";
    if (!read_whole(w, ADMIN_WORKER_NAME, ADMIN_WORKER_MAX, &b, &n)) sha_hex(b, n, got);
    free(b);
    b = NULL;
    if (strcmp(got, sha)) {
        /* missing (or not that file: never the case for a 12-hex name it was made under) */
        e = plat_rename_at(r->stage, STAGED_WORKER, w, ADMIN_WORKER_NAME);
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

/* ------------------------------------------------------------------ several versions */

/* Before installing into the current version (bridge.md §2 "After an update", one bridged
 * version at a time): Pioneer's file back in every other version folder that holds our bridge, a
 * stopped run's half state, or no live file but a complete saved set; then the saved sets of the
 * versions that are Pioneer's now, or whose folder is gone. A version whose bridge can't be put
 * back (no complete saved set; a folder that can't be checked but has one) stops the install
 * before anything changes in the current folder. A version that can't be looked at keeps its
 * saved set. 0, or an exit code. */
static int other_versions(run *r) {
    names *l = malloc(sizeof *l), *keep = malloc(sizeof *keep);
    int x = 0;
    if (!l || !keep) {
        free(l);
        free(keep);
        return ADMIN_EXIT_FAILED;
    }
    keep->n = 0;
    if (list_dir(r->rbroot, l)) {
        free(l);
        free(keep);
        say(r, "couldn't list rekordbox's folders: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    for (int i = 0; i < l->n && !x; i++) {
        char v[COMPAT_VERSION_MAX], set[65];
        if (!compat_folder_version(l->name[i], v, sizeof v) || !strcmp(l->name[i], r->vname)) continue;
        plat_dir d;
        plat_stat st;
        /* a link, a reparse point or a file in a version's name: a folder that can't be held */
        if (l->kind[i] != PLAT_DIR || r->os->hold_dir(r->os->ctx, r->rbroot, l->name[i], &d)) {
            if (plat_dir_ok(r->pioneer) && plat_stat_at(r->pioneer, v, &st) != ENOENT) {
                say(r, "rekordbox %s's folder can't be checked and it has a saved copy: nothing changed", v);
                x = ADMIN_EXIT_CHECK_FAILED;
                break;
            }
            say(r, "rekordbox %s's folder can't be checked: left as it is", v);
            if (keep->n < NAMES_MAX) snprintf(keep->name[keep->n++], 256, "%s", v);
            continue;
        }
        /* a stopped run's temp files there */
        unlink_quiet(d, NEW);
        unlink_quiet(d, PROBE);
        compat_file f;
        int c = classify(r, l->name[i], v, &f, set);
        switch (c) {
        case ADMIN_LIVE_OURS:
        case ADMIN_LIVE_HALF_RUN:
        case ADMIN_LIVE_ABSENT:
            if ((x = not_mapped(r, d, v)) == 0) {
                say(r, "putting Pioneer's file back into rekordbox %s (one bridged version at a time)", v);
                x = put_back_hard(r, d, l->name[i], v);
            }
            break;
        case ADMIN_LIVE_REFUSED:
            if (f.marker) {
                say(r, "rekordbox %s still has a bridge and no complete saved copy: reinstall rekordbox %s with Pioneer's installer. Nothing changed",
                    v, v);
                x = ADMIN_EXIT_CHECK_FAILED;
            }
            break;
        case ADMIN_LIVE_NO_ANSWER:
            say(r, "couldn't check rekordbox %s's ONNX Runtime: left as it is", v);
            if (keep->n < NAMES_MAX) snprintf(keep->name[keep->n++], 256, "%s", v);
            break;
        default: break;   /* Pioneer's, or nothing of ours */
        }
        plat_dir_close(d);
    }
    if (!x && plat_dir_ok(r->pioneer) && !list_dir(r->pioneer, l))
        for (int i = 0; i < l->n; i++) {
            if (!strcmp(l->name[i], r->version)) continue;
            int kept = 0;
            for (int k = 0; k < keep->n; k++) kept |= !strcmp(keep->name[k], l->name[i]);
            if (kept) continue;
            char vn[300];
            plat_stat st;
            snprintf(vn, sizeof vn, "rekordbox %s", l->name[i]);
            /* only once that version is Pioneer's again, or its folder is gone */
            int e = plat_stat_at(r->rbroot, vn, &st);
            if ((e == ENOENT || (!e && is_pioneers(r, vn))) && rm_tree(r, r->pioneer, l->name[i], 0))
                say(r, "couldn't remove the saved copy of rekordbox %s", l->name[i]);
        }
    free(l);
    free(keep);
    return x;
}

/* ------------------------------------------------------------------ the verbs */

static int listed_ort(run *r, const char *sha) {
    for (int i = 0; i < r->m->n_win_ort_sha256; i++)
        if (!strcmp(sha, r->m->win_ort_sha256[i])) return 1;
    return 0;
}

static int do_install(run *r) {
    int x;
    if ((x = open_root(r, 1))) return x;
    if ((x = processes_ok(r)) || (x = rekordbox_folder(r, NULL)) || (x = not_mapped(r, r->vdir, r->version))) return x;
    if ((!plat_dir_ok(r->pioneer) && sub_dir(r, r->root, "pioneer", 1, &r->pioneer)) || (!plat_dir_ok(r->ort) && sub_dir(r, r->root, "ort", 1, &r->ort)))
        return ADMIN_EXIT_CHECK_FAILED;
    /* leftovers in rekordbox's folder, then the write probe (Controlled Folder Access and other
     * refusals before the slow work) */
    if (unlink_quiet(r->vdir, NEW) || unlink_quiet(r->vdir, PROBE) || write_new(r->vdir, PROBE, NULL, 0) || plat_unlink_at(r->vdir, PROBE)) {
        unlink_quiet(r->vdir, PROBE);
        say(r, "couldn't write into rekordbox: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if ((x = make_stage(r))) return x;
    plat_dir src;
    if (plat_dir_open(r->a->source, &src)) {
        say(r, "couldn't open the app's payload folder: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    char bsha[65];
    if (!(x = verify_payload(r, src))) {
        int e = admin_plain_name_ok(r->m->win_bridge.file) ? stage_copy(r, src, r->m->win_bridge.file, ADMIN_BRIDGE_MAX, STAGED_BRIDGE, bsha) : EINVAL;
        if (e == PLAT_EFTYPE) say(r, "the bridge is over 32 MB or not a file: nothing changed");
        else if (e) say(r, "couldn't copy the bridge (error %d): nothing changed", e);
        else if (strcmp(bsha, r->m->win_bridge.sha256)) say(r, "the bridge's checksum doesn't match payload.json: nothing changed");
        if (e || strcmp(bsha, r->m->win_bridge.sha256)) x = ADMIN_EXIT_CHECK_FAILED;
    }
    plat_dir_close(src);
    if (x) return x;

    compat_file f;
    char set[65];
    int c = classify(r, r->vname, r->version, &f, set);
    if (c == ADMIN_LIVE_NO_ANSWER) {
        say(r, "couldn't check rekordbox's ONNX Runtime: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (c == ADMIN_LIVE_REFUSED || c == ADMIN_LIVE_NOTHING) {
        say(r, "rekordbox's ONNX Runtime isn't AlphaTheta's nor our bridge, and no saved copy is complete: reinstall rekordbox with Pioneer's installer. Nothing changed");
        return ADMIN_EXIT_NOT_PIONEERS;
    }
    if (c == ADMIN_LIVE_HALF_RUN || c == ADMIN_LIVE_ABSENT) {
        say(r, c == ADMIN_LIVE_ABSENT ? "rekordbox's ONNX Runtime is missing: putting Pioneer's file back first"
                                      : "rekordbox doesn't verify (a run was stopped half-way): putting Pioneer's file back first");
        if ((x = not_mapped(r, r->vdir, r->version))) return x;
        r->changing = 1;
        int pb = put_back(r, r->vdir, r->vname, r->version);
        if (pb == PB_UNCHANGED) {
            r->changing = 0;
            return ADMIN_EXIT_CHECK_FAILED;
        }
        if (pb != PB_OK || !is_pioneers(r, r->vname)) return rollback(r);
        r->changing = 0;
        c = classify(r, r->vname, r->version, &f, set);
        if (c != ADMIN_LIVE_PIONEER) return ADMIN_EXIT_ROLLBACK_FAILED;
    }
    /* the first install: the worker goes into bin\ too, staged and hash-checked (I75) */
    if (!bin_has_worker(r)) {
        if (plat_dir_open(r->a->source, &src)) {
            say(r, "couldn't open the app's payload folder: nothing changed");
            return ADMIN_EXIT_CHECK_FAILED;
        }
        x = install_worker_file(r, src);
        plat_dir_close(src);
        if (x) return x;
    }
    char ortp[RUN_PATH];
    if (!join3(ortp, sizeof ortp, r->e->root, "ort", LIVE)) return ADMIN_EXIT_CHECK_FAILED;
    /* ours: ort\'s copy Pioneer's and listed before anything loads it (the saved set's
     * completeness is classify's) */
    if (c == ADMIN_LIVE_OURS) {
        compat_file o;
        if (facts(r, ortp, 1, &o) || !o.readable || o.pin != COMPAT_PIN_OK || o.marker || !listed_ort(r, o.sha256)) {
            say(r, "Pioneer's ONNX Runtime copy isn't AlphaTheta's or isn't listed: nothing changed");
            return ADMIN_EXIT_CHECK_FAILED;
        }
    }
    if (c == ADMIN_LIVE_OURS && !r->a->force) {
        /* a run stopped between the swap and its load check left a bridge never load-checked:
         * checked here too, and rolled back when it fails */
        char live[RUN_PATH];
        path_in_version(r, r->vname, LIVE, live, sizeof live);
        if (r->os->load_check(r->os->ctx, live, MARKER_SYMBOL) || r->os->load_check(r->os->ctx, ortp, ORT_SYMBOL)) {
            say(r, "the bridge or Pioneer's ONNX Runtime doesn't load");
            if ((x = not_mapped(r, r->vdir, r->version))) return x;
            r->changing = 1;
            r->load_failed = 1;
            return rollback(r);
        }
        say(r, "Stems Cache is already in place");
        say(r, "@bridge %s", f.sha256);
        drop_old_workers(r);
        return ADMIN_EXIT_OK;
    }
    if (c == ADMIN_LIVE_PIONEER && !listed_ort(r, f.sha256)) {
        say(r, "rekordbox's ONNX Runtime isn't one this release supports: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    /* one bridged version at a time: the others' files back first */
    if ((x = other_versions(r))) return x;
    if (c == ADMIN_LIVE_PIONEER) {
        unsigned char *b = NULL;
        size_t n = 0;
        char got[65];
        if (read_whole(r->vdir, LIVE, ADMIN_ORT_MAX, &b, &n) || sha_hex(b, n, got) || strcmp(got, f.sha256)) {
            free(b);
            say(r, "rekordbox's ONNX Runtime changed after it was checked: nothing changed");
            return ADMIN_EXIT_CHECK_FAILED;
        }
        /* Pioneer's file saved once per version, again if Pioneer re-shipped it; SHA256SUMS last */
        if (!strcmp(set, f.sha256)) say(r, "Pioneer's file of rekordbox %s is saved already", r->version);
        else {
            plat_dir vs;
            char line[160], sp[RUN_PATH];
            compat_file s;
            say(r, "saving Pioneer's file of rekordbox %s", r->version);
            if ((x = sub_dir(r, r->pioneer, r->version, 1, &vs))) {
                free(b);
                return ADMIN_EXIT_CHECK_FAILED;
            }
            int e = unlink_quiet(vs, SUMS);
            if (!e) e = unlink_quiet(vs, LIVE);
            if (!e) e = write_atomic(r, vs, LIVE, b, n);
            if (!e && (!join4(sp, sizeof sp, r->e->root, "pioneer", r->version, LIVE) || facts(r, sp, 1, &s) || strcmp(s.sha256, f.sha256)
                       || s.pin != COMPAT_PIN_OK))
                e = EIO;
            if (!e && admin_sums_line(f.sha256, line, sizeof line)) e = write_atomic(r, vs, SUMS, line, strlen(line));
            plat_dir_close(vs);
            if (e) {
                free(b);
                say(r, "couldn't save Pioneer's file (error %d): nothing changed", e);
                return ADMIN_EXIT_CHECK_FAILED;
            }
        }
        /* the real ORT for the bridge, in ort\ */
        compat_file o;
        int e = write_atomic(r, r->ort, LIVE, b, n);
        free(b);
        if (e || facts(r, ortp, 1, &o) || strcmp(o.sha256, f.sha256) || o.pin != COMPAT_PIN_OK) {
            say(r, "couldn't copy Pioneer's ONNX Runtime to %s: nothing changed", ortp);
            return ADMIN_EXIT_CHECK_FAILED;
        }
    }
    /* the bridge next to its place, flushed and verified; rekordbox is unchanged so far */
    unsigned char *b = NULL;
    size_t n = 0;
    char newp[RUN_PATH], live[RUN_PATH];
    compat_file nf;
    path_in_version(r, r->vname, NEW, newp, sizeof newp);
    path_in_version(r, r->vname, LIVE, live, sizeof live);
    int e = read_whole(r->stage, STAGED_BRIDGE, ADMIN_BRIDGE_MAX, &b, &n);
    if (!e) e = unlink_quiet(r->vdir, NEW);
    if (!e) e = write_new(r->vdir, NEW, b, n);
    free(b);
    if (e || facts(r, newp, 0, &nf) || strcmp(nf.sha256, r->m->win_bridge.sha256) || !nf.marker) {
        unlink_quiet(r->vdir, NEW);
        say(r, "couldn't write the bridge into rekordbox (error %d): nothing changed", e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if ((x = not_mapped(r, r->vdir, r->version))) {
        unlink_quiet(r->vdir, NEW);
        return x;
    }
    /* the swap (bridge.md §2): atomic on NTFS; a refusal before anything changed is rekordbox open
     * only when the live file is mapped (I83) */
    if ((e = plat_rename_at(r->vdir, NEW, r->vdir, LIVE))) {
        unlink_quiet(r->vdir, NEW);
        if (r->os->mapped(r->os->ctx, r->vdir, LIVE) == 1) {
            say(r, "rekordbox's ONNX Runtime is in use (rekordbox is open): nothing changed");
            return ADMIN_EXIT_REKORDBOX_OPEN;
        }
        say(r, "couldn't swap the bridge in (error %d): nothing changed", e);
        return ADMIN_EXIT_CHECK_FAILED;
    }
    r->changing = 1;
    /* verified, then the load check of the bridge and of ort\ (each freed before any rollback) */
    if (facts(r, live, 0, &nf) || strcmp(nf.sha256, r->m->win_bridge.sha256) || !nf.marker) {
        say(r, "rekordbox's file isn't the bridge after the swap");
        return rollback(r);
    }
    if (r->os->load_check(r->os->ctx, live, MARKER_SYMBOL) || r->os->load_check(r->os->ctx, ortp, ORT_SYMBOL)) {
        say(r, "the bridge or Pioneer's ONNX Runtime doesn't load");
        r->load_failed = 1;
        return rollback(r);
    }
    if ((x = exe_ok(r))) return rollback(r);   /* rekordbox pinned again after */
    r->changing = 0;
    char rec[80];
    snprintf(rec, sizeof rec, "%s\n", r->m->win_bridge.sha256);
    if (write_atomic(r, r->root, RECORD, rec, strlen(rec))) say(r, "couldn't write the installed-bridge record");
    say(r, "@bridge %s", r->m->win_bridge.sha256);
    drop_old_workers(r);
    say(r, "Stems Cache installed");
    return ADMIN_EXIT_OK;
}

static int do_install_worker(run *r) {
    int x;
    if ((x = open_root(r, 1)) || (x = make_stage(r))) return x;
    plat_dir src;
    if (plat_dir_open(r->a->source, &src)) {
        say(r, "couldn't open the app's payload folder: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (!(x = verify_payload(r, src))) x = install_worker_file(r, src);
    plat_dir_close(src);
    if (x) return x;
    drop_old_workers(r);
    say(r, "run the app's own worker now");
    return ADMIN_EXIT_RUN_AGAIN;
}

/* everything in the root but the worker's own file, then the lock, then the worker's file and
 * its folders (I77, J7): 0, or ADMIN_EXIT_FAILED when something stayed */
static int remove_root(run *r) {
    close_dir(&r->pioneer);
    close_dir(&r->ort);
    close_dir(&r->bin);
    close_dir(&r->stage);
    int failed = 0;
    names *l = malloc(sizeof *l), *k = malloc(sizeof *k);
    if (!l || !k || list_dir(r->root, l)) failed = 1;
    for (int i = 0; !failed && i < l->n; i++) {
        if (!strcmp(l->name[i], "lock")) continue;
        if (r->self_in_bin && !strcmp(l->name[i], "bin")) {
            plat_dir b;
            if (plat_dir_open_at(r->root, "bin", &b) || list_dir(b, k)) {
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
                if (!list_dir(w, own))
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
    } else {
        /* not run from bin\ (a test, or a root without any worker): the root folder itself */
        char parent[RUN_PATH];
        snprintf(parent, sizeof parent, "%s", r->e->root);
        char *s = strrchr(parent, SEP[0]);
        plat_dir p;
        if (!s || s == parent) return ADMIN_EXIT_FAILED;
        *s = 0;
        if (plat_dir_open(parent, &p)) return ADMIN_EXIT_FAILED;
        int e = r->os->rmdir_at(r->os->ctx, p, s + 1);
        plat_dir_close(p);
        if (e && e != ENOENT) return ADMIN_EXIT_FAILED;
    }
    say(r, "removed %s", r->e->root);
    return ADMIN_EXIT_OK;
}

static int do_uninstall(run *r) {
    int x = open_root(r, 0), have_root = x == 0;
    if (x && x != ENOENT) return x;
    if ((x = processes_ok(r)) || (x = rekordbox_folder(r, NULL))) return x;
    /* every version folder classified before anything changes */
    names *l = malloc(sizeof *l);
    int *state = malloc(sizeof(int) * NAMES_MAX);
    if (!l || !state || list_dir(r->rbroot, l)) {
        free(l);
        free(state);
        say(r, "couldn't list rekordbox's folders: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    for (int i = 0; i < l->n && !x; i++) {
        char v[COMPAT_VERSION_MAX], set[65];
        state[i] = -1;
        if (l->kind[i] != PLAT_DIR || !compat_folder_version(l->name[i], v, sizeof v)) continue;
        plat_dir d;
        int e = r->os->hold_dir(r->os->ctx, r->rbroot, l->name[i], &d);
        if (e) {
            say(r, "rekordbox %s's folder isn't the administrators' alone: nothing changed", v);
            x = ADMIN_EXIT_NOT_PIONEERS;
            break;
        }
        /* a stopped install's temp files (the bridge's bytes in .rbsp-new) go, whatever the state */
        unlink_quiet(d, NEW);
        unlink_quiet(d, PROBE);
        compat_file f;
        state[i] = classify(r, l->name[i], v, &f, set);
        if (state[i] == ADMIN_LIVE_NO_ANSWER) say(r, "couldn't check rekordbox %s's ONNX Runtime: nothing changed", v), x = ADMIN_EXIT_CHECK_FAILED;
        else if (state[i] == ADMIN_LIVE_REFUSED && f.marker) {
            say(r, "no complete saved copy of rekordbox %s's file: nothing changed (reinstall rekordbox from Pioneer's installer)", v);
            x = ADMIN_EXIT_NO_ORIGINALS;
        } else if (state[i] == ADMIN_LIVE_REFUSED) {
            say(r, "rekordbox %s's ONNX Runtime isn't AlphaTheta's nor our bridge: nothing changed", v);
            x = ADMIN_EXIT_NOT_PIONEERS;
        } else if (state[i] != ADMIN_LIVE_PIONEER && state[i] != ADMIN_LIVE_NOTHING && state[i] != ADMIN_LIVE_ABSENT)
            x = not_mapped(r, d, v);
        plat_dir_close(d);
    }
    for (int i = 0; i < l->n && !x; i++) {
        char v[COMPAT_VERSION_MAX];
        if (state[i] != ADMIN_LIVE_OURS && state[i] != ADMIN_LIVE_HALF_RUN && state[i] != ADMIN_LIVE_ABSENT) continue;
        compat_folder_version(l->name[i], v, sizeof v);
        plat_dir d;
        if (r->os->hold_dir(r->os->ctx, r->rbroot, l->name[i], &d)) {
            x = ADMIN_EXIT_CHECK_FAILED;
            break;
        }
        say(r, "putting Pioneer's file back into rekordbox %s", v);
        x = put_back_hard(r, d, l->name[i], v);
        plat_dir_close(d);
    }
    free(l);
    free(state);
    if (x) return x;
    if ((x = exe_ok(r))) return x;
    if (!have_root) {
        say(r, "rekordbox is signed by AlphaTheta, and there is nothing of ours to remove");
        return ADMIN_EXIT_OK;
    }
    close_dir(&r->vdir);
    close_dir(&r->rbroot);
    if ((x = remove_root(r))) return x;
    say(r, "Stems Cache removed: rekordbox has Pioneer's file again");
    return ADMIN_EXIT_OK;
}

static int do_restore(run *r) {
    int x = open_root(r, 0);
    if (x && x != ENOENT) return x;
    if ((x = processes_ok(r)) || (x = rekordbox_folder(r, NULL))) return x;
    unlink_quiet(r->vdir, NEW);
    unlink_quiet(r->vdir, PROBE);
    compat_file f;
    char set[65];
    int c = classify(r, r->vname, r->version, &f, set);
    switch (c) {
    case ADMIN_LIVE_PIONEER:
        say(r, "rekordbox has Pioneer's file already");
        drop_old_workers(r);
        return ADMIN_EXIT_OK;
    case ADMIN_LIVE_NO_ANSWER:
        say(r, "couldn't check rekordbox's ONNX Runtime: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    case ADMIN_LIVE_NOTHING:
    case ADMIN_LIVE_REFUSED:
        say(r, "no complete saved copy of rekordbox %s's file: nothing changed (reinstall rekordbox from Pioneer's installer)", r->version);
        return ADMIN_EXIT_NO_ORIGINALS;
    default:
        break;
    }
    if (c != ADMIN_LIVE_ABSENT && (x = not_mapped(r, r->vdir, r->version))) return x;
    say(r, c == ADMIN_LIVE_ABSENT ? "rekordbox's ONNX Runtime is missing: putting Pioneer's file back"
                                  : "putting Pioneer's file back");
    if ((x = put_back_hard(r, r->vdir, r->vname, r->version))) return x;
    if ((x = exe_ok(r))) return x;
    drop_old_workers(r);
    return ADMIN_EXIT_OK;
}

static int do_remove_root_folder(run *r) {
    int x = open_root(r, 0);
    if (x == ENOENT) {
        say(r, "%s isn't there: nothing to remove", r->e->root);
        return ADMIN_EXIT_OK;
    }
    if (x || (x = processes_ok(r))) return x;
    int installed = 1;
    x = rekordbox_folder(r, &installed);
    if (installed) {
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
    r->root = r->pioneer = r->ort = r->bin = r->stage = r->rbroot = r->vdir = plat_dir_cwd();
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
    if (plat_dir_ok(r->vdir)) {
        unlink_quiet(r->vdir, NEW);
        unlink_quiet(r->vdir, PROBE);
    }
    if (plat_dir_ok(r->root)) drop_stage(r);
    close_dir(&r->vdir);
    close_dir(&r->rbroot);
    close_dir(&r->pioneer);
    close_dir(&r->ort);
    close_dir(&r->bin);
    if (r->lock) os->unlock(os->ctx, r->lock);
    close_dir(&r->root);
    free(m);
    free(r);
    return x;
}
