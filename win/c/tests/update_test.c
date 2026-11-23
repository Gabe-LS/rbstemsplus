/* update.c on plat_mac.c, a fake network and a fake OS (update_net, update_os), in a scratch folder:
 * the check (releases/latest only, release.md §2), what a payload means (the shown
 * version, installed-release, DisplayVersion; a carried release downloads nothing else), the old
 * app's staging (download, resume, checks, refusals), the new app's commit after its first render
 * (current, installed-release, the stub's .new swap, the watcher, the old folder), and every
 * rollback (never rendered, --swap failed, past the commit point, stale records). */
#include "check.h"

#include "../update.h"
#include "../update_shell.h"
#include "plat.h"
#include "sig_mac.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

#define BASE "https://test.invalid/releases"
#define ZIP "rbstemsplus-windows.zip"

/* ------------------------------------------------------------------ files */

static void wfile(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "can't write %s\n", path);
        abort();
    }
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

static char *rfile(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long k = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)k + 1);
    size_t got = fread(b, 1, (size_t)k, f);
    fclose(f);
    b[got] = 0;
    if (n) *n = got;
    return b;
}

static int exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

/* whether the file holds exactly s */
static int holds(const char *path, const char *s) {
    size_t n;
    char *b = rfile(path, &n);
    int ok = b && n == strlen(s) && !memcmp(b, s, n);
    free(b);
    return ok;
}

static int rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return ENOENT;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        for (struct dirent *e; d && (e = readdir(d));) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[2048];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            rm_rf(p);
        }
        if (d) closedir(d);
        return rmdir(path) ? errno : 0;
    }
    return unlink(path) ? errno : 0;
}

static void sha_hex(const void *d, size_t n, char out[65]) {
    unsigned char md[32];
    plat_sha256(d, n, md);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", md[i]);
}

/* ------------------------------------------------------------------ the fake network */

typedef struct {
    char url[256];
    unsigned char *data;
    size_t n;
    int status, offline, no_range, stall;   /* stall: the body never comes (until the sink's cancelled says so) */
    size_t cut;   /* the connection drops after this many bytes of the file (0: never) */
    int hits;
    int64_t last_from;
} route;
static route routes[32];
static int nroutes, net_calls;

static route *find(const char *url) {
    for (int i = 0; i < nroutes; i++)
        if (!strcmp(routes[i].url, url)) return &routes[i];
    return NULL;
}

static route *serve(const char *url, const void *data, size_t n) {
    route *r = find(url);
    if (!r) {
        r = &routes[nroutes++];
        memset(r, 0, sizeof *r);
        snprintf(r->url, sizeof r->url, "%s", url);
    }
    free(r->data);
    r->data = malloc(n ? n : 1);
    memcpy(r->data, data, n);
    r->n = n;
    r->status = 0, r->offline = 0, r->no_range = 0, r->cut = 0;
    return r;
}

static void net_reset(void) {
    for (int i = 0; i < nroutes; i++) free(routes[i].data);
    nroutes = 0;
    net_calls = 0;
}

static int fake_get(void *ctx, const char *url, int64_t from, int64_t max, const update_sink *sink, int *status, int *err) {
    (void)ctx;
    net_calls++;
    *status = 0;
    *err = 0;
    route *r = find(url);
    if (!r) {
        *status = 404;
        return UPDATE_NET_HTTP;
    }
    r->hits++;
    r->last_from = from;
    if (r->offline) return UPDATE_NET_OFFLINE;
    if (r->stall) {   /* a connection that sends nothing: only the sink's cancelled ends it (at most ~5 s here) */
        for (int i = 0; i < 5000; i++) {
            if (sink->cancelled && sink->cancelled(sink->ctx)) return UPDATE_NET_CANCELLED;
            plat_sleep_ms(1);
        }
        return UPDATE_NET_OFFLINE;
    }
    if (r->status && r->status != 200) {
        *status = r->status;
        return UPDATE_NET_HTTP;
    }
    if ((int64_t)r->n > max) return UPDATE_NET_TOO_BIG;
    size_t at = 0;
    if (from > 0 && !r->no_range) {
        if ((size_t)from >= r->n) {
            *status = 416;
            return UPDATE_NET_HTTP;
        }
        at = (size_t)from;
        *status = 206;
    } else {
        *status = 200;
    }
    if ((*err = sink->begin(sink->ctx, (int64_t)at, (int64_t)r->n))) return UPDATE_NET_SINK;
    size_t end = r->cut && r->cut < r->n ? r->cut : r->n;
    for (size_t i = at; i < end; i += 777) {
        size_t k = end - i < 777 ? end - i : 777;
        if ((*err = sink->write(sink->ctx, r->data + i, k))) return UPDATE_NET_SINK;
    }
    return end < r->n ? UPDATE_NET_OFFLINE : UPDATE_NET_OK;
}

static const update_net fake_net = {fake_get, NULL};

/* ------------------------------------------------------------------ the fake OS */

static struct {
    char self[1024];
    int nspawn, spawn_fail;
    char spawn_exe[16][1024], spawn_arg[16][64];
    int watcher_running, nstop;
    int replacement_running, stub_only;
    char stopped[1024];
    char display[64];
    int ndisplay;
    int wait_r, nwait;
    int running_left, running_always, running_fail;   /* running_under: 1 while running_left > 0 */
    char move_current[64], current_file[1100];         /* written into current_file when running_left reaches 0 */
    int move_now;                                      /* ... or at the next running_under call */
    int watchers_left, nwatch;                         /* watchers_in: 1 while watchers_left > 0 */
    int watchers_fail;                                 /* ... -1 (the list couldn't be read) */
    int nrm_at, rm_at_fail;                            /* remove_tree_at: calls; an errno it gives (0: none) */
    char rm_at_names[256];                             /* the names it was given, "|"-separated */
    char rm_at_swap[1100];                             /* at its next call: this folder moved to "<it>-moved" and an empty one made at its name */
} fo;

/* the running exe: fo.self (the world's own app), or the one an as() env names (ctx: its path) */
static int fo_self(void *ctx, char *out, size_t n) {
    snprintf(out, n, "%s", ctx ? (const char *)ctx : fo.self);
    return 0;
}
static int fo_spawn(void *ctx, const char *exe, const char *const *args) {
    (void)ctx;
    if (fo.spawn_fail) return fo.spawn_fail;
    int i = fo.nspawn++ % 16;
    snprintf(fo.spawn_exe[i], sizeof fo.spawn_exe[i], "%s", exe);
    snprintf(fo.spawn_arg[i], sizeof fo.spawn_arg[i], "%s", args[0] ? args[0] : "");
    if (args[0] && !strcmp(args[0], "--watcher") && !fo.stub_only) fo.replacement_running = 1;
    return 0;
}
static int fo_wait(void *ctx, uint32_t pid, int64_t started, uint32_t ms) {
    (void)ctx, (void)pid, (void)started, (void)ms;
    fo.nwait++;
    return fo.wait_r;
}
static int fo_stop(void *ctx, const char *dir) {
    (void)ctx;
    fo.nstop++;
    snprintf(fo.stopped, sizeof fo.stopped, "%s", dir);
    int r = fo.watcher_running;
    fo.watcher_running = 0;
    return r;
}
static int fo_running(void *ctx, const char *dir) {
    (void)ctx, (void)dir;
    if (fo.running_fail) return -1;
    if (fo.move_now) {   /* the new app commits right now */
        fo.move_now = 0;
        wfile(fo.current_file, fo.move_current, strlen(fo.move_current));
    }
    if (fo.running_always) return 1;
    if (!fo.running_left) return 0;
    if (--fo.running_left) return 1;
    /* this call finds it gone: it moved current first when told to */
    if (fo.move_current[0]) wfile(fo.current_file, fo.move_current, strlen(fo.move_current));
    return 0;
}
static int fo_watchers(void *ctx, const char *dir) {
    (void)ctx;
    fo.nwatch++;
    if (fo.watchers_fail) return -1;
    if (fo.watchers_left > 0) {
        fo.watchers_left--;
        return 1;
    }
    return fo.replacement_running && strcmp(dir, fo.stopped) != 0;
}
static int fo_rm(void *ctx, const char *path) {
    (void)ctx;
    return rm_rf(path);
}
/* plat_remove_tree_at on the Mac: name in dfd and its tree, never following a link (one is
 * unlinked itself) */
static int rm_tree_at(int dfd, const char *name) {
    struct stat st;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW)) return errno;
    if (!S_ISDIR(st.st_mode)) return unlinkat(dfd, name, 0) ? errno : 0;
    int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    DIR *dir = fd < 0 ? NULL : fdopendir(fd);
    if (!dir) {
        if (fd >= 0) close(fd);
        return errno;
    }
    for (struct dirent *x; (x = readdir(dir));)
        if (strcmp(x->d_name, ".") && strcmp(x->d_name, "..")) rm_tree_at(dirfd(dir), x->d_name);
    closedir(dir);
    return unlinkat(dfd, name, AT_REMOVEDIR) ? errno : 0;
}
static int fo_rm_at(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    fo.nrm_at++;
    size_t have = strlen(fo.rm_at_names);
    snprintf(fo.rm_at_names + have, sizeof fo.rm_at_names - have, "%s|", name);
    if (fo.rm_at_swap[0]) {   /* the apps folder swapped between its check and this delete, a decoy of the name in the new one */
        char mv[1200], decoy[1400];
        snprintf(mv, sizeof mv, "%s-moved", fo.rm_at_swap);
        snprintf(decoy, sizeof decoy, "%s/%s", fo.rm_at_swap, name);
        if (rename(fo.rm_at_swap, mv) || mkdir(fo.rm_at_swap, 0755) || mkdir(decoy, 0755)) abort();
        fo.rm_at_swap[0] = 0;
    }
    if (fo.rm_at_fail) return fo.rm_at_fail;
    return rm_tree_at(d.fd, name);   /* relative to the handle it was given */
}
static int fo_display(void *ctx, const char *v) {
    (void)ctx;
    fo.ndisplay++;
    snprintf(fo.display, sizeof fo.display, "%s", v);
    return 0;
}
static const update_os fake_os = {fo_self, fo_spawn, fo_wait, fo_stop, fo_running, fo_watchers, fo_rm, fo_display, fo_rm_at, NULL};

static char logbuf[1 << 16];
static void logline(void *ctx, const char *line) {
    (void)ctx;
    size_t k = strlen(logbuf);
    if (k + strlen(line) + 2 < sizeof logbuf) snprintf(logbuf + k, sizeof logbuf - k, "%s\n", line);
}
static void progress(void *ctx, int64_t done, int64_t total) {
    (void)ctx, (void)done, (void)total;
}

/* ------------------------------------------------------------------ payloads */

static test_key rel, oth;
static sig_pubkey keys[1];

static const char *const z64 = "0000000000000000000000000000000000000000000000000000000000000000";

/* a payload.json with the Mac keys; winver NULL: no windows block */
static char *mkpayload(const char *pv, const char *winver, const char *zipsha, const char *admin, const char *x64, const char *arm64) {
    char *o = malloc(4096);
    char win[2048] = "";
    if (winver)
        snprintf(win, sizeof win,
                 ", \"windows\": {\"app\": {\"version\": \"%s\", \"zip\": \"" ZIP "\", \"sha256\": \"%s\", \"admin_sha256\": \"%s\", "
                 "\"stub_sha256\": {\"x64\": \"%s\", \"arm64\": \"%s\"}}, "
                 "\"bridge\": {\"file\": \"b.dll\", \"sha256\": \"%s\"}, "
                 "\"compatible\": {\"rekordbox\": [\"7.2.19\"], \"ort_sha256\": [], \"stems_engine\": [\"0002\"]}}",
                 winver, zipsha ? zipsha : z64, admin ? admin : z64, x64 ? x64 : z64, arm64 ? arm64 : z64, z64);
    snprintf(o, 4096,
             "{\"payload_version\": \"%s\", %s"
             "\"model\": {\"file\": \"m.onnx\", \"sha256\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\", \"size\": 9}%s}",
             pv,
             "\"app\": {\"version\": \"1.0.0\", \"zip\": \"a.zip\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
             "\"bridge\": {\"file\": \"b.dylib\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
             "\"compatible\": {\"rekordbox\": [], \"ort_version_prefix\": \"1.18.\", \"stems_engine\": []}, ",
             win);
    return o;
}

