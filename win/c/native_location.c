/* CORE_SLOW_LOCATION's facts on Windows: the Mac's measureCheapState over plat.h (native_location.h) */
#include "native_location.h"

#include "cfg_ini.h"
#include "fs_write.h"
#include "internal.h"   /* the core's arena (bridge_state_parse) */
#include "marker.h"
#include "pure.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define NL_SEP "\\"
#else
#  define NL_SEP "/"   /* the Mac's tests */
#endif

/* ---------------------------------------------------------------- the bridge's lines */

static const char *find(const char *b, size_t n, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; k <= n && i <= n - k; i++)
        if (!memcmp(b + i, needle, k)) return b + i;
    return NULL;
}

int native_location_is_bridge(const char *b, size_t n) { return b && find(b, n, "rbstems bridge: ") != NULL; }

int native_location_bridge_says(const char *b, size_t n) {
    if (!native_location_is_bridge(b, n)) return 0;
    const char *s = find(b, n, "rbstems-cache: ");
    if (!s) return 0;
    s += strlen("rbstems-cache: ");
    size_t rest = (size_t)(b + n - s), lim = rest < 4096 ? rest : 4096;
    const char *end = memchr(s, ';', lim);
    return end && native_location_line_says(s, (size_t)(end - s));
}

int native_location_line_says(const char *line, size_t n) {
    if (!line) return 0;
    /* the tokens before "pioneer=", split at spaces */
    const char *stop = line + n;
    const char *p = find(line, n, "pioneer=");
    if (p) stop = p;
    for (const char *t = line; t < stop;) {
        while (t < stop && *t == ' ') t++;
        const char *e = t;
        while (e < stop && *e != ' ') e++;
        if (e - t == 10 && !memcmp(t, "location=1", 10)) return 1;
        t = e;
    }
    return 0;
}

/* ---------------------------------------------------------------- probeLocation */

static int plat_volume_of(const char *dir, char *mount, size_t n, int *system, int *local, int *readonly) {
    int e = plat_volume_holding(dir, mount, n, system);
    if (e) return e;
    plat_volume v;
    if ((e = plat_volume_info(mount, &v))) return e;
    *local = v.local;
    *readonly = v.readonly;
    return 0;
}

int native_location_probe(const native_location_ops *ops, const char *dir, const char *id) {
    char mount[PLAT_MOUNT_MAX];
    int system = 0, local = 1, readonly = 0;
    int win = dir[0] != '/';
    int e = ops && ops->volume ? ops->volume(ops->ctx, dir, mount, sizeof mount, &system, &local, &readonly)
                               : plat_volume_of(dir, mount, sizeof mount, &system, &local, &readonly);
    /* mountHolding: none, or this PC's own volume (the Mac's "/" isn't a location's) */
    if (e) return e == ENOENT && ops && ops->remote && ops->remote(ops->ctx, dir) ? PROBE_NETWORK : PROBE_NOT_CONNECTED;
    if (system) return PROBE_NOT_CONNECTED;
    if (!local) return PROBE_NETWORK;
    /* every component from the volume down a real folder, none followed */
    size_t n = strlen(dir), root = win ? paths_root_win(dir) : 1;
    if (!root || n > PATHS_LOCATION_MAX) return PROBE_FOLDER_MISSING;
    char p[PATHS_LOCATION_MAX + 1], sep = win ? '\\' : '/';
    memcpy(p, dir, n + 1);
    size_t start = win ? root : (strcmp(mount, "/") ? strlen(mount) : 0) + 1;
    if (!win && (strncmp(dir, mount, strlen(mount)) || (dir[strlen(mount)] != '/' && strcmp(mount, "/")))) return PROBE_FOLDER_MISSING;
    for (char *s = start <= n ? p + start : NULL; s && *s;) {
        char *c = strchr(s, sep);
        if (c) *c = 0;
        plat_stat st;
        int se = plat_stat_path(p, 0, &st);
        if (se) return se == EACCES || se == EPERM ? PROBE_NO_PERMISSION : PROBE_FOLDER_MISSING;
        if (st.kind == PLAT_OTHER) return PROBE_LINK;   /* a link, a junction or another reparse point */
        if (st.kind != PLAT_DIR) return PROBE_FOLDER_MISSING;
        if (!c) break;
        *c = sep;
        s = c + 1;
    }
    /* the marker (readMarker: the folder opened without following a link) */
    plat_dir d;
    if ((e = plat_dir_open(dir, &d))) return e == EACCES || e == EPERM ? PROBE_NO_PERMISSION : PROBE_MARKER_MISSING_OR_WRONG;
    const char *why = marker_check_at(d, id);
    if (why) {
        plat_dir_close(d);
        return !strcmp(why, "no-permission") ? PROBE_NO_PERMISSION : PROBE_MARKER_MISSING_OR_WRONG;
    }
    if (readonly) {
        plat_dir_close(d);
        return PROBE_READ_ONLY;
    }
    e = plat_dir_writable(d);
    plat_dir_close(d);
    if (e) return e == EROFS ? PROBE_READ_ONLY : PROBE_NO_PERMISSION;
    return PROBE_AVAILABLE;
}

