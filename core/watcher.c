/* The watcher's decisions (Watcher.swift, Actions.swift's reminders): its rounds, waiting for
 * rekordbox to settle, what to ask and what the answer does, its dialog. The host's operations
 * (sleeping, looking at processes, the lock) are callbacks; nothing here keeps state. The log
 * lines are the watcher log's own, word for word. */
#include "internal.h"
#include <stdio.h>

static void logf_n(void (*log)(void *, const char *), void *ctx, const char *pre, int64_t n, const char *post) {
    char b[256];
    snprintf(b, sizeof b, "%s%lld%s", pre, (long long)n, post);
    log(ctx, b);
}

/* ------------------------------------------------------------------ the rounds */

int32_t watcher_rounds(int32_t max, const watcher_round_ops *o) {
    int32_t n = 0;
    for (;;) {
        n++;
        int32_t go_on = o->check(o->ctx);
        o->release(o->ctx);
        if (!go_on || !o->again(o->ctx)) return n;
        if (n >= max) { logf_n(o->log, o->ctx, "changes kept coming during ", n, " checks: the next start looks again"); return n; }
        if (!o->retake(o->ctx)) { o->log(o->ctx, "something changed during the check; another start is checking it"); return n; }
        o->log(o->ctx, "something changed during the check: looking again");
    }
}

int32_t watcher_settle(const watcher_settle_ops *o) {
    for (;;) {
        int64_t waited = 0;
        int32_t busy_for = 0, said = 0;
        for (;;) {
            if (o->updating(o->ctx)) {
                if (!said) { o->log(o->ctx, "an update is running: waiting until it ends"); said = 1; }
                if (o->rekordbox_open(o->ctx)) o->in_session(o->ctx);
            } else if (o->app_busy(o->ctx) && busy_for < 120) busy_for++;
            else break;
            o->pause(o->ctx, 5);
            waited++;
        }
        if (waited > 0) logf_n(o->log, o->ctx, "waited ", waited * 5, " s for an update or RB Stems Plus to finish");
        if (o->app_busy(o->ctx)) { o->log(o->ctx, "RB Stems Plus is still busy: not asking"); return 0; }
        char id[128];
        if (o->half_uninstalled(o->ctx, id, sizeof id)) {
            char b[256];
            snprintf(b, sizeof b, "an uninstall didn't finish (%s): not asking", id);
            o->log(o->ctx, b);
            return 0;
        }
        if (!o->rekordbox_open(o->ctx)) return 1;
        o->in_session(o->ctx);
        o->log(o->ctx, "rekordbox is open: waiting until it quits");
        while (o->rekordbox_open(o->ctx)) { o->pause(o->ctx, 10); o->in_session(o->ctx); }
        o->pause(o->ctx, 3);
    }
}

/* ------------------------------------------------------------------ the question and its answer */

int32_t watcher_ask(const pstr *keys, int32_t nkeys, const keyset *put_off) {
    if (nkeys == 0) return WATCH_NOTHING;
    for (int32_t i = 0; i < nkeys; i++)
        if (!put_off || !keyset_has(put_off, keys[i].s, keys[i].n)) return WATCH_ASK;
    return WATCH_PUT_OFF;
}

void watcher_unsupported_key(tbuf *b, const char *key, size_t n, const char *const *rekordbox, int32_t nrb,
                             const char *const *engines, int32_t nengines, const char *ort_prefix) {
    tb_put(b, key, n);
    tb_puts(b, " unsupported with ");
    int first = 1;
    for (int32_t i = 0; i < nrb + nengines + 1; i++) {
        const char *v = i < nrb ? rekordbox[i] : i < nrb + nengines ? engines[i - nrb] : ort_prefix;
        if (!first) tb_puts(b, ",");
        first = 0;
        tb_puts(b, v ? v : "");
    }
}

/* ------------------------------------------------------------------ the dialog */

