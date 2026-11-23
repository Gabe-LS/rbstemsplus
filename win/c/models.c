/* rekordbox's stems model and Pioneer's saved originals (models.h): Models.swift over plat.h's
 * handles. */
#include "models.h"

#include "native_steps.h"   /* native_support_write: the engine file, atomically */
#include "plat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char model_name[] = "hdemucs.onnx", new_temp[] = ".hdemucs.new", restore_temp[] = ".hdemucs.restore",
                  engine_name[] = "demucs3_ver.txt";
static const char unknown_message[] = "rekordbox's stems model isn't one RB Stems Plus knows, so it was left as it is.";

int models_is_sha256(const char *s) {
    if (!s || strlen(s) != 64) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
    return 1;
}

static int is_ours(const models *m, const char *sha) {
    for (int i = 0; i < m->nours; i++)
        if (!strcmp(m->ours[i], sha)) return 1;
    return 0;
}

void models_add_ours(models *m, const char *sha) {
    if (models_is_sha256(sha) && !is_ours(m, sha) && m->nours < MODELS_OURS_MAX) snprintf(m->ours[m->nours++], 65, "%s", sha);
}

static void say(const models *m, const char *line) {
    if (m->log) m->log(m->ctx, line);
}

/* "  <s>" logged; the outcome */
static int fail(const models *m, int code, const char *s) {
    char line[300];
    snprintf(line, sizeof line, "  %s", s);
    say(m, line);
    return code;
}

/* an OS error's text: the UCRT has none for PLAT_EFTYPE or ELOOP (review MD12) */
static const char *err_text(int e) {
    if (e == PLAT_EFTYPE) return "not a regular file";
    if (e == ELOOP) return "it is a link";
    return strerror(e);
}

#ifdef MODELS_TEST_HOOKS
int (*models_test_stat_fault)(const char *name);
static int stat_at(plat_dir d, const char *name, plat_stat *st) {
    int r = models_test_stat_fault ? models_test_stat_fault(name) : 0;
    return r ? r : plat_stat_at(d, name, st);
}
int (*models_test_each_fault)(int call);
int (*models_test_originals_fault)(int call);
int models_test_each_calls, models_test_originals_calls;
static int each_at(plat_dir d, plat_each_fn fn, void *ctx) {
    int call = ++models_test_each_calls;
    int r = plat_dir_each(d, fn, ctx);
    return models_test_each_fault && models_test_each_fault(call) ? -1 : r;
}
#  define ORIGINALS_FAULT() (++models_test_originals_calls, models_test_originals_fault ? models_test_originals_fault(models_test_originals_calls) : 0)
int (*models_test_unlink_fault)(const char *name);
void (*models_test_before_place)(void);
int (*models_test_rename_fault)(void);
int (*models_test_engine_fault)(void);
#  define UNLINK_FAULT(name) (models_test_unlink_fault ? models_test_unlink_fault(name) : 0)
#  define BEFORE_PLACE() (models_test_before_place ? models_test_before_place() : (void)0)
#  define RENAME_FAULT() (models_test_rename_fault ? models_test_rename_fault() : 0)
#  define ENGINE_FAULT() (models_test_engine_fault ? models_test_engine_fault() : 0)
#else
#  define stat_at plat_stat_at
#  define each_at plat_dir_each
#  define ORIGINALS_FAULT() 0
#  define UNLINK_FAULT(name) 0
#  define BEFORE_PLACE() ((void)0)
#  define RENAME_FAULT() 0
#  define ENGINE_FAULT() 0
#endif

/* originals\ couldn't be opened for a reason that doesn't say "none there": not missing, not a
 * folder, not a link (never followed) */
static int originals_err_unknown(int e) { return e && e != ENOENT && e != ENOTDIR && e != ELOOP; }

/* ELOOP from support's absolute open: a link at support itself (never followed), or a junction
 * above it that Redirection Guard refused (unknown, EIO: VU4, J1) */
static int support_err(const models *m, int e) {
    plat_stat st;
    if (e == ELOOP && !(plat_stat_path(m->support, 0, &st) == 0 && st.kind == PLAT_OTHER)) return EIO;
    return e;
}

/* support\originals through a handle on support, neither followed (review MD5): with create, both
 * made first when missing. 0 with *od, or an errno (ELOOP: a link at either) */
static int open_originals(const models *m, int create, plat_dir *od) {
    if (!m->support || !m->support[0]) return ENOENT;
    int fe = ORIGINALS_FAULT();   /* test builds: a one-shot open failure (review VQ2) */
    if (fe) return support_err(m, fe);
    plat_dir sd;
    int e = plat_dir_open(m->support, &sd);
    if (e == ENOENT && create) {   /* made only when missing: a dangling junction gives ELOOP, nothing created (review MR3) */
        plat_mkdirs(m->support);
        e = plat_dir_open(m->support, &sd);
    }
    if (e) return support_err(m, e);
    if (create) {
        int c = plat_mkdir_at(sd, "originals");
        if (c && c != EEXIST) {
            plat_dir_close(sd);
            return c;
        }
    }
    e = plat_dir_open_at(sd, "originals", od);
    plat_dir_close(sd);
    return e;
}

