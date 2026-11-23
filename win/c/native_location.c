/* CORE_SLOW_LOCATION's facts on Windows: the Mac's measureCheapState over plat.h (native_location.h) */
#include "native_location.h"

#include "cfg_ini.h"
#include "fs_write.h"
#include "internal.h"   /* the core's arena (bridge_state_parse) */
#include "marker.h"
#include "pure.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

void native_location_identity(plat_id id, char out[NL_IDENTITY_MAX]) {
    snprintf(out, NL_IDENTITY_MAX, "%016" PRIx64 "%016" PRIx64 "%016" PRIx64, id.vol, id.file[0], id.file[1]);
}

static int saved_volume(const char *dir, char *mount, size_t n, vol_facts *facts) {
#if defined(_WIN32)
    size_t g = vol_guid_root(dir);
    if (!g || g >= n) return ENOENT;
    snprintf(mount, n, "%.*s", (int)g, dir);
    return vol_facts_read(mount, 1, facts);
#else
    (void)dir; (void)mount; (void)n; (void)facts;
    return ENOENT;
#endif
}

static int folder_probe_error(int e) {
    return e == EACCES || e == EPERM ? PROBE_NO_PERMISSION : e == ELOOP ? PROBE_LINK : PROBE_FOLDER_MISSING;
}

static int probe(const native_location_ops *ops, const char *dir, const char *id, char *identity) {
    char mount[PLAT_MOUNT_MAX];
    int win = dir[0] != '/';
    vol_facts facts;
    int e = win && !vol_guid_root(dir) ? ENOENT
          : ops && ops->volume ? ops->volume(ops->ctx, dir, mount, sizeof mount, &facts)
                               : saved_volume(dir, mount, sizeof mount, &facts);
    if (e) return e == ENOENT && ops && ops->remote && ops->remote(ops->ctx, dir) ? PROBE_NETWORK : PROBE_NOT_CONNECTED;
    int refusal = vol_location_refusal(&facts);
    if (refusal == VOL_REFUSE_NETWORK) return PROBE_NETWORK;
    if (refusal != VOL_OK && refusal != VOL_REFUSE_READ_ONLY) return PROBE_NOT_CONNECTED;
    size_t start = win ? vol_guid_root(dir) : (strcmp(mount, "/") ? strlen(mount) : 0) + 1;
    size_t n = strlen(dir);
    if (n <= start || n > PATHS_LOCATION_MAX) return PROBE_FOLDER_MISSING;
    if (win && !paths_location_ok_win(dir)) return PROBE_FOLDER_MISSING;
    if (win ? strncasecmp(dir, mount, start)
            : strncmp(dir, mount, start - 1) || dir[start - 1] != '/') return PROBE_FOLDER_MISSING;
    /* Open each component under its checked parent and take the identity from the marker's handle. */
    plat_dir d;
    if ((e = plat_dir_open(mount, &d))) return folder_probe_error(e);
    plat_stat st;
    if (plat_dir_stat(d, &st)) {
        plat_dir_close(d);
        return PROBE_FOLDER_MISSING;
    }
    uint64_t volume = st.id.vol;
    char path[PATHS_LOCATION_MAX + 1];
    memcpy(path, dir + start, n - start + 1);
    char *save = NULL;
    for (char *part = strtok_r(path, NL_SEP, &save); part; part = strtok_r(NULL, NL_SEP, &save)) {
        plat_dir child;
        e = plat_dir_open_at(d, part, &child);
        plat_dir_close(d);
        if (e) return folder_probe_error(e);
        d = child;
        if (plat_dir_stat(d, &st) || st.id.vol != volume) {
            plat_dir_close(d);
            return PROBE_FOLDER_MISSING;
        }
    }
    const char *why = marker_check_at(d, id);
    if (why) {
        plat_dir_close(d);
        return !strcmp(why, "no-permission") ? PROBE_NO_PERMISSION : PROBE_MARKER_MISSING_OR_WRONG;
    }
    if (refusal == VOL_REFUSE_READ_ONLY) {
        plat_dir_close(d);
        return PROBE_READ_ONLY;
    }
    e = plat_dir_writable(d);
    plat_dir_close(d);
    if (e) return e == EROFS ? PROBE_READ_ONLY : PROBE_NO_PERMISSION;
    if (identity) native_location_identity(st.id, identity);
    return PROBE_AVAILABLE;
}