/* ---------------------------------------------------------------- measureCheapState */

static int join(char *out, size_t n, const char *a, const char *b) {
    int w = snprintf(out, n, "%s" NL_SEP "%s", a, b);
    return w > 0 && (size_t)w < n;
}

static void copy(char *out, size_t n, const char *s, size_t len) {
    if (!n) return;
    if (len >= n) len = n - 1;
    memcpy(out, s, len);
    out[len] = 0;
}

/* cacheVolumePath: the folder, or the nearest one above it that exists */
static void volume_path(const char *folder, char *out, size_t n) {
    copy(out, n, folder, strlen(folder));
    plat_stat st;
    while (strlen(out) > 1 && plat_stat_path(out, 1, &st)) {
        char *cut = strrchr(out, NL_SEP[0]);
        if (!cut) break;
        if (cut == out || (cut > out && cut[-1] == ':')) {   /* "/" or "X:\": the root stays */
            cut[1] = 0;
            break;
        }
        *cut = 0;
    }
}

/* ---------------------------------------------------------------- driveName */

/* the support folder's location-drive: "dir=<cache_dir>\nlabel=<label>\n", the chosen folder's
 * volume label as last seen (review NL1: an unplugged drive keeps its name). No drive letter is
 * kept: letters get reassigned, so an unplugged drive is never named by one (owner, 2026-11-13). */
#define NL_HINT "location-drive"
typedef struct {
    char dir[NL_PATH_MAX], label[NL_DRIVE_MAX];
} nl_hint;

static int hint_read(const char *support, nl_hint *h) {
    memset(h, 0, sizeof *h);
    char path[NL_PATH_MAX], *b = NULL;
    size_t n = 0;
    if (!join(path, sizeof path, support, NL_HINT) || plat_read_file(path, 4096, &b, &n)) return 0;
    /* a NUL, broken UTF-8 or a field too long for its buffer: no hint (review NR3) */
    int bad = memchr(b, 0, n) != NULL || !cfg_utf8_ok((const unsigned char *)b, n);
    for (size_t i = 0; i < n;) {
        const char *s = b + i, *nl = memchr(s, '\n', n - i), *end = nl ? nl : b + n, *eq = memchr(s, '=', (size_t)(end - s));
        i += (size_t)(end - s) + 1;
        if (!eq) continue;
        size_t kl = (size_t)(eq - s), vl = (size_t)(end - eq - 1);
        if (kl == 3 && !memcmp(s, "dir", 3)) {
            bad |= vl >= sizeof h->dir;
            copy(h->dir, sizeof h->dir, eq + 1, vl);
        } else if (kl == 5 && !memcmp(s, "label", 5)) {
            bad |= vl >= sizeof h->label;
            copy(h->label, sizeof h->label, eq + 1, vl);
        }
    }
    free(b);
    if (bad) memset(h, 0, sizeof *h);
    return h->dir[0] != 0;
}