/* serves payload.json and its signature at a tag (NULL: latest), signed by k */
static void serve_payload(const char *tag, const char *json, const test_key *k) {
    char url[512];
    unsigned char sig[SIG_DER_MAX];
    size_t sn = test_sign(k, json, strlen(json), sig);
    update_url(BASE, tag, "payload.json", url, sizeof url);
    serve(url, json, strlen(json));
    update_url(BASE, tag, "payload.json.sig", url, sizeof url);
    serve(url, sig, sn);
}

/* a pair written into dir, as bootstrap.ps1 or an earlier update would */
static void put_pair(const char *dir, const char *json) {
    unsigned char sig[SIG_DER_MAX];
    size_t sn = test_sign(&rel, json, strlen(json), sig);
    char p[1100];
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/payload.json", dir);
    wfile(p, json, strlen(json));
    snprintf(p, sizeof p, "%s/payload.json.sig", dir);
    wfile(p, sig, sn);
}

/* ------------------------------------------------------------------ an environment */

typedef struct {
    char root[1024], support[1100], apps[1100];
    update_env e;
} world;

/* UPDATE_TEST_LOG=1: each world's log, on stderr */
static void dump_log(void) {
    if (getenv("UPDATE_TEST_LOG") && logbuf[0]) fprintf(stderr, "---- log\n%s", logbuf);
}

static void world_new(world *w, const char *scratch, const char *name, const char *own) {
    dump_log();
    snprintf(w->root, sizeof w->root, "%s/%s", scratch, name);
    snprintf(w->support, sizeof w->support, "%s/support", w->root);
    snprintf(w->apps, sizeof w->apps, "%s/apps", w->root);
    rm_rf(w->root);
    mkdir(w->root, 0755);
    mkdir(w->support, 0755);
    mkdir(w->apps, 0755);
    memset(&w->e, 0, sizeof w->e);
    w->e.support = w->support;
    w->e.apps = w->apps;
    w->e.own = own;
    w->e.arch = "x64";
    w->e.base = BASE;
    w->e.keys = keys;
    w->e.nkeys = 1;
    w->e.net = &fake_net;
    w->e.os = &fake_os;
    w->e.log = logline;
    w->e.progress = progress;
    net_reset();
    memset(&fo, 0, sizeof fo);
    update_test_self_reset();   /* a new world: its apps' ids taken again (installed_152, as()) */
    logbuf[0] = 0;
}

/* the same folders seen by another app version */
/* the env of another app in the same world: own's, running from <apps>/<own>/RB Stems Plus.exe
 * (its own fake OS whose ctx is that path; a few at once) */
static update_env as(const world *w, const char *own) {
    static update_os os[8];
    static char self[8][1200], owns[8][PAYLOAD_VERSION_MAX];
    static int used;
    int i = 0;
    while (i < used && strcmp(owns[i], own)) i++;   /* one fake OS per version: its id taken once */
    if (i == used) {
        if (used == 8) abort();
        snprintf(owns[used++], sizeof owns[0], "%s", own);
    }
    snprintf(self[i], sizeof self[i], "%s/%s/RB Stems Plus.exe", w->apps, own);
    os[i] = *w->e.os;
    os[i].ctx = self[i];
    update_env e = w->e;
    e.own = own;
    e.os = &os[i];
    (void)update_self_capture(&e);   /* at its start; none while its exe isn't there yet (taken later) */
    return e;
}

/* the world's own app runs from path now (a new start: its id taken again) */
static void self_is(world *w, const char *path) {
    snprintf(fo.self, sizeof fo.self, "%s", path);
    update_test_self_reset();
    (void)update_self_capture(&w->e);
}

static void path(char *out, size_t n, const char *a, const char *b) { snprintf(out, n, "%s/%s", a, b); }

/* ------------------------------------------------------------------ the check */

static payload_manifest m;

static void check_final(const char *scratch) {
    world w;
    world_new(&w, scratch, "final", "1.4.0");
    char p[1200], shown[PAYLOAD_VERSION_MAX];
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_OFFLINE, "nothing served: offline");
    char *p150 = mkpayload("1.5.0", "1.5.0", NULL, NULL, NULL, NULL);
    serve_payload(NULL, p150, &rel);
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_OK && !strcmp(m.payload_version, "1.5.0"), "latest with a newer app: taken");
    path(p, sizeof p, w.support, "payload/payload.json");
    CHECK(holds(p, p150), "saved");
    path(p, sizeof p, w.support, "payload-version");
    CHECK(holds(p, "1.5.0\n"), "the floor raised");
    CHECK(update_seen(&w.e, &m, shown) == 1 && !strcmp(shown, "1.4.0") && fo.ndisplay == 0, "an update offered; the version shown stays");
    path(p, sizeof p, w.support, "installed-release");
    CHECK(!exists(p), "no installed-release for another app");

    char *mac = mkpayload("1.6.0", NULL, NULL, NULL, NULL, NULL);
    serve_payload(NULL, mac, &rel);
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_NO_NEWS, "a Mac-only release: no news");
    path(p, sizeof p, w.support, "payload/payload.json");
    CHECK(holds(p, p150), "and nothing saved");
    char *p170 = mkpayload("1.7.0", "1.7.0", NULL, NULL, NULL, NULL);
    serve_payload(NULL, p170, &oth);
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_UNVERIFIED, "another key's signature: unverified");
    char *b = mkpayload("1.8.0-beta.1", "1.8.0-beta.1", NULL, NULL, NULL, NULL);
    serve_payload(NULL, b, &rel);
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_UNVERIFIED, "a suffixed version (no beta channel): refused");
    char *old = mkpayload("1.4.5", "1.4.5", NULL, NULL, NULL, NULL);
    serve_payload(NULL, old, &rel);
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_OLDER, "older than the floor: older (the Mac's .older)");
    char url[512];
    update_url(BASE, NULL, "payload.json.sig", url, sizeof url);
    serve_payload(NULL, p170, &rel);
    find(url)->status = 404;
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_UNVERIFIED, "no signature published: unverified");
    /* ST7: the signature not downloaded for a network error: offline, not unverified */
    find(url)->status = 0;
    find(url)->offline = 1;
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_OFFLINE, "the signature offline: offline");
    find(url)->offline = 0;
    find(url)->status = 404;
    /* ST4: payload.json that never comes stops at its deadline: offline */
    find(url)->status = 0;
    update_url(BASE, NULL, "payload.json", url, sizeof url);
    find(url)->stall = 1;
    w.e.small_fetch_ms = 50;
    uint64_t t0 = plat_now_mono_ns();
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_OFFLINE && plat_now_mono_ns() - t0 < 3000000000u, "a stalled payload.json: offline at its deadline");
    find(url)->stall = 0;
    w.e.small_fetch_ms = 0;
    free(p150), free(mac), free(p170), free(b), free(old);

    /* a release that carries this app: no download, the record and the version shown follow it */
    world_new(&w, scratch, "carried", "1.4.0");
    char *c141 = mkpayload("1.4.1", "1.4.0", NULL, NULL, NULL, NULL);
    serve_payload(NULL, c141, &rel);
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_OK && update_seen(&w.e, &m, shown) == 0, "carried: no update");
    CHECK(net_calls == 2, "only payload.json and its signature downloaded (%d)", net_calls);
    CHECK(!strcmp(shown, "1.4.1") && fo.ndisplay == 1 && !strcmp(fo.display, "1.4.1"), "1.4.1 shown, DisplayVersion too");
    path(p, sizeof p, w.support, "shown-version");
    CHECK(holds(p, "1.4.1\n1.4.0\n"), "recorded");
    static payload_manifest inst;
    CHECK(update_installed(&w.e, &inst) == PAYLOAD_OK && !strcmp(inst.payload_version, "1.4.1"), "installed-release is the carried release");
    {
        /* NA5: the folder whose pair verified: installed-release, or prev\ when only it does */
        char at[UPDATE_PATH_MAX], ir[UPDATE_PATH_MAX], pv[UPDATE_PATH_MAX], ja[UPDATE_PATH_MAX], sa[UPDATE_PATH_MAX];
        path(ir, sizeof ir, w.support, "installed-release");
        CHECK(update_installed_at(&w.e, &inst, at, sizeof at) == PAYLOAD_OK && !strcmp(at, ir), "the record's own folder: %s", at);
        snprintf(pv, sizeof pv, "%s/prev", ir);
        mkdir(pv, 0755);
        char *jb = NULL, *sb = NULL;
        size_t jn = 0, sn = 0;
        snprintf(ja, sizeof ja, "%s/payload.json", ir);
        snprintf(sa, sizeof sa, "%s/payload.json.sig", ir);
        CHECK(!plat_read_file(ja, 1 << 20, &jb, &jn) && !plat_read_file(sa, 4096, &sb, &sn), "the pair read");
        char c1[UPDATE_PATH_MAX], c2[UPDATE_PATH_MAX];
        snprintf(c1, sizeof c1, "%s/payload.json", pv);
        snprintf(c2, sizeof c2, "%s/payload.json.sig", pv);
        FILE *f = fopen(c1, "wb");
        fwrite(jb, 1, jn, f);
        fclose(f);
        f = fopen(c2, "wb");
        fwrite(sb, 1, sn, f);
        fclose(f);
        f = fopen(ja, "wb");   /* a write stopped half-way: payload.json doesn't verify */
        fputs("{}", f);
        fclose(f);
        CHECK(update_installed_at(&w.e, &inst, at, sizeof at) == PAYLOAD_OK && !strcmp(at, pv) && !strcmp(inst.payload_version, "1.4.1"),
              "only prev\\ verifies: it is the folder (%s)", at);
        CHECK(update_installed_at(&w.e, &inst, at, 4) == PAYLOAD_NONE && !at[0], "a folder that doesn't fit: none");
        f = fopen(ja, "wb");
        fwrite(jb, 1, jn, f);
        fclose(f);
        free(jb);
        free(sb);
        CHECK(update_installed_at(&w.e, &inst, at, sizeof at) == PAYLOAD_OK && !strcmp(at, ir), "repaired: its own folder again");
    }
    char *n150 = mkpayload("1.5.0", "1.5.0", NULL, NULL, NULL, NULL);
    serve_payload(NULL, n150, &rel);
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_OK && update_seen(&w.e, &m, shown) == 1 && !strcmp(shown, "1.4.1"), "a newer app fetched: 1.4.1 still shown");
    CHECK(update_installed(&w.e, &inst) == PAYLOAD_OK && !strcmp(inst.payload_version, "1.4.1") && fo.ndisplay == 1, "installed-release unchanged");
    update_shown_now(&w.e, shown);
    CHECK(!strcmp(shown, "1.4.1"), "read only: 1.4.1");
    char *u149 = mkpayload("1.4.9", "1.4.0", NULL, NULL, NULL, NULL);
    static payload_manifest unverified;
    CHECK(payload_decode(u149, strlen(u149), &unverified) == PAYLOAD_OK && update_seen(&w.e, &unverified, shown) == 0 && !strcmp(shown, "1.4.1"),
          "an unverified payload.json moves nothing");
    CHECK(update_installed(&w.e, &inst) == PAYLOAD_OK && !strcmp(inst.payload_version, "1.4.1"), "nor installed-release");
    free(u149);
    update_env replaced = as(&w, "1.5.0");
    CHECK(update_seen(&replaced, NULL, shown) == 0 && !strcmp(shown, "1.5.0") && !strcmp(fo.display, "1.5.0"), "replaced: its own version");
    CHECK(!exists(p), "the old app's record cleared");
    CHECK(update_installed(&replaced, &inst) == PAYLOAD_NONE, "the old app's installed-release isn't this one's");
    free(c141), free(n150);
}

