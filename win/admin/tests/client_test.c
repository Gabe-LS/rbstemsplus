/* admin.c and helper_proc.c (W4), the app's side: bin\'s listing, the worker choice (I76, J23),
 * the source folder, a run with a fake elevation (install-worker then "run again": two prompts,
 * I75; the update-first outcomes; UAC declined), the records; the helper's control rules
 * (Pause->Stop, Pause->Quit, Pause->Update, an estimate cancelled; I63) and clean environment. */
#include "../../c/tests/check.h"

#include "../../c/admin.h"
#include "../../c/helper_proc.h"
#include "../../c/native_admin.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

void test_client(const char *scratch);

static char S[1024], ROOT[1100];

static void spits(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    fputs(s, f);
    fclose(f);
}

static const char *P(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static const char *P(const char *fmt, ...) {
    static char ring[8][1400];
    static int at;
    char *o = ring[at++ & 7];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(o, 1400, fmt, ap);
    va_end(ap);
    return o;
}

static const char *sha_of(const char *s) {
    static char o[4][65];
    static int at;
    char *r = o[at++ & 3];
    unsigned char h[32];
    CC_SHA256(s, (CC_LONG)strlen(s), h);
    for (int i = 0; i < 32; i++) snprintf(r + 2 * i, 3, "%02x", h[i]);
    return r;
}

static void sh(const char *cmd) {
    if (system(cmd)) abort();
}

/* a worker in bin\ under its sha12 */
static void put_worker(const char *bytes) {
    char d12[13];
    memcpy(d12, sha_of(bytes), 12);
    d12[12] = 0;
    sh(P("mkdir -p '%s/bin/%s'", ROOT, d12));
    spits(P("%s/bin/%s/RB Stems Plus.exe", ROOT, d12), bytes);
}

static const char W1[] = "MZ one rbsp-admin worker: proto=1 minimum=1.5.0\n";
static const char W1B[] = "MZ one-b rbsp-admin worker: proto=1 minimum=1.6.0\n";
static const char W2[] = "MZ two rbsp-admin worker: proto=2 minimum=1.7.0\n";
static const char WX[] = "MZ no marker";

/* the fake elevation: what each start did, and a script of answers */
typedef struct {
    char exe[8][1200], params[8][3000];
    int n;
    int codes[8];             /* the exit code of each start */
    const char *lines[8];     /* what each start writes on the pipe */
    const char *installs;     /* install-worker puts this worker into bin\ */
    int declined;
    int no_start;             /* ShellExecuteEx failed for another reason: -1, no line */
    int declined_from;        /* > 0: the starts from this one (1-based) are declined */
    int no_started;           /* an elevation that never calls started: run_one releases after it */
    int held_at_started;      /* how many times the hold was still in place when started was called */
} fake_el;
static fake_el E;
static char logged[4096];

/* the order of what happened (NT1): H hold, S started, R release, L a logged line */
static char order[64];
static void ev(char ch) {
    size_t k = strlen(order);
    if (k + 1 < sizeof order) order[k] = ch, order[k + 1] = 0;
}

/* the hold's fake (NA4, NT1): refuses when distrust is set (every bin\ worker, or only the paths
 * that hold distrust_sub); when holding, may first write rewrites into the worker (after the
 * listing hashed it); counts its asks and releases */
static int distrust, holds, releases, held_now, bad_releases;
static const char *distrust_sub, *rewrites;
static char held_last[1200];
static int fake_hold(void *ctx, const char *exe, char *bad, size_t n, void **held) {
    (void)ctx;
    holds++;
    ev('H');
    snprintf(held_last, sizeof held_last, "%s", exe);
    if (distrust && (!distrust_sub || strstr(exe, distrust_sub))) {
        snprintf(bad, n, "%s", exe);
        return 1;
    }
    if (rewrites) spits(exe, rewrites);
    held_now++;
    *held = &held_now;
    return 0;
}
static void fake_release(void *ctx, void *held) {
    (void)ctx;
    releases++;
    ev('R');
    if (held == &held_now && held_now > 0) held_now--;
    else bad_releases++;
}

static int fake_elevate(void *ctx, const char *exe, const char *params, const char *pipe, void (*line)(void *, const char *, size_t), void *lctx,
                        void (*started)(void *), void *sctx, int *code) {
    (void)ctx;
    /* ShellExecuteExW has returned (whatever it did): admin_win.c calls started here */
    if (held_now) E.held_at_started++;
    if (!E.no_started && started) {
        ev('S');
        started(sctx);
    }
    if (!admin_pipe_name_ok(pipe) || !strstr(params, pipe)) return -1;
    if (E.declined) return ADMIN_EXIT_USER_STOPPED;
    if (E.no_start) return -1;
    if (E.declined_from && E.n + 1 >= E.declined_from) return ADMIN_EXIT_USER_STOPPED;
    int k = E.n++;
    snprintf(E.exe[k], sizeof E.exe[k], "%s", exe);
    snprintf(E.params[k], sizeof E.params[k], "%s", params);
    if (E.lines[k]) {
        admin_lines l;
        admin_lines_init(&l);
        admin_lines_feed(&l, E.lines[k], strlen(E.lines[k]), 1, line, lctx);
    }
    if (E.installs && strstr(params, "\"install-worker\"")) put_worker(E.installs);
    *code = E.codes[k];
    return 0;
}
static void fake_log(void *ctx, const char *line, size_t n) {
    (void)ctx;
    ev('L');
    strncat(logged, line, n);
    strcat(logged, "|");
}
static void fake_random(void *ctx, unsigned char out[16]) {
    (void)ctx;
    for (int i = 0; i < 16; i++) out[i] = (unsigned char)(i * 7 + 1);
}

static admin_client client(const char *own) {
    memset(&E, 0, sizeof E);
    logged[0] = order[0] = 0;
    distrust = holds = releases = held_now = bad_releases = 0;
    distrust_sub = rewrites = NULL;
    admin_client c = {ROOT, P("%s/app/admin/RB Stems Plus.exe", S), P("%s/source", S), own, fake_elevate, NULL, fake_log, fake_random, NULL, NULL};
    return c;
}
/* the same with the hold's fake */
static admin_client held_client(const char *own) {
    admin_client c = client(own);
    c.hold = fake_hold;
    c.release = fake_release;
    return c;
}

static void t_choose(void) {
    admin_bin_worker w[4];
    int i;
    memset(w, 0, sizeof w);
    CHECK(admin_choose(w, 0, sha_of(W1), 1, ADMIN_INSTALL, &i) == ADMIN_PICK_APP_COPY && i == -1, "bin\\ empty: the app folder's copy (I75)");
    snprintf(w[0].sha256, 65, "%s", sha_of(W1));
    w[0].proto = 1;
    snprintf(w[0].minimum, sizeof w[0].minimum, "1.5.0");
    snprintf(w[1].sha256, 65, "%s", sha_of(W2));
    w[1].proto = 2;
    snprintf(w[1].minimum, sizeof w[1].minimum, "1.7.0");
    CHECK(admin_choose(w, 2, sha_of(W1), 1, ADMIN_INSTALL, &i) == ADMIN_PICK_OWN && i == 0, "the app's own worker, named by its installed-release");
    CHECK(admin_choose(w, 2, sha_of(W1B), 1, ADMIN_UNINSTALL, &i) == ADMIN_PICK_MATCHING && i == 0, "uninstall: the newest worker of this protocol");
    CHECK(admin_choose(w, 2, sha_of(W1B), 1, ADMIN_INSTALL, &i) == ADMIN_PICK_INSTALL_OWN && i == 0, "install without its own: install-worker first, a matching one");
    CHECK(admin_choose(w + 1, 1, sha_of(W1B), 1, ADMIN_RESTORE, &i) == ADMIN_PICK_INSTALL_OWN && i == 0, "no match: install-worker through any (never refused by the protocol)");
    CHECK(admin_choose(w + 1, 1, "", 1, ADMIN_RESTORE, &i) == ADMIN_PICK_UPDATE_FIRST, "no match and no record of its own: update first (J23)");
    CHECK(admin_choose(w, 2, "", 1, ADMIN_INSTALL, &i) == ADMIN_PICK_UPDATE_FIRST, "install without a verified record: update first");
    snprintf(w[2].sha256, 65, "%s", sha_of(W1B));
    w[2].proto = 1;
    snprintf(w[2].minimum, sizeof w[2].minimum, "1.6.0");
    CHECK(admin_choose(w, 3, sha_of("MZ none"), 1, ADMIN_UNINSTALL, &i) == ADMIN_PICK_MATCHING && i == 2, "the newest by compiled minimum");
    CHECK(admin_choose(w, 3, "not a sha", 1, ADMIN_UNINSTALL, &i) == ADMIN_PICK_MATCHING, "a record that isn't a sha256 counts as none");
}

static void t_list(void) {
    sh(P("rm -rf '%s' && mkdir -p '%s'", ROOT, ROOT));
    admin_bin_worker w[8];
    int present = -1;
    CHECK(admin_list_bin(ROOT, w, 8, &present) == 0 && present == 0, "no bin\\: no worker");
    put_worker(W1);
    put_worker(W2);
    put_worker(WX);
    sh(P("mkdir -p '%s/bin/0123456789ab' '%s/bin/0123456789AB' '%s/bin/x'", ROOT, ROOT, ROOT));
    spits(P("%s/bin/0123456789ab/RB Stems Plus.exe", ROOT), "not its own sha");
    spits(P("%s/bin/0123456789AB/RB Stems Plus.exe", ROOT), "upper case");
    char d12[13];
    memcpy(d12, sha_of(W1), 12);
    d12[12] = 0;
    sh(P("mkdir -p '%s/bin/%s.del'", ROOT, d12));
    int n = admin_list_bin(ROOT, w, 8, &present);
    int p1 = 0, p2 = 0, none = 0;
    for (int i = 0; i < n; i++) {
        if (w[i].proto == 1 && !strcmp(w[i].minimum, "1.5.0")) p1++;
        if (w[i].proto == 2) p2++;
        if (w[i].proto == -1) none++;
    }
    CHECK(n == 3 && p1 == 1 && p2 == 1 && none == 1, "three workers: their bytes are their folder's name; protocols from the markers (%d)", n);
    CHECK(present == 4, "four present by name: the three, and the one whose bytes aren't its name (A1: %d)", present);
}

/* A1: a worker that is there but can't be listed, read or verified never sends the app to its
 * own folder's copy; A10: a verb that needs install-worker without a source is "update first" */
static void t_no_fallback(void) {
    admin_result res;
    admin_client c;
    int x;
    /* a damaged worker of the exact name */
    sh(P("rm -rf '%s' && mkdir -p '%s/bin/0123456789ab'", ROOT, ROOT));
    spits(P("%s/bin/0123456789ab/RB Stems Plus.exe", ROOT), "MZ damaged");
    c = client(sha_of(W1));
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && E.n == 0 && strstr(logged, "couldn't check the installed worker"), "a damaged worker: refused, nothing elevated (%d)", x);
    /* a worker folder someone keeps from being looked into */
    sh(P("rm -rf '%s/bin' && mkdir -p '%s/bin'", ROOT, ROOT));
    put_worker(W1);
    chmod(P("%s/bin/%.12s", ROOT, sha_of(W1)), 0);
    c = client(sha_of(W1));
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    chmod(P("%s/bin/%.12s", ROOT, sha_of(W1)), 0755);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && E.n == 0, "a worker folder that can't be opened: refused (%d)", x);
    /* the worker's file that can't be read */
    chmod(P("%s/bin/%.12s/RB Stems Plus.exe", ROOT, sha_of(W1)), 0);
    c = client(sha_of(W1));
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    chmod(P("%s/bin/%.12s/RB Stems Plus.exe", ROOT, sha_of(W1)), 0644);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && E.n == 0, "a worker that can't be read: refused (%d)", x);
    /* bin\ that can't be listed */
    chmod(P("%s/bin", ROOT), 0);
    c = client(sha_of(W1));
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    chmod(P("%s/bin", ROOT), 0755);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && E.n == 0, "bin\\ that can't be listed: refused (%d)", x);
    /* only a renamed *.del: no worker at all, the app folder's copy (J7) */
    sh(P("rm -rf '%s/bin' && mkdir -p '%s/bin/0123456789ab'", ROOT, ROOT));
    spits(P("%s/bin/0123456789ab/RB Stems Plus.exe.1234.del", ROOT), "old");
    c = client(sha_of(W1));
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    CHECK(x == 0 && E.n == 1 && strstr(E.exe[0], "/app/admin/"), "only a *.del: the first install from the app folder (%d)", x);
    /* the app folder's copy isn't the app's own worker */
    spits(P("%s/app/admin/RB Stems Plus.exe", S), "MZ replaced");
    c = client(sha_of(W1));
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    spits(P("%s/app/admin/RB Stems Plus.exe", S), W1);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && E.n == 0, "a replaced app folder's copy: never elevated (%d)", x);

    /* A10: uninstall that needs install-worker (only another protocol's worker), with no source */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    c = client(sha_of(W1B));
    c.source = "";
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_PROTOCOL && res.update_first && E.n == 0, "install-worker needed, no source: update first, nothing elevated (%d)", x);
    /* with the source prepared: install-worker, then its own runs the uninstall */
    c = client(sha_of(W1B));
    E.codes[0] = ADMIN_EXIT_RUN_AGAIN;
    E.installs = W1B;
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(x == 0 && E.n == 2 && strstr(E.params[0], "\"install-worker\"") && strstr(E.params[1], "\"uninstall\"") && strstr(E.params[1], "\"--source\" \"\""),
          "uninstall through install-worker with its source (%d)", x);
    /* NA4: install-worker's runner is a bin\\ worker too: refused, 4, nothing elevated */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    c = held_client(sha_of(W1B));
    distrust = 1;
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && E.n == 0 && holds == 1 && releases == 0 && !res.update_first,
          "an install-worker runner that can't be held: 4, not elevated (%d)", x);
}

