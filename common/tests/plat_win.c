/* plat_win.c's unit tests, for the Windows VM (bridge.md §4 "In the VM"): the twin of
 * common/tests/plat.c, each function against what plat.h promises, on a scratch folder. Built by
 * common/tests/win.sh for x64 and arm64 (with the fake clock and PLAT_TEST_OWNER); run in the VM:
 *
 *   plat-win-test.exe SCRATCH       SCRATCH: an empty folder on NTFS ("C:\rbtest\scratch");
 *                                   exits 1 when a check fails
 *   plat-win-test.exe SCRATCH fat   SCRATCH on an exFAT or FAT32 volume: the file layer's checks
 *                                   (t_files) and t_remove_empty_dir only, their junction (and
 *                                   delete-pending) cases skipped
 *
 * Not run on the Mac: it needs Windows. */
#include "../plat.h"
#include "../plat_win.h"
#include "../win_rules.h"
#include "check.h"

#include <aclapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>

static char root[1024];
static int fat_only;   /* "fat": t_files alone, without its junction case (FAT has no reparse points) */
static int redir_guard;   /* this account's junctions refused: Redirection Guard on, the token not elevated */

/* Redirection Guard (pc-round 2026-11-16): with EnforceRedirectionTrust on (Win32-OpenSSH's sshd
 * sets it, and its children inherit it), Windows won't follow a junction a non-admin made: an open
 * through one fails with STATUS_UNTRUSTED_MOUNT_POINT, which win_rules.h maps to ELOOP. A Windows
 * that doesn't know the policy fails the call: not enforced. */
static int redirection_enforced(void) {
    PROCESS_MITIGATION_REDIRECTION_TRUST_POLICY rt;
    memset(&rt, 0, sizeof rt);
    return GetProcessMitigationPolicy(GetCurrentProcess(), ProcessRedirectionTrustPolicy, &rt, sizeof rt) && rt.EnforceRedirectionTrust;
}

/* Redirection Guard refuses only the junctions a non-admin made: an elevated token's are followed.
 * TokenElevation, not group membership: a filtered admin token is medium integrity, and refused */
static int token_elevated(void) {
    HANDLE t;
    TOKEN_ELEVATION te = {0};
    DWORD len = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 0;
    BOOL ok = GetTokenInformation(t, TokenElevation, &te, sizeof te, &len);
    CloseHandle(t);
    return ok && te.TokenIsElevated;
}

static void path(char *out, size_t n, const char *name) { snprintf(out, n, "%s\\%s", root, name); }

/* system() with a UTF-8 command: the ANSI one loses a CJK profile's path */
static int run_cmd(const char *cmd) {
    wchar_t w[3000];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, cmd, -1, w, 3000)) return -1;
    return _wsystem(w);
}

static void hexs(const unsigned char *d, char *out) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void t_sha256(void) {
    unsigned char d[32];
    char h[65];
    CHECK(plat_sha256("", 0, d) == 0, "empty");
    hexs(d, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "empty: %s", h);
    plat_sha256("abc", 3, d);
    hexs(d, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "abc: %s", h);
    plat_sha256_ctx c;
    char a[1000];
    memset(a, 'a', sizeof a);
    CHECK(plat_sha256_init(&c) == 0, "init");
    for (int i = 0; i < 1000; i++) plat_sha256_update(&c, a, sizeof a);
    CHECK(plat_sha256_final(&c, d) == 0, "final");
    hexs(d, h);
    CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "a million a: %s", h);
    /* bcrypt.dll was loaded by its full path: it is System32's */
    HMODULE m = GetModuleHandleW(L"bcrypt.dll");
    wchar_t got[MAX_PATH], sys[MAX_PATH];
    CHECK(m && GetModuleFileNameW(m, got, MAX_PATH) && GetSystemDirectoryW(sys, MAX_PATH)
              && !_wcsnicmp(got, sys, wcslen(sys)), "bcrypt.dll from System32");
}

static void t_irfft(void) {
    /* the same reference as plat.c's: the plain sum over all n bins */
    for (unsigned lg = 2; lg <= 12; lg += 5) {
        size_t n = (size_t)1 << lg, h = n / 2;
        float *re = malloc(sizeof(float) * h), *im = malloc(sizeof(float) * h), *out = malloc(sizeof(float) * n);
        double *X = malloc(sizeof(double) * h * 2), worst = 0, peak = 0;
        uint32_t s = 12345;
        for (size_t k = 0; k < h; k++) {
            s = s * 1103515245u + 12345u; X[2 * k] = (double)(s >> 8) / (1 << 24) - 0.5;
            s = s * 1103515245u + 12345u; X[2 * k + 1] = (double)(s >> 8) / (1 << 24) - 0.5;
            re[k] = (float)X[2 * k];
            im[k] = (float)X[2 * k + 1];
        }
        plat_irfft *f = plat_irfft_new(lg);
        CHECK(f != NULL, "irfft %zu", n);
        if (!f) return;
        plat_irfft_run(f, re, im, out);
        plat_irfft_free(f);
        for (size_t t = 0; t < n; t++) {
            double v = X[0] + X[1] * (t % 2 ? -1 : 1);
            for (size_t k = 1; k < h; k++) {
                double a = 2 * 3.14159265358979323846 * (double)k * (double)t / (double)n;
                v += 2 * (X[2 * k] * cos(a) - X[2 * k + 1] * sin(a));
            }
            if (!isfinite(out[t])) worst = INFINITY;          /* fmax would drop a NaN */
            worst = fmax(worst, fabs(v - out[t]));
            peak = fmax(peak, fabs(v));
        }
        CHECK(worst <= 1e-6 * fmax(1, peak), "irfft %zu: off by %g", n, worst);
        free(re); free(im); free(out); free(X);
    }
}

/* ---------------------------------------------------------------- locks, clocks, queues */

static atomic_int once_runs;
static void once_fn(void) { atomic_fetch_add(&once_runs, 1); }
static plat_once_t once = PLAT_ONCE_INIT;

typedef struct { plat_mutex m; plat_cond c; int n; int order[64]; int classes; int prio; } qstate;
static void q_job(void *ctx) {
    qstate *s = ctx;
    plat_mutex_lock(&s->m);
    if (s->n < 64) s->order[s->n] = s->n;
    s->n++;
    s->classes |= 1 << plat_work_class();
    s->prio = GetThreadPriority(GetCurrentThread());
    plat_mutex_unlock(&s->m);
    plat_cond_broadcast(&s->c);
}

static atomic_int seen[1000];
static atomic_int maint_seen, below_seen;
static void par_job(void *ctx, size_t i) {
    (void)ctx;
    atomic_fetch_add(&seen[i], 1);
    if (plat_work_class() == PLAT_WORK_MAINT) atomic_fetch_add(&maint_seen, 1);
    if (GetThreadPriority(GetCurrentThread()) <= THREAD_PRIORITY_BELOW_NORMAL) atomic_fetch_add(&below_seen, 1);
}
static void par_from_maint(void *ctx) { (void)ctx; plat_par_for(1000, PLAT_WORK_MAINT, par_job, NULL); }

static void t_locks_queues(void) {
    plat_once(&once, once_fn);
    plat_once(&once, once_fn);
    CHECK(atomic_load(&once_runs) == 1, "once");
    plat_mutex m = PLAT_MUTEX_INIT;
    plat_cond c;
    plat_cond_init(&c);
    plat_mutex_lock(&m);
    uint64_t t0 = plat_now_mono_ns();
    CHECK(plat_cond_wait_ns(&c, &m, 30000000) == ETIMEDOUT, "timed wait");
    uint64_t t1 = plat_now_mono_ns();
    plat_mutex_unlock(&m);
    CHECK(t1 - t0 >= 30000000 && t1 - t0 < 2000000000, "waited %llu ns", (unsigned long long)(t1 - t0));
    int64_t w = plat_now_wall_s();
    CHECK(w > 1700000000 && w < 4000000000LL, "wall clock %lld", (long long)w);
    struct tm tm;
    plat_localtime(w, &tm);
    CHECK(tm.tm_year >= 123, "localtime");

    plat_queue *q = plat_queue_new("test.writer", PLAT_WORK_DEFAULT), *mq = plat_queue_new("test.maint", PLAT_WORK_MAINT);
    CHECK(q && mq, "queues");
    if (!q || !mq) return;
    /* a queue plat_queue_new couldn't make (NULL): every submission refused, nothing run (BW5) */
    CHECK(plat_async(NULL, q_job, NULL) == EINVAL && plat_after(NULL, 1000, q_job, NULL) == EINVAL, "no queue: refused");
    plat_queue_wait(NULL);
    qstate s = {PLAT_MUTEX_INIT, {0}, 0, {0}, 0, 0};
    plat_cond_init(&s.c);
    for (int i = 0; i < 50; i++) plat_async(q, q_job, &s);
    plat_queue_wait(q);
    CHECK(s.n == 50 && s.classes == 1, "50 jobs in order on the default class (%d, %x)", s.n, s.classes);
    for (int i = 0; i < 50; i++) CHECK(s.order[i] == i, "order %d", i);
    /* 100,000 waits on an empty queue: a smoke test of the allocation-free barrier (B7). It cannot
     * catch a late touch of the waiter, which lives on the stack; B2's order is checked by
     * inspection */
    for (int i = 0; i < 100000; i++) plat_queue_wait(q);
    CHECK(s.n == 50, "100,000 empty waits");
    qstate ms = {PLAT_MUTEX_INIT, {0}, 0, {0}, 0, 0};
    plat_cond_init(&ms.c);
    plat_async(mq, q_job, &ms);
    plat_queue_wait(mq);
    CHECK(ms.classes == 1 << PLAT_WORK_MAINT && ms.prio == THREAD_PRIORITY_BELOW_NORMAL, "maintenance: class and priority");
    qstate ls = {PLAT_MUTEX_INIT, {0}, 0, {0}, 0, 0};
    plat_cond_init(&ls.c);
    t0 = plat_now_mono_ns();
    plat_after(q, 50000000, q_job, &ls);
    plat_mutex_lock(&ls.m);
    while (ls.n == 0 && plat_now_mono_ns() - t0 < 3000000000u) plat_cond_wait_ns(&ls.c, &ls.m, 100000000);
    plat_mutex_unlock(&ls.m);
    CHECK(ls.n == 1 && plat_now_mono_ns() - t0 >= 45000000, "plat_after");
    plat_async(mq, par_from_maint, NULL);
    plat_queue_wait(mq);
    int all = 1;
    for (int i = 0; i < 1000; i++) all &= atomic_load(&seen[i]) == 1;
    CHECK(all, "par_for: each index once");
    CHECK(atomic_load(&maint_seen) == 1000 && atomic_load(&below_seen) == 1000, "par_for workers inherit the class and priority");
}