/* ------------------------------------------------------------------ staging and the swap */

typedef struct {
    unsigned char *zip;
    size_t zn;
    char zsha[65], admin[65], x64[65], arm64[65];
    char *payload;
} release;

static const char *pe_dir;
static const char *app_pe = "app-x64.exe";   /* the PE fixture the zip holds as the app */

/* the app zip of version 1.5.3 (the PE fixture's ProductVersion); drop: a required file left
 * out; admin: the worker's bytes; badname: a bad entry added */
static void make_release(release *r, const char *winver, const char *drop, const char *admin, int badname) {
    char pe[1100];
    snprintf(pe, sizeof pe, "%s/%s", pe_dir, app_pe);
    size_t pen;
    char *app = rfile(pe, &pen);
    CHECK(app != NULL, "the app PE fixture");
    if (!admin) admin = "worker";
    test_zfile all[] = {
        {"RB Stems Plus.exe", app, pen},
        {"x64/", "", 0},
        {"x64/rbsp-core.dll", "core x64", 8},
        {"arm64/rbsp-core.dll", "core arm64", 10},
        {"rbsp-watch-x64.exe", "watch x64", 9},
        {"rbsp-watch-arm64.exe", "watch arm64", 11},
        {"admin/RB Stems Plus.exe", admin, strlen(admin)},
        {"stub/x64/RB Stems Plus.exe", "stub-new-x64", 12},
        {"stub/arm64/RB Stems Plus.exe", "stub-new-arm64", 14},
        {"NOTICE", "notice", 6},
        {"NOTICE-windows.txt", "notice w", 8},
        {"LICENSE", "MIT", 3},
        {"../evil", "x", 1},
    };
    test_zfile f[16];
    int nf = 0;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        if (drop && !strcmp(all[i].name, drop)) continue;
        if (!badname && !strcmp(all[i].name, "../evil")) continue;
        f[nf++] = all[i];
    }
    r->zip = test_zip(f, nf, &r->zn);
    sha_hex(r->zip, r->zn, r->zsha);
    sha_hex(admin, strlen(admin), r->admin);
    sha_hex("stub-new-x64", 12, r->x64);
    sha_hex("stub-new-arm64", 14, r->arm64);
    r->payload = mkpayload(winver, winver, r->zsha, r->admin, r->x64, r->arm64);
    free(app);
}

static void release_free(release *r) {
    free(r->zip);
    free(r->payload);
}

/* an installed 1.5.2 (its folder, current, the old stub, its installed-release) and
 * 1.5.3 served */
static void installed_152(world *w, const char *scratch, const char *name, release *r) {
    world_new(w, scratch, name, "1.5.2");
    char p[1200];
    path(p, sizeof p, w->apps, "1.5.2");
    mkdir(p, 0755);
    path(p, sizeof p, w->apps, "1.5.2/RB Stems Plus.exe");
    wfile(p, "old app", 7);
    snprintf(fo.self, sizeof fo.self, "%s", p);
    CHECK(!update_self_capture(&w->e), "%s: the running exe's id taken at start", name);
    path(p, sizeof p, w->apps, "current");
    wfile(p, "1.5.2\n", 6);
    path(p, sizeof p, w->apps, "RB Stems Plus.exe");
    wfile(p, "stub-old", 8);
    char *inst = mkpayload("1.5.2", "1.5.2", NULL, NULL, NULL, NULL);
    path(p, sizeof p, w->support, "installed-release");
    put_pair(p, inst);
    free(inst);
    make_release(r, "1.5.3", NULL, NULL, 0);
    serve_payload(NULL, r->payload, &rel);
    char url[512];
    update_url(BASE, "1.5.3", ZIP, url, sizeof url);
    serve(url, r->zip, r->zn);
    CHECK(update_check(&w->e, &m) == UPDATE_CHECK_OK && !strcmp(m.payload_version, "1.5.3"), "%s: 1.5.3 found", name);
}

/* a variant of 1.5.3's payload, saved as if the check had taken it, and its zip served */
static void take(world *w, release *r) {
    update_store st;
    update_store_init(&w->e, &st);
    unsigned char sig[SIG_DER_MAX];
    size_t sn = test_sign(&rel, r->payload, strlen(r->payload), sig);
    CHECK(payload_accept(&st.files, r->payload, strlen(r->payload), sig, sn, &m) == PAYLOAD_OK, "the variant saved");
    char url[512];
    update_url(BASE, m.payload_version, ZIP, url, sizeof url);
    serve(url, r->zip, r->zn);
}

/* nothing of a failed staging is left in the app folder */
static int apps_clean(const world *w) {
    DIR *d = opendir(w->apps);
    int ok = 1;
    for (struct dirent *e; d && (e = readdir(d));) ok &= strncmp(e->d_name, ".staging", 8) != 0 && strcmp(e->d_name, "1.5.3") != 0;
    if (d) closedir(d);
    return ok;
}

/* UH2's swap: the checked staging folder moved away (to the world's root, as moved-staging) and an empty
 * one made at its name, between the check and the rename */
static char swap_staging[1300];
static int staging_swap_hook(int step) {
    if (step != UPDATE_STEP_STAGED || !swap_staging[0]) return 0;
    char to[1400];
    snprintf(to, sizeof to, "%s", swap_staging);
    char *cut = strrchr(to, '/');
    *cut = 0;
    cut = strrchr(to, '/');
    snprintf(cut, sizeof to - (size_t)(cut - to), "/moved-staging");
    if (rename(swap_staging, to) || mkdir(swap_staging, 0755)) abort();
    return 0;
}
/* UH3's swap: right after apps_open's check, apps moved away and a decoy made at its name whose
 * current names this app (1.5.2), once */
static char swap_apps[1100];
static int apps_swap_hook(int step) {
    if (step != UPDATE_STEP_APPS_OPEN || !swap_apps[0]) return 0;
    char mv[1200], p[1300];
    snprintf(mv, sizeof mv, "%s-moved", swap_apps);
    if (rename(swap_apps, mv) || mkdir(swap_apps, 0755)) abort();
    snprintf(p, sizeof p, "%s/current", swap_apps);
    wfile(p, "1.5.2\n", 6);
    swap_apps[0] = 0;
    return 0;
}

/* every delete in the apps folder through one checked handle (UH1-UH8): the sweep's stale folders
 * go from the folder it listed even when apps is swapped before the delete (a decoy of the name in
 * the new folder stays); an apps folder that isn't this app's deletes nothing; a delete that fails
 * keeps the folder and the step's result */
static void handles(const char *scratch) {
    world w;
    release r;
    char p[1200], q[1300], to[PAYLOAD_VERSION_MAX];
    int key;

    /* UH7: the sweep, apps swapped between the listing and the delete */
    installed_152(&w, scratch, "uh-sweep", &r);
    path(p, sizeof p, w.apps, "1.4.0");
    mkdir(p, 0755);
    snprintf(fo.rm_at_swap, sizeof fo.rm_at_swap, "%s", w.apps);
    CHECK(update_after_render(&w.e) == 0, "UH7: the sweep runs");
    snprintf(q, sizeof q, "%s-moved/1.4.0", w.apps);
    CHECK(!exists(q) && exists(p) && strstr(fo.rm_at_names, "1.4.0|"),
          "UH7: the stale folder went from the folder listed; the decoy at its name in the new one stays");
    release_free(&r);

    /* the sweep in an apps folder this app doesn't run from: nothing deleted */
    installed_152(&w, scratch, "uh-other", &r);
    path(p, sizeof p, w.apps, "1.4.0");
    mkdir(p, 0755);
    char self[1024];
    snprintf(self, sizeof self, "%s", fo.self);
    snprintf(q, sizeof q, "%s/elsewhere.exe", w.root);
    self_is(&w, q);
    CHECK(update_after_render(&w.e) == 0 && exists(p) && fo.nrm_at == 0 && strstr(logbuf, "isn't the folder this app runs from"),
          "the apps folder isn't this app's: nothing deleted, said");
    self_is(&w, self);
    /* apps named through a link to it: refused, no link followed at it */
    snprintf(q, sizeof q, "%s/apps-link", w.root);
    CHECK(!symlink(w.apps, q), "a link to apps");
    update_env l = w.e;
    l.apps = q;
    fo.nrm_at = 0;
    CHECK(update_after_render(&l) == 0 && exists(p) && fo.nrm_at == 0, "apps given as a link: nothing deleted (no link followed at it)");
    release_free(&r);

    /* UH5: the undo's delete fails: the folder stays, the result is the same */
    installed_152(&w, scratch, "uh-undo", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED && update_launch(&w.e, to) == 0, "uh-undo: staged and started");
    fo.rm_at_fail = EBUSY;
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_GENERIC && strstr(logbuf, "couldn't be removed yet"), "UH5: a delete that fails: the update failed all the same, said");
    path(p, sizeof p, w.apps, "1.5.3");
    CHECK(exists(p), "UH5: the new folder stays for the sweep");
    fo.rm_at_fail = 0;
    release_free(&r);

    /* UH1: a staging folder that can't be removed: the stage fails, nothing unpacked */
    installed_152(&w, scratch, "uh-staging", &r);
    path(p, sizeof p, w.apps, ".staging-1.5.3");
    mkdir(p, 0755);
    fo.rm_at_fail = EBUSY;
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGE_FAILED && key == UPDATE_FAIL_STAGING, "UH1: the old staging can't go: failed (STAGING)");
    path(p, sizeof p, w.apps, "1.5.3");
    CHECK(!exists(p) && strstr(fo.rm_at_names, ".staging-1.5.3|"), "UH1: nothing staged; the delete was asked through the handle");
    fo.rm_at_fail = 0;
    release_free(&r);

    /* review UH1 (round 2): apps renamed away and a copy put at its name, the exe's id taken at
     * start: the copy isn't this app's folder (its exe is another file) */
    installed_152(&w, scratch, "uh-renamed", &r);
    snprintf(q, sizeof q, "%s-orig", w.apps);
    CHECK(!rename(w.apps, q) && !mkdir(w.apps, 0755), "apps renamed away, a new one made");
    path(p, sizeof p, w.apps, "1.5.2");
    mkdir(p, 0755);
    path(p, sizeof p, w.apps, "1.5.2/RB Stems Plus.exe");
    wfile(p, "a copy", 6);
    path(p, sizeof p, w.apps, "current");
    wfile(p, "1.5.2\n", 6);
    path(p, sizeof p, w.apps, "1.4.0");
    mkdir(p, 0755);
    fo.nrm_at = 0;
    logbuf[0] = 0;
    CHECK(update_after_render(&w.e) == 0 && fo.nrm_at == 0 && exists(p) && strstr(logbuf, "isn't the folder this app runs from"),
          "UH1: the swapped-in copy: the sweep deletes nothing, said");
    CHECK(update_in_place(&w.e) == 1, "UH1: and isn't this app's place");
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGE_FAILED && key == UPDATE_FAIL_APP, "UH1: nor stages into it (not in its own place: APP)");
    /* UH's GPT re-run, G4b: the stub the last swap kept goes through the checked handle, never by
     * path: the swapped-in copy's RB Stems Plus.exe.old stays */
    path(p, sizeof p, w.apps, "RB Stems Plus.exe.old");
    wfile(p, "decoy", 5);
    CHECK(update_after_render(&w.e) == 0 && holds(p, "decoy"), "G4b: the decoy's .old stays");
    char cp[1300];
    snprintf(q, sizeof q, "%s-orig", w.apps);
    snprintf(cp, sizeof cp, "%s-copy", w.apps);
    CHECK(!rename(w.apps, cp) && !rename(q, w.apps), "this app's folder back at its name");
    wfile(p, "stub-old", 8);
    CHECK(update_after_render(&w.e) == 0 && !exists(p), "G4b: in this app's folder the .old goes");
    release_free(&r);

    /* review UH2: the staging folder swapped between its check and its rename: not renamed */
    installed_152(&w, scratch, "uh-stagingswap", &r);
    update_test_step = staging_swap_hook;
    snprintf(swap_staging, sizeof swap_staging, "%s/.staging-1.5.3", w.apps);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGE_FAILED && key == UPDATE_FAIL_STAGING, "UH2: a swapped staging folder: STAGING");
    update_test_step = NULL;
    swap_staging[0] = 0;
    path(p, sizeof p, w.apps, "1.5.3");
    snprintf(q, sizeof q, "%s/moved-staging/RB Stems Plus.exe", w.root);
    CHECK(!exists(p) && exists(q), "UH2: no version folder in apps; the checked one is where it was moved, not renamed into place");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(!exists(p), "UH2: no pending");
    release_free(&r);

    /* review UH3: current read through the checked handle: after a swap the sweep still keeps
     * the version current names in the folder it lists */
    installed_152(&w, scratch, "uh-current", &r);
    path(p, sizeof p, w.apps, "1.5.3");
    mkdir(p, 0755);
    path(p, sizeof p, w.apps, "current");
    wfile(p, "1.5.3\n", 6);   /* the new app committed meanwhile (another process) */
    snprintf(swap_apps, sizeof swap_apps, "%s", w.apps);
    update_test_step = apps_swap_hook;
    CHECK(update_after_render(&w.e) == 0, "UH3: the sweep runs");
    update_test_step = NULL;
    snprintf(q, sizeof q, "%s-moved/1.5.3", w.apps);
    CHECK(exists(q), "UH3: the version current names stays (current read through the handle, not the decoy's 1.5.2)");
    release_free(&r);
}