/* ---------------------------------------------------------------- files through a folder handle */

/* the sha256 of the regular file name in d, never through a link: 0, or an errno */
static int hash_at(plat_dir d, const char *name, char out[65]) {
    plat_file f;
    int e = plat_file_open_at(d, name, INT64_MAX, NULL, &f);
    if (e) return e;
    plat_sha256_ctx c;
    unsigned char *buf = malloc(1 << 20), sum[32];
    size_t got;
    int started = buf && !plat_sha256_init(&c);   /* review MD10: final only after an init */
    if (!started) e = ENOMEM;
    while (!e && !(e = plat_read(f, buf, 1 << 20, &got)) && got) plat_sha256_update(&c, buf, got);
    if (started && plat_sha256_final(&c, sum) && !e) e = EIO;
    plat_file_close(f);
    free(buf);
    if (!e)
        for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", sum[i]);
    return e;
}

/* name in d hashed equals sha (a regular file, never a link) */
static int has_sha(plat_dir d, const char *name, const char *sha) {
    char got[65];
    return !hash_at(d, name, got) && !strcmp(got, sha);
}

/* whether name exists in d (anything, not followed) */
static int there(plat_dir d, const char *name) {
    plat_stat st;
    return !plat_stat_at(d, name, &st);
}

/* this operation's own temporary name, base.<pid>.<monotonic ns> (review MD3): created exclusive,
 * verified and renamed by that name, so no other file is ever installed in its place */
static void run_temp(char *out, size_t cap, const char *base) {
    snprintf(out, cap, "%s.%d.%llu", base, plat_pid(), (unsigned long long)plat_now_mono_ns());
}

/* whether name is base itself or base.<digits>.<digits> */
static int is_temp_of(const char *name, const char *base) {
    size_t n = strlen(base);
    if (strncmp(name, base, n)) return 0;
    const char *p = name + n;
    if (!*p) return 1;
    for (int k = 0; k < 2; k++) {
        if (*p++ != '.' || !(*p >= '0' && *p <= '9')) return 0;
        while (*p >= '0' && *p <= '9') p++;
    }
    return !*p;
}

/* the regular files in a folder named base or base.<digits>.<digits> (base NULL: <sha>.onnx.part
 * or <sha>.onnx.part.<digits>.<digits>, any sha256), listed whole before anything is removed
 * (review WF1: a per-run name runs up to 106 bytes) */
typedef struct {
    const char *base;
    char (*names)[128];
    int n, cap;
} part_list;

static int part_each(void *ctx, const char *name, const plat_stat *st) {
    part_list *l = ctx;
    size_t n = strlen(name);
    if (st->kind != PLAT_FILE || n >= 128) return 0;
    if (l->base) {
        if (!is_temp_of(name, l->base)) return 0;
    } else {
        char sha[65], base[80];
        if (n < 74) return 0;
        memcpy(sha, name, 64);
        sha[64] = 0;
        if (!models_is_sha256(sha)) return 0;
        snprintf(base, sizeof base, "%s.onnx.part", sha);
        if (!is_temp_of(name, base)) return 0;
    }
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        char(*more)[128] = realloc(l->names, (size_t)cap * sizeof *more);
        if (!more) return -1;
        l->names = more;
        l->cap = cap;
    }
    snprintf(l->names[l->n++], 128, "%s", name);
    return 0;
}

/* the temporary files of runs that were stopped: base (the name before per-run names) and
 * base.<pid>.<ns>, regular files only, every one of them. One flow at a time and one copy of the
 * app (its mutex), so none of them is another run's. */
static void clear_stale(plat_dir d, const char *base) {
    part_list l = {base, NULL, 0, 0};
    plat_dir_each(d, part_each, &l);
    for (int i = 0; i < l.n; i++) plat_unlink_at(d, l.names[i]);
    free(l.names);
}

/* the regular file sname in sd copied to a new file dname in dd (which must not exist), flushed
 * to the disk: 0, or an errno with *sync 1 when only the flush failed. A copy that fails is
 * removed (it is ours). */
static int copy_at(plat_dir sd, const char *sname, plat_dir dd, const char *dname, int *sync) {
    plat_file s, d;
    int64_t size = 0;
    *sync = 0;
    int e = plat_file_open_at(sd, sname, INT64_MAX, &size, &s);
    if (e) return e;
    e = plat_file_create_at(dd, dname, 1, &d);
    if (e) {
        plat_file_close(s);
        return e;
    }
    void *addr = NULL;
    if (size > 0 && !(e = plat_map_rw(d, size, &addr))) {
        for (int64_t off = 0; !e && off < size;) {
            size_t got = 0, want = (size_t)(size - off < (1 << 20) ? size - off : (1 << 20));
            e = plat_read(s, (char *)addr + off, want, &got);
            if (!e && !got) e = EIO;   /* it shrank meanwhile */
            off += (int64_t)got;
        }
        int fe = plat_map_flush(addr, size);   /* the view written, then the file flushed below */
        plat_unmap(addr, size);
        if (!e && fe) {
            e = fe;
            *sync = 1;
        }
    }
    if (!e && (e = plat_fsync(d))) *sync = 1;
    plat_file_close(d);
    plat_file_close(s);
    if (e) plat_unlink_at(dd, dname);
    return e;
}