static void t_source(void) {
    sh(P("mkdir -p '%s/rel' '%s/app/admin'", S, S));
    spits(P("%s/rel/payload.json", S), "{}");
    spits(P("%s/rel/payload.json.sig", S), "sig");
    spits(P("%s/bridge.dll", S), "bridge");
    spits(P("%s/app/admin/RB Stems Plus.exe", S), W1);
    sh(P("mkdir -p '%s/source' && echo stale > '%s/source/stale'", S, S));
    int e = admin_source_prepare(P("%s/source", S), P("%s/rel", S), P("%s/bridge.dll", S), "rbstemsplus-bridge-win-x64.dll",
                                 P("%s/app/admin/RB Stems Plus.exe", S));
    struct stat st;
    CHECK(!e && !stat(P("%s/source/payload.json", S), &st) && !stat(P("%s/source/payload.json.sig", S), &st)
              && !stat(P("%s/source/rbstemsplus-bridge-win-x64.dll", S), &st) && !stat(P("%s/source/admin/RB Stems Plus.exe", S), &st)
              && stat(P("%s/source/stale", S), &st),
          "the source folder: the pair, the bridge, admin\\RB Stems Plus.exe; nothing stale (%d)", e);
    CHECK(admin_source_prepare(P("%s/source", S), P("%s/rel", S), P("%s/bridge.dll", S), "../x.dll", NULL) == EINVAL, "a bridge name that isn't plain");
    /* the source the runs below use */
    CHECK(!admin_source_prepare(P("%s/source", S), P("%s/rel", S), P("%s/bridge.dll", S), "rbstemsplus-bridge-win-x64.dll",
                                P("%s/app/admin/RB Stems Plus.exe", S)),
          "the source again");
}