/* UPDATE_CHECK's and APP_FATE's codes (review S61: APP_FATE's assertions) */
static void where_codes(const char *scratch) {
    world w;
    release r;
    char self[1024], p[1200];
    installed_152(&w, scratch, "where", &r);
    CHECK(update_where(&w.e) == 0 && update_app_fate(&w.e) == 0, "in its place, the apps folder writable: 0, APP_FATE 0");
    path(p, sizeof p, w.apps, "current");
    wfile(p, "1.5.1\n", 6);
    CHECK(update_where(&w.e) == 1 && update_app_fate(&w.e) == 2, "current names another version: 1, APP_FATE 2");
    wfile(p, "1.5.2\n", 6);
    snprintf(self, sizeof self, "%s", fo.self);
    snprintf(p, sizeof p, "%s/elsewhere.exe", w.root);
    self_is(&w, p);
    CHECK(update_where(&w.e) == 1 && update_app_fate(&w.e) == 2, "run from elsewhere: 1, APP_FATE 2");
    self_is(&w, self);
    const char *own = w.e.own, *apps = w.e.apps;
    w.e.own = NULL;
    CHECK(update_where(&w.e) == 1 && update_app_fate(&w.e) == 2, "no own version: 1, APP_FATE 2");
    w.e.own = own;
    w.e.apps = NULL;
    CHECK(update_where(&w.e) == 1 && update_app_fate(&w.e) == 2, "no apps folder: 1, APP_FATE 2");
    w.e.apps = apps;
    if (chmod(w.apps, 0555)) abort();
    CHECK(update_where(&w.e) == 2 && update_app_fate(&w.e) == 2, "the apps folder read-only: 2, APP_FATE 2");
    if (chmod(w.apps, 0755)) abort();
    CHECK(update_where(&w.e) == 0 && update_app_fate(&w.e) == 0, "back in place: 0, APP_FATE 0");
    release_free(&r);
}

static void stage(const char *scratch) {
    world w;
    release r;
    char to[PAYLOAD_VERSION_MAX], p[1200], url[512];
    int key;
    installed_152(&w, scratch, "stage", &r);
    CHECK(update_in_place(&w.e) == 0, "the old app is in its place");

    /* refusals before anything is downloaded or changed */
    char self[1024];
    snprintf(self, sizeof self, "%s", fo.self);
    snprintf(p, sizeof p, "%s/elsewhere.exe", w.root);
    wfile(p, "x", 1);
    self_is(&w, p);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGE_FAILED && key == UPDATE_FAIL_APP, "not in its own place: refused");
    self_is(&w, self);
    update_env newer = as(&w, "1.5.3");
    CHECK(update_stage(&newer, to, &key) == UPDATE_STAGE_FAILED && key == UPDATE_FAIL_NONE, "no newer app: nothing to do");

    /* the download: damaged bytes, refused after a second try */
    update_url(BASE, "1.5.3", ZIP, url, sizeof url);
    unsigned char *bad = malloc(r.zn);
    memcpy(bad, r.zip, r.zn);
    bad[r.zn / 2] ^= 1;
    serve(url, bad, r.zn);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGE_FAILED && key == UPDATE_FAIL_DOWNLOAD && find(url)->hits == 2, "a damaged zip: refused (2 tries)");
    char partrel[128];
    snprintf(partrel, sizeof partrel, "update/" ZIP ".%.12s.part", r.zsha);
    path(p, sizeof p, w.support, partrel);
    CHECK(!exists(p) && apps_clean(&w), "nothing kept");
    free(bad);
    /* dropped half-way: offline, the part kept; the next try resumes it */
    route *z = serve(url, r.zip, r.zn);
    z->cut = r.zn / 2;
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGE_OFFLINE, "the connection dropped: offline");
    struct stat st;
    CHECK(lstat(p, &st) == 0 && (size_t)st.st_size == r.zn / 2, "half of it kept");
    z->cut = 0;
    z->no_range = 1;   /* first a server that sends the whole file again */
    z->cut = r.zn / 3;
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGE_OFFLINE && z->last_from == (int64_t)(r.zn / 2), "asked to resume");
    CHECK(lstat(p, &st) == 0 && (size_t)st.st_size == r.zn / 3, "a 200 to the range request started it over");
    z->no_range = 0;
    z->cut = 0;

    /* the checks after unpacking, each on a variant */
    struct {
        const char *what, *winver, *drop, *admin;
        int badname, want;
    } variants[] = {
        {"the worker isn't the one named", "1.5.3", NULL, "another worker", 0, UPDATE_FAIL_COPY_CHECK},
        {"LICENSE missing", "1.5.3", "LICENSE", NULL, 0, UPDATE_FAIL_COPY_CHECK},
        {"the arm64 stub missing", "1.5.3", "stub/arm64/RB Stems Plus.exe", NULL, 0, UPDATE_FAIL_COPY_CHECK},
        {"an entry going up", "1.5.3", NULL, NULL, 1, UPDATE_FAIL_COPY},
    };
    for (size_t i = 0; i < sizeof variants / sizeof variants[0]; i++) {
        release v;
        make_release(&v, variants[i].winver, variants[i].drop, variants[i].admin, variants[i].badname);
        if (variants[i].admin) {   /* the payload names the real worker, the zip holds another */
            free(v.payload);
            v.payload = mkpayload(variants[i].winver, variants[i].winver, v.zsha, r.admin, v.x64, v.arm64);
        }
        take(&w, &v);
        CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGE_FAILED && key == variants[i].want, "%s: refused (%d)", variants[i].what, key);
        CHECK(apps_clean(&w), "%s: nothing left in the app folder", variants[i].what);
        release_free(&v);
    }
    take(&w, &r);
    /* leftovers of earlier tries: a staging folder and the version folder itself */
    path(p, sizeof p, w.apps, ".staging-1.5.3");
    mkdir(p, 0755);
    path(p, sizeof p, w.apps, ".staging-1.5.3/junk");
    wfile(p, "j", 1);
    path(p, sizeof p, w.apps, "1.5.3");
    mkdir(p, 0755);
    path(p, sizeof p, w.apps, "1.5.3/junk");
    wfile(p, "j", 1);
    char other[1200];
    path(other, sizeof other, w.support, "update/" ZIP ".0123456789ab.part");
    wfile(other, "another release's bytes", 23);
    path(p, sizeof p, w.support, partrel);
    wfile(p, r.zip, r.zn / 4);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED && !strcmp(to, "1.5.3"), "staged");
    CHECK(z->last_from == (int64_t)(r.zn / 4), "the part resumed");
    CHECK(!exists(other), "another release's part removed");
    path(p, sizeof p, w.apps, "1.5.3/junk");
    CHECK(!exists(p), "the earlier try's folder replaced");
    path(p, sizeof p, w.apps, "1.5.3/stub/arm64/RB Stems Plus.exe");
    CHECK(holds(p, "stub-new-arm64"), "unpacked into its version folder");
    CHECK(apps_clean(&w) == 0, "(the version folder is there)");
    path(p, sizeof p, w.apps, ".staging-1.5.3");
    CHECK(!exists(p), "no staging folder left");
    path(p, sizeof p, w.support, "update/pending");
    size_t pn;
    char *pend = rfile(p, &pn);
    update_pending pr;
    CHECK(pend && update_pending_parse(pend, pn, &pr) && !strcmp(pr.from, "1.5.2") && !strcmp(pr.to, "1.5.3") && pr.pid == (uint32_t)getpid(),
          "pending names both apps and this process");
    free(pend);
    path(p, sizeof p, w.support, "update/payload.json");
    CHECK(holds(p, r.payload), "the payload that installs it kept for the new app");
    path(p, sizeof p, w.support, "update/" ZIP);
    CHECK(!exists(p), "the zip removed");

    /* the launch, then the new app */
    CHECK(update_launch(&w.e, to) == 0 && fo.nspawn == 1 && !strcmp(fo.spawn_arg[0], "--swap"), "the new app started with --swap");
    path(p, sizeof p, w.apps, "1.5.3/RB Stems Plus.exe");
    CHECK(!strcmp(fo.spawn_exe[0], p), "from its version folder");
    update_env n = as(&w, "1.5.3");
    snprintf(fo.self, sizeof fo.self, "%s", p);
    CHECK(update_swap_wait(&w.e, 0) == 1, "--swap in the old app: no such update");
    fo.wait_r = ETIMEDOUT;
    CHECK(update_swap_wait(&n, 0) == ETIMEDOUT, "the old app still running: it waits");
    fo.wait_r = 0;
    CHECK(update_swap_wait(&n, 0) == 0, "the old app gone: ready");
    CHECK(update_at_start(&n) == UPDATE_FAIL_NONE, "the new app's start: nothing to say");
    fo.watcher_running = 1;
    fo.nspawn = 0;
    CHECK(update_after_render(&n) == 0, "the commit after the first render");
    path(p, sizeof p, w.apps, "current");
    CHECK(holds(p, "1.5.3\n"), "current moved");
    static payload_manifest inst;
    CHECK(update_installed(&n, &inst) == PAYLOAD_OK && !strcmp(inst.payload_version, "1.5.3"), "installed-release");
    path(p, sizeof p, w.support, "shown-version");
    CHECK(holds(p, "1.5.3\n1.5.3\n") && !strcmp(fo.display, "1.5.3"), "shown and DisplayVersion");
    path(p, sizeof p, w.apps, "RB Stems Plus.exe");
    CHECK(holds(p, "stub-new-x64"), "the stub of the machine's arch swapped in");
    path(p, sizeof p, w.apps, "RB Stems Plus.exe.old");
    CHECK(holds(p, "stub-old"), "the old one kept");
    path(p, sizeof p, w.apps, "RB Stems Plus.exe.new");
    CHECK(!exists(p), "no .new left");
    path(p, sizeof p, w.apps, "1.5.2");
    CHECK(!strcmp(fo.stopped, p) && fo.nspawn == 1 && !strcmp(fo.spawn_arg[0], "--watcher"), "the old watcher stopped, the new one started");
    path(p, sizeof p, w.apps, "RB Stems Plus.exe");
    CHECK(!strcmp(fo.spawn_exe[0], p), "through the stub");
    path(p, sizeof p, w.apps, "1.5.2");
    CHECK(!exists(p), "the old folder deleted");
    static const char *const gone[] = {"update/pending", "update/started", "update/payload.json", "update/payload.json.sig"};
    for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++) {
        path(p, sizeof p, w.support, gone[i]);
        CHECK(!exists(p), "%s removed", gone[i]);
    }
    /* the next start: the old stub goes, and leftovers */
    CHECK(update_at_start(&n) == UPDATE_FAIL_NONE, "the next start: nothing to say");
    static const char *const left[] = {"9.9.9", ".staging-2.0.0", "keep", "1.5.3"};
    for (size_t i = 0; i < 3; i++) {
        path(p, sizeof p, w.apps, left[i]);
        mkdir(p, 0755);
    }
    CHECK(update_after_render(&n) == 0, "after its first render");
    path(p, sizeof p, w.apps, "RB Stems Plus.exe.old");
    CHECK(!exists(p), "the old stub removed");
    for (size_t i = 0; i < 4; i++) {
        path(p, sizeof p, w.apps, left[i]);
        CHECK(exists(p) == (i >= 2), "%s %s", left[i], i >= 2 ? "kept" : "removed");
    }
    release_free(&r);

    /* the app's ProductVersion isn't the one the payload names (its own world: 1.5.4 raises the floor) */
    installed_152(&w, scratch, "productversion", &r);
    release v;
    make_release(&v, "1.5.4", NULL, NULL, 0);
    take(&w, &v);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGE_FAILED && key == UPDATE_FAIL_COPY_CHECK, "ProductVersion 1.5.3 for 1.5.4: refused");
    path(p, sizeof p, w.apps, "1.5.4");
    CHECK(!exists(p) && apps_clean(&w), "nothing left");
    release_free(&v);
    release_free(&r);

    /* the right ProductVersion, another ProductName */
    installed_152(&w, scratch, "productname", &r);
    app_pe = "notapp-x64.exe";
    make_release(&v, "1.5.3", NULL, NULL, 0);
    app_pe = "app-x64.exe";
    take(&w, &v);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGE_FAILED && key == UPDATE_FAIL_COPY_CHECK, "ProductName not RB Stems Plus: refused");
    CHECK(apps_clean(&w), "nothing left");
    release_free(&v);
    release_free(&r);
}