/* the STEMS Engine version (engineVersion: demucs3_ver.txt trimmed; "" none) */
static void engine_of(plat_dir dd, char out[64]) {
    plat_file f;
    out[0] = 0;
    if (plat_file_open_at(dd, engine_name, 4096, NULL, &f)) return;
    char b[4097];
    size_t got = 0, n = 0;
    while (n < 4096 && !plat_read(f, b + n, 4096 - n, &got) && got) n += got;
    plat_file_close(f);
    size_t a = 0;
    while (a < n && (b[a] == ' ' || b[a] == '\t' || b[a] == '\r' || b[a] == '\n')) a++;
    while (n > a && (b[n - 1] == ' ' || b[n - 1] == '\t' || b[n - 1] == '\r' || b[n - 1] == '\n')) n--;
    snprintf(out, 64, "%.*s", (int)(n - a), b + a);
}

/* ---------------------------------------------------------------- the saved originals */

typedef struct {
    char sha[65], engine[64];
    int64_t mtime;
    int rank;
} saved;

typedef struct {
    const models *m;
    plat_dir od;
    saved *list;
    int n, cap;
    const char *current;
    int unknown;   /* a <sha256>.onnx that can't be told a saved copy or not (originalsUnknown) */
} saved_acc;

static int read_engine(plat_dir od, const char *sha, char out[64]) {
    char name[80];
    plat_file f;
    out[0] = 0;
    snprintf(name, sizeof name, "%s.engine", sha);
    if (plat_file_open_at(od, name, 4096, NULL, &f)) return 0;
    char b[4097];
    size_t got = 0, n = 0;
    while (n < 4096 && !plat_read(f, b + n, 4096 - n, &got) && got) n += got;
    plat_file_close(f);
    size_t a = 0;
    while (a < n && (b[a] == ' ' || b[a] == '\t' || b[a] == '\r' || b[a] == '\n')) a++;
    while (n > a && (b[n - 1] == ' ' || b[n - 1] == '\t' || b[n - 1] == '\r' || b[n - 1] == '\n')) n--;
    snprintf(out, 64, "%.*s", (int)(n - a), b + a);
    return 1;
}

static int saved_each(void *ctx, const char *name, const plat_stat *st) {
    saved_acc *acc = ctx;
    size_t n = strlen(name);
    char sha[65];
    if (n != 69 || strcmp(name + 64, ".onnx") || st->kind == PLAT_DIR) return 0;
    memcpy(sha, name, 64);
    sha[64] = 0;
    if (!models_is_sha256(sha) || is_ours(acc->m, sha)) return 0;
    /* in the same scan as the list (review MP1: one look decides): not a regular file nor a
     * folder (a reparse point), or a file that can't be opened, is unknown (the Mac's SR2) */
    plat_file f;
    if (st->kind != PLAT_FILE) {
        acc->unknown = 1;
        return 0;
    }
    if (plat_file_open_at(acc->od, name, INT64_MAX, NULL, &f)) {   /* not a candidate: a good one next to it can still go back (SR2) */
        acc->unknown = 1;
        return 0;
    }
    plat_file_close(f);
    if (acc->n == acc->cap) {
        int cap = acc->cap ? acc->cap * 2 : 8;
        saved *more = realloc(acc->list, (size_t)cap * sizeof *more);
        if (!more) return -1;
        acc->list = more;
        acc->cap = cap;
    }
    saved *s = &acc->list[acc->n++];
    memcpy(s->sha, sha, 65);
    read_engine(acc->od, sha, s->engine);
    s->mtime = st->mtime;
    s->rank = acc->current[0] && !strcmp(s->engine, acc->current) ? 0 : s->engine[0] ? 1 : 2;
    return 0;
}

/* String.compare(options: .numeric): digit runs as numbers, the rest by character */
static int numeric_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (*ea >= '0' && *ea <= '9') ea++;
            while (*eb >= '0' && *eb <= '9') eb++;
            if (ea - a != eb - b) return ea - a < eb - b ? -1 : 1;
            int c = strncmp(a, b, (size_t)(ea - a));
            if (c) return c < 0 ? -1 : 1;
            a = ea, b = eb;
            continue;
        }
        if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        a++, b++;
    }
    return *a ? 1 : *b ? -1 : 0;
}

static int saved_order(const void *x, const void *y) {
    const saved *a = x, *b = y;
    if (a->rank != b->rank) return a->rank - b->rank;
    int c = numeric_cmp(a->engine, b->engine);
    if (c) return -c;   /* the higher engine version first */
    return a->mtime > b->mtime ? -1 : a->mtime < b->mtime ? 1 : 0;   /* the most recently saved first */
}