static void t_run(void) {
    admin_result res;
    /* the first install: bin\ empty, the app folder's copy */
    sh(P("rm -rf '%s' && mkdir -p '%s'", ROOT, ROOT));
    admin_client c = client(sha_of(W1));
    E.lines[0] = "rekordbox 7.2.19 checked\n@floor 1.5.0\n@bridge 1111111111111111111111111111111111111111111111111111111111111111\n@new-record x\n";
    int x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    CHECK(x == 0 && E.n == 1 && strstr(E.exe[0], "/app/admin/RB Stems Plus.exe") && strstr(E.params[0], "\"install\" \"--proto\" \"1\""),
          "first install from the app folder's copy (%d, %s)", x, E.params[0]);
    CHECK(strstr(E.params[0], P("\"--source\" \"%s/source\"", S)) != NULL, "--source passed to install");
    CHECK(!strcmp(res.floor, "1.5.0") && res.bridge[0] == '1' && !strcmp(logged, "rekordbox 7.2.19 checked|"), "records taken, text logged, unknown records ignored");
    /* its own worker in bin\ */
    put_worker(W1);
    c = client(sha_of(W1));
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(x == 0 && E.n == 1 && strstr(E.exe[0], P("/bin/%.12s/RB Stems Plus.exe", sha_of(W1))) && strstr(E.params[0], "\"--source\" \"\""),
          "its own bin\\ worker; no --source for uninstall");
    /* the app's own worker missing (an update changed it): install-worker, then its own: two prompts */
    c = client(sha_of(W1B));
    E.codes[0] = ADMIN_EXIT_RUN_AGAIN;
    E.installs = W1B;
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    CHECK(x == 0 && E.n == 2 && res.elevations == 2 && strstr(E.params[0], "\"install-worker\"") && strstr(E.exe[1], P("/bin/%.12s/", sha_of(W1B))),
          "install-worker through a bin\\ worker, then the app's own (I75: %d prompts)", res.elevations);
    /* the floor refuses an older app's payload: update first */
    c = client(sha_of("MZ older"));
    E.codes[0] = ADMIN_EXIT_OLDER;
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_OLDER && res.update_first && E.n == 1, "the floor refuses its payload: the update-first offer (J23)");
    /* install-worker said run again but its own still isn't there */
    c = client(sha_of("MZ never"));
    E.codes[0] = ADMIN_EXIT_RUN_AGAIN;
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_PROTOCOL && res.update_first && E.n == 1, "its own still missing after run-again: no loop, update first");
    /* a worker of another protocol answers 12 */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    c = client("");
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_PROTOCOL && res.update_first && E.n == 0, "only another protocol's worker and no record: update first, nothing elevated");
    /* UAC declined */
    c = client(sha_of(W2));
    E.declined = 1;
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_USER_STOPPED && !res.update_first, "UAC declined: the user stopped (1223)");
    /* the worker couldn't be started (NA3): -1 and no line, which native admin returns as 1 */
    c = client(sha_of(W2));
    E.no_start = 1;
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == -1 && res.code == -1 && !res.update_first && native_admin_code(&res) == ADMIN_EXIT_FAILED, "not started: 1, never a silent stop (%d)", x);

    /* NA4: a bin\ worker is elevated only when it is admin-only; refused, 4, nothing elevated,
     * never the app folder's copy instead */
    c = held_client(sha_of(W2));
    distrust = 1;
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && E.n == 0 && holds == 1 && releases == 0 && strstr(held_last, "/bin/") && strstr(logged, "isn't admin-only"),
          "a bin\\ worker that can't be held: 4, not elevated, nothing to release (%d, %d asks)", x, holds);
    CHECK(!strstr(logged, "/app/admin/"), "and the app folder's copy never tried");
    /* held: it runs, after one ask, released once */
    c = held_client(sha_of(W2));
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == 0 && E.n == 1 && holds == 1 && releases == 1 && !bad_releases && !strcmp(E.exe[0], held_last),
          "a held bin\\ worker: asked once, elevated, released once (%d)", x);
    /* the records of an uninstall's reboot branch, and the linger */
    c = client(sha_of(W2));
    E.lines[0] = "@still-closing\nrekordbox is still closing: waiting for it\n@restart\n";
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(x == 0 && res.still_closing && res.restart_pending, "@still-closing and @restart reach the result");
}

/* NT1, NT2: a bin\ worker held from its check through ShellExecuteExW, hashed again under the
 * hold, released once as soon as the elevation has started it (or on the way out) */