static char fired[64];
static size_t nfired;
static void fk(void *ctx) { if (nfired < sizeof fired - 1) fired[nfired++] = *(const char *)ctx; }

static void t_fake_clock(void) {
    plat_fake_clock(1, 1000, 5000000000LL);
    CHECK(plat_now_mono_ns() == 1000 && plat_now_wall_s() == 5, "fake clocks");
    plat_queue *q = plat_queue_new("fake", PLAT_WORK_DEFAULT);
    plat_after(q, 200, fk, "b");
    plat_after(q, 100, fk, "a");
    plat_timer *t = plat_timer_new(q, 150, fk, "x");
    CHECK(plat_fake_pending() == 3, "3 pending");
    CHECK(plat_timer_cancel(t) == 1 && plat_fake_pending() == 2, "cancelled");
    plat_fake_advance(500);
    CHECK(!strcmp(fired, "ab") && plat_now_mono_ns() == 1500, "fired %s", fired);
    plat_fake_clock(0, 0, 0);
}

/* ---------------------------------------------------------------- files */

typedef struct { char names[16][64]; int kinds[16]; int64_t alloc[16]; plat_id ids[16]; int n; } listing;
static int list_fn(void *ctx, const char *name, const plat_stat *st) {
    listing *l = ctx;
    if (l->n < 16) {
        snprintf(l->names[l->n], 64, "%s", name);
        l->kinds[l->n] = st->kind;
        l->alloc[l->n] = st->alloc;
        l->ids[l->n] = st->id;
        l->n++;
    }
    return 0;
}
/* every listed name's id is the id plat_stat_at gives (B16: ReFS's listing gave its 64-bit short
 * id, its stat the 128-bit one) */
static int ids_match(plat_dir d, const listing *l) {
    for (int i = 0; i < l->n; i++) {
        plat_stat st;
        if (plat_stat_at(d, l->names[i], &st) || !plat_id_same(st.id, l->ids[i])) {
            printf("  %s: listed %llx/%llx, stat %llx/%llx\n", l->names[i], (unsigned long long)l->ids[i].file[0],
                   (unsigned long long)l->ids[i].file[1], (unsigned long long)st.id.file[0], (unsigned long long)st.id.file[1]);
            return 0;
        }
    }
    return 1;
}
static int listed(const listing *l, const char *name, int kind) {
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->names[i], name)) return l->kinds[i] == kind;
    return 0;
}