/* only on a change; an empty label is written only to clear a label kept for the same folder */
static void hint_write(const char *support, const char *dir, const char *label) {
    nl_hint old;
    int had = hint_read(support, &old) && !strcmp(old.dir, dir);
    if (had ? !strcmp(old.label, label) : !label[0]) return;
    char text[NL_PATH_MAX + NL_DRIVE_MAX + 64];
    int k = snprintf(text, sizeof text, "dir=%s\nlabel=%s\n", dir, label);
    if (k > 0 && (size_t)k < sizeof text && !strpbrk(dir, "\r\n") && !strpbrk(label, "\r\n"))
        (void)win_write_atomic(support, NL_HINT, text, (size_t)k);   /* best effort: the name stays known while plugged in */
}

void native_location_drive_name(const native_location_env *e, const char *dir, int remember, char *out, size_t n) {
    if (!n) return;
    out[0] = 0;
    const native_location_ops *ops = e->ops;
    char label[NL_DRIVE_MAX], letter[8];
    if (ops && ops->volume_name && !ops->volume_name(ops->ctx, dir, label, sizeof label, letter, sizeof letter)) {
        const char *name = label[0] ? label : letter;   /* connected: the label, else "E:" (HF5) */
        copy(out, n, name, strlen(name));
        if (remember && e->support) hint_write(e->support, dir, label);
        return;
    }
    /* not there (unplugged): the label kept for that folder, else "" (the texts' nameless form) */
    nl_hint h;
    if (e->support && hint_read(e->support, &h) && !strcmp(h.dir, dir)) copy(out, n, h.label, strlen(h.label));
}