static void t_hold(void) {
    admin_result res;
    admin_client c;
    int x;
    /* held: hold, then started with the hold in place, then released, then the worker's lines */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    c = held_client(sha_of(W2));
    E.lines[0] = "restored\n";
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == 0 && E.n == 1 && !strcmp(order, "HSRL") && E.held_at_started == 1 && holds == 1 && releases == 1 && !held_now && !bad_releases,
          "held through the start, released at started, before the lines (%s)", order);
    /* the elevation couldn't start the worker (-1): released once */
    c = held_client(sha_of(W2));
    E.no_start = 1;
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == -1 && !strcmp(order, "HSR") && releases == 1 && !held_now && !bad_releases, "not started: released once (%s)", order);
    /* UAC declined: released once */
    c = held_client(sha_of(W2));
    E.declined = 1;
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_USER_STOPPED && !strcmp(order, "HSR") && releases == 1 && !held_now && !bad_releases, "UAC declined: released once (%s)", order);
    /* an elevation that never calls started: released once, after it */
    c = held_client(sha_of(W2));
    E.no_started = 1;
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == 0 && !strcmp(order, "HR") && releases == 1 && !held_now && !bad_releases, "no started: released on the way out (%s)", order);
    /* the worker rewritten after the listing hashed it (by the time it is held): 4, nothing
     * elevated, released once, never the app folder's copy */
    c = held_client(sha_of(W2));
    rewrites = "MZ two rbsp-admin worker: proto=2 minimum=1.7.0 but not the bytes picked\n";
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && E.n == 0 && holds == 1 && releases == 1 && !held_now && strstr(logged, "changed after it was checked")
              && strstr(logged, P("/bin/%.12s/", sha_of(W2))) && !strstr(logged, "/app/admin/"),
          "rewritten under the hold: 4, not elevated, released once (%d, %s)", x, order);
    /* I75's two prompts, both held: each released once */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    c = held_client(sha_of(W1B));
    E.codes[0] = ADMIN_EXIT_RUN_AGAIN;
    E.installs = W1B;
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    CHECK(x == 0 && E.n == 2 && holds == 2 && releases == 2 && !strcmp(order, "HSRHSR") && E.held_at_started == 2,
          "install-worker, then its own: each held and released (%s)", order);
    /* NT2: the second round refuses only the just-installed own worker: 4, install-worker ran */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    c = held_client(sha_of(W1B));
    E.codes[0] = ADMIN_EXIT_RUN_AGAIN;
    E.installs = W1B;
    static char own_sub[32];
    snprintf(own_sub, sizeof own_sub, "/bin/%.12s/", sha_of(W1B));
    distrust = 1;
    distrust_sub = own_sub;
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && E.n == 1 && strstr(E.params[0], "\"install-worker\"") && holds == 2 && releases == 1
              && strstr(logged, P("isn't admin-only: %s/bin/%.12s/", ROOT, sha_of(W1B))),
          "its own worker refused after install-worker: 4, its path logged (%d, %d asks)", x, holds);
    /* NT2: a matching worker that isn't the app's own, refused: 4, nothing elevated */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W1);
    c = held_client(sha_of(W1B));
    distrust = 1;
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_CHECK_FAILED && E.n == 0 && holds == 1 && strstr(held_last, P("/bin/%.12s/", sha_of(W1))), "a matching worker refused: 4 (%d)", x);
    /* NT2: bin\ empty, a hold that refuses everything: the app folder's copy, never held */
    sh(P("rm -rf '%s/bin'", ROOT));
    c = held_client(sha_of(W1));
    distrust = 1;
    x = admin_client_run(&c, ADMIN_INSTALL, "7.2.19", 0, &res);
    CHECK(x == 0 && E.n == 1 && strstr(E.exe[0], "/app/admin/") && holds == 0 && releases == 0, "the app folder's copy is never held (%d, %d asks)", x, holds);
}

static void t_quote(void) {
    const char *argv[4] = {"C:\\Program Files\\x\\rbsp-prepare.exe", "a b", "tr\"icky\\\"", "end\\"};
    char out[512];
    long n = admin_quote_argv(argv, 4, out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "\"C:\\Program Files\\x\\rbsp-prepare.exe\" \"a b\" \"tr\\\"icky\\\\\\\"\" \"end\\\\\""), "the helper's command line: %s", out);
    CHECK(admin_quote_argv(argv, 4, out, 10) < 0, "too small: -1");
}

