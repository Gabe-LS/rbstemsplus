/* The resident watcher's host for the core's prepared-playlists check. */
#ifndef RBSP_WIN_WATCH_KEPT_H
#define RBSP_WIN_WATCH_KEPT_H

#include "core.h"
#include "host_facts.h"
#include "watch.h"

typedef struct {
    const watch_env *env;
    host_facts_env facts;
    const char *kept, *cache, *ort;
    char checked[32];                     /* also remembers a session whose marker couldn't be written */
    char rekordbox[COMPAT_PATH_MAX];       /* the verified folder from this check's facts */
    void (*session)(char *out, size_t n);
    void (*date)(void *ctx, double t, char *out, size_t n);
    int (*running)(const char *set);
    int (*notify)(const char *title, const char *body);
    int (*helper)(const char *exe, const char *const *argv, int argc, unsigned seconds, core_facts *out);
    int (*write)(const watch_env *env, const char *name, const char *text, size_t n);
} watch_kept;

int32_t watch_kept_step(void *ctx, core_step kind, const char *const *args, int32_t nargs, core_facts *out);
void watch_kept_check(watch_kept *k);

#endif