void watcher_dialog_build(tbuf *m, watcher_dialog *d, int32_t model_off, int32_t cache_off, const char *engine,
                          const char *rb, int32_t engine_ok, int32_t rekordbox_ok, int32_t plus_works) {
    int model_blocked = model_off && !engine_ok, cache_blocked = cache_off && !rekordbox_ok;
    int both = model_off && cache_off;
    core_slot e = SLOT_S(engine, engine ? engine : ""), r = SLOT_S(rekordbox, rb ? rb : "");
    memset(d, 0, sizeof *d);
    if ((!model_off || model_blocked) && (!cache_off || cache_blocked)) {
        char wb[512], ob[128];
        tbuf w, off;
        tb_init(&w, m->os, wb, sizeof wb);
        tb_init(&off, m->os, ob, sizeof ob);
        if (!both) {
            if (model_off) tb_text(&w, T_W_IT_ENGINE, &e, 1);
            else tb_text(&w, T_W_IT_REKORDBOX, &r, 1);
        } else {
            core_slot er[2] = {e, r};
            tb_text(&w, T_W_BOTH_BLOCKED, er, 2);
        }
        TB_TEXT0(&off, both ? T_W_OFF_BOTH : model_off ? T_PLUS_OFF : T_CACHE_OFF);
        if (w.err || off.err) m->err = w.err ? w.err : off.err;
        core_slot sl[2] = {SLOT_N(state, ob, tb_len(&off)), SLOT_N(why, wb, tb_len(&w))};
        tb_text(m, T_W_BLOCKED_MSG, sl, 2);
        d->title = cache_off ? T_W_TITLE_UPDATED : T_W_TITLE_ENGINE;
        d->nbuttons = 2;
        d->buttons[0] = T_OK;
        d->buttons[1] = T_W_DONT_ASK;
        d->blocked = 1;
        return;
    }
    d->nbuttons = 3;
    d->reinstall = model_off && !model_blocked ? (cache_off && !cache_blocked ? "model cache" : "model") : "cache";
    d->buttons[0] = T_W_REINSTALL_NOW;
    d->buttons[1] = T_W_REMIND_LATER;
    d->buttons[2] = T_W_DONT_ASK;
    d->title = T_W_TITLE_UPDATED;
    if (both) {
        if (cache_blocked) tb_text(m, T_W_BOTH_CACHE_BLOCKED, &r, 1);
        else if (model_blocked) tb_text(m, T_W_BOTH_MODEL_BLOCKED, &e, 1);
        else TB_TEXT0(m, T_W_BOTH);
        return;
    }
    if (cache_off) { TB_TEXT0(m, plus_works ? T_W_CACHE_OFF_PLUS_WORKS : T_W_CACHE_OFF); return; }
    d->title = T_W_TITLE_MODEL_BACK;
    TB_TEXT0(m, T_W_MODEL_OFF);
}

/* watcherCheck's switch on the button pressed (its title in Swift; its index here; -1: none) */
int32_t watcher_answer(const watcher_dialog *d, int32_t button) {
    if (button < 0 || button >= d->nbuttons) return WANSWER_PUT_OFF;
    switch (d->buttons[button]) {
    case T_W_REINSTALL_NOW: return WANSWER_REINSTALL;
    case T_W_DONT_ASK: return WANSWER_REMEMBER;
    case T_OK: return WANSWER_REMEMBER_UNSUPPORTED;
    default: return WANSWER_PUT_OFF;
    }
}

/* ------------------------------------------------------------------ prepared playlists, the reminders */

void watcher_set_run_pattern(tbuf *b, const char *name) {
    tb_puts(b, "rbsp-prepare .*--until [^ ]+ --keep ");
    tb_puts(b, name);
    tb_puts(b, "( |$)");
}

int32_t watcher_newly_not_ready(const prep_estimate *e, const keyset *noticed) {
    for (int32_t i = 0; i < e->ntracks; i++) {
        const prep_track *t = &e->tracks[i];
        int not_ready = (t->result.n == 7 && !memcmp(t->result.s, "prepare", 7)) || (t->result.n == 7 && !memcmp(t->result.s, "skipped", 7));
        if (not_ready && (!noticed || !keyset_has(noticed, t->file.s, t->file.n))) return 1;
    }
    return 0;
}

int32_t watcher_launchd_disabled(const char *out, size_t n, const char *label) {
    size_t ln = strlen(label);
    const char *p = out, *end = out + n;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *e = nl ? nl : end;
        if (e == p) { p = e + 1; continue; }   /* split(separator:) drops empty lines */
        /* trimmingCharacters(in: .whitespaces): spaces and tabs (the output is ASCII) */
        const char *a = p, *z = e;
        while (a < z && (*a == ' ' || *a == '\t')) a++;
        while (z > a && (z[-1] == ' ' || z[-1] == '\t')) z--;
        if ((size_t)(z - a) >= ln + 2 && a[0] == '"' && !memcmp(a + 1, label, ln) && a[ln + 1] == '"') {
            const char *s = a + ln + 2;
            while (s < z && (*s == ' ' || *s == '\t')) s++;
            size_t k = (size_t)(z - s);
            return (k == 11 && !memcmp(s, "=> disabled", 11)) || (k == 7 && !memcmp(s, "=> true", 7));
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

int32_t watcher_off(int32_t plist_there, int32_t loaded, int32_t disabled) { return plist_there && (disabled || !loaded); }