static void t_helper(void) {
    helper_state s;
    /* Pause -> Stop: stdin's EOF, then one resume so it sees it */
    helper_state_init(&s, 1);
    CHECK(helper_ops(&s, HELPER_SIG_PAUSE) == HELPER_OP_SUSPEND, "Pause: suspended at once");
    CHECK(helper_ops(&s, HELPER_SIG_PAUSE) == 0, "Pause again: nothing");
    CHECK(helper_ops(&s, HELPER_SIG_STOP) == (HELPER_OP_CLOSE_STDIN | HELPER_OP_RESUME), "Pause -> Stop: EOF, then resumed once");
    CHECK(helper_ops(&s, HELPER_SIG_STOP) == 0 && helper_ops(&s, HELPER_SIG_RESUME) == 0, "then nothing more");
    /* Pause -> Quit and Pause -> Update: the core stops every run the same way */
    helper_state_init(&s, 1);
    helper_ops(&s, HELPER_SIG_PAUSE);
    helper_ops(&s, HELPER_SIG_RESUME);
    CHECK(helper_ops(&s, HELPER_SIG_STOP) == HELPER_OP_CLOSE_STDIN, "Resume -> Stop: EOF only");
    /* an estimate (not a run) cancelled: terminated at once, paused or not (I63) */
    helper_state_init(&s, 0);
    helper_ops(&s, HELPER_SIG_PAUSE);
    CHECK(helper_ops(&s, HELPER_SIG_STOP) == HELPER_OP_TERMINATE, "an estimate cancelled: TerminateJobObject");
    helper_state_init(&s, 1);
    s.ended = 1;
    CHECK(helper_ops(&s, HELPER_SIG_STOP) == 0, "after its exit: nothing");

    uint16_t env[256];
    long n = helper_env_block("C:\\Windows", "C:\\Users\\Jane\\AppData\\Local\\Temp", env, 256);
    char flat[256];
    for (long i = 0; i < n && i < 255; i++) flat[i] = env[i] ? (char)env[i] : '|';
    flat[n > 0 ? n : 0] = 0;
    CHECK(n > 0 && !strcmp(flat, "SystemRoot=C:\\Windows|TEMP=C:\\Users\\Jane\\AppData\\Local\\Temp|TMP=C:\\Users\\Jane\\AppData\\Local\\Temp||"),
          "the clean environment: SystemRoot, TEMP, TMP, sorted, double NUL (%s)", flat);
    CHECK(helper_env_block("C:\\Win=dows", "t", env, 256) < 0 && helper_env_block("C:\\Windows", "t", env, 20) < 0
              && helper_env_block("\xff", "t", env, 256) < 0,
          "'=' in a value, too small, invalid UTF-8: refused");
    /* PF3: the helper's paths under Program Files are the DLL's, whatever the core's argv says,
     * walked with prepare.c's grammar */
    const char *in[] = {"--model", "m", "--ort", "C:\\Program Files\\x\\ort.dll", "--rekordbox", "C:\\guess", "--keep", "a.mp3"};
    const char *av[8];
    const char *ort = "D:\\Program Files\\RB Stems Plus\\ort\\onnxruntime.dll", *rb = "D:\\Program Files\\rekordbox\\rekordbox 7.2.19";
    int has_rb = -1;
    CHECK(!helper_program_args(in, 8, ort, rb, av, &has_rb) && has_rb == 1 && av[3] == ort && av[5] == rb && av[0] == in[0] && av[1] == in[1]
              && av[2] == in[2] && av[4] == in[4] && av[6] == in[6] && av[7] == in[7],
          "--ort and --rekordbox: the DLL's, the rest as the core gave it, has_rb");
    CHECK(!helper_program_args(in, 8, ort, NULL, av, NULL) && !strcmp(av[5], "") && av[3] == ort, "no current rekordbox: \"\", nothing guessed");
#define SAME(v, n) (!helper_program_args(v, n, ort, rb, av, &has_rb) && !memcmp(av, v, sizeof v))
    const char *k1[] = {"--keep", "--ort", "--playlist", "--rekordbox", "--first", "5"};
    CHECK(SAME(k1, 6) && !has_rb, "values that look like the two options are other options' values: unchanged, no has_rb");
    const char *k2[] = {"--model", "m", "a.mp3", "--ort", "x"};
    CHECK(SAME(k2, 5) && !has_rb, "after the first operand nothing is an option: unchanged");
    const char *k3[] = {"--tracks", "--ort"};
    CHECK(SAME(k3, 2) && !has_rb, "--tracks' value \"--ort\": unchanged");
#undef SAME
    const char *last[] = {"--keys", "--rekordbox"};
    CHECK(helper_program_args(last, 2, ort, rb, av, &has_rb) == -1, "--rekordbox with no value: refused");
    const char *none[] = {"--keys", "f"};
    CHECK(!helper_program_args(none, 2, ort, rb, av, &has_rb) && av[0] == none[0] && av[1] == none[1] && !has_rb, "neither option: the same argv");
    /* a run (stdin's EOF stops it) by the same walk */
    const char *r1[] = {"--model", "m", "--keep", "--estimate", "--playlist", "7"};
    const char *r2[] = {"--estimate", "--keep", "s", "--playlist", "7"};
    const char *r3[] = {"--quick", "--keep", "s"};
    const char *r4[] = {"--playlist", "7", "--first", "5"};
    const char *r5[] = {"--model", "m", "x.mp3", "--keep", "s"};
    CHECK(helper_is_run(r1, 6) && !helper_is_run(r2, 5) && !helper_is_run(r3, 3) && !helper_is_run(r4, 4) && !helper_is_run(r5, 5),
          "a run: --keep as an option (a set named \"--estimate\" still a run), never with --estimate or --quick, nothing after an operand");
    /* review PF4: a run naming --rekordbox with no verified current folder isn't started; an
     * estimate is (with ""), and so is a call that doesn't name it */
    CHECK(helper_run_refused(1, "", 1) && helper_run_refused(1, NULL, 1) && !helper_run_refused(1, "rekordbox 7", 1) && !helper_run_refused(1, "", 0)
              && !helper_run_refused(0, "", 1),
          "the run refusal: only a run that names --rekordbox without a verified folder");
}

/* the helper's stdout lines (review NW1): whole past admin_lines' 1,023 bytes, up to the cap */
typedef struct {
    int n, too_long;
    size_t len[8];
    char *s[8];
} got_lines;
static void gl_line(void *ctx, const char *s, size_t len) {
    got_lines *g = ctx;
    if (g->n < 8) {
        g->s[g->n] = malloc(len + 1);
        memcpy(g->s[g->n], s, len);
        g->s[g->n][len] = 0;
        g->len[g->n] = len;
    }
    g->n++;
}
static void gl_too_long(void *ctx) { ((got_lines *)ctx)->too_long++; }
static void gl_free(got_lines *g) {
    for (int i = 0; i < g->n && i < 8; i++) free(g->s[i]);
    memset(g, 0, sizeof *g);
}

static void t_helper_lines(void) {
    got_lines g = {0};
    helper_lines l;
    /* a 1.2 KB estimate record with a non-Latin path, in one read: one line, whole */
    char rec[1400];
    int k = snprintf(rec, sizeof rec, "{\"file\":\"/Users/dj/Music/");
    while (k < 1180) k += snprintf(rec + k, sizeof rec - (size_t)k, "\xe6\x97\xa5\xe6\x9c\xac");
    k += snprintf(rec + k, sizeof rec - (size_t)k, ".mp3\",\"result\":\"ready\"}");
    size_t reclen = (size_t)k;
    rec[k++] = '\n';
    helper_lines_init(&l, 0);
    helper_lines_feed(&l, rec, (size_t)k, 0, gl_line, gl_too_long, &g);
    helper_lines_feed(&l, NULL, 0, 1, gl_line, gl_too_long, &g);
    CHECK(g.n == 1 && g.len[0] == reclen && reclen > 1200 && !memcmp(g.s[0], rec, reclen) && !g.too_long, "a 1.2 KB record: whole (%d lines)", g.n);
    helper_lines_free(&l);
    gl_free(&g);
    /* the same record split across reads, at every byte, with CRLF: one line, the '\r' dropped */
    memcpy(rec + reclen, "\r\n", 2);
    int ok = 1;
    for (size_t cut = 1; cut < reclen + 2; cut += 37) {
        helper_lines_init(&l, 0);
        helper_lines_feed(&l, rec, cut, 0, gl_line, gl_too_long, &g);
        helper_lines_feed(&l, rec + cut, reclen + 2 - cut, 0, gl_line, gl_too_long, &g);
        helper_lines_feed(&l, NULL, 0, 1, gl_line, gl_too_long, &g);
        ok &= g.n == 1 && g.len[0] == reclen && !memcmp(g.s[0], rec, reclen);
        helper_lines_free(&l);
        gl_free(&g);
    }
    CHECK(ok, "a record split across two reads, CRLF: one line without its '\\r'");
    /* CRLF lines, an empty one, one '\r' only dropped, and the last line without '\n' at the end */
    helper_lines_init(&l, 0);
    const char *crlf = "a\r\n\r\nb\r\r\nlast";
    helper_lines_feed(&l, crlf, strlen(crlf), 0, gl_line, gl_too_long, &g);
    CHECK(g.n == 3, "the last line waits for the end (%d)", g.n);
    helper_lines_feed(&l, NULL, 0, 1, gl_line, gl_too_long, &g);
    CHECK(g.n == 4 && !strcmp(g.s[0], "a") && g.len[1] == 0 && !strcmp(g.s[2], "b\r") && !strcmp(g.s[3], "last"),
          "CRLF, an empty line, one '\\r' dropped, the last line at the end");
    helper_lines_free(&l);
    gl_free(&g);
    /* the cap (a small one here; HELPER_LINE_MAX is 4 MiB): exactly max bytes (with CRLF) whole,
     * max + 1 discarded up to its '\n' and reported once, the next line whole again */
    enum { MAX = 1000 };
    char big[3 * MAX];
    memset(big, 'x', sizeof big);
    helper_lines_init(&l, MAX);
    helper_lines_feed(&l, big, MAX, 0, gl_line, gl_too_long, &g);
    helper_lines_feed(&l, "\r\n", 2, 0, gl_line, gl_too_long, &g);
    CHECK(g.n == 1 && g.len[0] == MAX && !g.too_long, "a line of exactly the cap: whole");
    helper_lines_feed(&l, big, MAX + 1, 0, gl_line, gl_too_long, &g);
    helper_lines_feed(&l, "\n", 1, 0, gl_line, gl_too_long, &g);
    helper_lines_feed(&l, big, 2 * MAX, 0, gl_line, gl_too_long, &g);
    helper_lines_feed(&l, big, 2 * MAX, 0, gl_line, gl_too_long, &g);
    helper_lines_feed(&l, "\nok\n", 4, 0, gl_line, gl_too_long, &g);
    CHECK(g.n == 2 && !strcmp(g.s[1], "ok") && g.too_long == 1, "past the cap: discarded to its '\\n', reported once (%d, %d)", g.n, g.too_long);
    helper_lines_feed(&l, big, MAX + 2, 1, gl_line, gl_too_long, &g);
    CHECK(g.n == 2 && g.too_long == 1, "a last line past the cap at the end: discarded");
    CHECK(l.cap <= MAX + 2, "the buffer never grows past the cap (%zu)", l.cap);
    helper_lines_free(&l);
    gl_free(&g);
}