static void rollbacks(const char *scratch) {
    world w;
    release r;
    char to[PAYLOAD_VERSION_MAX], p[1200];
    int key;

    /* the new app never rendered: the old one, started again, undoes it */
    installed_152(&w, scratch, "never", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED && update_launch(&w.e, to) == 0, "never: staged and started");
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_GENERIC, "the old app's next start: the update failed");
    path(p, sizeof p, w.apps, "1.5.3");
    CHECK(!exists(p), "the new folder removed");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(!exists(p), "pending removed");
    path(p, sizeof p, w.apps, "current");
    CHECK(holds(p, "1.5.2\n"), "current never moved");
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_GENERIC, "WC4: kept until UPDATE_RESULT says it");
    update_result_keep(UPDATE_FAIL_GENERIC);
    CHECK(update_result_take(&w.e, NULL) == 1 && update_at_start(&w.e) == UPDATE_FAIL_NONE, "said once: then gone");
    release_free(&r);

    /* --swap's checks failed in the new app */
    installed_152(&w, scratch, "swapfail", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED && update_launch(&w.e, to) == 0, "swapfail: staged and started");
    update_env n = as(&w, to);
    fo.nspawn = 0;
    CHECK(update_swap_failed(&n, UPDATE_FAIL_SWAP) == 0 && fo.nspawn == 1 && !fo.spawn_arg[0][0], "the old app started again through the stub");
    path(p, sizeof p, w.apps, "RB Stems Plus.exe");
    CHECK(!strcmp(fo.spawn_exe[0], p), "(the stub)");
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_SWAP, "the old app says why");
    CHECK(update_after_render(&w.e) == 0, "and after its first render");
    path(p, sizeof p, w.apps, "1.5.3");
    CHECK(!exists(p), "the new folder removed as a leftover");
    release_free(&r);

    /* past the commit point: the old app (started by hand) leaves it; the new one finishes it */
    installed_152(&w, scratch, "pastcommit", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "pastcommit: staged");
    path(p, sizeof p, w.apps, "current");
    wfile(p, "1.5.3\n", 6);
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_NONE, "the old app leaves an update past its commit point");
    path(p, sizeof p, w.apps, "1.5.3");
    CHECK(exists(p), "the new folder stays");
    n = as(&w, to);
    CHECK(update_at_start(&n) == UPDATE_FAIL_NONE && update_after_render(&n) == 0, "the new app finishes it");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(!exists(p), "done");
    path(p, sizeof p, w.apps, "1.5.2");
    CHECK(!exists(p), "the old folder deleted");
    release_free(&r);

    /* the launch failed */
    installed_152(&w, scratch, "nolaunch", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "nolaunch: staged");
    fo.spawn_fail = ENOENT;
    CHECK(update_launch(&w.e, to) == ENOENT, "the new app didn't start");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(!exists(p), "pending removed: the app keeps running as it was");
    release_free(&r);

    /* a stale record names neither app */
    world_new(&w, scratch, "stale", "1.5.0");
    path(p, sizeof p, w.support, "update");
    mkdir(p, 0755);
    path(p, sizeof p, w.support, "update/pending");
    wfile(p, "1.0.0\n1.1.0\n1\n0\n", 16);
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_NONE && !exists(p), "a stale pending removed");
    wfile(p, "garbage", 7);
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_NONE && !exists(p), "a broken one too");
    path(p, sizeof p, w.support, "update/result");
    wfile(p, "failed copy\n", 12);
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_COPY && exists(p), "a result is read, and kept for UPDATE_RESULT (WC4)");
    update_result_keep(UPDATE_FAIL_COPY);
    CHECK(update_result_take(&w.e, NULL) == 1 && !exists(p) && update_at_start(&w.e) == UPDATE_FAIL_NONE, "UPDATE_RESULT removes it: said once");

    /* the stub unchanged: no swap */
    installed_152(&w, scratch, "samestub", &r);
    path(p, sizeof p, w.apps, "RB Stems Plus.exe");
    wfile(p, "stub-new-x64", 12);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "samestub: staged");
    n = as(&w, to);
    CHECK(update_after_render(&n) == 0, "committed");
    path(p, sizeof p, w.apps, "RB Stems Plus.exe.old");
    CHECK(!exists(p), "the same stub: not replaced");
    CHECK(fo.nspawn == 0, "no watcher was running: none started");
    release_free(&r);
}

/* ------------------------------------------------------------------ the steps that need the shell (W3b) */

static int quits;
static void quit_cb(void *ctx) {
    (void)ctx;
    quits++;
}

static void shell_steps(const char *scratch) {
    world w;
    release r;
    char p[1200], url[512];
    const char *line = NULL;

    /* UPDATE_SWAP: staged, the new app started with --swap, then the shell's quit */
    installed_152(&w, scratch, "stepswap", &r);
    quits = 0;
    path(p, sizeof p, w.support, "update/pending");
    CHECK(update_step_swap(&w.e, NULL, NULL) == UPDATE_STAGE_FAILED && fo.nspawn == 0 && !exists(p) && net_calls == 2,
          "UPDATE_SWAP with no quit from the shell: 3, nothing downloaded or started");
    CHECK(update_step_swap(&w.e, quit_cb, NULL) == 0 && fo.nspawn == 1 && !strcmp(fo.spawn_arg[0], "--swap") && quits == 1,
          "UPDATE_SWAP: 0, the new app started with --swap, then the shell's quit, once");
    CHECK(exists(p), "pending written for the new app");
    update_env n = as(&w, "1.5.3");
    CHECK(update_swap_wait(&n, 0) == 0, "the new app's --swap finds its update");
    release_free(&r);

    /* the new app didn't start: 3, pending removed, the app not quit */
    installed_152(&w, scratch, "stepnolaunch", &r);
    fo.spawn_fail = ENOENT;
    quits = 0;
    path(p, sizeof p, w.support, "update/pending");
    CHECK(update_step_swap(&w.e, quit_cb, NULL) == UPDATE_STAGE_FAILED && quits == 0 && !exists(p),
          "UPDATE_SWAP, the start failed: 3, no quit, pending removed");
    release_free(&r);

    /* offline, and a damaged download: update_stage's codes, no quit */
    installed_152(&w, scratch, "stepoffline", &r);
    update_url(BASE, "1.5.3", ZIP, url, sizeof url);
    find(url)->offline = 1;
    quits = 0;
    CHECK(update_step_swap(&w.e, quit_cb, NULL) == UPDATE_STAGE_OFFLINE && quits == 0 && fo.nspawn == 0, "UPDATE_SWAP offline: 1, no quit");
    unsigned char *bad = malloc(r.zn);
    memcpy(bad, r.zip, r.zn);
    bad[r.zn / 2] ^= 1;
    serve(url, bad, r.zn);
    free(bad);
    CHECK(update_step_swap(&w.e, quit_cb, NULL) == UPDATE_STAGE_FAILED && quits == 0 && fo.nspawn == 0, "UPDATE_SWAP, a damaged zip: 3, no quit");
    release_free(&r);

    /* UPDATE_RESULT: the start's answer, said once */
    update_result_keep(UPDATE_FAIL_NONE);
    CHECK(update_result_take(NULL, &line) == 0 && line && !line[0], "UPDATE_RESULT: nothing kept, nothing to say");
    update_result_keep(UPDATE_BUSY);
    CHECK(update_result_take(NULL, &line) == 0, "UPDATE_BUSY keeps nothing");
    update_result_keep(UPDATE_FAIL_COPY_CHECK);
    CHECK(update_result_take(NULL, &line) == 1 && !strcmp(line, "failed copy-check"), "a failed update's key: its line");
    CHECK(update_result_take(NULL, &line) == 0 && !line[0], "said once");
    update_result_keep(UPDATE_FAIL_GENERIC);
    CHECK(update_result_take(NULL, NULL) == 1, "\"failed\" alone (the new app never rendered) is said too");
    update_result_keep(99);
    CHECK(update_result_take(NULL, &line) == 0, "a key that isn't one: nothing");

    /* end to end: the new app failed its checks; the old app's start keeps the answer for UPDATE_RESULT */
    installed_152(&w, scratch, "stepresult", &r);
    CHECK(update_step_swap(&w.e, quit_cb, NULL) == 0, "stepresult: started");
    n = as(&w, "1.5.3");
    CHECK(update_swap_failed(&n, UPDATE_FAIL_SWAP) == 0, "the new app's checks failed");
    update_result_keep(update_at_start(&w.e));
    path(p, sizeof p, w.support, "update/result");
    CHECK(exists(p), "WC4: the result kept on disk until UPDATE_RESULT says it");
    update_result_keep(UPDATE_FAIL_NONE);   /* the process ended before UPDATE_RESULT */
    update_result_keep(update_at_start(&w.e));
    CHECK(update_result_take(&w.e, &line) == 1 && !strcmp(line, "failed swap") && !exists(p),
          "the next start says it again; UPDATE_RESULT then removes it");
    update_result_keep(update_at_start(&w.e));
    CHECK(update_result_take(&w.e, &line) == 0, "said once: the start after that has nothing");
    release_free(&r);

    /* WC4: an undone update (the new app never started) keeps its generic failure too */
    installed_152(&w, scratch, "stepundone", &r);
    CHECK(update_step_swap(&w.e, quit_cb, NULL) == 0, "stepundone: started");
    int k = update_at_start(&w.e);
    path(p, sizeof p, w.support, "update/result");
    CHECK(k == UPDATE_FAIL_GENERIC && exists(p), "the undone update's \"failed\" kept on disk (%d)", k);
    update_result_keep(UPDATE_FAIL_NONE);
    update_result_keep(update_at_start(&w.e));
    CHECK(update_result_take(&w.e, &line) == 1 && !strcmp(line, "failed") && !exists(p), "said at the next start, then removed (%s)", line);
    release_free(&r);
}

