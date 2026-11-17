/* native.c: the "native" dispatch (API-V1-NOTES decision 22, §11). Windows-only, in core/build.sh's
 * WIN_SRCS (the test, fuzz and dll builds, never lib's Mac archive), and under CORE_WIN_POLICY,
 * so a Mac build compiles nothing of it.
 *
 * Every host call passes life->host.ctx first, and the shell's entries need the shell's ctx while
 * the backend's need its own state. So, when the start config lists entries, life->host becomes a
 * table of trampolines whose ctx is the life: a shell entry calls the shell's own (kept in
 * life->shell, with its ctx), a listed one calls the backend with life->nst. life->native is then
 * cleared: the listed entries are provided (or NULL, "not provided", where the backend has none),
 * and the core's call sites, unchanged, reach them as any host entry. One indirect call more per
 * host call, on Windows only. */
#include "internal.h"
#include "native.h"

#include <stddef.h>

#ifdef CORE_WIN_POLICY

#define LIFE(ctx) ((core_life *)(ctx))
#define SHELL(ctx) (&LIFE(ctx)->shell)

/* ---------------------------------------------------------------- the shell's entries */

static void CORE_CALL t_render(void *ctx, core_snap *snap, const core_delta *delta) { SHELL(ctx)->render(SHELL(ctx)->ctx, snap, delta); }
/* with the admin native, the shell can't see the elevation: the core adds its bit (app.md §4.3:
 * not in front from ShellExecuteEx until the worker's exit is handled) */
static uint32_t CORE_CALL t_front_facts(void *ctx) {
    uint32_t f = SHELL(ctx)->front_facts(SHELL(ctx)->ctx);
    if (atomic_load_explicit(&LIFE(ctx)->native_elevating, memory_order_acquire) > 0) f |= CORE_FRONT_ELEVATING;
    return f;
}
static int32_t CORE_CALL t_modal_open(void *ctx) { return SHELL(ctx)->modal_open(SHELL(ctx)->ctx); }
static void CORE_CALL t_facts(void *ctx, core_facts *out) { SHELL(ctx)->facts(SHELL(ctx)->ctx, out); }
static void CORE_CALL t_post_main(void *ctx, core_job *job) { SHELL(ctx)->post_main(SHELL(ctx)->ctx, job); }
static void CORE_CALL t_post_work(void *ctx, core_work kind, core_job *job) { SHELL(ctx)->post_work(SHELL(ctx)->ctx, kind, job); }
static int32_t CORE_CALL t_dialog(void *ctx, const core_dialog *d, core_reply *r) { return SHELL(ctx)->dialog(SHELL(ctx)->ctx, d, r); }
static int32_t CORE_CALL t_pick_folder(void *ctx, const char *start, size_t start_len, core_reply *r) {
    return SHELL(ctx)->pick_folder(SHELL(ctx)->ctx, start, start_len, r);
}
static int32_t CORE_CALL t_slow_fact(void *ctx, core_slow kind, const char *key, size_t key_len, core_facts *out) {
    return SHELL(ctx)->slow_fact(SHELL(ctx)->ctx, kind, key, key_len, out);
}
static int32_t CORE_CALL t_helper_spawn(void *ctx, uint64_t run, const char *const *argv, int32_t argc) {
    return SHELL(ctx)->helper_spawn(SHELL(ctx)->ctx, run, argv, argc);
}
static void CORE_CALL t_helper_signal(void *ctx, uint64_t run, core_helper_sig sig) { SHELL(ctx)->helper_signal(SHELL(ctx)->ctx, run, sig); }
static int32_t CORE_CALL t_admin(void *ctx, int32_t kind, const char *version, const char *bundle_id, const char *bridge_sha, const char *ort_sha,
                                 int32_t force) {
    return SHELL(ctx)->admin(SHELL(ctx)->ctx, kind, version, bundle_id, bridge_sha, ort_sha, force);
}
static int32_t CORE_CALL t_step(void *ctx, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    return SHELL(ctx)->step(SHELL(ctx)->ctx, kind, args, nargs, out);
}
static void CORE_CALL t_close_reply(void *ctx, core_close what, int32_t proceed) { SHELL(ctx)->close_reply(SHELL(ctx)->ctx, what, proceed); }
static void CORE_CALL t_log(void *ctx, const char *line, size_t len) { SHELL(ctx)->log(SHELL(ctx)->ctx, line, len); }