/* stderr's tail (review NW2): the last 4 KiB, from a UTF-8 boundary once cut */
static void t_helper_tail(void) {
    helper_tail t;
    size_t n;
    helper_tail_init(&t);
    const char *s = helper_tail_get(&t, &n);
    CHECK(n == 0 && s, "nothing written: \"\"");
    helper_tail_feed(&t, "error: ", 7);
    helper_tail_feed(&t, NULL, 0);
    helper_tail_feed(&t, "the library can't be read\n", 26);
    s = helper_tail_get(&t, &n);
    CHECK(n == 33 && !memcmp(s, "error: the library can't be read\n", 33), "short writes kept whole");
    /* many small writes past 4 KiB, then one with a 3-byte character cut by the window */
    char chunk[100];
    memset(chunk, 'a', sizeof chunk);
    for (int i = 0; i < 100; i++) helper_tail_feed(&t, chunk, sizeof chunk);
    s = helper_tail_get(&t, &n);
    CHECK(n == HELPER_ERR_TAIL && s[0] == 'a', "past 4 KiB: the last 4 KiB (%zu)", n);
    char big[HELPER_ERR_TAIL + 2];
    memset(big, 'b', sizeof big);
    memcpy(big + 1, "\xe6\x97\xa5", 3);   /* the window starts at byte 2: inside it */
    helper_tail_feed(&t, big, sizeof big);
    s = helper_tail_get(&t, &n);
    CHECK(n == HELPER_ERR_TAIL - 2 && s[0] == 'b' && s[n - 1] == 'b', "one write past 4 KiB: its end, from a whole character (%zu)", n);
    helper_tail_init(&t);
    helper_tail_feed(&t, big, HELPER_ERR_TAIL);
    s = helper_tail_get(&t, &n);
    CHECK(n == HELPER_ERR_TAIL && !t.cut, "exactly 4 KiB: nothing dropped");
    helper_tail_feed(&t, "z", 1);
    s = helper_tail_get(&t, &n);
    CHECK(n == HELPER_ERR_TAIL && (unsigned char)s[0] == 0xe6 && s[n - 1] == 'z', "one byte more: the oldest dropped (%zu)", n);
    helper_tail_feed(&t, "zz", 2);
    s = helper_tail_get(&t, &n);
    CHECK(n == HELPER_ERR_TAIL - 1 && s[0] == 'b' && s[n - 1] == 'z', "a character cut at the front: from the next whole one (%zu)", n);
}