/* savedOriginals into *out (malloc'd; free it), best first: the one saved for the current STEMS
 * Engine version, then the highest known version, then unknown ones; the newest first among
 * equals. Only regular files named <sha256>.onnx, never ours. The count. One scan (review MP1)
 * also gives, each may be NULL: *partial 1 when the folder couldn't be read whole (or no memory),
 * *unknown 1 when it couldn't, or a <sha256>.onnx can't be told a copy or not (originalsUnknown):
 * what was read counts, but the callers that decide on it fail closed (review VU1) */
static int saved_originals(const models *m, plat_dir od, const char *current, saved **out, int *partial, int *unknown) {
    saved_acc acc = {m, od, NULL, 0, 0, current, 0};
    int r = each_at(od, saved_each, &acc);
    if (partial) *partial = r != 0;
    if (unknown) *unknown = r != 0 || acc.unknown;
    if (acc.n) qsort(acc.list, (size_t)acc.n, sizeof *acc.list, saved_order);
    *out = acc.list;
    return acc.n;
}

static const char originals_unreadable[] = "The saved copies of rekordbox's own stems model can't be read.";

/* mayBePioneers: a model that isn't ours can be taken for Pioneer's when none is saved, when it
 * is one of them, or when the STEMS Engine version is known and differs from every saved one's */
static int may_be_pioneers(const char *sha, const char *engine, const saved *list, int n) {
    if (!n) return 1;
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].sha, sha)) return 1;
    if (!engine[0]) return 0;
    for (int i = 0; i < n; i++)
        if (!list[i].engine[0] || !strcmp(list[i].engine, engine)) return 0;
    return 1;
}

/* originals\<sha>.onnx is there and has that sha256 */
static int saved_is_good(plat_dir od, const char *sha) {
    char name[80];
    snprintf(name, sizeof name, "%s.onnx", sha);
    return has_sha(od, name, sha);
}

/* the checked part renamed to <sha>.onnx, never over another file: a saved copy there that
 * doesn't match its name is set aside first (<sha>.onnx.damaged, never used). 0, or nonzero */
static int place(const models *m, plat_dir od, const char *part, const char *sha) {
    char dest[80], damaged[96];
    snprintf(dest, sizeof dest, "%s.onnx", sha);
    if (there(od, dest) && !saved_is_good(od, sha)) {
        char line[96];
        snprintf(line, sizeof line, "saved model %.12s is damaged: set aside", sha);
        say(m, line);
        snprintf(damaged, sizeof damaged, "%s.damaged", dest);
        plat_stat st;
        if (!plat_stat_at(od, damaged, &st) && st.kind == PLAT_FILE) plat_unlink_at(od, damaged);   /* one this code set aside */
        plat_rename_at(od, dest, od, damaged);
    }
    /* never over a file that appeared meanwhile: EEXIST then (the .damaged rename above replaces) */
    BEFORE_PLACE();
    return plat_rename_noreplace_at(od, part, od, dest);
}

/* the saved model sha's STEMS Engine version e (not empty) as <sha>.engine, when it has none: 0, or
 * an errno (review b-a2a-F1: an untagged copy ranks below an older engine's tagged one) */
static int tag_engine(plat_dir od, const char *sha, const char *e) {
    char engine_file[80], text[70];
    snprintf(engine_file, sizeof engine_file, "%s.engine", sha);
    if (there(od, engine_file)) return 0;
    int n = snprintf(text, sizeof text, "%s\n", e);
    int r = ENGINE_FAULT();
    return r ? r : native_support_write_at(od, engine_file, text, (size_t)n);
}

/* saveOriginal: rekordbox's model (dd\hdemucs.onnx, checksum sha taken just before) saved as
 * Pioneer's original: copied to <sha>.onnx.part.<pid>.<ns>, checked, flushed, then put in place. */