/* ---------------------------------------------------------------- the backend's entries */

static void CORE_CALL n_post_work(void *ctx, core_work kind, core_job *job) { LIFE(ctx)->nb->post_work(LIFE(ctx)->nst, kind, job); }
static int32_t CORE_CALL n_slow_fact(void *ctx, core_slow kind, const char *key, size_t key_len, core_facts *out) {
    return LIFE(ctx)->nb->slow_fact(LIFE(ctx)->nst, kind, key, key_len, out);
}
static int32_t CORE_CALL n_helper_spawn(void *ctx, uint64_t run, const char *const *argv, int32_t argc) {
    return LIFE(ctx)->nb->helper_spawn(LIFE(ctx)->nst, run, argv, argc);
}
static void CORE_CALL n_helper_signal(void *ctx, uint64_t run, core_helper_sig sig) { LIFE(ctx)->nb->helper_signal(LIFE(ctx)->nst, run, sig); }
static int32_t CORE_CALL n_admin(void *ctx, int32_t kind, const char *version, const char *bundle_id, const char *bridge_sha, const char *ort_sha,
                                 int32_t force) {
    core_life *l = LIFE(ctx);
    atomic_fetch_add_explicit(&l->native_elevating, 1, memory_order_acq_rel);
    int32_t r = l->nb->admin(l->nst, kind, version, bundle_id, bridge_sha, ort_sha, force);
    atomic_fetch_sub_explicit(&l->native_elevating, 1, memory_order_acq_rel);
    return r;
}
static int32_t CORE_CALL n_step(void *ctx, core_step kind, const char *const *args, int32_t nargs, core_facts *out) {
    return LIFE(ctx)->nb->step(LIFE(ctx)->nst, kind, args, nargs, out);
}

/* ---------------------------------------------------------------- wiring */

/* wire() sets every entry: one added to core_host (after log, under CORE_WIN_POLICY) needs its
 * trampoline here, or it would be called with the life as its ctx */
_Static_assert(offsetof(core_host, log) + sizeof(void (*)(void)) == sizeof(core_host), "wire(): a new core_host entry needs a trampoline");

/* no backend (every build but the Windows DLL): each listed entry stays "not provided" */
static const native_backend native_none = {0};

#ifdef CORE_TEST_HOOKS
/* core_test_native_default's: test builds only, set by a test on main before its core_new (the
 * one mutable state of this file, never in a release object) */
static const native_backend *test_default;
static int test_fail_after_open;
#endif

static const native_backend *native_default(void) {
#ifdef CORE_TEST_HOOKS
    if (test_default) return test_default;
#endif
#if defined(_WIN32) && defined(CORE_NATIVE_WIN)
    return &native_win_backend;
#else
    return &native_none;
#endif
}

/* host = trampolines over the shell's table, the listed entries from the backend (NULL where it
 * has none); nb and nst set (nst may be NULL: then no listed entry is provided) */
static void wire(core_life *l) {
    const core_host *s = &l->shell;
    const native_backend *b = l->nst ? l->nb : &native_none;
    uint32_t n = l->native_listed;
    core_host *h = &l->host;
    h->ctx = l;
    h->render = s->render ? t_render : NULL;
    h->front_facts = s->front_facts ? t_front_facts : NULL;
    h->modal_open = s->modal_open ? t_modal_open : NULL;
    h->facts = s->facts ? t_facts : NULL;
    h->post_main = s->post_main ? t_post_main : NULL;
    h->dialog = s->dialog ? t_dialog : NULL;
    h->pick_folder = s->pick_folder ? t_pick_folder : NULL;
    h->close_reply = s->close_reply ? t_close_reply : NULL;
    h->log = s->log ? t_log : NULL;
    if (n & NATIVE_POST_WORK) h->post_work = b->post_work ? n_post_work : NULL;
    else h->post_work = s->post_work ? t_post_work : NULL;
    if (n & NATIVE_SLOW_FACT) h->slow_fact = b->slow_fact ? n_slow_fact : NULL;
    else h->slow_fact = s->slow_fact ? t_slow_fact : NULL;
    if (n & NATIVE_HELPER) {
        h->helper_spawn = b->helper_spawn ? n_helper_spawn : NULL;
        h->helper_signal = b->helper_signal ? n_helper_signal : NULL;
    } else {
        h->helper_spawn = s->helper_spawn ? t_helper_spawn : NULL;
        h->helper_signal = s->helper_signal ? t_helper_signal : NULL;
    }
    if (n & NATIVE_ADMIN) h->admin = b->admin ? n_admin : NULL;
    else h->admin = s->admin ? t_admin : NULL;
    if (n & NATIVE_STEP) h->step = b->step ? n_step : NULL;
    else h->step = s->step ? t_step : NULL;
}