int native_location_measure(const native_location_env *e, native_location *out) {
    memset(out, 0, sizeof *out);
    out->probe = -1;
    out->paused_floor = -1;
    const native_location_ops *ops = e->ops;
    if (!e->support || !e->cache_default || !ops || !ops->live_bridge || !ops->rekordbox_running || !ops->process_start || !ops->volume_name)
        return -1;
    /* config.ini (readConfigState): no file, this PC; unreadable or bad, its status */
    char path[NL_PATH_MAX];
    cfg_config c;
    memset(&c, 0, sizeof c);
    int unreadable = 0;
    if (!join(path, sizeof path, e->support, "config.ini")) return -1;
    char *b = NULL;
    size_t n = 0;
    int r = plat_read_file(path, CFG_MAX, &b, &n);
    if (r && r != ENOENT) unreadable = 1;
    else if (!r && cfg_parse(b, n, &c, NULL) < 0) unreadable = 1;
    free(b);
    int installed = 0, says = 0;
    if (ops->live_bridge(ops->ctx, &installed, &says)) return -1;
    installed = installed != 0;
    says = installed && says;   /* installed && bridgeSaysLocation() */
    int chosen = !unreadable && c.location == 1;
    if (chosen) {   /* the probe runs once, and the drive's name with it (state()) */
        out->probe = native_location_probe(ops, c.dir, c.id);
        /* the label is remembered only once the marker confirmed the folder (review NR4) */
        native_location_drive_name(e, c.dir, out->probe == PROBE_AVAILABLE || out->probe == PROBE_READ_ONLY, out->status_drive,
                                   sizeof out->status_drive);
        out->has_status_drive = 1;
    }
    /* locationStatus */
    if (unreadable) out->kind = LOC_CONFIG_UNREADABLE;
    else if (c.location == 0) out->kind = LOC_THIS_MAC;
    else if (c.location < 0) out->kind = LOC_CONFIG_MALFORMED;
    else if (installed && !says) out->kind = LOC_BRIDGE_TOO_OLD;
    else out->kind = LOC_CHOSEN;
    /* rekordbox and the bridge's state for it, for the location config.ini names now */
    int running = ops->rekordbox_running(ops->ctx);
    if (running < 0) return -1;
    out->rekordbox_running = running;
    if (running && join(path, sizeof path, e->support, "Logs" NL_SEP "bridge-state")) {
        b = NULL;
        n = 0;
        if (!plat_read_file(path, 8192, &b, &n)) {
            arena a = {0};
            bridge_state bs;
            int64_t started = -1;   /* review NL3: no such process (-1) is never live, whatever start= says */
            if (bridge_state_parse(&a, b, n, &bs) == 1 && (started = ops->process_start(ops->ctx, bs.pid)) >= 0
                && bridge_state_live(&bs, chosen ? c.dir : "", started)) {
                out->has_bridge = 1;
                copy(out->bridge_state, sizeof out->bridge_state, bs.state.s, bs.state.n);
                copy(out->bridge_reason, sizeof out->bridge_reason, bs.reason.s, bs.reason.n);
                if (bs.location.n) {
                    char where[NL_PATH_MAX];
                    copy(where, sizeof where, bs.location.s, bs.location.n);
                    native_location_drive_name(e, where, 0, out->bridge_drive, sizeof out->bridge_drive);
                    out->has_bridge_drive = 1;
                }
            }
            arena_free(&a);
        }
        free(b);
    }
    /* folderInUse and its floor (cachePausedFloor on the folder's disk) */
    const char *in_use = out->kind == LOC_THIS_MAC || out->kind == LOC_BRIDGE_TOO_OLD ? e->cache_default
                       : out->kind == LOC_CHOSEN && out->probe == PROBE_AVAILABLE ? c.dir
                                                                                  : NULL;
    if (in_use) {
        copy(out->in_use, sizeof out->in_use, in_use, strlen(in_use));
        out->has_in_use = 1;
        char vp[NL_PATH_MAX];
        volume_path(in_use, vp, sizeof vp);
        /* review NL6: the bridge's measure, so the floor is where the bridge pauses: free space from
         * plat_free_bytes (0 when it can't be read, on which the bridge pauses too), the total 0 when
         * unknown (a SUBST or mapped drive has no volume info): the 50 GB floor then */
        uint64_t avail = 0, total = 0;
        if (ops->space) ops->space(ops->ctx, vp, &avail, &total);
        else {
            const char *how;
            plat_volume v;
            avail = plat_free_bytes(vp, 0, &how);
            total = plat_volume_info(vp, &v) ? 0 : v.total;
        }
        int64_t floor;
        if (state_paused_floor((int64_t)avail, (int64_t)total, &floor)) out->paused_floor = floor;
    }
    return 0;
}

/* ---------------------------------------------------------------- the worker */

/* one measure, owned by its caller while it waits and by the queue's job after (under r->lock) */
typedef struct {
    native_location_runner *r;
    native_location_ops ops;
    native_location_env env;
    native_location out;
    int rc, finished, waiting;
    char strings[];   /* support, then cache_default */
} nl_job;

static void nl_job_run(void *ctx) {
    nl_job *j = ctx;
    native_location_runner *r = j->r;
    int rc = native_location_measure(&j->env, &j->out);
    plat_mutex_lock(&r->lock);
    j->rc = rc;
    j->finished = 1;
    r->busy = 0;
    int waiting = j->waiting;
    if (waiting) plat_cond_broadcast(&r->done);
    plat_mutex_unlock(&r->lock);
    if (!waiting) free(j);   /* its caller stopped waiting: the answer is dropped */
}