static int save_original(const models *m, plat_dir dd, const char *sha) {
    char line[200];
    if (!models_is_sha256(sha) || is_ours(m, sha))
        return fail(m, MODELS_NO_CODE, "rekordbox's stems model is the Demucs v4 model, so it wasn't saved as rekordbox's own.");

    plat_dir od;
    if (open_originals(m, 1, &od)) return fail(m, MODELS_NOT_SAVED, "A copy of rekordbox's own stems model couldn't be saved.");
    char e[64], part_base[80], part[128];
    engine_of(dd, e);
    int r;
    if (saved_is_good(od, sha)) {
        /* the live model is that copy, so the current engine is its engine: a missing tag written */
        if (e[0] && (r = tag_engine(od, sha, e))) {
            snprintf(line, sizeof line, "save Pioneer's model: FAILED its STEMS Engine version (%s)", err_text(r));
            say(m, line);
            plat_dir_close(od);
            return fail(m, MODELS_NOT_SAVED, "A copy of rekordbox's own stems model couldn't be saved.");
        }
        snprintf(line, sizeof line, "save Pioneer's model: already saved (%.12s)", sha);
        say(m, line);
        plat_dir_close(od);
        return MODELS_OK;
    }
    snprintf(part_base, sizeof part_base, "%s.onnx.part", sha);
    run_temp(part, sizeof part, part_base);   /* <sha>.onnx.part.<pid>.<ns>: this save's own */
    clear_stale(od, part_base);               /* left by runs that were stopped */
    if (m->will_save) m->will_save(m->ctx);   /* the tests change the model here: the copy must not verify */
    int sync;
    r = copy_at(dd, model_name, od, part, &sync);
    if (!r && !has_sha(od, part, sha)) {
        plat_unlink_at(od, part);
        r = EIO;
    }
    if (r) {
        snprintf(line, sizeof line, "save Pioneer's model: FAILED %s", r == EIO && !sync ? "the copy didn't match" : err_text(r));
        say(m, line);
        plat_dir_close(od);
        return r == ENOSPC && !sync ? MODELS_NO_SPACE_SAVE : MODELS_NOT_SAVED;
    }
    /* the engine version before the model: a saved model never appears without it */
    if (e[0] && (r = tag_engine(od, sha, e))) {
        plat_unlink_at(od, part);
        snprintf(line, sizeof line, "save Pioneer's model: FAILED its STEMS Engine version (%s)", err_text(r));
        say(m, line);
        plat_dir_close(od);
        return fail(m, MODELS_NOT_SAVED, "A copy of rekordbox's own stems model couldn't be saved.");
    }
    if (place(m, od, part, sha)) {
        plat_unlink_at(od, part);
        plat_dir_close(od);
        return fail(m, MODELS_NOT_SAVED, "A copy of rekordbox's own stems model couldn't be saved.");
    }
    plat_dir_close(od);
    snprintf(line, sizeof line, "save Pioneer's model: saved (%.12s, STEMS Engine %s)", sha, e[0] ? e : "unknown");
    say(m, line);
    return MODELS_OK;
}

/* ---------------------------------------------------------------- installing ours */

/* rekordbox's model's sha256 into live ("" none: surely not there): 0; MODELS_NOT_A_FILE (strict)
 * or MODELS_UNREADABLE, also when it can't be looked at (review MD1: never taken for "none") */
static int live_sha(plat_dir dd, int strict, char live[65]) {
    plat_stat st;
    live[0] = 0;
    int e = stat_at(dd, model_name, &st);
    if (e == ENOENT) return 0;
    if (e) return MODELS_UNREADABLE;
    if (strict && st.kind != PLAT_FILE) return MODELS_NOT_A_FILE;
    return hash_at(dd, model_name, live) ? MODELS_UNREADABLE : 0;
}

/* rekordbox's model unchanged since it was hashed (both surely absent counts) */
static int unchanged(const models *m, plat_dir dd, const char *live) {
    if (m->will_replace) m->will_replace(m->ctx);
    char now[65];
    return !live_sha(dd, 0, now) && !strcmp(now, live);
}

static int install_copy(const models *m, plat_dir dd, const char *source_dir, const char *source_name, const char *sha, const char *live,
                        char why[128]);

int models_install_ours(const models *m, const char *source_dir, const char *source_name, const char *sha, char why[128]) {
    why[0] = 0;
    plat_dir dd;
    int r = plat_dir_open(m->dir, &dd);
    if (r) {   /* Swift's copy into a folder that isn't there fails the same way */
        snprintf(why, 128, "%s", err_text(r));
        return fail(m, MODELS_NOT_COPIED_WHY, "The Demucs v4 model couldn't be copied.");
    }
    char live[65];
    r = live_sha(dd, 1, live);
    if (r) {
        plat_dir_close(dd);
        return fail(m, r, r == MODELS_NOT_A_FILE ? "rekordbox's stems model isn't a file." : "rekordbox's stems model can't be read.");
    }
    if (live[0] && !is_ours(m, live)) {
        plat_dir od;
        saved *list = NULL;
        int n = 0, may, unknown = 0;
        char e[64];
        engine_of(dd, e);
        int oe = open_originals(m, 0, &od);
        if (!oe) {
            n = saved_originals(m, od, e, &list, NULL, &unknown);
            plat_dir_close(od);
        }
        if (originals_err_unknown(oe) || unknown) {   /* what's saved isn't known: nothing decided on it (review VU1) */
            free(list);
            plat_dir_close(dd);
            return fail(m, MODELS_NOT_SAVED, "A copy of rekordbox's own stems model couldn't be saved.");
        }
        may = may_be_pioneers(live, e, list, n);
        free(list);
        if (!may) {
            plat_dir_close(dd);
            return fail(m, MODELS_UNKNOWN, unknown_message);
        }
        r = save_original(m, dd, live);
        int good = 0;
        if (!r && !open_originals(m, 0, &od)) {
            good = saved_is_good(od, live);
            plat_dir_close(od);
        }
        if (r || !good) {
            plat_dir_close(dd);
            return r ? r : fail(m, MODELS_NOT_SAVED, "A copy of rekordbox's own stems model couldn't be saved.");
        }
    } else if (!live[0])
        say(m, "Pioneer's model: none to save");
    /* the copy through the rename only, in CriticalWork (MC3/MC4): the save above stays outside */
    critical_work *w = m->critical ? m->critical : critical_process();
    if (!critical_try_begin(w)) {
        plat_dir_close(dd);
        say(m, "Demucs v4: not copied: the app is quitting");   /* nothing in rekordbox's folder changed */
        return MODELS_STOPPED;
    }
    r = install_copy(m, dd, source_dir, source_name, sha, live, why);
    critical_end(w);
    return r;
}