/* the backend's state for c; 0, or CORE_E_NOMEM (nst NULL) */
static int open_backend(core_life *l, const native_backend *b) {
    l->nb = b;
    l->nst = NULL;
    if (b->open && !(l->nst = b->open(&l->c))) {
        wire(l);
        return CORE_E_NOMEM;
    }
    if (!b->open) l->nst = (void *)b;   /* a backend without state: any non-NULL marks it opened */
    wire(l);
    return CORE_OK;
}

static void close_backend(core_life *l) {
    if (l->nst && l->nb && l->nb->open && l->nb->close) l->nb->close(l->nst);
    l->nst = NULL;
}

int native_install(core_life *l, uint32_t listed) {
    l->shell = l->host;
    l->native_listed = listed;
    atomic_init(&l->native_elevating, 0);
    l->native = 0;   /* the listed entries are now provided by the table itself (or NULL) */
    int r = open_backend(l, native_default());
#ifdef CORE_TEST_HOOKS
    if (!r && test_fail_after_open) r = CORE_E_NOMEM;   /* core_new failing after a good open */
#endif
    return r;
}

void native_release(core_life *l) {
    if (l->native_listed) close_backend(l);
}

/* core_new's path keys (core.c's path_keys), by P_* */
static const struct { const char *key; int which; } path_names[] = {
    {"home", P_HOME},       {"support", P_SUPPORT}, {"app", P_APP},           {"helper", P_HELPER},
    {"cache_default", P_CACHE_DEFAULT}, {"log", P_LOG}, {"tmp", P_TMP},       {"watcher", P_WATCHER},
    {"model", P_MODEL},     {"root_ort", P_ROOT_ORT}, {"rekordbox", P_REKORDBOX}, {"kept", P_KEPT},
};
_Static_assert(sizeof path_names / sizeof *path_names == P_COUNT, "a name per path");

const char *native_core_path(const core *c, const char *key) {
    for (size_t i = 0; i < sizeof path_names / sizeof *path_names; i++)
        if (!strcmp(path_names[i].key, key)) return c->paths[path_names[i].which];
    return NULL;
}

const char *native_core_version(const core *c) { return c->version; }

const char *native_core_programs(const core *c) { return c->win_programs; }

void native_core_log(core *c, const char *line) { core_log(c->life, line); }

/* blockedByVersionList (Compat.swift): stemsPlusBlocked's and stemsCacheBlocked's reasons, each in
 * its own order, count only when they end with "checks again" */
int32_t native_blocked_by_list(const native_blocked_facts *f) {
    int32_t plus = 0, cache = 0;
    if (f->plus_answered) {
        plus_facts pf = {f->installed, f->engine ? f->engine : "", f->engine_supported};
        plus = feature_why_checks_again(plus_blocked(&pf));
    }
    if (f->cache_answered) {
        cache_facts kf = {f->admin, f->installed, f->rekordbox_version ? f->rekordbox_version : "", f->rekordbox_supported, 0,
                          f->root_ort_exists, 0, f->lib_exists, f->lib_signed, NULL, NULL};
        cache = feature_why_checks_again(cache_blocked_win(&kf, f->ort_listed, 0));
    }
    return plus || cache;
}

#ifdef CORE_TEST_HOOKS
void core_test_native_default(const native_backend *b, int fail_after_open) {
    test_default = b;
    test_fail_after_open = fail_after_open;
}

int32_t core_test_native(core *c, const native_backend *b) {
    core_life *l = c->life;
    if (!core_on_main(c) || !l->native_listed || !b) return CORE_E_STATE;
    close_backend(l);
    return open_backend(l, b);
}
#endif

#else
typedef int core_native_c_is_windows_only;   /* ISO C: a translation unit isn't empty */
#endif