/* ------------------------------------------------------------------ WC3: the sweep never takes a folder being staged */

static update_env sweep_env;
static atomic_int sweep_held, sweep_go, swap_started, swap_done;
static int sweep_hook(int step) {
    if (step == UPDATE_STEP_SWEEP) {
        sweep_held = 1;
        for (int i = 0; !sweep_go && i < 2000; i++) plat_sleep_ms(5);   /* 10 s at most: a fail, never a hang */
    }
    return 0;
}
static void *after_render_thread(void *arg) {
    (void)arg;
    update_after_render(&sweep_env);
    return NULL;
}
static int swap_r = -9;
static void *step_swap_thread(void *arg) {
    (void)arg;
    swap_started = 1;
    swap_r = update_step_swap(&sweep_env, quit_cb, NULL);
    swap_done = 1;
    return NULL;
}

static void sweep_during_swap(const char *scratch) {
    world w;
    release r;
    char p[1200];
    installed_152(&w, scratch, "sweepswap", &r);
    sweep_env = w.e;
    sweep_held = sweep_go = swap_started = swap_done = 0;
    quits = 0;
    update_test_step = sweep_hook;
    pthread_t a, b;
    int err = pthread_create(&a, NULL, after_render_thread, NULL);
    CHECK(!err, "WC3: started the sweep thread (%d)", err);
    if (err) goto done;
    for (int i = 0; !sweep_held && i < 2000; i++) plat_sleep_ms(5);   /* the sweep is about to list the apps folder (10 s at most) */
    CHECK(sweep_held, "WC3: the after-render sweep reached its listing");
    err = pthread_create(&b, NULL, step_swap_thread, NULL);
    CHECK(!err, "WC3: started the swap thread (%d)", err);
    if (!err) {
        for (int i = 0; !swap_started && i < 200; i++) plat_sleep_ms(5);
        CHECK(swap_started, "WC3: the swap thread entered its work");
        /* Give an unlocked swap a chance to stage and launch before the sweep lists the folders. */
        for (int i = 0; !swap_done && i < 200; i++) plat_sleep_ms(5);
    }
    sweep_go = 1;
    pthread_join(a, NULL);
    if (!err) {
        pthread_join(b, NULL);
        path(p, sizeof p, w.apps, "1.5.3");
        CHECK(swap_r == 0 && quits == 1 && fo.nspawn == 1 && exists(p), "WC3: UPDATE_SWAP during the sweep: the staged folder survives and the new app starts (%d)", swap_r);
    }
done:
    update_test_step = NULL;
    release_free(&r);
}

/* ------------------------------------------------------------------ WU10: no new update while this app's own commits */

static update_env stage_env;
static int stage_r = -1, stage_key = -9;
static int stage_hook(int step) {
    if (step == UPDATE_STEP_WATCHER) {
        char to[PAYLOAD_VERSION_MAX];
        stage_r = update_stage(&stage_env, to, &stage_key);
    }
    return 0;
}

static void stage_during_commit(const char *scratch) {
    world w;
    release r;
    char to[PAYLOAD_VERSION_MAX], p[1200];
    int key;
    installed_152(&w, scratch, "stageduring", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "stageduring: staged");
    stage_env = as(&w, to);
    stage_r = -1;
    update_test_step = stage_hook;
    CHECK(update_after_render(&stage_env) == 0, "stageduring: committed");
    update_test_step = NULL;
    CHECK(stage_r == UPDATE_STAGE_FAILED && stage_key == UPDATE_FAIL_NONE && strstr(logbuf, "isn't finished yet"),
          "update_stage while this app's own update commits: refused, logged (%d)", stage_r);
    path(p, sizeof p, w.support, "update/pending");
    CHECK(!exists(p), "the commit finished after it");
    release_free(&r);
}

/* ------------------------------------------------------------------ WU6: installed-release only over an older record */

static char newer_dir[1200];
static int newer_hook(int step) {
    if (step == UPDATE_STEP_INSTALLED) {   /* the native PAYLOAD step records a newer payload for this app meanwhile */
        char *j = mkpayload("1.5.4", "1.5.3", NULL, NULL, NULL, NULL);
        put_pair(newer_dir, j);
        free(j);
    }
    return 0;
}

static void installed_newer(const char *scratch) {
    world w;
    release r;
    char to[PAYLOAD_VERSION_MAX], p[1200];
    int key;
    static payload_manifest inst;
    installed_152(&w, scratch, "installednewer", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "installednewer: staged");
    update_env n = as(&w, to);
    path(newer_dir, sizeof newer_dir, w.support, "installed-release");
    update_test_step = newer_hook;
    CHECK(update_after_render(&n) == 0, "installednewer: committed");
    update_test_step = NULL;
    CHECK(update_installed(&n, &inst) == PAYLOAD_OK && !strcmp(inst.payload_version, "1.5.4"),
          "a newer record for this app, written during the commit, stays (%s)", inst.payload_version);
    path(p, sizeof p, w.support, "update/pending");
    CHECK(!exists(p), "the commit finished");
    char shown[PAYLOAD_VERSION_MAX];
    update_shown_now(&n, shown);
    CHECK(!strcmp(shown, "1.5.4"), "the version shown follows the kept record (%s)", shown);
    release_free(&r);

    /* the usual case: the record names the old app, so the commit writes its own */
    installed_152(&w, scratch, "installedolder", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "installedolder: staged");
    n = as(&w, to);
    CHECK(update_after_render(&n) == 0 && update_installed(&n, &inst) == PAYLOAD_OK && !strcmp(inst.payload_version, "1.5.3"),
          "installedolder: the commit's own record written");
    release_free(&r);
}

/* ------------------------------------------------------------------ the commit's stops (U1, U5) */

static int stop_at, stop_errno, stop_calls, stub_missing;
static char stub_path[1200];
/* before each of the commit's writes: the stub's name must exist; at stop_at, the write fails */
static int step_hook(int step) {
    stop_calls++;
    if (!exists(stub_path)) stub_missing++;
    return step == stop_at ? stop_errno : 0;
}

static void commit_stops(const char *scratch) {
    world w;
    release r;
    char to[PAYLOAD_VERSION_MAX], p[1200], name[64];
    int key;
    static payload_manifest inst;
    /* a stop before each step of the stub's swap: the stub's name is there throughout; the commit
     * finishes without it, and a later start swaps it */
    static const int steps[] = {UPDATE_STEP_STUB_NEW, UPDATE_STEP_STUB_OLD, UPDATE_STEP_STUB_REPLACE};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        snprintf(name, sizeof name, "stubstop%d", steps[i]);
        installed_152(&w, scratch, name, &r);
        CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "%s: staged", name);
        update_env n = as(&w, to);
        path(stub_path, sizeof stub_path, w.apps, "RB Stems Plus.exe");
        stop_at = steps[i], stop_errno = steps[i] == UPDATE_STEP_STUB_REPLACE ? EBUSY : EIO, stop_calls = stub_missing = 0;
        update_test_step = step_hook;
        CHECK(update_after_render(&n) == 0, "%s: the commit finishes without the stub", name);
        CHECK(holds(stub_path, "stub-old") && stub_missing == 0 && stop_calls >= 1, "%s: the old stub stays under its name throughout", name);
        path(p, sizeof p, w.support, "update/pending");
        CHECK(!exists(p), "%s: the records don't wait for the stub", name);
        stop_at = 0;
        CHECK(update_after_render(&n) == 0 && holds(stub_path, "stub-new-x64") && stub_missing == 0, "%s: the next start swaps it", name);
        path(p, sizeof p, w.apps, "RB Stems Plus.exe.old");
        CHECK(holds(p, "stub-old"), "%s: the old one kept as .old", name);
        CHECK(update_after_render(&n) == 0 && !exists(p) && holds(stub_path, "stub-new-x64"), "%s: and removed at the start after", name);
        update_test_step = NULL;
        release_free(&r);
    }

    /* installed-release can't be written: the commit stops with that errno, the records stay, and
     * the next start finishes it */
    installed_152(&w, scratch, "installedfail", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "installedfail: staged");
    update_env n = as(&w, to);
    path(stub_path, sizeof stub_path, w.apps, "RB Stems Plus.exe");
    stop_at = UPDATE_STEP_INSTALLED, stop_errno = ENOSPC;
    update_test_step = step_hook;
    CHECK(update_after_render(&n) == ENOSPC, "installed-release's ENOSPC returned");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(exists(p), "pending kept");
    path(p, sizeof p, w.support, "update/payload.json");
    CHECK(exists(p), "and the payload that installs it");
    CHECK(update_installed(&n, &inst) == PAYLOAD_NONE, "no installed-release for the new app yet");
    path(p, sizeof p, w.apps, "current");
    CHECK(holds(p, "1.5.3\n"), "current moved (past the commit point)");
    stop_at = 0;
    CHECK(update_at_start(&n) == UPDATE_FAIL_NONE && update_after_render(&n) == 0, "the next start finishes it");
    CHECK(update_installed(&n, &inst) == PAYLOAD_OK && !strcmp(inst.payload_version, "1.5.3"), "installed-release written");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(!exists(p), "the records gone");
    update_test_step = NULL;
    release_free(&r);
}

/* a real interruption of the commit at each step of the stub's swap (longjmp out of it, as a
 * process killed there): the stub's name is there, pending kept, and a fresh start finishes it */
static jmp_buf jump;
static int jump_at;
static int jump_hook(int step) {
    if (!exists(stub_path)) stub_missing++;
    if (step == jump_at) longjmp(jump, 1);
    return 0;
}

static void commit_kills(const char *scratch) {
    static const int steps[] = {UPDATE_STEP_STUB_NEW, UPDATE_STEP_STUB_OLD, UPDATE_STEP_STUB_REPLACE};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        static world w;   /* static: kept across the longjmp */
        static release r;
        static char to[PAYLOAD_VERSION_MAX], name[64], p[1200];
        static update_env n;
        int key;
        snprintf(name, sizeof name, "stubkill%d", steps[i]);
        installed_152(&w, scratch, name, &r);
        CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "%s: staged", name);
        n = as(&w, to);
        path(stub_path, sizeof stub_path, w.apps, "RB Stems Plus.exe");
        stub_missing = 0;
        jump_at = steps[i];
        update_test_step = jump_hook;
        if (!setjmp(jump)) {
            update_after_render(&n);
            CHECK(0, "%s: the commit wasn't interrupted", name);
        }
        update_test_killed();   /* the process ended there: no lock outlives it */
        update_test_step = NULL;
        CHECK(exists(stub_path) && stub_missing == 0, "%s: the stub's name there throughout and after", name);
        path(p, sizeof p, w.support, "update/pending");
        CHECK(exists(p), "%s: pending kept", name);
        CHECK(update_at_start(&n) == UPDATE_FAIL_NONE && update_after_render(&n) == 0, "%s: a fresh start finishes the commit", name);
        CHECK(holds(stub_path, "stub-new-x64"), "%s: the new stub in place", name);
        path(p, sizeof p, w.apps, "RB Stems Plus.exe.new");
        CHECK(!exists(p), "%s: no .new left", name);
        release_free(&r);
    }
}