static void t_files(void) {
    char p[1100];
    plat_dir d, sub;
    CHECK(plat_dir_open(root, &d) == 0, "open scratch");
    CHECK(plat_mkdir_at(d, "ab") == 0 && plat_mkdir_at(d, "ab") == EEXIST, "mkdir, EEXIST");
    CHECK(plat_dir_open_at(d, "ab", &sub) == 0, "open ab");
    CHECK(plat_dir_name_is(sub, "ab") && !plat_dir_name_is(sub, "AB"), "name_is");
    plat_dir up;
    CHECK(plat_dir_open_at(d, "AB", &up) == 0 && !plat_dir_name_is(up, "AB"), "a volume that ignores case");
    plat_dir_close(up);
    CHECK(plat_dir_open_at(d, "..", &up) == EINVAL && plat_dir_open_at(d, "a\\b", &up) == EINVAL, "one component only");
    plat_file f;
    CHECK(plat_file_create_at(sub, "x.tmp", 0, &f) == 0, "create");
    FILE *s = plat_file_stdio(f, "w");
    CHECK(s != NULL, "stdio");
    if (s) { fputs("line\n", s); fclose(s); }
    CHECK(plat_file_create_at(sub, "x.tmp", 0, &f) == EEXIST, "create twice");
    int64_t sz = 0;
    CHECK(plat_file_open_at(sub, "x.tmp", 100, &sz, &f) == 0 && sz == 5, "open_at, size %lld", (long long)sz);
    char b[16] = {0};
    CHECK(plat_read_all(f, b, 5) == 0 && !memcmp(b, "line\n", 5), "binary: no CR (r4:I50)");
    size_t got = 9;
    CHECK(plat_read(f, b, 5, &got) == 0 && got == 0, "end of file");
    CHECK(plat_pread(f, b, 2, 1, &got) == 0 && got == 2 && !memcmp(b, "in", 2), "pread");
    plat_stat st;
    CHECK(plat_file_stat(f, &st) == 0 && st.kind == PLAT_FILE && st.size == 5 && st.alloc >= 0, "file_stat");
    CHECK(plat_file_touch(f) == 0, "touch a hit");
    plat_file_close(f);
    CHECK(plat_file_open_at(sub, "x.tmp", 4, NULL, &f) == PLAT_EFTYPE, "over max");
    CHECK(plat_file_open_at(d, "ab", 100, NULL, &f) == PLAT_EFTYPE, "a folder");
    CHECK(plat_file_open_at(sub, "missing", 100, NULL, &f) == ENOENT, "missing");
    CHECK(plat_rename_at(sub, "x.tmp", sub, "x.flac") == 0, "rename");
    CHECK(plat_file_create_at(sub, "y.tmp", 1, &f) == 0, "create rw");
    void *map;
    CHECK(plat_map_rw(f, 4096, &map) == 0 && map, "map");
    if (map) {
        memset(map, 7, 4096);
        CHECK(plat_map_flush(map, 4096) == 0 && plat_map_flush(NULL, 0) == 0, "map_flush (FlushViewOfFile)");
        plat_unmap(map, 4096);
    }
    plat_file_close(f);
    CHECK(plat_rename_at(sub, "y.tmp", sub, "x.flac") == 0, "rename over");
    CHECK(plat_stat_at(sub, "x.flac", &st) == 0 && st.size == 4096, "replaced");
    /* never over an existing name (ReplaceIfExists FALSE): EEXIST, both kept; to a free name, done */
    CHECK(plat_file_create_at(sub, "n.tmp", 1, &f) == 0, "create n.tmp");
    plat_file_close(f);
    CHECK(plat_rename_noreplace_at(sub, "n.tmp", sub, "x.flac") == EEXIST && plat_stat_at(sub, "n.tmp", &st) == 0
              && plat_stat_at(sub, "x.flac", &st) == 0 && st.size == 4096,
          "rename_noreplace over a file: EEXIST, both kept");
    CHECK(plat_rename_noreplace_at(sub, "n.tmp", sub, "n") == 0 && plat_stat_at(sub, "n.tmp", &st) == ENOENT && plat_stat_at(sub, "n", &st) == 0,
          "rename_noreplace to a free name");
    plat_unlink_at(sub, "n");
    plat_stat id1, id2;
    CHECK(plat_dir_stat(sub, &id1) == 0 && id1.kind == PLAT_DIR, "dir_stat");
    path(p, sizeof p, "ab");
    CHECK(plat_stat_path(p, 0, &id2) == 0 && plat_id_same(id1.id, id2.id), "identity");
    listing l = {0};
    CHECK(plat_dir_each(d, list_fn, &l) == 0 && l.n == 1 && listed(&l, "ab", PLAT_DIR), "each: %d", l.n);
    memset(&l, 0, sizeof l);
    CHECK(plat_dir_each(sub, list_fn, &l) == 0 && l.n == 1 && listed(&l, "x.flac", PLAT_FILE) && l.alloc[0] >= 4096, "each in ab");
    CHECK(ids_match(sub, &l), "each's ids are plat_stat_at's");
    memset(&l, 0, sizeof l);
    CHECK(plat_dir_each(d, list_fn, &l) == 0 && ids_match(d, &l), "and a folder's id too");
    /* a file held open without FILE_SHARE_DELETE: the rename and the delete see EBUSY (on this
     * thread, not a queue's: no retry) */
    path(p, sizeof p, "ab\\x.flac");
    wchar_t wp[1100];
    MultiByteToWideChar(CP_UTF8, 0, p, -1, wp, 1100);
    HANDLE held = CreateFileW(wp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(held != INVALID_HANDLE_VALUE, "hold");
    CHECK(plat_rename_at(sub, "x.flac", sub, "z") == EBUSY && plat_unlink_at(sub, "x.flac") == EBUSY, "held: EBUSY");
    CloseHandle(held);
    CHECK(plat_touch_at(sub, "x.flac") == 0, "touch_at");
    CHECK(plat_unlink_at(sub, "x.flac") == 0 && plat_stat_at(sub, "x.flac", &st) == ENOENT, "unlink");
    CHECK(plat_unlink_at(sub, "x.flac") == ENOENT, "unlink missing");
    /* a junction at the last component is refused (ELOOP), never entered */
    path(p, sizeof p, "jn");
    char cmd[1300];
    snprintf(cmd, sizeof cmd, "mklink /J \"%s\" \"%s\" >NUL", p, root);
    if (fat_only) {
        printf("junction: skipped (FAT)\n");
    } else if (run_cmd(cmd) == 0) {
        CHECK(plat_dir_open_at(d, "jn", &up) == ELOOP && plat_dir_open(p, &up) == ELOOP, "a junction refused");
        CHECK(plat_stat_at(d, "jn", &st) == 0 && st.kind == PLAT_OTHER, "a junction is OTHER");
        /* plat_dir_open_follow (review SF2): the junction resolved, to the folder it names (by id) */
        {
            plat_dir fd;
            plat_stat fa, fb;
            int fe = plat_dir_open_follow(p, &fd);
            if (redir_guard)                                /* this account's junction isn't followed */
                CHECK(fe == ELOOP, "open_follow: Redirection Guard refuses the junction (%d)", fe);
            else
                CHECK(fe == 0 && !plat_dir_stat(fd, &fa) && !plat_dir_stat(d, &fb) && plat_id_same(fa.id, fb.id),
                      "open_follow: a junction opens its target (%d)", fe);
            if (!fe) plat_dir_close(fd);
        }
        memset(&l, 0, sizeof l);
        CHECK(plat_dir_each(d, list_fn, &l) == 0 && listed(&l, "jn", PLAT_OTHER), "listed as OTHER");
        snprintf(cmd, sizeof cmd, "rmdir \"%s\"", p);
        run_cmd(cmd);
    } else CHECK(0, "mklink /J failed");
    /* paths */
    path(p, sizeof p, "log.txt");
    s = plat_fopen_w(p, 1);
    CHECK(s != NULL, "fopen_w append");
    if (s) { fputs("a\n", s); fclose(s); }
    s = plat_fopen_w(p, 1);
    if (s) { fputs("b\n", s); fclose(s); }
    char *buf = NULL;
    size_t n = 0;
    CHECK(plat_read_file(p, 100, &buf, &n) == 0 && n == 4 && !memcmp(buf, "a\nb\n", 4), "appended, no CR");
    free(buf);
    s = plat_fopen_w(p, 0);
    if (s) { fputs("c\n", s); fclose(s); }
    CHECK(plat_read_file(p, 100, &buf, &n) == 0 && n == 2, "truncated");
    free(buf);
    CHECK(plat_read_file(p, 1, &buf, &n) == PLAT_EFTYPE, "over max");
    char p2[1100];
    path(p2, sizeof p2, "log.txt.1");
    CHECK(plat_rename_path(p, p2) == 0 && plat_unlink_path(p2) == 0, "rename_path, unlink_path");
    path(p, sizeof p, "deep\\er\\x");
    plat_mkdirs(p);
    CHECK(plat_stat_path(p, 0, &st) == 0 && st.kind == PLAT_DIR, "mkdirs");
    /* a long path (over 260 characters) */
    char lp[1100];
    snprintf(lp, sizeof lp, "%s\\%0200d\\%0100d", root, 0, 1);
    plat_mkdirs(lp);
    CHECK(plat_stat_path(lp, 0, &st) == 0 && st.kind == PLAT_DIR, "a path over 260 characters");
    /* a non-ASCII and a CJK name */
    CHECK(plat_mkdir_at(d, "J\xc3\xbcrgen \xe6\x9d\xb1\xe4\xba\xac") == 0, "a Unicode name");
    memset(&l, 0, sizeof l);
    plat_dir_each(d, list_fn, &l);
    CHECK(listed(&l, "J\xc3\xbcrgen \xe6\x9d\xb1\xe4\xba\xac", PLAT_DIR), "listed back in UTF-8");
    CHECK(plat_mkdir_at(d, "bad\xff") == EINVAL, "invalid UTF-8 refused");
    CHECK(plat_dir_writable(d) == 0, "writable");
    plat_dir_close(sub);
    plat_dir_close(d);
}

/* ---------------------------------------------------------------- the low I/O hint (r4:I51, B21)
 * A folder and a file opened in a maintenance job carry IoPriorityLow (1); in a default job,
 * Normal (2). The query's buffer is 8-byte aligned, as Windows requires for this class. */
typedef LONG(NTAPI *hint_query_fn)(HANDLE, void *, void *, ULONG, int);
typedef struct { ULONG dir, file; int ok; } hints;
static void hint_job(void *ctx) {
    hints *h = ctx;
    hint_query_fn q;
    FARPROC p = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
    memcpy(&q, &p, sizeof p);
    plat_dir d;
    plat_file f;
    if (!q || plat_dir_open(root, &d)) return;
    if (plat_file_open_at(d, "hint.txt", 100, NULL, &f) == 0) {
        ULONG_PTR io[2];
        _Alignas(8) ULONG a = 99, b = 99;
        h->ok = q((HANDLE)d.h, io, &a, sizeof a, 43) == 0 && q((HANDLE)f.h, io, &b, sizeof b, 43) == 0;
        h->dir = a;
        h->file = b;
        plat_file_close(f);
    }
    plat_dir_close(d);
}

static void t_io_hint(void) {
    char p[1100];
    path(p, sizeof p, "hint.txt");
    FILE *s = plat_fopen_w(p, 0);
    if (s) fclose(s);
    plat_queue *mq = plat_queue_new("test.hint.maint", PLAT_WORK_MAINT), *q = plat_queue_new("test.hint", PLAT_WORK_DEFAULT);
    hints m = {0, 0, 0}, n = {0, 0, 0};
    if (mq && q) {
        plat_async(mq, hint_job, &m);
        plat_queue_wait(mq);
        plat_async(q, hint_job, &n);
        plat_queue_wait(q);
    }
    CHECK(m.ok && m.dir == 1 && m.file == 1, "maintenance job: the low I/O hint (folder %lu, file %lu)", m.dir, m.file);
    CHECK(n.ok && n.dir == 2 && n.file == 2, "default job: normal I/O (folder %lu, file %lu)", n.dir, n.file);
    plat_unlink_path(p);
}

/* ---------------------------------------------------------------- the owner check's fixtures
 * (review B10, B11). Folders under C:\ inherit Authenticated Users:(M), so the scratch folder's own
 * ACL can't be relied on: every case builds its tree under SCRATCH\prot, a folder given a
 * protected DACL (SYSTEM, Administrators and this account, (OI)(CI) full) with
 * SetNamedSecurityInfoW, and checks a test build's accept_self=1 there. Per-object changes use
 * SetFileSecurityW, which doesn't propagate to what is already below. */

static PSID self_sid(void) {
    static union { TOKEN_USER u; unsigned char b[256]; } tu;
    HANDLE t;
    DWORD len = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return NULL;
    BOOL ok = GetTokenInformation(t, TokenUser, &tu, sizeof tu, &len);
    CloseHandle(t);
    return ok ? tu.u.User.Sid : NULL;
}

static PSID known_sid(WELL_KNOWN_SID_TYPE k, unsigned char *buf, DWORD n) {
    return CreateWellKnownSid(k, NULL, (PSID)buf, &n) ? (PSID)buf : NULL;
}

enum { ACL_BASE, ACL_EMPTY, ACL_NULL };
/* a protected DACL on p: base (SYSTEM, Administrators, this account: (OI)(CI) full) plus, if
 * extra_sid, one more ALLOW ACE; or an empty DACL; or a NULL one. named: SetNamedSecurityInfoW
 * (propagates to children), else SetFileSecurityW (this object only). */
static int set_acl(const char *p8, int mode, PSID extra_sid, DWORD extra_mask, BYTE extra_flags, int named) {
    wchar_t w[1100];
    unsigned char sys[SECURITY_MAX_SID_SIZE], adm[SECURITY_MAX_SID_SIZE];
    union { ACL acl; unsigned char b[1024]; } buf;
    PACL acl = NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, p8, -1, w, 1100)) return 0;
    if (mode != ACL_NULL) {
        acl = &buf.acl;
        if (!InitializeAcl(acl, sizeof buf, ACL_REVISION)) return 0;
    }
    if (mode == ACL_BASE) {
        BYTE inh = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        PSID s = known_sid(WinLocalSystemSid, sys, sizeof sys), a = known_sid(WinBuiltinAdministratorsSid, adm, sizeof adm), me = self_sid();
        if (!s || !a || !me || !AddAccessAllowedAceEx(acl, ACL_REVISION, inh, FILE_ALL_ACCESS, s)
            || !AddAccessAllowedAceEx(acl, ACL_REVISION, inh, FILE_ALL_ACCESS, a) || !AddAccessAllowedAceEx(acl, ACL_REVISION, inh, FILE_ALL_ACCESS, me))
            return 0;
        if (extra_sid && !AddAccessAllowedAceEx(acl, ACL_REVISION, extra_flags, extra_mask, extra_sid)) return 0;
    }
    if (named)
        return SetNamedSecurityInfoW(w, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, acl, NULL)
               == ERROR_SUCCESS;
    SECURITY_DESCRIPTOR sd;
    return InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE)
           && SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED) && SetFileSecurityW(w, DACL_SECURITY_INFORMATION, &sd);
}

