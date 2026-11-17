/* The core DLL's native admin as pure rules (native_admin.h). Pure C11. */
#include "native_admin.h"

#include <stdio.h>
#include <string.h>

int native_admin_verb(int kind) {
    switch (kind) {
    case NA_KIND_INSTALL:
    case NA_KIND_REINSTALL: return ADMIN_INSTALL;
    case NA_KIND_UNINSTALL: return ADMIN_UNINSTALL;
    case NA_KIND_REMOVE_ROOT: return ADMIN_REMOVE_ROOT_FOLDER;
    case NA_KIND_RESTORE: return ADMIN_RESTORE;
    case NA_KIND_FORGET: return NA_FORGET;
    default: return -1;
    }
}

int native_admin_needs_bridge(int verb) { return verb == ADMIN_INSTALL; }

int native_admin_code(const admin_result *r) {
    if (r->code == ADMIN_EXIT_USER_STOPPED) return ADMIN_EXIT_USER_STOPPED;
    if (r->update_first && r->code != ADMIN_EXIT_OLDER) return ADMIN_EXIT_PROTOCOL;
    if (r->code < 0) return ADMIN_EXIT_FAILED;
    return r->code;
}

int native_admin_bridge_ok(const payload_manifest *m, const char *bridge_sha, const char *bridge_name) {
    if (!m || !m->has_windows || !bridge_sha || !bridge_name) return 0;
    return !strcmp(bridge_sha, m->win_bridge.sha256) && !strcmp(bridge_name, m->win_bridge.file);
}

int native_admin_source_failed(int has_record) { return has_record ? ADMIN_EXIT_CHECK_FAILED : -1; }

void native_admin_result_line(int verb, int code, char *out, size_t n) {
    static const char *const names[ADMIN_VERB_COUNT] = {"install", "uninstall", "restore", "install-worker", "remove-root-folder"};
    const char *v = verb >= 0 && verb < ADMIN_VERB_COUNT ? names[verb] : "?";
    if (code == 0) snprintf(out, n, "  ok: %s", v);
    else if (code < 0) snprintf(out, n, "  FAILED: %s [exit 0x%08X]", v, (unsigned)code);
    else snprintf(out, n, "  FAILED: %s [exit %d]", v, code);
}

void native_admin_track_init(native_admin_track *t) { memset(t, 0, sizeof *t); }

unsigned native_admin_line(native_admin_track *t, int kind) {
    unsigned out = 0;
    if (!t->attempt) {
        t->attempt = 1;
        out |= NA_ATTEMPT;
    }
    if (kind == ADMIN_LINE_STILL_CLOSING) {
        if (!t->closing) out |= NA_CLOSING;
        t->closing = 1;
        return out;
    }
    if (kind == ADMIN_LINE_CLOSED && t->closing) {   /* rekordbox's process is gone */
        t->closing = 0;
        out |= NA_CLOSED;
    }
    if (kind == ADMIN_LINE_RESTART) out |= NA_RESTART;
    return out;
}

void native_admin_run_init(native_admin_run *r, admin_elevate_fn inner, void *inner_ctx, void (*tell)(void *ctx, unsigned what),
                           void (*not_started)(void *ctx), void *ctx) {
    memset(r, 0, sizeof *r);
    r->inner = inner;
    r->inner_ctx = inner_ctx;
    r->tell = tell;
    r->not_started = not_started;
    r->ctx = ctx;
    native_admin_track_init(&r->t);
}

static void run_line(void *ctx, const char *s, size_t n) {
    native_admin_run *r = ctx;
    if (r->critical && !r->in_critical) {   /* the worker runs: quitting waits for it from here */
        r->in_critical = 1;
        r->critical(r->ctx, 1);
    }
    char v[80];
    unsigned what = native_admin_line(&r->t, admin_line_kind(s, n, v, sizeof v));
    if (what) r->tell(r->ctx, what);
    r->line(r->lctx, s, n);   /* admin.c's: the log, the records into its result */
}

int native_admin_elevate(void *ctx, const char *exe, const char *params, const char *pipe, void (*line)(void *lctx, const char *s, size_t n),
                         void *lctx, void (*started)(void *sctx), void *sctx, int *code) {
    native_admin_run *r = ctx;
    r->line = line;
    r->lctx = lctx;
    int x;
    if (r->admit && !r->admit(r->ctx)) {   /* the quit is decided: no new elevation, no prompt (WC1) */
        if (started) started(sctx);
        x = ADMIN_EXIT_USER_STOPPED;
    } else
        x = r->inner(r->inner_ctx, exe, params, pipe, run_line, r, started, sctx, code);
    if (r->in_critical) {   /* the worker has ended (or its run can't be read any more) */
        r->in_critical = 0;
        r->critical(r->ctx, 0);
    }
    if (x == -1 && r->not_started) r->not_started(r->ctx);
    unsigned what = native_admin_end(&r->t, x == 0);
    if (what) r->tell(r->ctx, what);
    return x;
}

unsigned native_admin_end(native_admin_track *t, int ran) {
    unsigned out = 0;
    if (ran && !t->attempt) {
        t->attempt = 1;
        out |= NA_ATTEMPT;
    }
    if (t->closing) {
        t->closing = 0;
        out |= NA_CLOSED;
    }
    return out;
}