/* a commit stopped after the old watcher was stopped, before the new one started: the next start
 * starts it (update\watcher), once */
static void watcher_restart(const char *scratch) {
    world w;
    release r;
    char to[PAYLOAD_VERSION_MAX], p[1200];
    int key;
    installed_152(&w, scratch, "watcherstop", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "watcherstop: staged");
    update_env n = as(&w, to);
    path(stub_path, sizeof stub_path, w.apps, "RB Stems Plus.exe");
    fo.watcher_running = 1;
    fo.watchers_left = 1;   /* a watcher found in the old folder, then none in the new one */
    stop_at = UPDATE_STEP_WATCHER, stop_errno = EINTR;
    update_test_step = step_hook;
    CHECK(update_after_render(&n) == EINTR && fo.nspawn == 0 && fo.nstop == 1, "stopped after the old watcher, before the new one");
    path(p, sizeof p, w.support, "update/watcher");
    CHECK(exists(p), "the running watcher noted");
    stop_at = 0;
    CHECK(update_after_render(&n) == 0, "the commit runs again");
    CHECK(fo.nspawn == 1 && !strcmp(fo.spawn_arg[0], "--watcher"), "and starts the new watcher");
    CHECK(!exists(p), "the note goes with the records");
    update_test_step = NULL;
    release_free(&r);

    /* review GH3: the list can't be read (-1): not noted, so a commit cut off after the stop
     * starts none at the next run (no watcher for a user who had none; the next logon's Run value
     * starts it) */
    installed_152(&w, scratch, "watchunknown", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "watchunknown: staged");
    n = as(&w, to);
    fo.watchers_fail = 1;
    fo.watcher_running = 1;
    stop_at = UPDATE_STEP_WATCHER, stop_errno = EINTR;
    update_test_step = step_hook;
    CHECK(update_after_render(&n) == EINTR && fo.nspawn == 0 && fo.nstop == 1, "watchunknown: stopped after the old watcher");
    path(p, sizeof p, w.support, "update/watcher");
    CHECK(!exists(p), "an unreadable list isn't noted");
    stop_at = 0;
    CHECK(update_after_render(&n) == 0 && fo.nspawn == 0, "watchunknown: the next run starts none");
    CHECK(!exists(p), "and notes none");
    update_test_step = NULL;
    release_free(&r);

    /* review GH2: this session's watcher noted and stopped while another session's stays (the
     * stop gives -1): the new one still starts, and the note goes with the records */
    installed_152(&w, scratch, "watchother", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "watchother: staged");
    n = as(&w, to);
    fo.watcher_running = -1;
    fo.watchers_left = 1;   /* one in the old folder, then none in the new one */
    CHECK(update_after_render(&n) == 0 && fo.nstop == 1 && fo.nspawn == 1 && !strcmp(fo.spawn_arg[0], "--watcher"),
          "watchother: one session's watcher stopped, another's left: the new one starts");
    path(p, sizeof p, w.support, "update/watcher");
    CHECK(!exists(p), "watchother: the note goes with the records");
    /* review GH4: the other session's watcher keeps the old folder, at the commit and at each
     * later start's sweep while something runs from it (or the list can't be read); the sweep
     * takes it once nothing does */
    path(p, sizeof p, w.apps, "1.5.2");
    CHECK(exists(p), "watchother: the old folder kept while another session's watcher runs from it");
    fo.running_always = 1;
    CHECK(update_after_render(&n) == 0 && exists(p), "watchother: the sweep keeps it while something runs from it");
    fo.running_always = 0, fo.running_fail = 1;
    CHECK(update_after_render(&n) == 0 && exists(p), "watchother: and while the list can't be read");
    fo.running_fail = 0;
    CHECK(update_after_render(&n) == 0 && !exists(p), "watchother: the sweep takes it once nothing runs from it");
    release_free(&r);

    /* review UF1: no watcher, but something else of ours still runs from the old folder (another
     * session's old app, an elevated process): the stop gives 0, the commit keeps the folder, and
     * the sweep takes it once nothing runs there */
    installed_152(&w, scratch, "oldapp", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "oldapp: staged");
    n = as(&w, to);
    fo.watcher_running = 0;
    fo.running_always = 1;
    path(p, sizeof p, w.apps, "1.5.2");
    CHECK(update_after_render(&n) == 0 && fo.nstop == 1 && exists(p), "oldapp: the old folder kept while something runs from it");
    fo.running_always = 0;
    CHECK(update_after_render(&n) == 0 && !exists(p), "oldapp: the sweep takes it once nothing runs from it");
    release_free(&r);

    /* review GR1: only another session runs a watcher (watchers_in, by session, finds none; the
     * stop gives -1): nothing noted and none started in this session, the other's stays */
    installed_152(&w, scratch, "watchelse", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "watchelse: staged");
    n = as(&w, to);
    fo.watcher_running = -1;
    fo.watchers_left = 0;
    CHECK(update_after_render(&n) == 0 && fo.nstop == 1 && fo.nspawn == 0,
          "watchelse: another session's watcher only: none starts in this one");
    path(p, sizeof p, w.support, "update/watcher");
    CHECK(!exists(p), "watchelse: nothing noted");
    release_free(&r);

    /* the note can't be written: the commit stops before the watcher is touched (V4) */
    installed_152(&w, scratch, "notefail", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "notefail: staged");
    n = as(&w, to);
    fo.watcher_running = 1;
    fo.watchers_left = 1;
    path(p, sizeof p, w.support, "update/watcher");
    mkdir(p, 0755);   /* the note's rename then fails */
    path(stub_path, sizeof stub_path, w.support, "update/watcher/x");
    wfile(stub_path, "x", 1);   /* (not empty: the note's rename can't replace it) */
    CHECK(update_after_render(&n) != 0 && fo.nstop == 0 && fo.nspawn == 0, "the note failed: the old watcher not stopped, nothing started");
    char pend[1200];
    path(pend, sizeof pend, w.support, "update/pending");
    CHECK(exists(pend), "pending kept");
    unlink(stub_path);
    rmdir(p);
    CHECK(update_after_render(&n) == 0 && fo.nstop == 1 && fo.nspawn == 1 && !strcmp(fo.spawn_arg[0], "--watcher"), "the retry stops it and starts the new one");
    CHECK(!exists(pend), "done");
    release_free(&r);

    /* a process under the old folder that isn't a watcher: no note, nothing started (V5) */
    installed_152(&w, scratch, "notawatcher", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "notawatcher: staged");
    n = as(&w, to);
    fo.running_always = 1;   /* (an old app still closing, say) */
    fo.watcher_running = 0;
    CHECK(update_after_render(&n) == 0 && fo.nspawn == 0, "no watcher was running: none started");
    CHECK(!exists(p), "and no note");
    release_free(&r);

    /* a process under the new folder that isn't a watcher, and a watcher stopped: one started */
    installed_152(&w, scratch, "ownbusy", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "ownbusy: staged");
    n = as(&w, to);
    fo.running_always = 1;   /* the new app itself, its other processes */
    fo.watcher_running = 1;
    CHECK(update_after_render(&n) == 0 && fo.nspawn == 1 && !strcmp(fo.spawn_arg[0], "--watcher"), "the stopped watcher replaced");
    release_free(&r);
}

static void watcher_restart_failure(const char *scratch) {
    for (int stub_only = 0; stub_only <= 1; stub_only++) {
        world w;
        char file[1200], marker[1200], pending[1200], record[UPDATE_RECORD_MAX];
        update_pending next = {.from = "1.5.2", .to = "1.5.3", .pid = 1, .started = 1};
        const char *scenario = stub_only ? "watcher-stub-only" : "watcher-spawn-fails";
        world_new(&w, scratch, scenario, next.from);
        path(file, sizeof file, w.apps, next.to);
        mkdir(file, 0755);
        path(file, sizeof file, w.apps, "1.5.3/RB Stems Plus.exe");
        wfile(file, "new app", 7);
        path(file, sizeof file, w.support, "update");
        mkdir(file, 0755);
        path(pending, sizeof pending, file, "pending");
        size_t length = update_pending_format(&next, record, sizeof record);
        wfile(pending, record, length);
        update_env n = as(&w, next.to);
        fo.watcher_running = 1;
        fo.watchers_left = 1;
        fo.spawn_fail = stub_only ? 0 : EACCES;
        fo.stub_only = stub_only;
        path(marker, sizeof marker, w.support, "update/watcher");

        int error = update_after_render(&n);
        CHECK(error == (stub_only ? ETIMEDOUT : EACCES) && exists(marker) && exists(pending) && !fo.replacement_running &&
                  strstr(logbuf, "the next start retries it") && !strstr(logbuf, " -> 1.5.3 done"),
              "review 5: %s leaves the update and restart marker pending and reports the failure", scenario);

        fo.spawn_fail = fo.stub_only = 0;
        int before = fo.nspawn;
        CHECK(update_at_start(&n) == UPDATE_FAIL_NONE && update_after_render(&n) == 0 && fo.nspawn == before + 1 &&
                  fo.replacement_running && !exists(marker) && !exists(pending),
              "review 5: %s retries successfully on the next start and clears the records", scenario);
    }
}

/* installed-release's write stopped between the signature and payload.json: the record before it
 * still counts */
static void installed_tear(const char *scratch) {
    world w;
    static payload_manifest inst;
    char shown[PAYLOAD_VERSION_MAX];
    world_new(&w, scratch, "tear", "1.4.0");
    char *c1 = mkpayload("1.4.1", "1.4.0", NULL, NULL, NULL, NULL), *c2 = mkpayload("1.4.2", "1.4.0", NULL, NULL, NULL, NULL);
    char p[1200];
    path(p, sizeof p, w.support, "installed-release");
    put_pair(p, c1);
    serve_payload(NULL, c2, &rel);
    CHECK(update_check(&w.e, &m) == UPDATE_CHECK_OK && !strcmp(m.payload_version, "1.4.2"), "tear: 1.4.2 carries the app");
    stop_at = UPDATE_STEP_INSTALLED_HALF, stop_errno = EINTR;
    path(stub_path, sizeof stub_path, w.apps, "x");   /* (no stub here) */
    update_test_step = step_hook;
    update_seen(&w.e, &m, shown);
    stop_at = 0;
    update_test_step = NULL;
    CHECK(update_installed(&w.e, &inst) == PAYLOAD_OK && !strcmp(inst.payload_version, "1.4.1"),
          "the torn record: the one before it counts");
    CHECK(update_seen(&w.e, &m, shown) == 0 && update_installed(&w.e, &inst) == PAYLOAD_OK && !strcmp(inst.payload_version, "1.4.2"),
          "the next payload seen writes it whole");
    free(c1), free(c2);
}

/* a signed installed-release whose payload_version has a suffix is refused (no beta channel) */
static void installed_suffixed(const char *scratch) {
    world w;
    static payload_manifest inst;
    world_new(&w, scratch, "suffixrec", "1.4.0");
    char *c = mkpayload("1.5.1-beta.1", "1.4.0", NULL, NULL, NULL, NULL);
    char p[1200];
    path(p, sizeof p, w.support, "installed-release");
    put_pair(p, c);
    CHECK(update_installed(&w.e, &inst) == PAYLOAD_NOT_VALID, "a suffixed installed-release is refused");
    free(c);
}