/* SCRATCH\prot\<name>\p1\p2\x.dll; the three paths into file, p1, p2 */
static void tree(const char *name, char *file, char *p1, char *p2) {
    char d[1100];
    snprintf(d, sizeof d, "%s\\prot\\%s\\p1\\p2", root, name);
    plat_mkdirs(d);
    snprintf(p1, 1100, "%s\\prot\\%s\\p1", root, name);
    snprintf(p2, 1100, "%s\\prot\\%s\\p1\\p2", root, name);
    snprintf(file, 1100, "%s\\x.dll", p2);
    FILE *f = plat_fopen_w(file, 0);
    if (f) fclose(f);
}

static void t_checked_load(void);

static void t_owner(void) {
    char prot[1100], file[1100], p1[1100], p2[1100], bad[1100];
    unsigned char users_b[SECURITY_MAX_SID_SIZE], auth_b[SECURITY_MAX_SID_SIZE];
    PSID users = known_sid(WinBuiltinUsersSid, users_b, sizeof users_b), auth = known_sid(WinAuthenticatedUserSid, auth_b, sizeof auth_b);
    snprintf(prot, sizeof prot, "%s\\prot", root);
    plat_mkdirs(prot);
    CHECK(users && auth && set_acl(prot, ACL_BASE, NULL, 0, 0, 1), "the protected folder");

    tree("ok", file, p1, p2);                               /* B10: everything protected, this account the owner */
    CHECK(plat_owner_check(file, 2, 1, bad, sizeof bad) == PLAT_OWNER_OK, "accept_self in a protected tree: %s", bad);
    CHECK(plat_owner_check(file, 2, 0, bad, sizeof bad) == PLAT_OWNER_FILE && !strcmp(bad, file), "the account isn't an owner without accept_self");

    /* an inherited write ACE (as C:\ gives every folder: Authenticated Users:(OI)(CI)(M)) */
    char open[1100], mine[1100];
    snprintf(open, sizeof open, "%s\\open", prot);
    plat_mkdirs(open);
    CHECK(set_acl(open, ACL_BASE, auth, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE,
                  OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, 1), "the open folder");
    snprintf(mine, sizeof mine, "%s\\mine.dll", open);
    FILE *f = plat_fopen_w(mine, 0);
    if (f) fclose(f);
    CHECK(plat_owner_check(mine, 0, 1, bad, sizeof bad) == PLAT_OWNER_FILE, "an inherited Authenticated Users write ACE: refused");

    /* B11: the parents, the ACEs, a reparse point (with accept_self, in fresh trees) */
    tree("users-p2", file, p1, p2);
    CHECK(set_acl(p2, ACL_BASE, users, FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE, 0, 0), "Users:(M) on p2");
    CHECK(plat_owner_check(file, 2, 1, bad, sizeof bad) == PLAT_OWNER_FOLDER && !strcmp(bad, p2), "Users:(M) on the parent: %s", bad);
    tree("users-p1", file, p1, p2);
    CHECK(set_acl(p1, ACL_BASE, users, FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE, 0, 0), "Users:(M) on p1");
    CHECK(plat_owner_check(file, 2, 1, bad, sizeof bad) == PLAT_OWNER_FOLDER && !strcmp(bad, p1), "Users:(M) on the second parent: %s", bad);
    CHECK(plat_owner_check(file, 1, 1, bad, sizeof bad) == PLAT_OWNER_OK, "levels = 1 doesn't reach it");
    tree("users-file", file, p1, p2);
    CHECK(set_acl(file, ACL_BASE, users, FILE_GENERIC_READ | FILE_GENERIC_WRITE, 0, 0), "Users write on the file");
    CHECK(plat_owner_check(file, 2, 1, bad, sizeof bad) == PLAT_OWNER_FILE && !strcmp(bad, file), "a Users write ACE on the file");
    tree("inherit-only", file, p1, p2);
    CHECK(set_acl(p2, ACL_BASE, users, FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE, OBJECT_INHERIT_ACE | INHERIT_ONLY_ACE, 0),
          "Users:(OI)(IO)(M) on p2");
    CHECK(plat_owner_check(file, 2, 1, bad, sizeof bad) == PLAT_OWNER_OK, "an inherit-only ACE on the parent: %s", bad);
    tree("empty", file, p1, p2);
    CHECK(set_acl(p2, ACL_EMPTY, NULL, 0, 0, 0), "an empty DACL on p2");
    CHECK(plat_owner_check(file, 2, 1, bad, sizeof bad) == PLAT_OWNER_OK, "an empty DACL grants nothing: %s", bad);
    tree("null", file, p1, p2);
    CHECK(set_acl(p2, ACL_NULL, NULL, 0, 0, 0), "a NULL DACL on p2");
    CHECK(plat_owner_check(file, 2, 1, bad, sizeof bad) == PLAT_OWNER_FOLDER && !strcmp(bad, p2), "a NULL DACL on the parent");
    /* a junction as the parent: the file opens through it, the parent itself is refused */
    tree("junction", file, p1, p2);
    char jn[1100], via[1100], cmd[2400];
    snprintf(jn, sizeof jn, "%s\\jn", p1);
    snprintf(cmd, sizeof cmd, "mklink /J \"%s\" \"%s\" >NUL", jn, p2);
    if (run_cmd(cmd) == 0) {
        snprintf(via, sizeof via, "%s\\x.dll", jn);
        if (redir_guard)                                    /* the file can't be opened through jn: refused there */
            CHECK(plat_owner_check(via, 2, 1, bad, sizeof bad) == PLAT_OWNER_FILE && !strcmp(bad, via),
                  "a junction as the parent, Redirection Guard: refused at the file: %s", bad);
        else
            CHECK(plat_owner_check(via, 2, 1, bad, sizeof bad) == PLAT_OWNER_FOLDER && !strcmp(bad, jn), "a junction as the parent: %s", bad);
        plat_lib *jl = NULL;
        CHECK(plat_lib_open_checked(via, 2, 1, bad, sizeof bad, &jl, NULL) == PLAT_OWNER_FOLDER && !jl && !strcmp(bad, jn),
              "checked load: a junction as the parent: %s", bad);
    } else CHECK(0, "mklink /J failed");
    t_checked_load();
    /* cleanup: empty\p1\p2's empty DACL lets nobody list or delete it (the same user included), so
     * it gets the base DACL back (this account owns it: WRITE_DAC), then SCRATCH\prot goes whole.
     * x64 under emulation keeps a freed DLL open for about a second: EBUSY retried for up to 3 s */
    snprintf(p2, sizeof p2, "%s\\prot\\empty\\p1\\p2", root);
    CHECK(set_acl(p2, ACL_BASE, NULL, 0, 0, 0), "cleanup: the empty DACL reset");
    int rr = plat_remove_tree(prot);
    for (int i = 0; i < 30 && rr == EBUSY; i++) {
        Sleep(100);
        rr = plat_remove_tree(prot);
    }
    CHECK(rr == 0, "cleanup: SCRATCH\\prot removed (%d)", rr);
}

