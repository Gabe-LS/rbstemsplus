/* The lights and the gating (Controller.swift, Compat.swift, Location.swift, Prepare.swift):
 * what each light, button and status line says, from facts the host reads. */
#include "internal.h"

static int str_eq(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static int starts_with(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

/* ------------------------------------------------------------------ the lights */

core_text_id light_ax_text(int32_t look) {
    return look == LOOK_ON ? T_LIGHT_AX_ON : look == LOOK_LIMITED ? T_LIGHT_AX_LIMITED : T_LIGHT_AX_OFF;
}

int32_t light_plus(tbuf *tip, int32_t installed) {
    TB_TEXT0(tip, installed ? T_PLUS_ON : T_PLUS_OFF);
    return installed ? LOOK_ON : LOOK_OFF;
}

int32_t light_cache(tbuf *tip, const cache_light_in *in) {
#ifdef CORE_WIN_POLICY
    /* Windows: R1 (app.md §5.2): rekordbox's ONNX Runtime missing or not the expected one, so
     * Stems Cache isn't working, whatever else holds; the tip is R1's line (review C19) */
    if (in->win_r1 != R1_OK) { w_r1_line(tip, in->win_r1); return LOOK_LIMITED; }
#endif
    if (!in->installed) { TB_TEXT0(tip, T_CACHE_OFF); return LOOK_OFF; }
    if (!in->plus_on) {
        const char *engine = in->engine ? in->engine : "";
        if (!*engine) { TB_TEXT0(tip, T_CACHE_NO_ENGINE); return LOOK_LIMITED; }
        if (!in->chosen) { TB_TEXT0(tip, T_CACHE_OTHER_ACCOUNT); return LOOK_LIMITED; }
        if (!in->rekordbox_model) { TB_TEXT0(tip, T_CACHE_RB_MODEL_OFF); return LOOK_LIMITED; }
        if (!in->has_bridge_models) { TB_TEXT0(tip, T_CACHE_NEEDS_UPDATE); return LOOK_LIMITED; }
        if (in->model) {
            int listed = 0;
            for (int32_t i = 0; i < in->n_bridge_models && !listed; i++) listed = str_eq(in->bridge_models[i], in->model);
            if (!listed) {
                core_slot sl = SLOT_S(engine, engine);
                tb_text(tip, T_CACHE_ENGINE, &sl, 1);
                return LOOK_LIMITED;
            }
        }
    }
    if (in->location) { tb_puts(tip, in->location); return LOOK_LIMITED; }
    if (in->has_paused_floor) { w_cache_paused_tip(tip, in->paused_floor, in->drive); return LOOK_LIMITED; }
    TB_TEXT0(tip, T_CACHE_ON);
    return LOOK_ON;
}

/* ------------------------------------------------------------------ where the stems are saved */

static void drive_text(tbuf *b, core_text_id drive_id, core_text_id this_mac_id, const char *drive, const core_slot *why) {
    core_slot sl[2] = {SLOT_S(drive, drive ? drive : ""), why ? *why : SLOT_S(drive, "")};
#ifdef CORE_WIN_POLICY
    if (TB_NAMELESS(b, drive)) { tb_text(b, nameless_text(drive_id), why ? &sl[1] : NULL, why ? 1 : 0); return; }
#endif
    if (drive) tb_text(b, drive_id, sl, why ? 2 : 1);
    else tb_text(b, this_mac_id, why ? &sl[1] : NULL, why ? 1 : 0);
}

int location_light(tbuf *b, const loc_status *s, const loc_bridge *br, int32_t rekordbox_running) {
#ifdef CORE_WIN_POLICY
    const loc_bridge local_problem = {"unavailable", "folder", NULL};
    if (b->os == CORE_OS_WIN && !br && s->kind == LOC_THIS_MAC && s->probe == PROBE_MARKER_MISSING_OR_WRONG)
        br = &local_problem;
#endif
    int config_wrong = s->kind == LOC_CONFIG_UNREADABLE || s->kind == LOC_CONFIG_MALFORMED;
    if (br && !str_eq(br->state, "available") && !str_eq(br->reason, "disabled") && !(str_eq(br->reason, "config") && config_wrong)) {
        if (str_eq(br->state, "pending")) { drive_text(b, T_LOC_CHECKING_DRIVE, T_LOC_CHECKING_THIS_MAC, br->drive, NULL); return 1; }
        if (str_eq(br->reason, "not-connected") && s->kind == LOC_CHOSEN && s->probe == PROBE_AVAILABLE) {
            drive_text(b, T_LOC_AGAIN_SOON_DRIVE, T_LOC_AGAIN_SOON_THIS_MAC, br->drive, NULL);
            return 1;
        }
        if (str_eq(br->reason, "config")) { TB_TEXT0(b, T_LOC_CONFIG_SESSION); return 1; }
        const char *r = br->reason ? br->reason : "";
        core_text_id why = !strcmp(r, "not-connected") ? T_LOC_R_NOT_CONNECTED
            : !strcmp(r, "folder") || !strcmp(r, "marker") ? T_LOC_R_FOLDER
            : !strcmp(r, "read-only") ? T_LOC_R_READ_ONLY
            : !strcmp(r, "no-permission") ? T_LOC_R_NO_PERMISSION
            : !strcmp(r, "not-local") ? T_LOC_R_NOT_LOCAL
            : T_LOC_R_FAILED;
        char wb[512];
        tbuf w;
        tb_init(&w, b->os, wb, sizeof wb);
        TB_TEXT0(&w, why);
        if (w.err) b->err = w.err;
        core_slot ws = SLOT_N(why, wb, tb_len(&w));
        drive_text(b, T_LOC_NOT_USING_DRIVE, T_LOC_NOT_USING_THIS_MAC, br->drive, &ws);
        return 1;
    }
    core_slot d = SLOT_S(drive, s->drive ? s->drive : "");
#ifdef CORE_WIN_POLICY
#define D_TEXT(id) (TB_NAMELESS(b, s->drive) ? tb_text(b, nameless_text(id), NULL, 0) : tb_text(b, (id), &d, 1))
#else
#define D_TEXT(id) tb_text(b, (id), &d, 1)
#endif
    switch (s->kind) {
    case LOC_THIS_MAC: return 0;
    case LOC_CONFIG_UNREADABLE: TB_TEXT0(b, T_LOC_CONFIG_UNREADABLE); return 1;
    case LOC_CONFIG_MALFORMED: TB_TEXT0(b, T_LOC_CONFIG_MALFORMED); return 1;
    case LOC_BRIDGE_TOO_OLD: D_TEXT(T_LOC_BRIDGE_TOO_OLD); return 1;
    default: break;
    }
    switch (s->probe) {
    case PROBE_AVAILABLE: return 0;
    case PROBE_NOT_CONNECTED: {
        char cb[256];
        tbuf c;
        tb_init(&c, b->os, cb, sizeof cb);
        TB_TEXT0(&c, br && str_eq(br->state, "available") ? T_LOC_CONNECT_AUTO : rekordbox_running ? T_LOC_CONNECT_RESTART : T_LOC_CONNECT_OPEN);
        if (c.err) b->err = c.err;
        core_slot sl[2] = {d, SLOT_N(connect, cb, tb_len(&c))};
#ifdef CORE_WIN_POLICY
        if (TB_NAMELESS(b, s->drive)) { tb_text(b, nameless_text(T_LOC_NOT_CONNECTED), &sl[1], 1); return 1; }
#endif
        tb_text(b, T_LOC_NOT_CONNECTED, sl, 2);
        return 1;
    }
    case PROBE_FOLDER_MISSING: case PROBE_MARKER_MISSING_OR_WRONG: D_TEXT(T_LOC_FOLDER_MISSING); return 1;
    case PROBE_LINK: D_TEXT(T_LOC_LINK); return 1;
    case PROBE_NETWORK: D_TEXT(T_LOC_NETWORK); return 1;
    case PROBE_READ_ONLY: D_TEXT(T_LOC_READ_ONLY); return 1;
    default: D_TEXT(T_LOC_NO_PERMISSION); return 1;
    }
#undef D_TEXT
}

/* ------------------------------------------------------------------ what can be installed */

int32_t version_listed(const char *v, const char *const *list, int32_t n) {
    if (!v || !*v) return 0;
    for (int32_t i = 0; i < n; i++) {
        const char *l = list[i];
        size_t k = strlen(l);
        if (!strcmp(v, l) || (!strncmp(v, l, k) && v[k] == '.')) return 1;
    }
    return 0;
}

/* split(separator: ".") drops empty pieces; the first three, joined by "." */
void w_short_version(tbuf *b, const char *v) {
    int pieces = 0;
    for (const char *p = v ? v : ""; *p && pieces < 3;) {
        if (*p == '.') { p++; continue; }
        const char *e = strchr(p, '.');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (pieces++) tb_puts(b, ".");
        tb_put(b, p, n);
        p += n;
    }
}

int32_t plus_blocked(const plus_facts *f) {
    if (!f->rekordbox_installed) return FEAT_RB_NOT_INSTALLED;
    if (!f->engine || !*f->engine) return FEAT_NO_ENGINE;
    if (!f->engine_supported) return FEAT_ENGINE_UNLISTED;
    return FEAT_OK;
}

int32_t cache_blocked(const cache_facts *f) {
    if (!f->user_is_admin) return FEAT_NEEDS_ADMIN;
    if (!f->rekordbox_installed) return FEAT_RB_NOT_INSTALLED;
    if (!f->rekordbox_supported) return FEAT_RB_UNLISTED;
    if (f->cache_present && !f->root_ort_exists && f->prototype_exists) return FEAT_TEST_VERSION;
    if (!f->lib_exists) return FEAT_NO_ORT;
    if (!f->lib_signed) return FEAT_ORT_NOT_ORIGINAL;
    const char *ort = f->ort_version ? f->ort_version : "unknown";
    if (!starts_with(ort, f->ort_prefix ? f->ort_prefix : "")) return FEAT_ORT_UNLISTED;
    return FEAT_OK;
}

#ifdef CORE_WIN_POLICY
int32_t cache_blocked_win(const cache_facts *f, int32_t ort_listed, int32_t lib_unchecked) {
    if (!f->user_is_admin) return FEAT_NEEDS_ADMIN;
    if (!f->rekordbox_installed) return FEAT_RB_NOT_INSTALLED;
    if (!f->rekordbox_supported) return FEAT_RB_UNLISTED;
    if (f->cache_present && !f->root_ort_exists && f->prototype_exists) return FEAT_TEST_VERSION;
    if (lib_unchecked) return FEAT_NOT_CHECKED;   /* not "can't find it": it couldn't be looked at (HF7) */
    if (!f->lib_exists) return FEAT_NO_ORT;
    if (!f->lib_signed) return FEAT_ORT_NOT_ORIGINAL;
    if (!ort_listed) return FEAT_ORT_UNLISTED;
    return FEAT_OK;
}
#endif

int32_t feature_why_checks_again(int32_t why) {
    return why == FEAT_ENGINE_UNLISTED || why == FEAT_RB_UNLISTED || why == FEAT_ORT_UNLISTED;
}

void w_feature_why(tbuf *b, int32_t why, const char *engine, const char *rekordbox_version, const char *ort_version) {
    switch (why) {
    case FEAT_OK: return;
    case FEAT_RB_NOT_INSTALLED: TB_TEXT0(b, T_RB_NOT_INSTALLED); return;
    case FEAT_NO_ENGINE: TB_TEXT0(b, T_PLUS_NO_ENGINE); return;
    case FEAT_NEEDS_ADMIN: TB_TEXT0(b, T_NEEDS_ADMIN); return;
    case FEAT_TEST_VERSION: TB_TEXT0(b, T_CACHE_TEST_VERSION); return;
    case FEAT_NO_ORT: TB_TEXT0(b, T_CACHE_NO_ORT); return;
    case FEAT_ORT_NOT_ORIGINAL: TB_TEXT0(b, T_CACHE_ORT_NOT_ORIGINAL); return;
#ifdef CORE_WIN_POLICY
    case FEAT_NOT_CHECKED: TB_TEXT0(b, T_WIN_CACHE_NOT_CHECKED); return;
#endif
    case FEAT_ENGINE_UNLISTED: {
        core_slot sl = SLOT_S(engine, engine ? engine : "");
        tb_text(b, T_PLUS_ENGINE_UNLISTED, &sl, 1);
        break;
    }
    case FEAT_RB_UNLISTED: {
        char vb[256];
        tbuf v;
        tb_init(&v, b->os, vb, sizeof vb);
        w_short_version(&v, rekordbox_version);
        core_slot sl = SLOT_N(rekordbox, vb, tb_len(&v));
        tb_text(b, T_CACHE_RB_UNLISTED, &sl, 1);
        break;
    }
    default: {   /* FEAT_ORT_UNLISTED */
        char ub[64];
        tbuf u;
        tb_init(&u, b->os, ub, sizeof ub);
        if (!ort_version) TB_TEXT0(&u, T_UNKNOWN);
        core_slot sl = SLOT_S(ort, ort_version ? ort_version : ub);
        tb_text(b, T_CACHE_ORT_UNLISTED, &sl, 1);
        break;
    }
    }
    TB_TEXT0(b, T_CHECKS_AGAIN);
}

int w_standard_account_line(tbuf *b, int32_t admin, int32_t cache_traces, int32_t cache_chosen) {
    if (admin || !cache_traces) return 0;
    TB_TEXT0(b, cache_chosen ? T_STANDARD_REINSTALL_REMOVE : T_STANDARD_REMOVE);
    return 1;
}

/* ------------------------------------------------------------------ the buttons and the status line */

void gate_buttons(const gate_in *in, gate_out *g) {
    int plus_ok = in->plus_why == FEAT_OK, cache_ok = in->cache_why == FEAT_OK;
    memset(g, 0, sizeof *g);
    g->install_plus = !in->plus && plus_ok;
    g->install_cache = in->cache ? !in->chosen_cache && in->rekordbox_installed : cache_ok;
    g->reinstall = (in->chosen_model && plus_ok) || (in->chosen_cache && cache_ok);
    int p = in->chosen_model || !in->chosen_cache, k = in->chosen_cache;
    g->reinstall_features = p && k ? T_FEATURES_BOTH : k ? T_FEATURE_CACHE : T_FEATURE_PLUS;
    g->reinstall_plus = in->plus && in->chosen_model && plus_ok;
    g->reinstall_cache = in->cache && in->chosen_cache && cache_ok;
    g->settings = 1;
    g->uninstall_plus = in->plus;
    g->uninstall_cache = in->cache_traces && in->user_is_admin;
    g->uninstall_all = !in->cache_traces || in->user_is_admin;
}

void w_reinstall_title(tbuf *b, const gate_out *g) {
    char fb[128];
    tbuf f;
    tb_init(&f, b->os, fb, sizeof fb);
    TB_TEXT0(&f, (core_text_id)g->reinstall_features);
    if (f.err) b->err = f.err;
    core_slot sl = SLOT_N(features, fb, tb_len(&f));
    tb_text(b, T_REINSTALL_FEATURES, &sl, 1);
}

/* the status line's lines, each built into its own buffer so "lines.contains(why)" compares texts */
typedef struct { char buf[1024]; size_t n; } line_t;

static void line_of(line_t *l, tbuf *b, void (*fill)(tbuf *, const void *), const void *arg) {
    tbuf t;
    tb_init(&t, b->os, l->buf, sizeof l->buf);
    fill(&t, arg);
    if (t.err) b->err = t.err;
    l->n = t.len < sizeof l->buf ? t.len : sizeof l->buf - 1;
}

static void fill_plus_why(tbuf *t, const void *a) {
    const gate_in *in = a;
    w_feature_why(t, in->plus_why, in->engine, in->rekordbox_version, in->ort_version);
}
static void fill_cache_why(tbuf *t, const void *a) {
    const gate_in *in = a;
    w_feature_why(t, in->cache_why, in->engine, in->rekordbox_version, in->ort_version);
}

void w_status_lines(tbuf *b, const gate_in *in) {
    int n = 0;
#define LINE_SEP() do { if (n++) tb_puts(b, "\n"); } while (0)
    if (!in->own_bundle) { LINE_SEP(); TB_TEXT0(b, T_MOVE_TO_APPLICATIONS); }
    line_t pw = {{0}, 0}, cw = {{0}, 0};
    int plus_line = in->plus_why != FEAT_OK && (!in->plus || in->chosen_model);
    if (plus_line) {
        line_of(&pw, b, fill_plus_why, in);
        LINE_SEP();
        tb_put(b, pw.buf, pw.n);
    }
    char sb[256];
    tbuf st;
    tb_init(&st, b->os, sb, sizeof sb);
    int standard = w_standard_account_line(&st, in->user_is_admin, in->cache_traces, in->chosen_cache);
    if (st.err) b->err = st.err;
#ifdef CORE_WIN_POLICY
    if (b->os == CORE_OS_WIN && in->win_cache_problem && *in->win_cache_problem) {
        LINE_SEP();
        tb_puts(b, in->win_cache_problem);
    } else
#endif
    if (in->cache_why != FEAT_OK && (!in->cache || in->chosen_cache)) {
        line_of(&cw, b, fill_cache_why, in);
        int dup = (plus_line && pw.n == cw.n && !memcmp(pw.buf, cw.buf, cw.n));
        if (!in->own_bundle && !dup) {   /* moveToApplications is a line too */
            char mb[256];
            tbuf m;
            tb_init(&m, b->os, mb, sizeof mb);
            TB_TEXT0(&m, T_MOVE_TO_APPLICATIONS);
            dup = m.len == cw.n && !memcmp(mb, cw.buf, cw.n);
        }
        if (!dup && (!standard || in->cache_why != FEAT_NEEDS_ADMIN)) {
            LINE_SEP();
            tb_put(b, cw.buf, cw.n);
        }
    }
    if (standard) { LINE_SEP(); tb_put(b, sb, st.len < sizeof sb ? st.len : sizeof sb - 1); }
    if (in->cache_is_link) { LINE_SEP(); TB_TEXT0(b, T_CACHE_LINK); }
    if (in->location_notice) { LINE_SEP(); tb_puts(b, in->location_notice); }
    if (in->watcher_off) { LINE_SEP(); TB_TEXT0(b, T_WATCHER_OFF); }
    if (in->model_notice) { LINE_SEP(); tb_puts(b, in->model_notice); }
    if (in->notice) { LINE_SEP(); tb_puts(b, in->notice); }
#undef LINE_SEP
}

/* ------------------------------------------------------------------ why playlists can't be prepared */

int32_t w_prepare_problems(tbuf *b, const prep_facts *f) {
    if (!f->apple_silicon) { TB_TEXT0(b, T_PREP_NEEDS_ARM); return 1; }
    if (!f->helper_there) { TB_TEXT0(b, T_PREP_NO_HELPER); return 1; }
    if (!f->rekordbox_installed) { w_rb_text(b, T_PREP_NO_REKORDBOX, f->programs); return 1; }
    if (!f->cache_installed) { TB_TEXT0(b, T_PREP_NO_CACHE); return 1; }
    int32_t n = 0;
    size_t starts[8];   /* where each line starts in b: "out.contains(f.cacheTip)" compares texts */
#define NEXT() do { if (n) tb_puts(b, "\n"); starts[n++] = b->len; } while (0)
    if (!f->bridge_keeps || !f->ort_there) { NEXT(); TB_TEXT0(b, T_PREP_REINSTALL_CACHE); }
    if (!f->rekordbox_ok) {
        NEXT();
        char vb[256];
        tbuf v;
        tb_init(&v, b->os, vb, sizeof vb);
        w_short_version(&v, f->rekordbox_version);
        core_slot sl = SLOT_N(rekordbox, vb, tb_len(&v));
        tb_text(b, T_PREP_RB_UNLISTED, &sl, 1);
        TB_TEXT0(b, T_CHECKS_AGAIN);
    }
    if (f->config_unreadable) {
        NEXT();
        core_slot sl = SLOT_S(why, f->config_unreadable);
        tb_text(b, T_PREP_CONFIG_UNREADABLE, &sl, 1);
    } else if (!f->enabled) { NEXT(); TB_TEXT0(b, T_PREP_DISABLED); }
    if (f->cache_look != LOOK_ON && f->cache_tip && *f->cache_tip) {
        size_t k = strlen(f->cache_tip);
        int dup = 0;
        for (int32_t i = 0; i < n && !dup && !b->err; i++) {
            size_t end = i + 1 < n ? starts[i + 1] - 1 : b->len;
            dup = end - starts[i] == k && !memcmp(b->p + starts[i], f->cache_tip, k);
        }
        if (!dup) { NEXT(); tb_puts(b, f->cache_tip); }
    }
    if (f->quality == 1) { NEXT(); TB_TEXT0(b, T_PREP_SPEED); }
    if (f->model_sha && f->plus_on) {
        int ours = 0;
        for (int32_t i = 0; i < f->n_our_models && !ours; i++) ours = str_eq(f->our_models[i], f->model_sha);
        if (!ours) { NEXT(); TB_TEXT0(b, T_PREP_MODEL_CHANGED); }
    }
    if (!f->cache_folder && n == 0) { NEXT(); TB_TEXT0(b, T_PREP_NO_FOLDER); }
#undef NEXT
    return n;
}