/* the core DLL's native admin rules (native_admin.c) */
static void t_native_admin(void) {
    CHECK(native_admin_verb(NA_KIND_INSTALL) == ADMIN_INSTALL && native_admin_verb(NA_KIND_REINSTALL) == ADMIN_INSTALL
              && native_admin_verb(NA_KIND_UNINSTALL) == ADMIN_UNINSTALL && native_admin_verb(NA_KIND_REMOVE_ROOT) == ADMIN_REMOVE_ROOT_FOLDER
              && native_admin_verb(NA_KIND_RESTORE) == ADMIN_RESTORE && native_admin_verb(NA_KIND_FORGET) == NA_FORGET
              && native_admin_verb(6) == -1 && native_admin_verb(-1) == -1,
          "the core's kinds as the worker's verbs; FORGET is nothing");
    CHECK(native_admin_needs_bridge(ADMIN_INSTALL) && !native_admin_needs_bridge(ADMIN_UNINSTALL) && !native_admin_needs_bridge(ADMIN_RESTORE)
              && !native_admin_needs_bridge(ADMIN_REMOVE_ROOT_FOLDER),
          "only the install needs the flow's bridge");
    admin_result r;
    memset(&r, 0, sizeof r);
    r.code = 0;
    CHECK(native_admin_code(&r) == 0, "ok");
    r.code = ADMIN_EXIT_REKORDBOX_OPEN;
    CHECK(native_admin_code(&r) == 5, "the worker's code as is");
    r.code = ADMIN_EXIT_USER_STOPPED;
    CHECK(native_admin_code(&r) == 1223, "UAC declined: the user stopped");
    r.code = -1;
    CHECK(native_admin_code(&r) == ADMIN_EXIT_FAILED, "not started: failed, never a silent stop");
    r.code = (int)0xC0000005u;
    CHECK(native_admin_code(&r) == ADMIN_EXIT_FAILED, "a crashed worker (an NTSTATUS): failed");
    char line[96];
    native_admin_result_line(ADMIN_INSTALL, 0, line, sizeof line);
    CHECK(!strcmp(line, "  ok: install"), "%s", line);
    native_admin_result_line(ADMIN_UNINSTALL, ADMIN_EXIT_REKORDBOX_OPEN, line, sizeof line);
    CHECK(!strcmp(line, "  FAILED: uninstall [exit 5]"), "%s", line);
    native_admin_result_line(ADMIN_RESTORE, (int)0xC0000005u, line, sizeof line);
    CHECK(!strcmp(line, "  FAILED: restore [exit 0xC0000005]"), "the raw NTSTATUS: %s", line);
    native_admin_result_line(ADMIN_REMOVE_ROOT_FOLDER, -1, line, sizeof line);
    CHECK(!strcmp(line, "  FAILED: remove-root-folder [exit 0xFFFFFFFF]"), "not started: %s", line);
    r.code = ADMIN_EXIT_OLDER;
    r.update_first = 1;
    CHECK(native_admin_code(&r) == ADMIN_EXIT_OLDER, "below the floor: 14");
    r.code = ADMIN_EXIT_PROTOCOL;
    CHECK(native_admin_code(&r) == ADMIN_EXIT_PROTOCOL, "no worker can serve: 12");

    /* NA2: the install stages only this app's release's bridge; NA6: a failed source folder */
    payload_manifest *m = calloc(1, sizeof *m);
    m->has_windows = 1;
    snprintf(m->win_bridge.file, sizeof m->win_bridge.file, "onnxruntime.dll");
    memset(m->win_bridge.sha256, 'a', 64);
    char sha[65];
    memset(sha, 'a', 64);
    sha[64] = 0;
    CHECK(native_admin_bridge_ok(m, sha, "onnxruntime.dll"), "this release's bridge: staged");
    sha[0] = 'b';
    CHECK(!native_admin_bridge_ok(m, sha, "onnxruntime.dll"), "another sha256 (a newer release's): update first");
    sha[0] = 'a';
    CHECK(!native_admin_bridge_ok(m, sha, "other.dll") && !native_admin_bridge_ok(m, NULL, "onnxruntime.dll")
              && !native_admin_bridge_ok(m, sha, NULL) && !native_admin_bridge_ok(NULL, sha, "onnxruntime.dll"),
          "another name, nothing checked, no record: update first");
    m->has_windows = 0;
    CHECK(!native_admin_bridge_ok(m, sha, "onnxruntime.dll"), "a record without the windows block: update first");
    free(m);
    CHECK(native_admin_source_failed(1) == ADMIN_EXIT_CHECK_FAILED && native_admin_source_failed(0) == -1,
          "a failed source folder: 4 with a verified record, the client's update-first without one");

    native_admin_track t;
    native_admin_track_init(&t);
    CHECK(native_admin_line(&t, ADMIN_LINE_TEXT) == NA_ATTEMPT, "the first line: the attempt");
    CHECK(native_admin_line(&t, ADMIN_LINE_TEXT) == 0, "then nothing");
    /* the worker's real lines (processes_ok): @still-closing, then its text at once, then the
     * linger, then @closed once rekordbox's process is gone */
    {
        static const char *const real[] = {"@still-closing", "rekordbox is still closing: waiting for it", "rekordbox 7.2.19: checked",
                                           "@closed", "rekordbox 7.2.19's ONNX Runtime: Pioneer's"};
        unsigned got[5];
        int up[5];
        char v[80];
        for (int i = 0; i < 5; i++) {
            got[i] = native_admin_line(&t, admin_line_kind(real[i], strlen(real[i]), v, sizeof v));
            up[i] = t.closing;
        }
        CHECK(got[0] == NA_CLOSING && up[0], "@still-closing: the line goes up");
        CHECK(got[1] == 0 && got[2] == 0 && up[1] && up[2], "the worker's text right after it: still closing");
        CHECK(got[3] == NA_CLOSED && !up[3], "@closed: the line comes down");
        CHECK(got[4] == 0, "then nothing");
    }
    CHECK(native_admin_line(&t, ADMIN_LINE_STILL_CLOSING) == NA_CLOSING && native_admin_line(&t, ADMIN_LINE_STILL_CLOSING) == 0,
          "@still-closing once");
    CHECK(native_admin_line(&t, ADMIN_LINE_TEXT) == 0 && t.closing, "text keeps it up");
    CHECK(native_admin_line(&t, ADMIN_LINE_RESTART) == NA_RESTART && t.closing, "@restart: the note, still closing");
    CHECK(native_admin_end(&t, 1) == NA_CLOSED, "exit 5 after the linger (no @closed): the run's end brings it down");
    CHECK(native_admin_line(&t, ADMIN_LINE_CLOSED) == 0, "@closed with nothing up: nothing");
    native_admin_track_init(&t);
    CHECK(native_admin_line(&t, ADMIN_LINE_STILL_CLOSING) == (NA_ATTEMPT | NA_CLOSING), "a first line that is @still-closing: both");
    CHECK(native_admin_end(&t, 1) == NA_CLOSED, "still closing at the end (a crash): the line comes down");
    native_admin_track_init(&t);
    CHECK(native_admin_end(&t, 0) == 0, "UAC declined: no attempt");
    CHECK(native_admin_end(&t, 1) == NA_ATTEMPT && native_admin_end(&t, 1) == 0, "a run with no line: one attempt");
}

/* NA7: native_admin.c's glue (native_admin_elevate) around the fake elevation, as core_native_admin.c
 * runs it around admin_win.c's: what the core is told, in order */
static unsigned told[16];
static int ntold, not_starts;
static void glue_tell(void *ctx, unsigned what) {
    (void)ctx;
    if (ntold < 16) told[ntold] = what;
    ntold++;
}
static void glue_not_started(void *ctx) {
    (void)ctx;
    not_starts++;
}

/* the worker's run counts in CriticalWork: from its first line to the elevation's end */
static int crit_level, crit_begins, crit_bad, crit_at_attempt;
static void glue_critical(void *ctx, int on) {
    (void)ctx;
    if (on) {
        crit_level++;
        crit_begins++;
        ev('C');
    } else {
        if (crit_level <= 0) crit_bad++;
        crit_level--;
        ev('c');
    }
}
static void crit_tell(void *ctx, unsigned what) {
    if (what & NA_ATTEMPT) crit_at_attempt = crit_level;
    glue_tell(ctx, what);
}
/* CriticalWork's admission: open or closed (the quit decided) */
static int admits;
static int glue_open(void *ctx) {
    (void)ctx;
    admits++;
    return 1;
}
static int glue_closed(void *ctx) {
    (void)ctx;
    admits++;
    return 0;
}
static admin_client crit_client(native_admin_run *nr, const char *own) {
    admin_client c = client(own);
    native_admin_run_init(nr, fake_elevate, NULL, crit_tell, glue_not_started, NULL);
    nr->critical = glue_critical;
    c.elevate = native_admin_elevate;
    c.ctx = nr;
    admits = 0;
    crit_level = crit_begins = crit_bad = 0;
    crit_at_attempt = -1;
    ntold = not_starts = 0;
    return c;
}