/* plat_lib_open_checked (owner decided, review B1): the rule held through the load */
static void t_checked_load(void) {
    char file[1100], p1[1100], p2[1100], bad[1100], src[1100];
    wchar_t wsrc[1100], wdst[1100], w2[1100];
    plat_lib *l = NULL;
    const char *err = NULL;
    unsigned char users_b[SECURITY_MAX_SID_SIZE];
    DWORD un = sizeof users_b;
    PSID users = CreateWellKnownSid(WinBuiltinUsersSid, NULL, (PSID)users_b, &un) ? (PSID)users_b : NULL;
    /* checked.dll (common/tests/checked_dll.c) sits next to this program */
    DWORD k = GetModuleFileNameW(NULL, wsrc, 1100);
    wchar_t *slash = k ? wcsrchr(wsrc, L'\\') : NULL;
    if (!slash) { CHECK(0, "this program's folder"); return; }
    wcscpy(slash + 1, L"checked.dll");
    WideCharToMultiByte(CP_UTF8, 0, wsrc, -1, src, sizeof src, NULL, NULL);
    tree("load", file, p1, p2);
    MultiByteToWideChar(CP_UTF8, 0, file, -1, wdst, 1100);
    CHECK(CopyFileW(wsrc, wdst, FALSE), "copy %s", src);        /* the copy inherits the protected DACL */
    /* CopyFileW keeps the source's attributes: a build copied in from \\Mac\ is read-only, and the
     * hold for writing below would fail on that, not on the check */
    SetFileAttributesW(wdst, FILE_ATTRIBUTE_NORMAL);
    CHECK(plat_lib_open_checked(file, 2, 1, bad, sizeof bad, &l, &err) == PLAT_OWNER_OK && l && plat_lib_sym(l, "checked_dll_ok"),
          "a protected DLL loads: %s %s", bad, err ? err : "");
    if (l) FreeLibrary((HMODULE)(void *)l);                 /* a mapped image can't be opened for writing below */
    l = NULL;
    CHECK(plat_lib_open_checked(file, 2, 0, bad, sizeof bad, &l, &err) == PLAT_OWNER_FOLDER && !l, "not without accept_self (top-down: the top parent)");
    /* someone holding the file for writing, or a parent for deleting: a sharing violation, refused */
    /* x64 under emulation on ARM64 keeps the image open for about a second after FreeLibrary
     * (measured in the VM: a sharing violation, then the open works): retried for up to 3 s */
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 30 && h == INVALID_HANDLE_VALUE; i++) {
        h = CreateFileW(wdst, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_SHARING_VIOLATION) break;
        if (h == INVALID_HANDLE_VALUE) Sleep(100);
    }
    CHECK(h != INVALID_HANDLE_VALUE, "hold the DLL for writing (error %lu)", h == INVALID_HANDLE_VALUE ? GetLastError() : 0);
    l = NULL;
    CHECK(plat_lib_open_checked(file, 2, 1, bad, sizeof bad, &l, &err) == PLAT_OWNER_FILE && !l && !strcmp(bad, file),
          "the DLL held for writing: %s", bad);
    CloseHandle(h);
    MultiByteToWideChar(CP_UTF8, 0, p2, -1, w2, 1100);
    h = CreateFileW(w2, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "hold the folder for deleting");
    CHECK(plat_lib_open_checked(file, 2, 1, bad, sizeof bad, &l, &err) == PLAT_OWNER_FOLDER && !strcmp(bad, p2), "the folder held: %s", bad);
    CloseHandle(h);
    CHECK(plat_lib_open_checked(file, 2, 1, bad, sizeof bad, &l, &err) == PLAT_OWNER_OK && l, "released: loads again");
    if (l) FreeLibrary((HMODULE)(void *)l);
    /* the rule's other refusals, as plat_owner_check's */
    tree("load-users", file, p1, p2);
    MultiByteToWideChar(CP_UTF8, 0, file, -1, wdst, 1100);
    CopyFileW(wsrc, wdst, FALSE);
    CHECK(users && set_acl(p1, ACL_BASE, users, FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE, 0, 0), "Users:(M) on p1");
    CHECK(plat_lib_open_checked(file, 2, 1, bad, sizeof bad, &l, &err) == PLAT_OWNER_FOLDER && !strcmp(bad, p1), "Users:(M) on p1: %s", bad);
    tree("load-empty", file, p1, p2);                       /* not a DLL: checked, then the loader refuses it */
    l = (plat_lib *)(void *)1;
    CHECK(plat_lib_open_checked(file, 2, 1, bad, sizeof bad, &l, &err) == PLAT_OWNER_OK && !l && err && *err, "an empty file: %s",
          err ? err : "");
    /* not an image at all (no MZ): the loader's hard error (a modal "Bad Image" box, which would
     * hang this run: main turns the process's error mode off, as rekordbox's is) is failed
     * instead, and the error mode is the caller's again afterwards */
    tree("load-garbage", file, p1, p2);
    FILE *g = plat_fopen_w(file, 0);
    if (g) {
        for (int i = 0; i < 4096; i++) fputc(i % 251, g);
        fclose(g);
    }
    UINT mode_before = GetThreadErrorMode();
    l = (plat_lib *)(void *)1;
    CHECK(plat_lib_open_checked(file, 2, 1, bad, sizeof bad, &l, &err) == PLAT_OWNER_OK && !l && err && *err, "not an image, no box: %s",
          err ? err : "");
    CHECK(GetThreadErrorMode() == mode_before, "the thread's error mode given back");
    snprintf(file, sizeof file, "%s\\prot\\none\\p1\\x.dll", root);
    CHECK(plat_lib_open_checked(file, 2, 1, bad, sizeof bad, &l, &err) == PLAT_OWNER_MISSING, "a missing folder");
    CHECK(plat_lib_open_checked("relative.dll", 2, 1, bad, sizeof bad, &l, &err) == PLAT_OWNER_REFUSED, "not absolute");
    /* System32: the real rule, without accept_self */
    char k32[MAX_PATH * 3];
    GetSystemDirectoryW(w2, MAX_PATH);
    WideCharToMultiByte(CP_UTF8, 0, w2, -1, k32, sizeof k32, NULL, NULL);
    strcat(k32, "\\kernel32.dll");
    CHECK(plat_lib_open_checked(k32, 2, 0, bad, sizeof bad, &l, &err) == PLAT_OWNER_OK && l && plat_lib_sym(l, "GetTickCount"),
          "kernel32.dll: %s", bad);

    /* plat_owner_hold (review NT1, the admin worker's elevation): held until released; a reader
     * shares with it, a writer or a deleter doesn't */
    tree("hold", file, p1, p2);
    MultiByteToWideChar(CP_UTF8, 0, file, -1, wdst, 1100);
    CHECK(CopyFileW(wsrc, wdst, FALSE), "copy %s", src);
    SetFileAttributesW(wdst, FILE_ATTRIBUTE_NORMAL);
    plat_owner_held *held = (plat_owner_held *)(void *)1;
    CHECK(plat_owner_hold(file, 2, 0, bad, sizeof bad, &held) == PLAT_OWNER_FOLDER && !held, "refused: nothing held");
    CHECK(plat_owner_hold(file, 2, 1, bad, sizeof bad, &held) == PLAT_OWNER_OK && held, "held: %s", bad);
    h = CreateFileW(wdst, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "while held: no writer");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    MultiByteToWideChar(CP_UTF8, 0, p2, -1, w2, 1100);
    h = CreateFileW(w2, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    CHECK(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "while held: its folder can't be deleted");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    char *rb = NULL;
    size_t rn = 0;
    CHECK(!plat_read_file(file, 1 << 24, &rb, &rn) && rn > 0, "while held: read (the admin client's hash again)");
    free(rb);
    l = NULL;
    CHECK((l = plat_lib_open(file, &err)) != NULL, "while held: the loader's read/execute open shares with it");
    if (l) FreeLibrary((HMODULE)(void *)l);
    plat_owner_release(held);
    plat_owner_release(NULL);
    h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 30 && h == INVALID_HANDLE_VALUE; i++) {   /* the emulated image's linger, as above */
        h = CreateFileW(wdst, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_SHARING_VIOLATION) break;
        if (h == INVALID_HANDLE_VALUE) Sleep(100);
    }
    CHECK(h != INVALID_HANDLE_VALUE, "released: a writer again");
    /* a writer kept from before: the hold is refused, nothing held */
    held = (plat_owner_held *)(void *)1;
    CHECK(plat_owner_hold(file, 2, 1, bad, sizeof bad, &held) == PLAT_OWNER_FILE && !held && !strcmp(bad, file), "a kept writer: refused (%s)", bad);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void t_system(void) {
    plat_volume v;
    CHECK(plat_volume_info(root, &v) == 0 && !strncmp(v.mount, "\\\\?\\Volume{", 11) && v.local && v.total > 0, "volume_info %s %s",
          v.mount, v.fstype);
    char mount[PLAT_MOUNT_MAX];
    int sys = 0;
    CHECK(plat_volume_holding(root, mount, sizeof mount, &sys) == 0 && !strcmp(mount, v.mount), "holding: %s", mount);
    CHECK(sys == (toupper((unsigned char)root[0]) == 'C'), "the system volume");
    CHECK(plat_volume_mounted(v.mount) && !plat_volume_mounted("\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\"), "mounted");
    const char *how;
    CHECK(plat_free_bytes(root, 1, &how) > 0, "free bytes");
    CHECK(plat_pid() == (int)GetCurrentProcessId(), "pid");
    int64_t st = plat_process_start(), now = plat_now_wall_s();
    CHECK(st > 0 && st <= now && now - st < 3600, "process start %lld", (long long)st);
    char home[1024];
    CHECK(plat_home(home, sizeof home) == 0 && strstr(home, "\\AppData\\Local"), "home: %s", home);
    /* Program Files (win_rules.h's rule): what the shell's FOLDERID_ProgramFiles names, on whichever
     * drive Windows is; RBSP_TEST_PROGRAMS through the same rule in this test build */
    char pf[400], kf[400] = "";
    PWSTR kw = NULL;
    static const GUID programs = {0x905e63b6, 0xc1bf, 0x494e, {0xb2, 0x9c, 0x65, 0xb7, 0x32, 0xd3, 0xd2, 0x1a}};   /* FOLDERID_ProgramFiles */
    if (SHGetKnownFolderPath(&programs, KF_FLAG_DEFAULT, NULL, &kw) == S_OK)
        WideCharToMultiByte(CP_UTF8, 0, kw, -1, kf, sizeof kf, NULL, NULL);
    CoTaskMemFree(kw);
    SetEnvironmentVariableW(L"RBSP_TEST_PROGRAMS", NULL);
    CHECK(plat_program_files(NULL, pf, sizeof pf) == 0 && kf[0] && !_stricmp(pf, kf), "Program Files: %s (the shell: %s)", pf, kf);
    CHECK(plat_program_files(WR_PF_REAL_ORT, pf, sizeof pf) == 0 && strstr(pf, ":\\") && strstr(pf, "\\RB Stems Plus\\ort\\onnxruntime.dll"),
          "the real ORT: %s", pf);
    CHECK(plat_program_files(WR_PF_ROOT, pf, 8) == -1 && !pf[0], "too small: refused");
    SetEnvironmentVariableW(L"RBSP_TEST_PROGRAMS", L"D:\\Fake Programs\\");
    CHECK(plat_program_files(WR_PF_REKORDBOX, pf, sizeof pf) == 0 && !strcmp(pf, "D:\\Fake Programs\\rekordbox"), "the test override: %s", pf);
    SetEnvironmentVariableW(L"RBSP_TEST_PROGRAMS", L"%SystemDrive%\\Program Files");
    CHECK(plat_program_files(NULL, pf, sizeof pf) == -1 && !pf[0], "a refused override fails closed");
    SetEnvironmentVariableW(L"RBSP_TEST_PROGRAMS", NULL);
    char bad[1024];
    char k32[MAX_PATH * 3];
    wchar_t w[MAX_PATH];
    GetSystemDirectoryW(w, MAX_PATH);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, k32, sizeof k32, NULL, NULL);
    strcat(k32, "\\kernel32.dll");
    CHECK(plat_owner_check(k32, 2, 0, bad, sizeof bad) == PLAT_OWNER_OK, "System32 is admin-only: %s", bad);
    char p[1100];
    t_owner();
    path(p, sizeof p, "none.dll");
    CHECK(plat_owner_check(p, 2, 0, bad, sizeof bad) == PLAT_OWNER_MISSING, "missing");
    CHECK(plat_owner_check("relative.dll", 2, 0, bad, sizeof bad) == PLAT_OWNER_REFUSED, "not absolute");
    const char *err = NULL;
    plat_lib *l = plat_lib_open(k32, &err);
    CHECK(l && plat_lib_sym(l, "GetTickCount") && !plat_lib_sym(l, "NoSuchExport"), "lib_open");
    CHECK(plat_lib_open("C:\\nothing\\x.dll", &err) == NULL && err && *err, "lib_open fails: %s", err ? err : "");
    const char *me = plat_lib_path_of((const void *)t_system);
    CHECK(me && strstr(me, "plat-win-test"), "path_of: %s", me ? me : "NULL");
}

/* plat_remove_tree (T3b): a tree with a read-only file and a junction to a folder outside it; the
 * junction goes as itself, the folder it points to stays; a file held open without delete sharing
 * gives EBUSY and the rest goes */
static void put(const char *p, const char *s) {
    FILE *f = plat_fopen_w(p, 0);
    if (f) { fputs(s, f); fclose(f); }
}
static void t_remove_tree(void) {
    char t[1100], p[1300], out[1100], cmd[2700];
    plat_stat st;
    path(t, sizeof t, "tree");
    path(out, sizeof out, "outside");
    snprintf(p, sizeof p, "%s\\a\\b", t);
    plat_mkdirs(p);
    plat_mkdirs(out);
    snprintf(p, sizeof p, "%s\\a\\b\\x.dll", t);
    put(p, "x");
    snprintf(p, sizeof p, "%s\\ro.txt", t);
    put(p, "ro");
    wchar_t w[1300];
    MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1300);
    SetFileAttributesW(w, FILE_ATTRIBUTE_READONLY);
    snprintf(p, sizeof p, "%s\\keep.txt", out);
    put(p, "keep");
    snprintf(cmd, sizeof cmd, "mklink /J \"%s\\jn\" \"%s\" >NUL", t, out);
    CHECK(run_cmd(cmd) == 0, "mklink /J in the tree");
    CHECK(plat_remove_tree(t) == 0, "the tree removed");
    CHECK(plat_stat_path(t, 0, &st) == ENOENT, "gone");
    CHECK(plat_stat_path(p, 0, &st) == 0 && st.kind == PLAT_FILE, "the junction's target kept, with its file");
    CHECK(plat_remove_tree(t) == ENOENT, "again: ENOENT");
    /* a file held open without FILE_SHARE_DELETE */
    snprintf(p, sizeof p, "%s\\held", t);
    plat_mkdirs(p);
    snprintf(p, sizeof p, "%s\\held\\h.bin", t);
    put(p, "h");
    snprintf(cmd, sizeof cmd, "%s\\other.bin", t);
    put(cmd, "o");
    MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1300);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "held open");
    CHECK(plat_remove_tree(t) == EBUSY, "a held file: EBUSY");
    CHECK(plat_stat_path(cmd, 0, &st) == ENOENT && plat_stat_path(p, 0, &st) == 0, "the rest gone, the held file kept");
    CloseHandle(h);
    CHECK(plat_remove_tree(t) == 0 && plat_stat_path(t, 0, &st) == ENOENT, "once closed, all of it");
    /* a file held for writing, sharing write and delete but not read: no data access is asked, it goes */
    snprintf(p, sizeof p, "%s\\w", t);
    plat_mkdirs(p);
    snprintf(p, sizeof p, "%s\\w\\noread.bin", t);
    put(p, "w");
    MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1300);
    h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "held for writing, read not shared");
    CHECK(plat_remove_tree(t) == 0 && plat_stat_path(t, 0, &st) == ENOENT, "a file held without read sharing: deleted (POSIX)");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    plat_dir d;
    CHECK(plat_dir_open(root, &d) == 0 && plat_remove_tree_at(d, "outside") == 0 && plat_remove_tree_at(d, "..") == EINVAL, "_at");
    plat_dir_close(d);
}