int native_location_measure_bounded(native_location_runner *r, const native_location_env *e, const plat_thread_id *ui, uint64_t wait_ns,
                                    native_location *out) {
    memset(out, 0, sizeof *out);
    out->probe = -1;
    out->paused_floor = -1;
    if (ui && plat_thread_equal(plat_thread_self(), *ui)) return NL_ON_UI;   /* never a volume query on the UI thread */
    if (!e->support || !e->cache_default || !e->ops) return NL_NO_ANSWER;
    size_t ns = strlen(e->support) + 1, nc = strlen(e->cache_default) + 1;
    nl_job *j = malloc(sizeof *j + ns + nc);
    if (!j) return NL_NO_ANSWER;
    memset(j, 0, sizeof *j);
    memcpy(j->strings, e->support, ns);
    memcpy(j->strings + ns, e->cache_default, nc);
    j->r = r;
    j->ops = *e->ops;
    j->env.support = j->strings;
    j->env.cache_default = j->strings + ns;
    j->env.ops = &j->ops;
    j->waiting = 1;
    plat_mutex_lock(&r->lock);
    if (!r->ready && (r->q = plat_queue_new("rbsp.core.location", PLAT_WORK_DEFAULT))) {
        plat_cond_init(&r->done);
        r->ready = 1;
    }
    int busy = r->busy;
    if (r->ready && !busy) r->busy = 1;
    int ready = r->ready;
    plat_mutex_unlock(&r->lock);
    if (!ready || busy) {
        free(j);
        return busy ? NL_BUSY : NL_NO_ANSWER;
    }
    if (plat_async(r->q, nl_job_run, j)) {
        plat_mutex_lock(&r->lock);
        r->busy = 0;
        plat_mutex_unlock(&r->lock);
        free(j);
        return NL_NO_ANSWER;
    }
    plat_mutex_lock(&r->lock);
    /* review NW2: a wakeup that isn't the job's end waits only for what is left of wait_ns */
    uint64_t now = plat_now_mono_ns(), end = wait_ns > UINT64_MAX - now ? UINT64_MAX : now + wait_ns;
    while (!j->finished && now < end) {
        if (plat_cond_wait_ns(&r->done, &r->lock, end - now) == ETIMEDOUT) break;
        now = plat_now_mono_ns();
    }
    int finished = j->finished;
    j->waiting = 0;   /* not finished: the job frees itself when it is */
    plat_mutex_unlock(&r->lock);
    if (!finished) return NL_TIMEOUT;
    int rc = j->rc;
    if (!rc) memcpy(out, &j->out, sizeof *out);
    free(j);
    return rc ? NL_NO_ANSWER : 0;
}

/* CORE_SLOW_LOCATION (review HF5): off until the VM rounds (the NL rulings' wintest rows): until
 * then the answer is -1 (core.h: the core then keeps no memo of its own) */
#define NATIVE_LOCATION_LIVE 0
#ifdef NATIVE_LOCATION_TEST_HOOKS
int native_location_test_live, native_location_test_no_memory;
#endif

int32_t native_location_slow_fact(native_location_runner *r, const native_location_env *e, const plat_thread_id *ui, uint64_t wait_ns,
                                  int (*write)(void *ctx, const native_location *l), void (*log)(void *ctx, const char *line), void *ctx) {
    int live = NATIVE_LOCATION_LIVE, no_memory = 0;
#ifdef NATIVE_LOCATION_TEST_HOOKS
    live |= native_location_test_live;
    no_memory = native_location_test_no_memory;
#endif
    if (!live || !e->support || !e->cache_default) return -1;
    native_location *l = no_memory ? NULL : malloc(sizeof *l);
    if (!l) return CORE_SLOW_NOT_NOW;
    int rc = native_location_measure_bounded(r, e, ui, wait_ns, l);
    if (rc == NL_ON_UI) log(ctx, "location: a measure asked on the UI thread: refused");
    else if (rc == NL_TIMEOUT) log(ctx, "location: the measure took over 15 s: no answer");
    else if (rc == NL_BUSY) log(ctx, "location: the measure before is still running: no answer");
    else if (rc == NL_NO_ANSWER) log(ctx, "location: the measure had no answer");
    if (!rc) rc = write(ctx, l);
    free(l);
    if (rc == NL_TIMEOUT || rc == NL_BUSY || rc == NL_NO_ANSWER) return CORE_SLOW_NOT_NOW;
    return rc ? -1 : 0;
}