static void t_native_critical(void) {
    native_admin_run nr;
    admin_result res;
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    /* a run with lines: counted from the first line (before it reaches the log) to the end */
    admin_client c = crit_client(&nr, sha_of(W2));
    E.lines[0] = "restored\n";
    int x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == 0 && crit_begins == 1 && crit_level == 0 && !crit_bad && crit_at_attempt == 1 && !nr.in_critical,
          "a worker's run: one step, running at its first line, ended with it (%d, %d, %d)", crit_begins, crit_level, crit_at_attempt);
    CHECK(strchr(order, 'C') && strchr(order, 'c') && strchr(order, 'C') < strchr(order, 'L') && strchr(order, 'L') < strrchr(order, 'c'),
          "the step brackets its lines (%s)", order);
    /* UAC declined: never counted */
    c = crit_client(&nr, sha_of(W2));
    E.declined = 1;
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_USER_STOPPED && crit_begins == 0 && crit_level == 0, "UAC declined: nothing counted (the prompt never is)");
    /* not started: never counted */
    c = crit_client(&nr, sha_of(W2));
    E.no_start = 1;
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == -1 && crit_begins == 0 && crit_level == 0, "not started: nothing counted");
    /* a worker that ended before any line (it changed nothing): not counted */
    c = crit_client(&nr, sha_of(W2));
    E.codes[0] = ADMIN_EXIT_CHECK_FAILED;
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(crit_begins == 0 && crit_level == 0, "no line: nothing counted (%d)", x);
    /* install-worker, then the action: each elevation its own step, back to 0 */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    c = crit_client(&nr, sha_of(W1B));
    E.codes[0] = ADMIN_EXIT_RUN_AGAIN;
    E.installs = W1B;
    E.lines[0] = "installed\n";
    E.lines[1] = "uninstalled\n";
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(res.elevations == 2 && crit_begins == 2 && crit_level == 0 && !crit_bad, "two elevations: two steps, none left (%d, %d, %d)", x, crit_begins, crit_level);
    /* the second prompt declined: the first step ended, none left */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    c = crit_client(&nr, sha_of(W1B));
    E.codes[0] = ADMIN_EXIT_RUN_AGAIN;
    E.installs = W1B;
    E.lines[0] = "installed\n";
    E.declined_from = 2;
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_USER_STOPPED && crit_begins == 1 && crit_level == 0 && !crit_bad, "the second prompt declined: one step, ended (%d)", crit_begins);
    /* the quit decided (CriticalWork closed, WC1): no elevation tried, no prompt, "stopped" */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    c = crit_client(&nr, sha_of(W2));
    nr.admit = glue_closed;
    E.lines[0] = "restored\n";
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_USER_STOPPED && E.n == 0 && crit_begins == 0 && crit_level == 0 && admits == 1,
          "closed: stopped, the worker never started, nothing counted (%d, %d elevations)", x, E.n);
    /* still open: elevated as before */
    c = crit_client(&nr, sha_of(W2));
    nr.admit = glue_open;
    E.lines[0] = "restored\n";
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == 0 && E.n == 1 && crit_begins == 1 && admits == 1, "open: the worker ran, counted (%d)", x);
}

static void t_native_glue(void) {
    native_admin_run nr;
    admin_result res;
    /* exit 5 after @still-closing (the linger ran out): the attempt, the line up, and the run's
     * end brings it down */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    admin_client c = client(sha_of(W2));
    native_admin_run_init(&nr, fake_elevate, NULL, glue_tell, glue_not_started, NULL);
    c.elevate = native_admin_elevate;
    c.ctx = &nr;
    ntold = not_starts = 0;
    E.lines[0] = "@still-closing\nrekordbox is still closing: waiting for it\nrekordbox is open: nothing changed\n";
    E.codes[0] = ADMIN_EXIT_REKORDBOX_OPEN;
    int x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_REKORDBOX_OPEN && ntold == 2 && told[0] == (NA_ATTEMPT | NA_CLOSING) && told[1] == NA_CLOSED && res.still_closing,
          "exit 5 after @still-closing: attempt and closing, then closed at the end (%d told)", ntold);
    CHECK(strstr(logged, "rekordbox is still closing") != NULL, "its lines still reach the log");
    /* @closed while the worker goes on */
    c = client(sha_of(W2));
    native_admin_run_init(&nr, fake_elevate, NULL, glue_tell, glue_not_started, NULL);
    c.elevate = native_admin_elevate;
    c.ctx = &nr;
    ntold = 0;
    E.lines[0] = "@still-closing\nrekordbox is still closing: waiting for it\n@closed\nput back\n@restart\n";
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(x == 0 && ntold == 3 && told[0] == (NA_ATTEMPT | NA_CLOSING) && told[1] == NA_CLOSED && told[2] == NA_RESTART && res.restart_pending,
          "@closed, then @restart (%d told)", ntold);
    /* I75: install-worker, then the second prompt declined: one attempt, no line left up, 1223 */
    sh(P("rm -rf '%s/bin'", ROOT));
    put_worker(W2);
    c = client(sha_of(W1B));
    native_admin_run_init(&nr, fake_elevate, NULL, glue_tell, glue_not_started, NULL);
    c.elevate = native_admin_elevate;
    c.ctx = &nr;
    ntold = 0;
    E.codes[0] = ADMIN_EXIT_RUN_AGAIN;
    E.installs = W1B;
    E.declined_from = 2;
    x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    CHECK(x == ADMIN_EXIT_USER_STOPPED && E.n == 1 && res.elevations == 2 && ntold == 1 && told[0] == NA_ATTEMPT && native_admin_code(&res) == 1223,
          "two prompts, the second declined: one attempt told, the user stopped (%d, %d told)", x, ntold);
    /* a worker that couldn't be started: not_started, no attempt, 1 */
    c = client(sha_of(W2));
    native_admin_run_init(&nr, fake_elevate, NULL, glue_tell, glue_not_started, NULL);
    c.elevate = native_admin_elevate;
    c.ctx = &nr;
    ntold = not_starts = 0;
    E.no_start = 1;
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == -1 && not_starts == 1 && ntold == 0 && native_admin_code(&res) == ADMIN_EXIT_FAILED, "not started: logged, nothing told, 1");
    /* NT1: the glue passes started through to the real elevation */
    c = held_client(sha_of(W2));
    native_admin_run_init(&nr, fake_elevate, NULL, glue_tell, glue_not_started, NULL);
    c.elevate = native_admin_elevate;
    c.ctx = &nr;
    E.lines[0] = "restored\n";
    x = admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
    CHECK(x == 0 && !strcmp(order, "HSRL") && E.held_at_started == 1 && releases == 1 && !held_now, "through the glue: released at started (%s)", order);
}

void test_client(const char *scratch) {
    if (!realpath(scratch, S)) abort();
    snprintf(ROOT, sizeof ROOT, "%s/RB Stems Plus", S);
    t_choose();
    t_list();
    t_source();
    t_run();
    t_no_fallback();
    t_hold();
    t_quote();
    t_helper();
    t_helper_lines();
    t_helper_tail();
    t_native_admin();
    t_native_glue();
    t_native_critical();
}