/* plat_remove_empty_dir_at (review MT1): only the folder expected, only empty, never followed; in
 * a folder of its own, run in fat mode too (without the junction, MQ4) */
static void t_remove_empty_dir(void) {
    char t[1100], p[1300], cmd[2700];
    plat_stat st, es;
    plat_dir e, sub;
    path(t, sizeof t, "empty-dirs");
    plat_mkdirs(t);
    snprintf(p, sizeof p, "%s\\empty-one", t);
    plat_mkdirs(p);
    CHECK(plat_dir_open(t, &e) == 0, "empty dir: the parent opened");
    CHECK(plat_dir_open_at(e, "empty-one", &sub) == 0 && plat_dir_stat(sub, &es) == 0, "empty dir: its id taken");
    plat_dir_close(sub);
    snprintf(cmd, sizeof cmd, "%s\\empty-one\\late.txt", t);
    put(cmd, "late");
    CHECK(plat_remove_empty_dir_at(e, "empty-one", es.id) == ENOTEMPTY && plat_stat_path(cmd, 0, &st) == 0, "not empty: ENOTEMPTY, kept");
    DeleteFileA(cmd);
    plat_id other = es.id;
    other.file[1] ^= 1;
    other.file[0] ^= 1;   /* FAT's id has no second half */
    CHECK(plat_remove_empty_dir_at(e, "empty-one", other) == ESTALE && plat_stat_path(p, 0, &st) == 0, "another folder: ESTALE, kept");
    if (!fat_only) {
        snprintf(cmd, sizeof cmd, "mklink /J \"%s\\empty-jn\" \"%s\" >NUL", t, p);
        CHECK(run_cmd(cmd) == 0, "empty dir: a junction to it");
        CHECK(plat_remove_empty_dir_at(e, "empty-jn", es.id) == ELOOP && plat_stat_path(p, 0, &st) == 0, "a junction: ELOOP, its target kept");
    }
    snprintf(cmd, sizeof cmd, "%s\\empty-file", t);
    put(cmd, "f");
    CHECK(plat_remove_empty_dir_at(e, "empty-file", es.id) == ENOTDIR, "a file: ENOTDIR");
    CHECK(plat_remove_empty_dir_at(e, "empty-one", es.id) == 0 && plat_stat_path(p, 0, &st) == ENOENT, "the one expected, empty: gone");
    CHECK(plat_remove_empty_dir_at(e, "empty-one", es.id) == ENOENT, "again: ENOENT");
    if (!fat_only) {
        /* a folder already delete-pending under someone's handle: an error, no crash; gone once closed */
        snprintf(p, sizeof p, "%s\\empty-dp", t);
        plat_mkdirs(p);
        CHECK(plat_dir_open_at(e, "empty-dp", &sub) == 0 && plat_dir_stat(sub, &es) == 0, "delete-pending: its id taken");
        plat_dir_close(sub);
        wchar_t w[1300];
        MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1300);
        HANDLE h = CreateFileW(w, DELETE, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);
        FILE_DISPOSITION_INFO di = {TRUE};
        CHECK(h != INVALID_HANDLE_VALUE && SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof di), "delete-pending: set");
        int r = plat_remove_empty_dir_at(e, "empty-dp", es.id);
        CHECK(r != 0, "delete-pending: an error (%d)", r);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        CHECK(plat_stat_path(p, 0, &st) == ENOENT, "delete-pending: gone once the handle closed");
    }
    plat_dir_close(e);
}

static int count_each(void *ctx, const char *name, const plat_stat *st) {
    (void)name;
    (void)st;
    ++*(int *)ctx;
    return 0;
}

/* folders a user may read but not traverse: WER's ReportArchive grants Authenticated Users
 * (OI)(CI)(R,W,D), no execute (seen in the VM, round 3). plat_dir_open, _open_at, plat_stat_at and
 * plat_dir_each read them as PowerShell does; a folder opened for traversal was refused (EACCES) */