#if defined(_WIN32) && defined(CORE_TEST_HOOKS)
/* The VM can pause a successful staged copy before the rename, while CriticalWork owns it. */
static void test_hold_model_copy(const models *m) {
    const char *name = getenv("RBSP_TEST_COPY_HOLD");
    if (!name || !*name || strlen(name) > 120) return;
    char held[128], release[129];
    snprintf(held, sizeof held, "%s-held", name);
    snprintf(release, sizeof release, "%s-release", name);
    HANDLE h = OpenEventA(EVENT_MODIFY_STATE, FALSE, held);
    HANDLE r = OpenEventA(SYNCHRONIZE, FALSE, release);
    if (h && r && SetEvent(h) && WaitForSingleObject(r, 60000) != WAIT_OBJECT_0)
        say(m, "test build: model copy hold timed out");
    if (r) CloseHandle(r);
    if (h) CloseHandle(h);
}
#endif

/* models_install_ours' copy: ours to .hdemucs.new, checked, rekordbox's model unchanged, renamed
 * over it. dd is closed on every way out */
static int install_copy(const models *m, plat_dir dd, const char *source_dir, const char *source_name, const char *sha, const char *live,
                        char why[128]) {
    plat_dir sd;
    int r;
    if ((r = plat_dir_open(source_dir, &sd))) {
        plat_dir_close(dd);
        snprintf(why, 128, "%s", err_text(r));
        return fail(m, MODELS_NOT_COPIED_WHY, "The Demucs v4 model couldn't be copied.");
    }
    char tmp[80];
    run_temp(tmp, sizeof tmp, new_temp);
    clear_stale(dd, new_temp);   /* left by runs that were stopped */
    int sync;
    r = copy_at(sd, source_name, dd, tmp, &sync);
    plat_dir_close(sd);
    if (r && !sync) {
        plat_dir_close(dd);
        if (r == ENOSPC) return fail(m, MODELS_NO_SPACE_COPY, "There isn't enough free space to copy the Demucs v4 model.");
        snprintf(why, 128, "%s", err_text(r));
        return fail(m, MODELS_NOT_COPIED_WHY, "The Demucs v4 model couldn't be copied.");
    }
    if (r || !has_sha(dd, tmp, sha)) {
        plat_unlink_at(dd, tmp);
        plat_dir_close(dd);
        return fail(m, MODELS_COPY_BAD, "The Demucs v4 model didn't copy correctly.");
    }
#if defined(_WIN32) && defined(CORE_TEST_HOOKS)
    test_hold_model_copy(m);
#endif
    if (!unchanged(m, dd, live)) {
        plat_unlink_at(dd, tmp);
        plat_dir_close(dd);
        return fail(m, MODELS_CHANGED, "rekordbox's stems model changed meanwhile.");
    }
    if ((r = RENAME_FAULT()) || (r = plat_rename_at(dd, tmp, dd, model_name))) {
        plat_unlink_at(dd, tmp);
        plat_dir_close(dd);
        snprintf(why, 128, "%s", err_text(r));
        return fail(m, MODELS_NOT_PLACED_WHY, "The Demucs v4 model couldn't be put in place.");
    }
    plat_dir_close(dd);
    char line[64];
    snprintf(line, sizeof line, "Demucs v4: installed (%.12s)", sha);
    say(m, line);
    return MODELS_OK;
}

/* ---------------------------------------------------------------- putting Pioneer's back */

static int done(const models *m, const char *s) {
    char line[200];
    snprintf(line, sizeof line, "restore Pioneer's model: %s", s);
    say(m, line);
    return MODELS_OK;
}