/* a pin click while the new app runs its --swap (U4) */
static void busy(const char *scratch) {
    world w;
    release r;
    char to[PAYLOAD_VERSION_MAX], p[1200], dir[1200];
    int key;

    /* the old app that started the update still runs: busy, nothing touched, the new app not even
     * probed (V2) */
    installed_152(&w, scratch, "busy-old", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "busy-old: staged");
    fo.wait_r = ETIMEDOUT;
    fo.running_left = 5;
    CHECK(update_at_start(&w.e) == UPDATE_BUSY, "the old app still runs: UPDATE_BUSY");
    path(dir, sizeof dir, w.apps, "1.5.3");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(exists(dir) && exists(p) && fo.running_left == 5, "nothing touched, nothing probed");
    fo.wait_r = 0;
    release_free(&r);

    /* the new app commits by the first probe and is gone: nothing undone (V1) */
    installed_152(&w, scratch, "busy-commits-early", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "busy-commits-early: staged");
    fo.running_left = 1;
    path(fo.current_file, sizeof fo.current_file, w.apps, "current");
    snprintf(fo.move_current, sizeof fo.move_current, "1.5.3\n");
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_NONE, "committed by the first probe: nothing to undo");
    path(dir, sizeof dir, w.apps, "1.5.3");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(exists(dir) && exists(p), "its folder and records stay");
    release_free(&r);

    /* the new app commits and keeps running: nothing undone, and no waiting out the bound (V1) */
    installed_152(&w, scratch, "busy-commits-stays", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "busy-commits-stays: staged");
    fo.running_always = 1;
    fo.move_now = 1;
    path(fo.current_file, sizeof fo.current_file, w.apps, "current");
    snprintf(fo.move_current, sizeof fo.move_current, "1.5.3\n");
    w.e.busy_wait_ms = 1500;
    uint64_t t0 = plat_now_mono_ns();
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_NONE, "committed, still running: nothing to undo");
    uint64_t took = (plat_now_mono_ns() - t0) / 1000000u;
    CHECK(took < 200, "at once (%llu ms)", (unsigned long long)took);
    path(dir, sizeof dir, w.apps, "1.5.3");
    CHECK(exists(dir), "its folder stays");
    release_free(&r);

    /* it runs a little, then exits without moving current: the update failed, rolled back */
    installed_152(&w, scratch, "busy-exits", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "busy-exits: staged");
    fo.running_left = 3;
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_GENERIC, "the new app exited without its commit: rolled back");
    path(dir, sizeof dir, w.apps, "1.5.3");
    CHECK(!exists(dir) && fo.running_left == 0, "after it was gone");
    release_free(&r);

    /* it runs, then commits (current moved): the old app's start leaves it all */
    installed_152(&w, scratch, "busy-commits", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "busy-commits: staged");
    fo.running_left = 2;
    path(fo.current_file, sizeof fo.current_file, w.apps, "current");
    snprintf(fo.move_current, sizeof fo.move_current, "1.5.3\n");
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_NONE, "the new app moved current meanwhile: nothing to undo");
    path(dir, sizeof dir, w.apps, "1.5.3");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(exists(dir) && exists(p), "its folder and records stay");
    release_free(&r);

    /* it keeps running past the wait: busy, nothing touched */
    installed_152(&w, scratch, "busy-stays", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "busy-stays: staged");
    fo.running_always = 1;
    w.e.busy_wait_ms = 60;
    CHECK(update_at_start(&w.e) == UPDATE_BUSY, "still running: UPDATE_BUSY");
    path(dir, sizeof dir, w.apps, "1.5.3");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(exists(dir) && exists(p), "nothing touched");
    fo.running_always = 0;
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_GENERIC && !exists(dir) && !exists(p), "once it is gone, the rollback");
    release_free(&r);

    /* the process list can't be read: nothing changes, the update left to a later start (V3) */
    installed_152(&w, scratch, "busy-unknown", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "busy-unknown: staged");
    fo.running_fail = 1;
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_NONE, "the list unreadable: nothing to say");
    path(dir, sizeof dir, w.apps, "1.5.3");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(exists(dir) && exists(p), "its folder and pending kept");
    CHECK(update_after_render(&w.e) == 0 && exists(dir), "and not swept after the first render");
    fo.running_fail = 0;
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_GENERIC && !exists(dir) && !exists(p), "the list readable again: rolled back");
    release_free(&r);

    /* --swap failed and its result couldn't be written: pending stays, read as failed (U11) */
    installed_152(&w, scratch, "noresult", &r);
    CHECK(update_stage(&w.e, to, &key) == UPDATE_STAGED, "noresult: staged");
    path(p, sizeof p, w.support, "update/result");
    mkdir(p, 0755);   /* the result's rename then fails */
    update_env n = as(&w, to);
    CHECK(update_swap_failed(&n, UPDATE_FAIL_SWAP) != 0, "the result can't be written");
    path(p, sizeof p, w.support, "update/pending");
    CHECK(exists(p), "pending kept");
    CHECK(update_at_start(&w.e) == UPDATE_FAIL_GENERIC, "the old app reads it as a failed update");
    release_free(&r);
}

/* fetchAsset's parts: the model and the bridge share support\payload\<v>\, so each keeps its own
 * part (S28); a part or a file past the size is damaged, never offline (S22) */
static void fetch_assets(const char *scratch) {
    world w;
    world_new(&w, scratch, "assets", "1.5.1");
    enum { MN = 9000, BN = 4000 };
    unsigned char *model = malloc(MN), *bridge = malloc(BN);
    for (size_t i = 0; i < MN; i++) model[i] = (unsigned char)(i * 7 + 3);
    for (size_t i = 0; i < BN; i++) bridge[i] = (unsigned char)(i * 13 + 1);
    char msha[65], bsha[65], murl[512], burl[512], dir[1200], mpart[1300], bpart[1300], stale[1300], p[1300];
    sha_hex(model, MN, msha);
    sha_hex(bridge, BN, bsha);
    update_url(BASE, "1.5.1", "model.onnx", murl, sizeof murl);
    update_url(BASE, "1.5.1", "bridge.dll", burl, sizeof burl);
    path(dir, sizeof dir, w.support, "payload/1.5.1");
    snprintf(mpart, sizeof mpart, "%s/model.onnx.%.12s.part", dir, msha);
    snprintf(bpart, sizeof bpart, "%s/bridge.dll.%.12s.part", dir, bsha);
    route *rm = serve(murl, model, MN), *rb = serve(burl, bridge, BN);
    struct stat st;

    /* an interrupted model survives the bridge's download */
    rm->cut = 2500;
    CHECK(update_fetch_asset(&w.e, "1.5.1", dir, "model.onnx", msha, MN) == UPDATE_FETCH_OFFLINE && !lstat(mpart, &st) && st.st_size == 2500,
          "the model dropped half-way: its part kept");
    snprintf(stale, sizeof stale, "%s/bridge.dll.0123456789ab.part", dir);
    wfile(stale, "an older bridge's bytes", 23);
    CHECK(update_fetch_asset(&w.e, "1.5.1", dir, "bridge.dll", bsha, BN) == UPDATE_FETCH_OK, "the bridge downloaded");
    CHECK(!lstat(mpart, &st) && st.st_size == 2500, "the model's part survives the bridge's download");
    CHECK(!exists(stale), "the bridge's own stale part (another sha12) removed");
    rm->cut = 0;
    CHECK(update_fetch_asset(&w.e, "1.5.1", dir, "model.onnx", msha, MN) == UPDATE_FETCH_OK && rm->last_from == 2500, "the model resumed and verified");

    /* the reverse: an interrupted bridge survives a verified model's recheck */
    path(p, sizeof p, dir, "bridge.dll");
    unlink(p);
    rb->cut = 1500;
    CHECK(update_fetch_asset(&w.e, "1.5.1", dir, "bridge.dll", bsha, BN) == UPDATE_FETCH_OFFLINE && !lstat(bpart, &st) && st.st_size == 1500,
          "the bridge dropped: its part kept");
    int hits = rm->hits;
    CHECK(update_fetch_asset(&w.e, "1.5.1", dir, "model.onnx", msha, MN) == UPDATE_FETCH_OK && rm->hits == hits, "the model there already: kept");
    CHECK(!lstat(bpart, &st) && st.st_size == 1500, "the bridge's part survives the model's recheck");
    rb->cut = 0;
    CHECK(update_fetch_asset(&w.e, "1.5.1", dir, "bridge.dll", bsha, BN) == UPDATE_FETCH_OK && rb->last_from == 1500, "the bridge resumed");

    /* a part past the size: dropped, downloaded again from 0 */
    path(p, sizeof p, dir, "model.onnx");
    unlink(p);
    unsigned char *big = calloc(1, MN + 1);
    wfile(mpart, big, MN + 1);
    CHECK(update_fetch_asset(&w.e, "1.5.1", dir, "model.onnx", msha, MN) == UPDATE_FETCH_OK && rm->last_from == 0, "a part past the size: from scratch");
    /* a payload whose size is smaller than the file: damaged, its part gone (the Mac's) */
    unlink(p);
    CHECK(update_fetch_asset(&w.e, "1.5.1", dir, "model.onnx", msha, MN - 1) == UPDATE_FETCH_DAMAGED && !exists(mpart), "the file past the size: damaged");
    wfile(mpart, model, MN);   /* exactly at the size, still a part: recovers */
    CHECK(update_fetch_asset(&w.e, "1.5.1", dir, "model.onnx", msha, MN) == UPDATE_FETCH_OK, "a whole part at the size: verified");
    free(big);
    free(model);
    free(bridge);
}

static void shown_now(const char *scratch) {
    char dir[1200], record[1200], target[1200], shown[PAYLOAD_VERSION_MAX];
    path(dir, sizeof dir, scratch, "shown-now");
    CHECK(!mkdir(dir, 0755), "About's support folder");
    path(record, sizeof record, dir, "shown-version");
    CHECK(core_win_update_shown(dir, "1.5.0", shown, sizeof shown) == 5 && !strcmp(shown, "1.5.0"), "About without a record: the app's version");
    wfile(record, "1.5.1\n1.5.0\n", 12);
    CHECK(core_win_update_shown(dir, "1.5.0", shown, sizeof shown) == 5 && !strcmp(shown, "1.5.1"), "About after a carried release: the shown version");
    CHECK(core_win_update_shown(dir, "1.4.0", shown, sizeof shown) == 5 && !strcmp(shown, "1.4.0"), "About in another app: its own version");
    path(target, sizeof target, dir, "other-record");
    CHECK(!rename(record, target) && !symlink(target, record), "replace shown-version with a link");
    CHECK(core_win_update_shown(dir, "1.5.0", shown, sizeof shown) == 5 && !strcmp(shown, "1.5.0"), "About does not follow a shown-version link");
}

void test_update(const char *scratch, const char *pedir) {
    pe_dir = pedir;
    CHECK(test_key_new(&rel) == 0 && test_key_new(&oth) == 0, "keys");
    keys[0].name = "release";
    memcpy(keys[0].xy, rel.xy, 64);
    shown_now(scratch);
    check_final(scratch);
    handles(scratch);
    where_codes(scratch);
    stage(scratch);
    rollbacks(scratch);
    shell_steps(scratch);
    installed_newer(scratch);
    stage_during_commit(scratch);
    sweep_during_swap(scratch);
    commit_stops(scratch);
    watcher_restart(scratch);
    watcher_restart_failure(scratch);
    commit_kills(scratch);
    installed_tear(scratch);
    installed_suffixed(scratch);
    busy(scratch);
    fetch_assets(scratch);
    dump_log();
    net_reset();
    test_key_free(&rel), test_key_free(&oth);
}