static int set_rwd(const char *p8, DWORD mask) {
    wchar_t w[1100];
    union { ACL acl; unsigned char b[256]; } buf;
    PSID me = self_sid();
    if (!me || !MultiByteToWideChar(CP_UTF8, 0, p8, -1, w, 1100) || !InitializeAcl(&buf.acl, sizeof buf, ACL_REVISION)
        || !AddAccessAllowedAceEx(&buf.acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, mask, me))
        return 0;
    return SetNamedSecurityInfoW(w, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, &buf.acl, NULL)
           == ERROR_SUCCESS;
}
static void t_no_traverse(void) {
    char t[1100], p[1300];
    path(t, sizeof t, "rwd");
    snprintf(p, sizeof p, "%s\\AppCrash_rekordbox.exe_1", t);
    plat_mkdirs(p);
    snprintf(p, sizeof p, "%s\\AppCrash_rekordbox.exe_1\\Report.wer", t);
    put(p, "Version=1\r\n");
    CHECK(set_rwd(t, FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE), "WER's (OI)(CI)(R,W,D) on the folder");
    plat_dir d = {0}, sub = {0};
    plat_stat st;
    int n = 0;
    CHECK(plat_dir_open(t, &d) == 0, "a folder without traverse access opens");
    CHECK(plat_dir_ok(d) && plat_dir_each(d, count_each, &n) == 0 && n == 1, "and lists: %d", n);
    CHECK(plat_dir_ok(d) && plat_dir_open_at(d, "AppCrash_rekordbox.exe_1", &sub) == 0, "its subfolder opens");
    CHECK(plat_dir_ok(sub) && plat_stat_at(sub, "Report.wer", &st) == 0 && st.kind == PLAT_FILE && st.size == 11, "its file stats");
    /* as report_wer_text reads it: the subfolder by its absolute path, then its file */
    snprintf(p, sizeof p, "%s\\AppCrash_rekordbox.exe_1", t);
    plat_dir a = {0};
    plat_file f = {0};
    int64_t sz = 0;
    char b[16] = {0};
    CHECK(plat_dir_open(p, &a) == 0, "its subfolder opens by path");
    CHECK(plat_dir_ok(a) && plat_file_open_at(a, "Report.wer", INT64_MAX, &sz, &f) == 0 && sz == 11, "its file opens");
    CHECK(plat_file_ok(f) && plat_read_all(f, b, 11) == 0 && !memcmp(b, "Version=1\r\n", 11), "and reads");
    plat_file_close(f);
    plat_dir_close(a);
    plat_dir_close(sub);
    plat_dir_close(d);
    CHECK(set_rwd(t, FILE_ALL_ACCESS) && plat_remove_tree(t) == 0, "full access again, removed");
}

/* plat_map_flush's retry of FlushViewOfFile's ERROR_LOCK_VIOLATION (review WF3: 10 tries, about
 * 0.3 s in all, then EBUSY), two ways:
 *  - t_map_flush_lock, a probe: another handle's exclusive LockFileEx on the mapped range. Windows
 *    flushes a dirty view under it anyway (VM round 8, 26300 on ARM64, x64 and arm64: paging
 *    writes bypass byte-range locks), which is printed as a note; were it ever to fail with
 *    ERROR_LOCK_VIOLATION, the lock released by a thread after 20 ms must be waited out and one
 *    never released must end with EBUSY within the budget.
 *  - t_map_flush_retry, the proof: plat_win.c's test-only fault (PLAT_TEST_FLUSH) makes the next N
 *    flushes report ERROR_LOCK_VIOLATION, and records the loop's sleeps: plat_map_flush's own loop
 *    retries them with exactly its schedule, and another error (ERROR_ACCESS_DENIED) not at all.
 * Each flush is of a view written again (a clean view has nothing to write). */
enum { FL_LEN = 65536 };

static double fl_ms(LARGE_INTEGER a, LARGE_INTEGER b) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
}

static DWORD WINAPI fl_unlock_later(void *h) {
    Sleep(20);
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    UnlockFileEx((HANDLE)h, 0, FL_LEN, 0, &ov);
    return 0;
}

static int fl_lock(HANDLE h) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    return LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, FL_LEN, 0, &ov) ? 0 : (int)GetLastError();
}

static void t_map_flush_lock(void) {
    plat_dir d;
    plat_file f;
    void *map = NULL;
    char p[1100];
    wchar_t w[1100];
    path(p, sizeof p, "flushlock.bin");
    CHECK(plat_dir_open(root, &d) == 0, "flush lock: the scratch folder");
    CHECK(plat_file_create_at(d, "flushlock.bin", 1, &f) == 0, "flush lock: create");
    CHECK(plat_map_rw(f, FL_LEN, &map) == 0 && map, "flush lock: map");
    HANDLE h = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, w, 1100)
                   ? CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                   : INVALID_HANDLE_VALUE;
    CHECK(h != INVALID_HANDLE_VALUE, "flush lock: a second handle (%lu)", GetLastError());
    if (map && h != INVALID_HANDLE_VALUE) {
        /* whether a dirty view under the other handle's lock flushes: it does (a note); any error
         * but ERROR_LOCK_VIOLATION is a failure */
        int le = fl_lock(h);
        CHECK(le == 0, "flush lock: LockFileEx (%d)", le);
        memset(map, 1, FL_LEN);
        BOOL ok = FlushViewOfFile(map, FL_LEN);
        DWORD g = ok ? 0 : GetLastError();
        if (ok) printf("note: flush lock: FlushViewOfFile flushes under another handle's LockFileEx; the retry is proven by t_map_flush_retry\n");
        else CHECK(g == WR_ERROR_LOCK_VIOLATION, "flush lock: FlushViewOfFile under the lock fails with %lu, not ERROR_LOCK_VIOLATION", g);
        if (!ok && g == WR_ERROR_LOCK_VIOLATION) {
            /* released after 20 ms: plat_map_flush waits it out and succeeds */
            memset(map, 2, FL_LEN);
            LARGE_INTEGER t0, t1;
            HANDLE th = CreateThread(NULL, 0, fl_unlock_later, h, 0, NULL);
            CHECK(th != NULL, "flush lock: the unlocking thread");
            QueryPerformanceCounter(&t0);
            int e = plat_map_flush(map, FL_LEN);
            QueryPerformanceCounter(&t1);
            double ms = fl_ms(t0, t1);
            if (th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); }
            CHECK(e == 0 && ms >= 15.0, "flush lock: released after 20 ms, the flush is retried and succeeds (%d after %.1f ms)", e, ms);
            printf("flush lock: released after 20 ms, flushed after %.1f ms\n", ms);
            /* never released: EBUSY, within the budget (10 tries, about 0.3 s, Sleep's granularity on top) */
            le = fl_lock(h);
            CHECK(le == 0, "flush lock: LockFileEx again (%d)", le);
            memset(map, 3, FL_LEN);
            QueryPerformanceCounter(&t0);
            e = plat_map_flush(map, FL_LEN);
            QueryPerformanceCounter(&t1);
            ms = fl_ms(t0, t1);
            CHECK(e == EBUSY && ms >= 100.0 && ms < 1500.0, "flush lock: never released, EBUSY after the tries (%d after %.1f ms)", e, ms);
            printf("flush lock: never released, %d after %.1f ms\n", e, ms);
        }
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        UnlockFileEx(h, 0, FL_LEN, 0, &ov);
        CHECK(plat_map_flush(map, FL_LEN) == 0, "flush lock: unlocked, the flush succeeds");
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    plat_unmap(map, FL_LEN);
    plat_file_close(f);
    CHECK(plat_unlink_at(d, "flushlock.bin") == 0, "flush lock: removed");
    plat_dir_close(d);
}

/* the sleeps plat_map_flush took in the last fl_flush_faulted (ms each, in order) */
static unsigned fl_sleep[16];
static int fl_nsleep;

/* plat_map_flush(map) with the next n flushes failing with err (0: ERROR_LOCK_VIOLATION): its
 * result, the time it took (ms), the faults left (how many of the n its loop didn't use), and its
 * sleeps in fl_sleep/fl_nsleep */
static int fl_flush_faulted(void *map, int n, unsigned err, double *ms, int *left) {
    LARGE_INTEGER t0, t1;
    memset(map, n & 0xff, FL_LEN);
    plat_test_flush_faults(n, err);
    QueryPerformanceCounter(&t0);
    int e = plat_map_flush(map, FL_LEN);
    QueryPerformanceCounter(&t1);
    *ms = fl_ms(t0, t1);
    *left = plat_test_flush_faults_left();
    const unsigned *s;
    fl_nsleep = plat_test_flush_sleeps(&s);
    for (int i = 0; i < fl_nsleep && i < 16; i++) fl_sleep[i] = s[i];
    plat_test_flush_faults(0, 0);
    return e;
}

/* the last fl_flush_faulted slept exactly want[0..n) ms, in that order */
static int fl_sleeps_are(const unsigned *want, int n) {
    if (fl_nsleep != n) return 0;
    for (int i = 0; i < n; i++)
        if (fl_sleep[i] != want[i]) return 0;
    return 1;
}

/* the sleeps as text, for a failed check's message */
static const char *fl_sleeps_text(void) {
    static char t[200];
    size_t k = 0;
    t[0] = 0;
    for (int i = 0; i < fl_nsleep && i < 16 && k < sizeof t - 12; i++) k += (size_t)snprintf(t + k, sizeof t - k, "%s%u", i ? "," : "", fl_sleep[i]);
    return t;
}