int models_restore(const models *m, char why[128]) {
    why[0] = 0;
    plat_dir dd, od;
    memset(&od, 0, sizeof od);
    int r = plat_dir_open(m->dir, &dd);
    /* only a folder surely not there is "nothing to put back": a link there (ELOOP) or another
     * error isn't proof that rekordbox's model isn't ours (review MD7) */
    if (r == ENOENT || r == ENOTDIR) return done(m, "rekordbox's model folder isn't there: nothing to put back.");
    if (r) return fail(m, MODELS_NOT_BACK, "rekordbox's own stems model couldn't be put back.");
    char live[65], e[64];
    if (live_sha(dd, 0, live)) {
        plat_dir_close(dd);
        return fail(m, MODELS_UNREADABLE, "rekordbox's stems model can't be read.");
    }
    int present = live[0] != 0;   /* live_sha 0 with "": surely not there (review MD1) */
    engine_of(dd, e);
    saved *list = NULL;
    int n = 0, oe = open_originals(m, 0, &od), have_od = !oe;
    int unknown = 0, partial = 0;
    if (have_od) n = saved_originals(m, od, e, &list, &partial, &unknown);   /* one scan decides (review MP1) */
    if (originals_err_unknown(oe) || partial) {
        free(list);
        if (have_od) plat_dir_close(od);
        plat_dir_close(dd);
        return fail(m, MODELS_NOT_BACK, originals_unreadable);
    }
    if (live[0] && !is_ours(m, live)) {
        int may = !unknown && may_be_pioneers(live, e, list, n);
        free(list);
        if (unknown) {
            if (have_od) plat_dir_close(od);
            plat_dir_close(dd);
            return fail(m, MODELS_NOT_BACK, originals_unreadable);
        }
        if (have_od) plat_dir_close(od);
        plat_dir_close(dd);
        return may ? done(m, "Pioneer's stems model is already in place.") : fail(m, MODELS_UNKNOWN, unknown_message);
    }
    if (!n) {
        if (have_od) plat_dir_close(od);
        plat_dir_close(dd);
        if (unknown) return fail(m, MODELS_NOT_BACK, originals_unreadable);
        return !present ? done(m, "rekordbox has no stems model and none was saved: left as it is.")
                        : fail(m, MODELS_NO_SAVED_COPY, "There's no saved copy of rekordbox's own stems model on this PC.");
    }
    /* the candidates' copy through the rename, in CriticalWork (MC3/MC4) */
    critical_work *w = m->critical ? m->critical : critical_process();
    if (!critical_try_begin(w)) {
        free(list);
        plat_dir_close(od);
        plat_dir_close(dd);
        say(m, "restore Pioneer's model: not put back: the app is quitting");   /* nothing changed */
        return MODELS_STOPPED;
    }
    r = MODELS_SAVED_DAMAGED;
    char tmp[80];
    clear_stale(dd, restore_temp);   /* left by runs that were stopped */
    for (int i = 0; i < n; i++) {
        /* each pass its own name: a failed copy that couldn't be removed (EBUSY) never makes the next
         * candidate's copy fail with EEXIST (review WF5) */
        run_temp(tmp, sizeof tmp, restore_temp);
        char name[80], line[200];
        snprintf(name, sizeof name, "%s.onnx", list[i].sha);
        int sync, c = copy_at(od, name, dd, tmp, &sync);
        if (c && !sync) {
            if (c == ENOSPC) {
                r = fail(m, MODELS_NO_SPACE_BACK, err_text(c));
            } else {
                snprintf(why, 128, "%s", err_text(c));
                r = fail(m, MODELS_NOT_BACK_WHY, "rekordbox's own stems model couldn't be put back.");
            }
            break;
        }
        if (c) {   /* the flush failed: the copy is gone */
            snprintf(line, sizeof line, "  the copy of saved model %.12s couldn't be written to the disk", list[i].sha);
            say(m, line);
            r = c == ENOSPC ? MODELS_NO_SPACE_BACK : fail(m, MODELS_NOT_BACK, "rekordbox's own stems model couldn't be put back.");
            break;
        }
        if (!has_sha(dd, tmp, list[i].sha)) {
            if (!UNLINK_FAULT(tmp)) plat_unlink_at(dd, tmp);
            snprintf(line, sizeof line, "  saved model %.12s didn't verify: trying the next one", list[i].sha);
            say(m, line);
            continue;
        }
        if (!unchanged(m, dd, live)) {
            plat_unlink_at(dd, tmp);
            r = fail(m, MODELS_CHANGED, "rekordbox's stems model changed meanwhile.");
            break;
        }
        if ((c = plat_rename_at(dd, tmp, dd, model_name))) {
            plat_unlink_at(dd, tmp);
            snprintf(why, 128, "%s", err_text(c));
            r = fail(m, MODELS_NOT_BACK_WHY, "rekordbox's own stems model couldn't be put back.");
            break;
        }
        if (m->after_rename) m->after_rename(m->ctx);   /* the tests change the model or its folder here */
        plat_dir current;
        if (plat_dir_open(m->dir, &current)) {
            r = fail(m, MODELS_NOT_BACK, "rekordbox's own stems model couldn't be put back.");
            break;
        }
        plat_stat before, after;
        int same_dir = !plat_dir_stat(dd, &before) && !plat_dir_stat(current, &after) && plat_id_same(before.id, after.id);
        int verified = same_dir && has_sha(current, model_name, list[i].sha);
        plat_dir_close(current);
        if (!same_dir) {
            r = fail(m, MODELS_NOT_BACK, "rekordbox's own stems model couldn't be put back.");
            break;
        }
        if (!verified) {
            r = fail(m, MODELS_NOT_VERIFIED, "rekordbox's stems model didn't verify after it was put back.");
            break;
        }
        snprintf(line, sizeof line, "Pioneer's stems model is back (%.12s, STEMS Engine %s).", list[i].sha, list[i].engine[0] ? list[i].engine : "unknown");
        r = done(m, line);
        break;
    }
    critical_end(w);
    if (r == MODELS_SAVED_DAMAGED) fail(m, r, "The saved copy of rekordbox's own stems model is damaged.");
    free(list);
    if (have_od) plat_dir_close(od);
    plat_dir_close(dd);
    return r;
}

/* ---------------------------------------------------------------- MODELS_INFO */

int models_any_saved(const models *m) {
    int saved;
    models_originals_info(m, &saved, NULL);   /* no hashing */
    return saved;
}

