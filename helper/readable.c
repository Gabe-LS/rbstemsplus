/* readable.c: opening a track without waiting forever (readable.h) */
#include "readable.h"
#include "path_rules.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const PROTECTED[] = {"Documents", "Desktop", "Downloads"};
#define SETTINGS "System Settings › Privacy & Security › Files and Folders › RB Stems Plus"

const char *home_folder(void) {
    static char h[PATH_MAX];
    if (!h[0]) plat_home(h, sizeof h);          /* "" when there is none */
    return h;
}

const char *protected_folder(const char *path) {
    const char *h = home_folder();
    size_t hl = strlen(h);
    if (!hl || strncmp(path, h, hl) || path[hl] != '/') return "";
    for (size_t i = 0; i < sizeof PROTECTED / sizeof *PROTECTED; i++) {
        size_t l = strlen(PROTECTED[i]);
        if (!strncmp(path + hl + 1, PROTECTED[i], l) && (path[hl + 1 + l] == '/' || !path[hl + 1 + l])) return PROTECTED[i];
    }
    return "";
}

/* the folder a wait is remembered for: the protected folder, else the volume (/Volumes/<name>),
 * else the file's own folder */
static void wait_key(const char *path, char *key, size_t n) {
    const char *pf = protected_folder(path);
    if (pf[0]) { snprintf(key, n, "%s/%s", home_folder(), pf); return; }
    size_t root = paths_other_drive(path);
    if (root) {
        snprintf(key, n, "%.*s", (int)root, path);
        return;
    }
    const char *s = strrchr(path, '/');
    snprintf(key, n, "%.*s", s ? (int)(s - path) : 0, path);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done, abandoned, err;
    plat_finfo st;
    char path[PATH_MAX];
} job;

static void *run(void *arg) {
    job *j = arg;
    plat_finfo st = {0};
    plat_file f;
    int err = plat_finfo_path(j->path, &st);
    if (!err && st.kind == PLAT_FILE && !(err = plat_file_open_path(j->path, &f))) plat_file_close(f);
    pthread_mutex_lock(&j->mu);
    j->done = 1;
    j->err = err;
    j->st = st;
    int gone = j->abandoned;
    pthread_cond_signal(&j->cv);
    pthread_mutex_unlock(&j->mu);
    if (gone) { pthread_mutex_destroy(&j->mu); pthread_cond_destroy(&j->cv); free(j); }
    return NULL;
}

static char waited[32][PATH_MAX];       /* folders whose read was given up */
static int nwaited;

read_state readable(const char *path, double secs, plat_finfo *st, char *why, size_t n, const char **folder) {
    const char *pf = protected_folder(path);
    char key[PATH_MAX];
    *folder = pf;
    why[0] = 0;
    wait_key(path, key, sizeof key);
    char place[PATH_MAX + 16];
    if (pf[0]) snprintf(place, sizeof place, "your %s folder", pf);
    else snprintf(place, sizeof place, "%s", key[0] ? key : "/");
    for (int i = 0; i < nwaited; i++)
        if (!strcmp(waited[i], key)) {
            snprintf(why, n, "macOS is asking whether RB Stems Plus may read %s: answer its prompt, then check again", place);
            return READ_ASKING;
        }
    job *j = calloc(1, sizeof *j);
    if (!j || strlen(path) >= sizeof j->path) { free(j); snprintf(why, n, "the file can't be read: its path is too long"); return READ_FAILED; }
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    snprintf(j->path, sizeof j->path, "%s", path);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, run, j)) {
        pthread_attr_destroy(&a);
        run(j);                         /* no thread: read here (it may wait) */
    } else pthread_attr_destroy(&a);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    long long ns = until.tv_nsec + (long long)(secs * 1e9);
    until.tv_sec += (time_t)(ns / 1000000000LL);
    until.tv_nsec = (long)(ns % 1000000000LL);
    pthread_mutex_lock(&j->mu);
    while (!j->done && pthread_cond_timedwait(&j->cv, &j->mu, &until) != ETIMEDOUT) {}
    if (!j->done) {
        j->abandoned = 1;               /* the thread frees it when its read returns */
        pthread_mutex_unlock(&j->mu);
        if (nwaited < 32) snprintf(waited[nwaited++], PATH_MAX, "%s", key);
        snprintf(why, n, "macOS is asking whether RB Stems Plus may read %s: answer its prompt, then check again", place);
        return READ_ASKING;
    }
    int err = j->err;
    *st = j->st;
    pthread_mutex_unlock(&j->mu);
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv);
    free(j);
    if (!err && st->kind != PLAT_FILE) { snprintf(why, n, "not a file"); return READ_FAILED; }
    if (!err) return READ_OK;
    if (err == ENOENT || err == ENOTDIR) {
        snprintf(why, n, "the file isn't there (moved, renamed, or its drive isn't connected)");
        return READ_ABSENT;
    }
    if (err == EPERM) {                 /* the privacy setting (a file's own permissions: EACCES) */
        if (pf[0]) snprintf(why, n, "macOS doesn't let RB Stems Plus read %s: allow it in " SETTINGS, place);
        else snprintf(why, n, "macOS doesn't let RB Stems Plus read it: allow it in " SETTINGS);
        return READ_DENIED;
    }
    snprintf(why, n, "the file can't be read: %s", strerror(err));
    return READ_FAILED;
}