int native_location_probe(const native_location_ops *ops, const char *dir, const char *id) {
    return probe(ops, dir, id, NULL);
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

/* Only on a change. An empty label clears a label kept for the same folder. */
void native_location_remember_label(const char *support, const char *dir, const char *label) {
    nl_hint old;
    int had = hint_read(support, &old) && !strcmp(old.dir, dir);
    if (had ? !strcmp(old.label, label) : !label[0]) return;
    char text[NL_PATH_MAX + NL_DRIVE_MAX + 64];
    int k = snprintf(text, sizeof text, "dir=%s\nlabel=%s\n", dir, label);
    if (k > 0 && (size_t)k < sizeof text && !strpbrk(dir, "\r\n") && !strpbrk(label, "\r\n"))
        (void)win_write_atomic(support, NL_HINT, text, (size_t)k);   /* best effort: the name stays known while plugged in */
}

static int drive_name(const native_location_env *e, const char *dir, char label[NL_DRIVE_MAX], char *out, size_t n) {
    out[0] = 0;
    const native_location_ops *ops = e->ops;
    char letter[8];
    if (ops && ops->volume_name && !ops->volume_name(ops->ctx, dir, label, NL_DRIVE_MAX, letter, sizeof letter)) {
        const char *name = label[0] ? label : letter;   /* connected: the label, else "E:" (HF5) */
        copy(out, n, name, strlen(name));
        return 1;
    }
    /* not there (unplugged): the label kept for that folder, else "" (the texts' nameless form) */
    nl_hint h;
    if (e->support && hint_read(e->support, &h) && !strcmp(h.dir, dir)) copy(out, n, h.label, strlen(h.label));
    return 0;
}

void native_location_drive_name(const native_location_env *e, const char *dir, int remember, char *out, size_t n) {
    char label[NL_DRIVE_MAX];
    if (n && drive_name(e, dir, label, out, n) && remember && e->support)
        native_location_remember_label(e->support, dir, label);
}

int native_location_picker_name(const native_location_env *e, const char *dir, char label[NL_DRIVE_MAX], char *out, size_t n) {
    return drive_name(e, dir, label, out, n);
}

static void disk_space(const native_location_env *e, const char *folder, uint64_t *avail, uint64_t *total) {
    char vp[NL_PATH_MAX];
    volume_path(folder, vp, sizeof vp);
    *avail = *total = 0;
    if (e->ops && e->ops->space) e->ops->space(e->ops->ctx, vp, avail, total);
    else {
        const char *how;
        plat_volume v;
        *avail = plat_free_bytes(vp, 0, &how);
        *total = plat_volume_info(vp, &v) ? 0 : v.total;
    }
}

void native_location_describe(const native_location_env *e, const native_location *l,
                              const char *dir, const char *id, native_location_details *out) {
    memset(out, 0, sizeof *out);
    if (l && !native_location_matches_config(l, dir, id)) l = NULL;
    if (dir[0]) {
        if (l && l->has_status_drive) copy(out->drive, sizeof out->drive, l->status_drive, strlen(l->status_drive));
        else {
            native_location_env hint = {.support = e->support};
            native_location_drive_name(&hint, dir, 0, out->drive, sizeof out->drive);
        }
    }
    if (l && l->has_in_use) {
        out->has_disk = 1;
        out->available = l->available;
        out->total = l->total;
    }
}

/* config.ini (readConfigState): no file, this PC; unreadable or bad, its status */
static int read_config_location(const char *support, cfg_config *c) {
#ifdef NATIVE_LOCATION_TEST_HOOKS
    if (native_location_test_config_read) native_location_test_config_read(support);
#endif
    char path[NL_PATH_MAX];
    memset(c, 0, sizeof *c);
    int unreadable = 0;
    if (!join(path, sizeof path, support, "config.ini")) return NL_NO_ANSWER;
    char *b = NULL;
    size_t n = 0;
    int r = plat_read_file(path, CFG_MAX, &b, &n);
    if (r && r != ENOENT) unreadable = 1;
    else if (!r && cfg_parse(b, n, c, NULL) < 0) unreadable = 1;
    free(b);
    return unreadable ? LOC_CONFIG_UNREADABLE : c->location == 0 ? LOC_THIS_MAC
                       : c->location < 0 ? LOC_CONFIG_MALFORMED : LOC_CHOSEN;
}

int native_location_matches_config(const native_location *l, const char *dir, const char *id) {
    return !strcmp(l->config_dir, dir) && !strcmp(l->config_id, id);
}

int native_location_checked(const native_location *l) {
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    if (l->kind == LOC_THIS_MAC && l->probe == PROBE_AVAILABLE) return 1;
#endif
    return l->kind != LOC_THIS_MAC || l->has_bridge;
}

static int measure_config(const native_location_env *e, int kind, const cfg_config *config, native_location *out) {
    memset(out, 0, sizeof *out);
    out->probe = -1;
    out->paused_floor = -1;
    const native_location_ops *ops = e->ops;
    if (kind == NL_NO_ANSWER || !e->cache_default || !ops || !ops->live_bridge || !ops->rekordbox_running || !ops->process_start || !ops->volume_name)
        return -1;
    char path[NL_PATH_MAX];
    int installed = 0, says = 0;
    if (ops->live_bridge(ops->ctx, &installed, &says)) return -1;
    installed = installed != 0;
    says = installed && says;   /* installed && bridgeSaysLocation() */
    int chosen = kind == LOC_CHOSEN;
    if (chosen) {   /* the probe runs once, and the drive's name with it (state()) */
        memcpy(out->config_dir, config->dir, sizeof out->config_dir);
        memcpy(out->config_id, config->id, sizeof out->config_id);
        out->probe = probe(ops, config->dir, config->id, out->identity);
        /* the label is remembered only once the marker confirmed the folder (review NR4) */
        native_location_drive_name(e, config->dir, out->probe == PROBE_AVAILABLE || out->probe == PROBE_READ_ONLY, out->status_drive,
                                   sizeof out->status_drive);
        out->has_status_drive = 1;
    }
    /* locationStatus */
    out->kind = chosen && installed && !says ? LOC_BRIDGE_TOO_OLD : kind;
    /* rekordbox and the bridge's state for it, for the location config.ini names now */
    int running = ops->rekordbox_running(ops->ctx);
    if (running < 0) return -1;
    out->rekordbox_running = running;
    if (running && join(path, sizeof path, e->support, "Logs" NL_SEP "bridge-state")) {
        char *b = NULL;
        size_t n = 0;
        if (!plat_read_file(path, 8192, &b, &n)) {
            arena a = {0};
            bridge_state bs;
            int64_t started = -1;   /* review NL3: no such process (-1) is never live, whatever start= says */
            if (bridge_state_parse(&a, b, n, &bs) == 1 && (started = ops->process_start(ops->ctx, bs.pid)) >= 0
                && bridge_state_live(&bs, chosen ? config->dir : "", started)) {
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
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    if (out->kind == LOC_THIS_MAC && !out->has_bridge) {
        if (!ops->pc_cache_probe) return -1;
        int r = ops->pc_cache_probe(e->support, e->cache_default);
        out->probe = !r || r == ENOENT ? PROBE_AVAILABLE : PROBE_MARKER_MISSING_OR_WRONG;
        if (out->probe != PROBE_AVAILABLE) return 0;
    }
#endif
    const char *in_use = out->kind == LOC_THIS_MAC || out->kind == LOC_BRIDGE_TOO_OLD ? e->cache_default
                       : out->kind == LOC_CHOSEN && out->probe == PROBE_AVAILABLE ? config->dir
                                                                                  : NULL;
    if (in_use) {
        copy(out->in_use, sizeof out->in_use, in_use, strlen(in_use));
        out->has_in_use = 1;
        /* The bridge pauses on unknown free space (0) and uses the 50 GB floor
         * for an unknown total. Keep the inputs for Create Report (review N3R9). */
        disk_space(e, in_use, &out->available, &out->total);
        int64_t floor;
        if (state_paused_floor((int64_t)out->available, (int64_t)out->total, &floor)) out->paused_floor = floor;
    }
    return 0;
}

int native_location_measure(const native_location_env *e, native_location *out) {
    cfg_config config;
    int kind = e->support ? read_config_location(e->support, &config) : NL_NO_ANSWER;
    return measure_config(e, kind, &config, out);
}

/* A timed-out read owns only its copied inputs and result. Keep a stuck drive from
 * accumulating queued checks on retries, and leave a second worker for another drive. */
struct native_location_io {
    plat_fn check;
    void (*commit)(native_location_io *, void *);
    uint64_t deadline;
    int slot, kind, done, abandoned, mutating, replacing, replaced;
    uint64_t finished;
    char path[NL_PATH_MAX];   /* immutable even while check writes its result */
    max_align_t data[];
};

static plat_mutex choice_lock = PLAT_MUTEX_INIT;
static plat_cond choice_done;
/* Drive jobs and support-folder jobs each have NL_MAX_JOBS workers. */
static plat_queue *choice_queues[2 * NL_MAX_JOBS];
static native_location_io *choice_active[2 * NL_MAX_JOBS];
static int choice_initialized;

static void choice_job_run(void *ctx) {
    native_location_io *j = ctx;
    if (plat_now_mono_ns() < j->deadline) {
        if (j->commit) j->commit(j, j->data);
        else j->check(j->data);
    }
    plat_mutex_lock(&choice_lock);
    j->done = 1;
    j->finished = plat_now_mono_ns();
    choice_active[j->slot] = NULL;
    int abandoned = j->abandoned;
    plat_cond_broadcast(&choice_done);
    plat_mutex_unlock(&choice_lock);
    if (abandoned) free(j);
}

static int wait_io(plat_fn check, void (*commit)(native_location_io *, void *), void *data, size_t size,
                   uint64_t deadline, int kind, const char *path, int local, int concurrent, int *mutating, int *replaced) {
    if (plat_now_mono_ns() >= deadline) return NL_TIMEOUT;
    size_t path_len = strlen(path);
    if (path_len >= NL_PATH_MAX) return NL_NO_ANSWER;
    plat_mutex_lock(&choice_lock);
    if (!choice_initialized) {
        plat_cond_init(&choice_done);
        choice_initialized = 1;
    }
    int slot = -1;
    for (int i = 0; i < 2 * NL_MAX_JOBS; i++) {
        int same_pool = (i >= NL_MAX_JOBS) == local;
        native_location_io *active = choice_active[i];
        if (!active) {
            if (same_pool && slot < 0) slot = i;
            continue;
        }
        if ((!same_pool && !commit) || active->check != check || active->commit != commit ||
            active->kind != kind || strcmp(active->path, path)) continue;
        int expired = plat_now_mono_ns() >= active->deadline;
        if (concurrent && !expired) continue;
        /* Use This PC can pass an expired drive commit once it can no longer replace config.ini. */
        if (!same_pool && local && expired && !active->replacing) continue;
        plat_mutex_unlock(&choice_lock);
        return NL_TIMEOUT;
    }
    if (slot < 0) {
        plat_mutex_unlock(&choice_lock);
        return NL_BUSY;
    }
    if (!choice_queues[slot]) choice_queues[slot] = plat_queue_new("rbsp.core.location-choice", PLAT_WORK_DEFAULT);
    native_location_io *j = choice_queues[slot] ? malloc(sizeof *j + size) : NULL;
    if (!j) {
        plat_mutex_unlock(&choice_lock);
        return NL_NO_ANSWER;
    }
    *j = (native_location_io){.check = check, .commit = commit, .deadline = deadline, .slot = slot, .kind = kind};
    memcpy(j->path, path, path_len + 1);
    memcpy(j->data, data, size);
    if (plat_async(choice_queues[slot], choice_job_run, j)) {
        plat_mutex_unlock(&choice_lock);
        free(j);
        return NL_NO_ANSWER;
    }
    choice_active[slot] = j;
    while (!j->done) {
        uint64_t now = plat_now_mono_ns();
        if (j->replacing) plat_cond_wait(&choice_done, &choice_lock);
        else if (now >= deadline) break;
        else plat_cond_wait_ns(&choice_done, &choice_lock, deadline - now);
    }
    int done = j->done;
    int rc = done && j->finished < deadline ? 0 : NL_TIMEOUT;
    if (!rc) memcpy(data, j->data, size);
    if (mutating) *mutating = j->mutating;
    if (replaced) *replaced = j->replaced;
    j->abandoned = !done;
    plat_mutex_unlock(&choice_lock);
    if (done) free(j);
    return rc;
}

int native_choice_read_keyed(plat_fn check, void *data, size_t size, uint64_t deadline, int kind, const char *path) {
    return wait_io(check, NULL, data, size, deadline, kind, path, 0, 0, NULL, NULL);
}

int native_support_read_bounded(plat_fn check, void *data, size_t size, uint64_t deadline, const char *path) {
    return wait_io(check, NULL, data, size, deadline, 0, path, 1, 0, NULL, NULL);
}

int native_location_commit_wait(void (*commit)(native_location_io *, void *), void *data, size_t size,
                                uint64_t deadline, const char *support, int local, int *mutating, int *replaced) {
    return wait_io(NULL, commit, data, size, deadline, 0, support, local, 0, mutating, replaced);
}

int native_location_begin_mutation(native_location_io *io, int replace) {
    plat_mutex_lock(&choice_lock);
    int allowed = plat_now_mono_ns() < io->deadline;
    if (allowed) {
        io->mutating = 1;
        io->replacing = replace;
    }
    plat_mutex_unlock(&choice_lock);
    return allowed;
}

int native_location_io_expired(const native_location_io *io) {
    return io && plat_now_mono_ns() >= io->deadline;
}

void native_location_end_replace(native_location_io *io, int replaced) {
    plat_mutex_lock(&choice_lock);
    io->replaced = replaced;
    io->replacing = 0;
    plat_cond_broadcast(&choice_done);
    plat_mutex_unlock(&choice_lock);
}

typedef struct {
    char support[NL_PATH_MAX];
    int kind;
    cfg_config config;
} location_config_read;

static void run_config_read(void *ctx) {
    location_config_read *read = ctx;
    read->kind = read_config_location(read->support, &read->config);
}

#ifdef NATIVE_LOCATION_TEST_HOOKS
int native_location_test_commit_pending(const char *support) {
    int pending = 0;
    plat_mutex_lock(&choice_lock);
    for (int i = 0; i < 2 * NL_MAX_JOBS; i++) {
        const native_location_io *j = choice_active[i];
        if (j && j->commit && !strcmp(j->path, support)) pending = 1;
    }
    plat_mutex_unlock(&choice_lock);
    return pending;
}
#endif

static int bounded_config_read(const char *support, uint64_t end, cfg_config *config) {
    location_config_read read;
    if (snprintf(read.support, sizeof read.support, "%s", support) >= (int)sizeof read.support) return NL_NO_ANSWER;
    int rc = wait_io(run_config_read, NULL, &read, sizeof read, end, 0, support, 1, 1, NULL, NULL);
    if (rc) return rc;
    *config = read.config;
    return read.kind;
}

/* ---------------------------------------------------------------- the worker */

/* The queue owns a job until it finishes; each caller holds it while it waits (under r->lock). */
typedef struct nl_job {
    native_location_runner *r;
    native_location_ops ops;
    native_location_env env;
    cfg_config config;
    native_location out;
    uint64_t generation;
    int slot, kind, rc, finished, waiters;
    char strings[];   /* support, then cache_default */
} nl_job;

static void nl_job_run(void *ctx) {
    nl_job *j = ctx;
    native_location_runner *r = j->r;
    int rc = measure_config(&j->env, j->kind, &j->config, &j->out);
    plat_mutex_lock(&r->lock);
    j->rc = rc;
    j->finished = 1;
    r->jobs[j->slot] = NULL;
    int waiters = j->waiters;
    if (waiters) plat_cond_broadcast(&r->done);
    plat_mutex_unlock(&r->lock);
    if (!waiters) free(j);
}

static int nl_same_target(int kind, const char *dir, const char *id, int other_kind, const cfg_config *other) {
    return kind == other_kind && (kind != LOC_CHOSEN || (!strcmp(dir, other->dir) && !strcmp(id, other->id)));
}

/* Under r->lock, advance only when the location keys change, even if every slot is busy. */
static void nl_observe_config(native_location_runner *r, int kind, const cfg_config *config) {
    if (!r->generation) plat_cond_init(&r->done);
    else if (nl_same_target(r->config_kind, r->config_dir, r->config_id, kind, config)) return;
    r->generation++;
    r->config_kind = kind;
    if (kind == LOC_CHOSEN) {
        memcpy(r->config_dir, config->dir, sizeof r->config_dir);
        memcpy(r->config_id, config->id, sizeof r->config_id);
    }
}

/* Read the local config without holding the runner lock. If another call observed a change
 * meanwhile, read again before selecting a generation. Success returns with the lock held. */
static int nl_lock_config(native_location_runner *r, const char *support, uint64_t end, cfg_config *config) {
    plat_mutex_lock(&r->lock);
    for (;;) {
        uint64_t generation = r->generation;
        plat_mutex_unlock(&r->lock);
        int kind = bounded_config_read(support, end, config);
        plat_mutex_lock(&r->lock);
        if (kind < 0) {
            plat_mutex_unlock(&r->lock);
            return kind;
        }
        if (generation == r->generation) return kind;
        if (plat_now_mono_ns() >= end) break;
    }
    plat_mutex_unlock(&r->lock);
    return NL_NO_ANSWER;
}

/* Under r->lock, use a separate serial queue per slot so a hung job cannot block another slot. */
static nl_job *nl_job_start(native_location_runner *r, int slot, const native_location_env *e, int kind, const cfg_config *config) {
    if (!r->queues[slot]) {
        r->queues[slot] = plat_queue_new("rbsp.core.location", PLAT_WORK_DEFAULT);
        if (!r->queues[slot]) return NULL;
    }
    size_t ns = strlen(e->support) + 1, nc = strlen(e->cache_default) + 1;
    nl_job *j = malloc(sizeof *j + ns + nc);
    if (!j) return NULL;
    memset(j, 0, sizeof *j);
    memcpy(j->strings, e->support, ns);
    memcpy(j->strings + ns, e->cache_default, nc);
    j->r = r;
    j->ops = *e->ops;
    j->env.support = j->strings;
    j->env.cache_default = j->strings + ns;
    j->env.ops = &j->ops;
    j->slot = slot;
    j->kind = kind;
    j->config = *config;
    j->generation = r->generation;
    if (plat_async(r->queues[slot], nl_job_run, j)) {
        free(j);
        return NULL;
    }
    return r->jobs[slot] = j;
}

int native_location_measure_bounded(native_location_runner *r, const native_location_env *e, const plat_thread_id *ui, uint64_t wait_ns,
                                    native_location *out) {
    memset(out, 0, sizeof *out);
    out->probe = -1;
    out->paused_floor = -1;
    if (ui && plat_thread_equal(plat_thread_self(), *ui)) return NL_ON_UI;   /* never a volume query on the UI thread */
    if (!e->support || !e->cache_default || !e->ops) return NL_NO_ANSWER;
    uint64_t now = plat_now_mono_ns(), end = wait_ns > UINT64_MAX - now ? UINT64_MAX : now + wait_ns;
    cfg_config config;
    uint64_t generation = 0;
    int kind;
retry:
    kind = nl_lock_config(r, e->support, end, &config);
    if (kind < 0) return kind;
    nl_job *j = NULL;
    int slot = -1;
    for (int i = 0; i < NL_MAX_JOBS; i++) {
        nl_job *job = r->jobs[i];
        if (!job) {
            if (slot < 0) slot = i;
        } else if (strcmp(e->support, job->env.support) || strcmp(e->cache_default, job->env.cache_default)) {
            plat_mutex_unlock(&r->lock);
            return NL_BUSY;
        } else if (nl_same_target(job->kind, job->config.dir, job->config.id, kind, &config)) {
            j = job;
        }
    }
    nl_observe_config(r, kind, &config);
    if (generation && generation != r->generation) {
        plat_mutex_unlock(&r->lock);
        return NL_NO_ANSWER;
    }
    generation = r->generation;
    if (!j) {
        if (slot < 0) {
            plat_mutex_unlock(&r->lock);
            return NL_BUSY;
        }
        j = nl_job_start(r, slot, e, kind, &config);
    }
    if (!j) {
        plat_mutex_unlock(&r->lock);
        return NL_NO_ANSWER;
    }
    /* A waiter can wait for a matching target's slot, but only its own generation can answer. */
    j->waiters++;
    /* review NW2: a wakeup that isn't the job's end waits only for what is left of wait_ns */
    now = plat_now_mono_ns();
    while (!j->finished && now < end) {
        if (plat_cond_wait_ns(&r->done, &r->lock, end - now) == ETIMEDOUT) break;
        now = plat_now_mono_ns();
    }
    int stale = generation != j->generation;
    int retry_stale = j->finished && stale && generation == r->generation;
    int rc = !j->finished ? NL_TIMEOUT : j->rc || stale || generation != r->generation ? NL_NO_ANSWER : 0;
    native_location l;
    if (!rc) memcpy(&l, &j->out, sizeof l);
    int last = --j->waiters == 0 && j->finished;
    plat_mutex_unlock(&r->lock);
    if (last) free(j);
    if (retry_stale && plat_now_mono_ns() < end) goto retry;
    if (rc) return rc;
    /* review N3R18: a probe that started before a config change is for the old folder or marker */
    int current = bounded_config_read(e->support, end, &config);
    if (current < 0) return current;
    plat_mutex_lock(&r->lock);
    if (generation == r->generation) nl_observe_config(r, current, &config);
    if (current != kind || generation != r->generation || (kind == LOC_CHOSEN && !native_location_matches_config(&l, config.dir, config.id)))
        rc = NL_NO_ANSWER;
    else memcpy(out, &l, sizeof *out);
    plat_mutex_unlock(&r->lock);
    return rc;
}

/* Windows 1.5.0 enables the location choice and the bounded CORE_SLOW_LOCATION measure. */
#define NATIVE_LOCATION_LIVE 1
#ifdef NATIVE_LOCATION_TEST_HOOKS
int native_location_test_off, native_location_test_no_memory;
void (*native_location_test_config_read)(const char *support);
int native_location_test_waiters(native_location_runner *r) {
    plat_mutex_lock(&r->lock);
    int waiters = 0;
    for (int i = 0; i < NL_MAX_JOBS; i++) {
        nl_job *job = r->jobs[i];
        if (job && nl_same_target(r->config_kind, r->config_dir, r->config_id, job->kind, &job->config))
            waiters += job->waiters;
    }
    plat_mutex_unlock(&r->lock);
    return waiters;
}
#endif

int native_location_live(void) {
#ifdef NATIVE_LOCATION_TEST_HOOKS
    if (native_location_test_off) return 0;
#endif
    return NATIVE_LOCATION_LIVE;
}

int32_t native_location_slow_fact(native_location_runner *r, const native_location_env *e, const plat_thread_id *ui, uint64_t wait_ns,
                                  int (*write)(void *ctx, const native_location *l), void (*log)(void *ctx, const char *line), void *ctx) {
    int no_memory = 0;
#ifdef NATIVE_LOCATION_TEST_HOOKS
    no_memory = native_location_test_no_memory;
#endif
    if (!native_location_live() || !e->support || !e->cache_default) return -1;
    native_location *l = no_memory ? NULL : malloc(sizeof *l);
    if (!l) return CORE_SLOW_NOT_NOW;
    int rc = native_location_measure_bounded(r, e, ui, wait_ns, l);
    if (rc == NL_ON_UI) log(ctx, "location: a measure asked on the UI thread: refused");
    else if (rc == NL_TIMEOUT) log(ctx, "location: the measure took over 15 s: no answer");
    else if (rc == NL_BUSY) log(ctx, "location: the measure before is still running: no answer");
    else if (rc == NL_NO_ANSWER) log(ctx, "location: the measure had no answer");
#if defined(_WIN32) || defined(RBSTEMS_WIN_CACHE_TEST)
    if (!rc) rc = write(ctx, l);
#else
    if (!rc) rc = native_location_checked(l) ? write(ctx, l) : NL_NO_ANSWER;
#endif
    free(l);
    if (rc == NL_TIMEOUT || rc == NL_BUSY || rc == NL_NO_ANSWER) return CORE_SLOW_NOT_NOW;
    return rc ? -1 : 0;
}