int models_saved_list(const models *m, char (*shas)[65], int max) {
    plat_dir od, dd;
    char e[64] = "";
    if (open_originals(m, 0, &od)) return 0;
    if (!plat_dir_open(m->dir, &dd)) {
        engine_of(dd, e);
        plat_dir_close(dd);
    }
    saved *list = NULL;
    int n = saved_originals(m, od, e, &list, NULL, NULL), k = 0;
    for (; k < n && k < max; k++) memcpy(shas[k], list[k].sha, 65);
    free(list);
    plat_dir_close(od);
    return k;
}

typedef struct {
    const models *m;
    plat_dir od;
    int saved, verified, want_verified;
} info_acc;

/* one <sha256>.onnx (never ours, never a folder): a regular file is saved; it is verified when
 * good, or when it can't be opened or read, or isn't a regular file (unknown): either one is kept;
 * a hash that differs (damaged) is not */
static int info_each(void *ctx, const char *name, const plat_stat *st) {
    info_acc *acc = ctx;
    char sha[65], got[65];
    if (strlen(name) != 69 || strcmp(name + 64, ".onnx") || st->kind == PLAT_DIR) return 0;
    memcpy(sha, name, 64);
    sha[64] = 0;
    if (!models_is_sha256(sha) || is_ours(acc->m, sha)) return 0;
    if (st->kind != PLAT_FILE) acc->saved = acc->verified = 1;   /* a reparse point (WOF, dedup, cloud; B15): can't be checked, kept (review VU2) */
    else {
        acc->saved = 1;
        if (acc->want_verified && !acc->verified) {   /* hashed only when asked */
            int e = hash_at(acc->od, name, got);
            if (e != ENOENT && (e || !strcmp(got, sha))) acc->verified = 1;   /* ENOENT: gone meanwhile */
        }
    }
    return acc->saved && (acc->verified || !acc->want_verified);   /* stops the walk once all asked is known */
}

/* MODELS_INFO's two answers from one look at originals\ (review MP1: a transient error can't flip
 * one against the other). *saved: savedOriginals non-empty, or unknown (never "no saved copy" for
 * originals that can't be read, VU4). *verified: verifiedOriginals non-empty, failing closed
 * (review J1, the Mac's twin): a copy that can't be told good or damaged counts as one, so
 * "Remove Anyway" never takes it for "none verified": originals\ that can't be opened (other than
 * not there, not a folder, or a link: never followed, never deleted through), a listing that
 * can't be read whole, or a <sha>.onnx that can't be opened or read, or isn't a regular file (a
 * reparse point: VU2) */
void models_originals_info(const models *m, int *saved, int *verified) {
    plat_dir od;
    int e = open_originals(m, 0, &od);
    if (e) {
        *saved = originals_err_unknown(e);
        if (verified) *verified = *saved;
        return;
    }
    info_acc acc = {m, od, 0, 0, verified != NULL};
    int r = each_at(od, info_each, &acc);
    plat_dir_close(od);
    *saved = acc.saved || r != 0;
    if (verified) *verified = acc.verified || r != 0;
}

int models_any_verified(const models *m) {
    int saved, verified;
    models_originals_info(m, &saved, &verified);
    return verified;
}

/* name in d removed when it is a regular file, logged: 1 when it went */
static int leftover_at(const models *m, plat_dir d, const char *name) {
    plat_stat st;
    if (plat_stat_at(d, name, &st) || st.kind != PLAT_FILE) return 0;
    int e = plat_unlink_at(d, name);
    char line[160];
    snprintf(line, sizeof line, "leftover %s removed: %s", name, e && e != ENOENT ? "FAILED" : "ok");
    say(m, line);
    return !e;
}

/* every leftover of base in d (part_list's rule), listed first: nothing removed while it is read */
static int leftovers_of(const models *m, plat_dir d, const char *base) {
    part_list l = {base, NULL, 0, 0};
    plat_dir_each(d, part_each, &l);
    int gone = 0;
    for (int i = 0; i < l.n; i++) gone += leftover_at(m, d, l.names[i]);
    free(l.names);
    return gone;
}

int models_remove_leftovers(const models *m) {
    plat_dir d;
    int gone = 0;
    if (!plat_dir_open(m->dir, &d)) {   /* the fixed names and the per-run ones (review WF1) */
        gone += leftovers_of(m, d, new_temp);
        gone += leftovers_of(m, d, restore_temp);
        plat_dir_close(d);
    }
    if (!open_originals(m, 0, &d)) {   /* through support, neither followed (review MD5) */
        gone += leftovers_of(m, d, NULL);
        plat_dir_close(d);
    }
    return gone;
}

int models_left_behind(const models *m) {
    plat_dir dd;
    int e = plat_dir_open(m->dir, &dd);
    if (e) return e == ENOENT || e == ENOTDIR ? 0 : 1;   /* no folder: no model; a link or an error: unreadable (MD7) */
    char live[65];
    int r = live_sha(dd, 0, live) ? 1 : !live[0] ? 0 : is_ours(m, live) ? 2 : 3;
    plat_dir_close(dd);
    return r;
}