static void t_map_flush_retry(void) {
    plat_dir d;
    plat_file f;
    void *map = NULL;
    CHECK(plat_dir_open(root, &d) == 0, "flush retry: the scratch folder");
    CHECK(plat_file_create_at(d, "flushretry.bin", 1, &f) == 0, "flush retry: create");
    CHECK(plat_map_rw(f, FL_LEN, &map) == 0 && map, "flush retry: map");
    if (map) {
        double ms;
        int left, e;
        static const unsigned three[] = {1, 2, 4}, all[] = {1, 2, 4, 8, 16, 32, 64, 64, 64};
        /* none: flushed at once, no sleep */
        e = fl_flush_faulted(map, 0, 0, &ms, &left);
        CHECK(e == 0 && left == 0 && fl_sleeps_are(NULL, 0), "flush retry: no fault, flushed at once (%d, sleeps %s)", e, fl_sleeps_text());
        printf("flush retry: N=0, %d after %.1f ms\n", e, ms);
        /* three: tried four times (all three faults used), sleeping 1, 2 and 4 ms between */
        e = fl_flush_faulted(map, 3, 0, &ms, &left);
        CHECK(e == 0 && left == 0 && fl_sleeps_are(three, 3), "flush retry: 3 faults, retried and flushed (%d after %.1f ms, %d left, sleeps %s)", e, ms,
              left, fl_sleeps_text());
        printf("flush retry: N=3, %d after %.1f ms, sleeps %s\n", e, ms, fl_sleeps_text());
        /* more than the budget: EBUSY after exactly 10 tries (10 of the 25 faults used), the nine
         * sleeps between them and none after the last; at least their 255 ms waited (Sleep ran) */
        e = fl_flush_faulted(map, 25, 0, &ms, &left);
        CHECK(e == EBUSY && left == 15 && fl_sleeps_are(all, 9) && ms >= 230.0,
              "flush retry: 25 faults, EBUSY after 10 tries (%d after %.1f ms, %d left, sleeps %s)", e, ms, left, fl_sleeps_text());
        printf("flush retry: N=25, %d after %.1f ms, %d faults left, sleeps %s\n", e, ms, left, fl_sleeps_text());
        /* any other error is not tried again: EACCES at once, 4 of the 5 faults left, no sleep */
        e = fl_flush_faulted(map, 5, WR_ERROR_ACCESS_DENIED, &ms, &left);
        CHECK(e == EACCES && left == 4 && fl_sleeps_are(NULL, 0), "flush retry: ERROR_ACCESS_DENIED, EACCES at once (%d, %d left, sleeps %s)", e, left,
              fl_sleeps_text());
        printf("flush retry: ERROR_ACCESS_DENIED x5, %d after %.1f ms, %d faults left\n", e, ms, left);
        /* the faults gone, the view flushes */
        memset(map, 9, FL_LEN);
        CHECK(plat_map_flush(map, FL_LEN) == 0, "flush retry: no fault left, flushed");
    }
    plat_unmap(map, FL_LEN);
    plat_file_close(f);
    CHECK(plat_unlink_at(d, "flushretry.bin") == 0, "flush retry: removed");
    plat_dir_close(d);
}

/* the follow-link opens (plat_stat_path with follow, plat_file_open_path) take a path as Win32
 * reads it: relative, dotted, UNC (B14, BW11); the no-follow ones keep the strict rule (EINVAL) */
static void t_full_paths(void) {
    char p[1100], q[1300];
    plat_stat st, id;
    plat_file f;
    path(p, sizeof p, "fp");
    plat_mkdirs(p);
    path(p, sizeof p, "fp\\m.onnx");
    put(p, "model");
    CHECK(plat_stat_path(p, 1, &id) == 0 && id.kind == PLAT_FILE && id.size == 5, "the absolute path");
    /* relative to the current folder, which the test sets to SCRATCH and puts back */
    wchar_t was[1100], wr[1100];
    DWORD wl = GetCurrentDirectoryW(1100, was);
    CHECK(wl > 0 && wl < 1100 && MultiByteToWideChar(CP_UTF8, 0, root, -1, wr, 1100) && SetCurrentDirectoryW(wr), "cd SCRATCH");
    CHECK(plat_stat_path("fp\\m.onnx", 1, &st) == 0 && plat_id_same(st.id, id.id), "relative: stat");
    CHECK(plat_stat_path("fp/m.onnx", 1, &st) == 0 && plat_id_same(st.id, id.id), "relative with '/'");
    CHECK(plat_stat_path(".\\fp\\..\\fp\\m.onnx", 1, &st) == 0 && plat_id_same(st.id, id.id), "relative and dotted");
    char b[8] = {0};
    CHECK(plat_file_open_path("fp\\m.onnx", &f) == 0 && plat_read_all(f, b, 5) == 0 && !memcmp(b, "model", 5), "relative: open_path reads it");
    plat_file_close(f);
    CHECK(plat_file_open_path("fp", &f) == EINVAL && plat_file_open_path("fp\\missing", &f) == ENOENT, "relative: a folder, a missing file");
    CHECK(plat_stat_path("fp\\m.onnx", 0, &st) == EINVAL && plat_stat_path(".\\fp", 0, &st) == EINVAL, "no-follow: relative refused");
    CHECK(SetCurrentDirectoryW(was), "cd back");
    /* dotted absolute paths: folded; with "\\?\" kept as they are (not folded) */
    snprintf(q, sizeof q, "%s\\fp\\..\\fp\\.\\m.onnx", root);
    CHECK(plat_stat_path(q, 1, &st) == 0 && plat_id_same(st.id, id.id), "dotted: stat");
    CHECK(plat_file_open_path(q, &f) == 0, "dotted: open_path");
    plat_file_close(f);
    CHECK(plat_stat_path(q, 0, &st) == EINVAL, "no-follow: dotted refused");
    snprintf(q, sizeof q, "%s/fp/../fp/m.onnx", root);
    CHECK(plat_stat_path(q, 1, &st) == 0 && plat_id_same(st.id, id.id), "dotted with '/'");
    snprintf(q, sizeof q, "\\\\?\\%s\\fp\\m.onnx", root);
    CHECK(plat_stat_path(q, 1, &st) == 0 && plat_id_same(st.id, id.id), "\\\\?\\ as it is");
    snprintf(q, sizeof q, "\\\\?\\%s\\fp\\..\\fp\\m.onnx", root);
    CHECK(plat_stat_path(q, 1, &st) != 0, "\\\\?\\ with \"..\": not folded");
    snprintf(q, sizeof q, "\\\\?\\%c:", root[0]);
    CHECK(plat_stat_path(q, 1, &st) == EINVAL && plat_file_open_path(q, &f) == EINVAL, "\\\\?\\X: with no '\\': refused (BX7)");
    /* UNC: the administrative share of SCRATCH's drive, when this account may use it */
    if (isalpha((unsigned char)root[0]) && root[1] == ':' && root[2] == '\\') {
        snprintf(q, sizeof q, "\\\\localhost\\%c$\\%s\\fp\\m.onnx", root[0], root + 3);
        CHECK(plat_stat_path(q, 0, &st) == EINVAL, "no-follow: UNC refused");
        int e = plat_stat_path(q, 1, &st);
        if (e == 0) {
            CHECK(st.kind == PLAT_FILE && st.size == 5, "UNC: stat");
            CHECK(plat_file_open_path(q, &f) == 0, "UNC: open_path");
            plat_file_close(f);
            snprintf(q, sizeof q, "\\\\localhost\\%c$\\%s\\fp\\..\\fp\\m.onnx", root[0], root + 3);
            CHECK(plat_stat_path(q, 1, &st) == 0 && st.size == 5, "UNC and dotted");
        } else printf("UNC: skipped (\\\\localhost\\%c$: errno %d)\n", root[0], e);
    }
    CHECK(plat_stat_path("\\\\.\\C:\\x", 1, &st) == EINVAL && plat_stat_path("", 1, &st) == EINVAL, "a device, an empty path");
    path(p, sizeof p, "fp");
    CHECK(plat_remove_tree(p) == 0, "fp removed");
}

int main(int argc, char **argv) {
    /* SCRATCH from the UTF-16 command line: argv is in the ANSI code page, which loses a CJK
     * profile's path (seen in the VM: a profile named J, u-umlaut, "rgen" and "Tokyo" in kanji
     * arrived with "?" for the kanji) */
    (void)argv;
    int wn = 0;
    wchar_t **wa = CommandLineToArgvW(GetCommandLineW(), &wn);
    if (!wa || argc != wn || !(wn == 2 || (wn == 3 && !wcscmp(wa[2], L"fat")))
        || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wa[1], -1, root, sizeof root, NULL, NULL)) {
        fprintf(stderr, "usage: plat-win-test SCRATCH [fat]\n");
        return 2;
    }
    fat_only = wn == 3;
    LocalFree(wa);
    int rg = redirection_enforced(), elevated = token_elevated();
    redir_guard = rg && !elevated;
    if (rg)
        printf("junction: Redirection Guard enforced, token %s (an sshd session)\n",
               elevated ? "elevated: junctions followed" : "not elevated: junctions refused");
    SetErrorMode(GetErrorMode() & ~(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX));   /* hard errors shown, as in rekordbox; a crash box stays as the shell set it */
    if (fat_only) {                                         /* an exFAT or FAT32 volume: the file layer alone */
        t_files();
        t_remove_empty_dir();
        printf("plat_win (fat): %d checks, %d failed\n", checks, failures);
        return failures ? 1 : 0;
    }
    t_sha256();
    t_irfft();
    t_locks_queues();
    t_fake_clock();
    t_files();
    t_map_flush_lock();
    t_map_flush_retry();
    t_io_hint();
    t_system();
    t_remove_tree();
    t_remove_empty_dir();
    t_no_traverse();
    t_full_paths();
    printf("plat_win: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
