/* The pure functions (M5) against 1.3.0's Swift: every table of vectors_swift.h (generated from
 * the frozen Swift, core/tests/swift/oracle, by core/tests/gen-vectors.sh, core/tests/swift/vectors.swift) run through the C port,
 * whose result must be the Swift's byte for byte. Each table's rows say what the Swift was given;
 * the dumps here are written as vectors.swift writes them. */
#include "../internal.h"
#include "vectors_swift.h"
#include <stdio.h>
#include <stdlib.h>

static int *vp, *vf;
static int shown;
static void fail_row(const char *table, int row, const char *got, const char *want) {
    (*vf)++;
    if (shown++ < 25) printf("FAIL %s row %d\n  got:  %s\n  want: %s\n", table, row, got, want);
}
#define COMPARE(table, row, got, want) do { if (!strcmp((got), (want))) (*vp)++; else fail_row(table, row, got, want); } while (0)

/* ------------------------------------------------------------------ dumps, as vectors.swift's */

typedef struct { char *p; size_t n, cap; } dbuf;
static void dput(dbuf *d, const char *s, size_t n) {
    if (d->n + n + 1 > d->cap) {
        size_t c = d->cap ? d->cap : 1024;
        while (c < d->n + n + 1) c *= 2;
        d->p = realloc(d->p, c);
        if (!d->p) abort();
        d->cap = c;
    }
    memcpy(d->p + d->n, s, n);
    d->n += n;
    d->p[d->n] = 0;
}
static void dputs(dbuf *d, const char *s) { dput(d, s, strlen(s)); }
static void desc(dbuf *d, pstr s) {
    char h[4];
    for (size_t i = 0; i < s.n; i++) {
        unsigned char b = (unsigned char)s.s[i];
        if (b < 0x20 || b == '\\' || b == '|') { snprintf(h, sizeof h, "\\%02x", b); dputs(d, h); }
        else dput(d, (const char *)&b, 1);
    }
}
static void dd17(dbuf *d, double x) { char b[64]; snprintf(b, sizeof b, "%.17g", x); dputs(d, b); }
static void di(dbuf *d, int64_t x) { char b[32]; snprintf(b, sizeof b, "%lld", (long long)x); dputs(d, b); }
static void dbool(dbuf *d, int x) { dputs(d, x ? "true" : "false"); }
static void dtb(dbuf *d, const tbuf *t) { dput(d, t->p, t->len < t->cap ? t->len : t->cap - 1); }
#define SEP(d) dputs(d, "|")

static void dump_track(dbuf *d, const prep_track *t) {
    dputs(d, "track|"); desc(d, t->file); SEP(d); desc(d, t->result); SEP(d); desc(d, t->why); SEP(d); desc(d, t->access); SEP(d); desc(d, t->folder); SEP(d);
    di(d, t->entries); SEP(d); di(d, t->written); SEP(d); di(d, t->already); SEP(d); di(d, t->missing); SEP(d); di(d, t->refused); SEP(d);
    dd17(d, t->seconds); SEP(d); dd17(d, t->gb); SEP(d); desc(d, prep_last_path_component(t->file));
}

static void dump_run(dbuf *d, int r, const prep_run_line *l) {
    if (r <= 0) { dputs(d, "nil"); return; }
    switch (l->kind) {
    case RUN_PROGRESS: dputs(d, "progress|"); di(d, l->at); SEP(d); di(d, l->files); SEP(d); di(d, l->chunk); SEP(d); di(d, l->chunks); break;
    case RUN_TRACK: dump_track(d, &l->track); break;
    case RUN_ROOM: dputs(d, "room|"); di(d, l->removed); SEP(d); dd17(d, l->cache_gb); break;
    default:
        dputs(d, "done|"); di(d, l->done); SEP(d);
        if (l->has_stopped) desc(d, l->stopped); else dputs(d, "nil");
        SEP(d); dd17(d, l->kept_gb); SEP(d);
        if (l->has_error) desc(d, l->error); else dputs(d, "nil");
    }
}

static void dump_estimate(dbuf *d, const prep_estimate *e) {
    if (!prep_estimate_ok(e)) { dputs(d, "nil"); return; }
    dputs(d, "est|");
    if (e->has_refused) desc(d, e->refused); else dputs(d, "nil");
    SEP(d); di(d, e->ready); SEP(d); di(d, e->prepare); SEP(d); di(d, e->skipped); SEP(d); di(d, e->unchecked); SEP(d); di(d, e->asking); SEP(d);
    di(d, e->fit_files); SEP(d); dd17(d, e->time_s); SEP(d); dd17(d, e->need_gb); SEP(d); dd17(d, e->kept_gb); SEP(d); dd17(d, e->share_gb); SEP(d);
    dd17(d, e->max_gb); SEP(d); dd17(d, e->max_gb_needed); SEP(d); dbool(d, e->fits); SEP(d); dbool(d, e->disk_ok); SEP(d);
    for (int32_t i = 0; i < e->nfolders; i++) { if (i) dputs(d, ","); desc(d, e->folders[i]); }
    for (int32_t i = 0; i < e->ntracks; i++) { dputs(d, "\n"); dump_track(d, &e->tracks[i]); }
}

/* the lines of a whole output as Swift's split(separator: "\n") gives them: a "\r\n" doesn't split */
typedef void (*line_fn)(void *arg, const char *p, size_t n);
static void swift_lines(const char *s, line_fn f, void *arg) {
    size_t n = strlen(s), start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && (s[i] != '\n' || (i > 0 && s[i - 1] == '\r'))) continue;
        if (i > start) f(arg, s + start, i - start);   /* omittingEmptySubsequences */
        start = i + 1;
    }
}

typedef struct { arena *a; prep_estimate *e; } est_arg;
static void est_line(void *arg, const char *p, size_t n) {
    est_arg *x = arg;
    if (prep_estimate_line(x->a, x->e, p, n) < 0) abort();
}

static double num(const char *s) { return strtod(s, NULL); }
static int nil(const char *s) { return !strcmp(s, "nil"); }
static int b01(const char *s) { return s[0] == '1'; }

#define TB(name, cap) char name##_buf[cap]; tbuf name; tb_init(&name, CORE_OS_MAC, name##_buf, sizeof name##_buf)
#define ROWS(t) (int)(sizeof t / sizeof *t)

/* ------------------------------------------------------------------ the tables */

static void t_mask(void) {
    static const char *const names1[] = {"DJ Smith", "dj"}, *const names2[] = {"José Müller", "test", "a"};
    static const char *const keep[] = {"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"};
    for (int i = 0; i < ROWS(SV_MASK); i++) {
        const swift_vec *v = &SV_MASK[i];
        core_mask_ctx mc = {sizeof mc, 0, "/Users/dj", names1, 2, 2, keep};
        if (v->n == 2) { mc.home = "/Users/test"; mc.names = names2; mc.n_names = 3; mc.n_allowed = 0; mc.allowed = NULL; }
        size_t len = 0;
        char b[8192];
        int32_t r = core_mask(&mc, v->a[0], CORE_NTS, b, sizeof b, &len);
        if (r != CORE_OK) snprintf(b, sizeof b, "<error %d>", r);
        COMPARE("SV_MASK", i, b, v->want);
    }
}

/* a row of [text, home, names...] through core_mask, no keep set: the result in b */
static void mask_row(const swift_vec *v, char *b, size_t cap) {
    core_mask_ctx mc = {sizeof mc, 0, v->a[1], v->a + 2, v->n - 2, 0, NULL};
    size_t len;
    int32_t r = core_mask(&mc, v->a[0], CORE_NTS, b, cap, &len);
    if (r != CORE_OK) snprintf(b, cap, "<error %d>", r);
}

/* masking outside ASCII (review Q7: at least what 1.3.0 masks): equal to Swift */
static void t_mask_unicode(void) {
    static char b[65536];
    for (int i = 0; i < ROWS(SV_MASK_UNICODE); i++) {
        mask_row(&SV_MASK_UNICODE[i], b, sizeof b);
        COMPARE("SV_MASK_UNICODE", i, b, SV_MASK_UNICODE[i].want);
    }
}

/* where the core masks more than 1.3.0 by design (vectors.swift says why): C's result pinned, and
 * Swift's must still be the other one, so a change on either side shows */
static void t_mask_more(void) {
    static const char *const pinned[] = {
        "a <user> b",
        "a <user> b",
    };
    char b[1024];
    for (int i = 0; i < ROWS(SV_MASK_MORE) && i < ROWS(pinned); i++) {
        const swift_vec *v = &SV_MASK_MORE[i];
        mask_row(v, b, sizeof b);
        COMPARE("SV_MASK_MORE (C, pinned)", i, b, pinned[i]);
        if (strcmp(v->want, pinned[i])) (*vp)++;
        else fail_row("SV_MASK_MORE (Swift now equals C: make it an SV_MASK_UNICODE row)", i, b, v->want);
    }
    if (ROWS(SV_MASK_MORE) != ROWS(pinned)) fail_row("SV_MASK_MORE", -1, "a row count", "one pinned C result per row");
}

static void t_lines(void) {
    arena a = {0};
    dbuf d = {0};
    for (int i = 0; i < ROWS(SV_RUN_LINE); i++) {
        prep_run_line l;
        int r = prep_run_line_parse(&a, SV_RUN_LINE[i].a[0], strlen(SV_RUN_LINE[i].a[0]), &l);
        d.n = 0;
        dump_run(&d, r, &l);
        COMPARE("SV_RUN_LINE", i, d.p, SV_RUN_LINE[i].want);
        arena_reset(&a);
    }
    for (int i = 0; i < ROWS(SV_PLAYLIST); i++) {
        prep_playlist p;
        int r = prep_playlist_parse(&a, SV_PLAYLIST[i].a[0], strlen(SV_PLAYLIST[i].a[0]), &p);
        d.n = 0;
        if (r <= 0) dputs(&d, "nil");
        else { dputs(&d, "pl|"); desc(&d, p.id); SEP(&d); desc(&d, p.parent); SEP(&d); desc(&d, p.name); SEP(&d); desc(&d, p.kind); SEP(&d); di(&d, p.tracks); }
        COMPARE("SV_PLAYLIST", i, d.p, SV_PLAYLIST[i].want);
        arena_reset(&a);
    }
    for (int i = 0; i < ROWS(SV_TRACK_LIST); i++) {
        pstr f;
        int r = prep_track_list_parse(&a, SV_TRACK_LIST[i].a[0], strlen(SV_TRACK_LIST[i].a[0]), &f);
        d.n = 0;
        if (r <= 0) dputs(&d, "nil"); else desc(&d, f);
        COMPARE("SV_TRACK_LIST", i, d.p, SV_TRACK_LIST[i].want);
        arena_reset(&a);
    }
    free(d.p);
    arena_free(&a);
}

static int parse_est(arena *a, const char *text, prep_estimate *e) {
    prep_estimate_init(e);
    est_arg x = {a, e};
    swift_lines(text, est_line, &x);
    return prep_estimate_ok(e);
}

/* the input of the table's rows of the same text: found in SV_ESTIMATE */
static void t_estimates(void) {
    arena a = {0};
    dbuf d = {0};
    for (int i = 0; i < ROWS(SV_ESTIMATE); i++) {
        prep_estimate e;
        parse_est(&a, SV_ESTIMATE[i].a[0], &e);
        d.n = 0;
        dump_estimate(&d, &e);
        COMPARE("SV_ESTIMATE", i, d.p, SV_ESTIMATE[i].want);
        arena_reset(&a);
    }
    for (int i = 0; i < ROWS(SV_SET_STATE); i++) {
        prep_estimate e;
        int ok = parse_est(&a, SV_SET_STATE[i].a[0], &e);
        TB(t, 4096);
        w_set_state(&t, ok ? &e : NULL);
        COMPARE("SV_SET_STATE", i, t.p, SV_SET_STATE[i].want);
        arena_reset(&a);
    }
    for (int i = 0; i < ROWS(SV_EST_SUMMARY); i++) {
        prep_estimate e;
        parse_est(&a, SV_EST_SUMMARY[i].a[0], &e);
        TB(t, 4096);
        w_estimate_summary(&t, &e);
        COMPARE("SV_EST_SUMMARY", i, t.p, SV_EST_SUMMARY[i].want);
        arena_reset(&a);
    }
    for (int i = 0; i < ROWS(SV_DOESNT_FIT); i++) {
        prep_estimate e;
        parse_est(&a, SV_DOESNT_FIT[i].a[0], &e);
        TB(t, 4096);
        fit_choices c;
        w_doesnt_fit(&t, &e, num(SV_DOESNT_FIT[i].a[1]), &c);
        d.n = 0;
        dtb(&d, &t);
        for (int32_t k = 0; k < c.nbuttons; k++) {
            TB(bt, 256);
            w_fit_button(&bt, &c, k);
            SEP(&d);
            dtb(&d, &bt);
        }
        SEP(&d);
        if (c.has_raise) dd17(&d, c.raise_to); else dputs(&d, "nil");
        COMPARE("SV_DOESNT_FIT", i, d.p, SV_DOESNT_FIT[i].want);
        arena_reset(&a);
    }
    for (int i = 0; i < ROWS(SV_KEPT_NOTICE); i++) {
        prep_estimate e;
        parse_est(&a, SV_KEPT_NOTICE[i].a[0], &e);
        TB(t, 4096);
        if (!w_kept_notice(&t, &e)) tb_puts(&t, "nil");
        COMPARE("SV_KEPT_NOTICE", i, t.p, SV_KEPT_NOTICE[i].want);
        arena_reset(&a);
    }
    free(d.p);
    arena_free(&a);
}

static void t_words(void) {
    for (int i = 0; i < ROWS(SV_NUMBERS); i++) {
        double x = num(SV_NUMBERS[i].a[0]);
        TB(t, 512);
        w_duration(&t, x);
        tb_puts(&t, "|");
        w_size(&t, x);
        char c[32], s[32];
        fmt_config_number(x, c);
        fmt_swift_double(x, s);
        tb_puts(&t, "|");
        tb_puts(&t, c);
        tb_puts(&t, "|");
        tb_puts(&t, s);
        COMPARE("SV_NUMBERS", i, t.p, SV_NUMBERS[i].want);
    }
    for (int i = 0; i < ROWS(SV_BYTES); i++) {
        int64_t b = strtoll(SV_BYTES[i].a[0], NULL, 10);
        TB(t, 512);
        w_about_size(&t, b);
        tb_puts(&t, "|");
        w_size_words(&t, b);
        tb_puts(&t, "|");
        w_stems_size(&t, STEMS_MEASURED, b);
        tb_puts(&t, "|");
        w_stems_size(&t, STEMS_MEASURING, 0);
        w_stems_size(&t, STEMS_FAILED, 0);
        w_stems_size(&t, STEMS_NOT_CONNECTED, 0);
        COMPARE("SV_BYTES", i, t.p, SV_BYTES[i].want);
    }
    for (int i = 0; i < ROWS(SV_FLOOR); i++) {
        const swift_vec *v = &SV_FLOOR[i];
        int64_t free_b = nil(v->a[0]) ? -1 : strtoll(v->a[0], NULL, 10), disk = nil(v->a[1]) ? -1 : strtoll(v->a[1], NULL, 10);
        int64_t floor, tip_floor = state_free_floor(disk);
        int paused = state_paused_floor(free_b, disk, &floor);
        TB(t, 1024);
        tb_i64(&t, tip_floor);
        tb_puts(&t, "|");
        if (paused) tb_i64(&t, floor); else tb_puts(&t, "nil");
        tb_puts(&t, "|");
        tb_i64(&t, cache_floor_gb(tip_floor));
        tb_puts(&t, "|");
        w_cache_paused_tip(&t, tip_floor, NULL);
        tb_puts(&t, "|");
        w_cache_paused_tip(&t, tip_floor, "GLS");
        tb_puts(&t, "|");
        w_cache_paused_at_install(&t, tip_floor, NULL);
        tb_puts(&t, "|");
        w_cache_paused_at_install(&t, tip_floor, "My \"Drive\"");
        COMPARE("SV_FLOOR", i, t.p, v->want);
    }
    for (int i = 0; i < ROWS(SV_TRACK_ROW); i++) {
        const swift_vec *v = &SV_TRACK_ROW[i];
        prep_track tr;
        memset(&tr, 0, sizeof tr);
        tr.file = (pstr){"/m/x.mp3", 8};
        tr.result = (pstr){v->a[0], strlen(v->a[0])};
        tr.written = atoi(v->a[1]);
        tr.already = atoi(v->a[2]);
        tr.refused = atoi(v->a[3]);
        tr.missing = atoi(v->a[4]);
        tr.why = (pstr){v->a[5], strlen(v->a[5])};
        tr.seconds = num(v->a[6]);
        tr.entries = 1;
        tr.gb = 0.1;
        TB(t, 1024);
        w_result_text(&t, &tr, 1);
        tb_puts(&t, "|");
        w_result_text(&t, &tr, 0);
        tb_puts(&t, "|");
        w_prepared_note(&t, &tr);
        tb_puts(&t, "|");
        w_track_note(&t, &tr);
        COMPARE("SV_TRACK_ROW", i, t.p, v->want);
    }
    for (int i = 0; i < ROWS(SV_RUN_TEXTS); i++) {
        const swift_vec *v = &SV_RUN_TEXTS[i];
        pstr st = {v->a[2], strlen(v->a[2])};
        int has_st = !nil(v->a[2]), has_err = !nil(v->a[3]);
        TB(t, 1024);
        w_last_run_line(&t, atoi(v->a[0]), atoi(v->a[1]), has_st ? &st : NULL, has_err);
        tb_puts(&t, "|");
        const char *name = "Fri “day”";
        w_run_end_title(&t, (pstr){name, strlen(name)}, atoi(v->a[4]), b01(v->a[5]), has_st ? &st : NULL, atoi(v->a[0]), atoi(v->a[1]), has_err, b01(v->a[6]));
        COMPARE("SV_RUN_TEXTS", i, t.p, v->want);
    }
}

static const char *look_name(int32_t look) { return look == LOOK_ON ? "on" : look == LOOK_LIMITED ? "limited" : "off"; }

static void t_lights(void) {
    for (int i = 0; i < ROWS(SV_CACHE_LIGHT); i++) {
        const swift_vec *v = &SV_CACHE_LIGHT[i];
        TB(t, 1024);
        if (!strcmp(v->a[0], "plus")) {
            int32_t l1 = light_plus(&t, 1);
            tb_puts(&t, "|");
            int32_t l0 = light_plus(&t, 0);
            tb_puts(&t, "|");
            tb_puts(&t, look_name(l1));
            tb_puts(&t, "|");
            tb_puts(&t, look_name(l0));
            COMPARE("SV_CACHE_LIGHT", i, t.p, v->want);
            continue;
        }
        cache_light_in in;
        memset(&in, 0, sizeof in);
        in.installed = b01(v->a[0]);
        in.plus_on = b01(v->a[1]);
        in.chosen = b01(v->a[2]);
        in.rekordbox_model = b01(v->a[3]);
        const char *models[4];
        char mcopy[64];
        if (!nil(v->a[4])) {
            in.has_bridge_models = 1;
            snprintf(mcopy, sizeof mcopy, "%s", v->a[4]);
            for (char *p = mcopy, *tok; p && *p && (tok = strsep(&p, ","));) models[in.n_bridge_models++] = tok;
            in.bridge_models = models;
        }
        in.model = nil(v->a[5]) ? NULL : v->a[5];
        in.engine = v->a[6];
        if (!nil(v->a[7])) { in.has_paused_floor = 1; in.paused_floor = strtoll(v->a[7], NULL, 10); }
        in.location = nil(v->a[8]) ? NULL : v->a[8];
        in.drive = nil(v->a[9]) ? NULL : v->a[9];
        int32_t look = light_cache(&t, &in);
        TB(row, 1024);
        tb_puts(&row, look_name(look));
        tb_puts(&row, "|");
        tb_put(&row, t.p, t.len);
        tb_puts(&row, "|");
        TB_TEXT0(&row, light_ax_text(look));
        COMPARE("SV_CACHE_LIGHT", i, row.p, v->want);
    }
}

/* driveName for the vectors' dirs (the mounts vectors.swift gives locationLight) */
static const char *drive_of(const char *dir) {
    if (!strncmp(dir, "/Volumes/GLS/", 13)) return "GLS";
    if (!strncmp(dir, "/Volumes/My \"Drive\"/", 20)) return "My \"Drive\"";
    return "?";
}

static void t_location(void) {
    static const char *const probes[] = {"available", "notConnected", "folderMissing", "markerMissingOrWrong", "link", "network", "readOnly", "noPermission"};
    for (int i = 0; i < ROWS(SV_LOCATION_LIGHT); i++) {
        const swift_vec *v = &SV_LOCATION_LIGHT[i];
        char name[256], bridge[256];
        snprintf(name, sizeof name, "%s", v->a[0]);
        snprintf(bridge, sizeof bridge, "%s", v->a[1]);
        loc_status s = {LOC_THIS_MAC, 0, NULL};
        char *p = name, *kind = strsep(&p, "|");
        if (!strcmp(kind, "unreadable")) s.kind = LOC_CONFIG_UNREADABLE;
        else if (!strcmp(kind, "malformed")) s.kind = LOC_CONFIG_MALFORMED;
        else if (!strcmp(kind, "old")) { s.kind = LOC_BRIDGE_TOO_OLD; s.drive = drive_of(strsep(&p, "|")); }
        else if (!strcmp(kind, "chosen")) {
            s.kind = LOC_CHOSEN;
            s.drive = drive_of(strsep(&p, "|"));
            const char *pr = strsep(&p, "|");
            for (int k = 0; k < 8; k++) if (!strcmp(pr, probes[k])) s.probe = k;
        }
        loc_bridge b, *bp = NULL;
        if (!nil(bridge)) {
            char *q = bridge;
            b.state = strsep(&q, "|");
            b.reason = strsep(&q, "|");
            const char *loc = q ? q : "";
            b.drive = *loc ? drive_of(loc) : NULL;
            bp = &b;
        }
        TB(t, 1024);
        if (!location_light(&t, &s, bp, b01(v->a[2]))) tb_puts(&t, "nil");
        COMPARE("SV_LOCATION_LIGHT", i, t.p, v->want);
    }
}

static void t_problems(void) {
    for (int i = 0; i < ROWS(SV_PREPARE_PROBLEMS); i++) {
        const swift_vec *v = &SV_PREPARE_PROBLEMS[i];
        prep_facts f;
        memset(&f, 0, sizeof f);
        f.apple_silicon = b01(v->a[0]); f.helper_there = b01(v->a[1]); f.rekordbox_installed = b01(v->a[2]); f.cache_installed = b01(v->a[3]);
        f.bridge_keeps = b01(v->a[4]); f.ort_there = b01(v->a[5]); f.rekordbox_ok = b01(v->a[6]); f.enabled = b01(v->a[7]); f.plus_on = b01(v->a[8]);
        f.cache_folder = b01(v->a[9]);
        f.rekordbox_version = v->a[10];
        f.config_unreadable = nil(v->a[11]) ? NULL : v->a[11];
        f.cache_look = !strcmp(v->a[12], "on") ? LOOK_ON : !strcmp(v->a[12], "limited") ? LOOK_LIMITED : LOOK_OFF;
        f.cache_tip = v->a[13];
        f.quality = nil(v->a[14]) ? -1 : atoi(v->a[14]);
        f.model_sha = nil(v->a[15]) ? NULL : v->a[15];
        char models[512];
        const char *list[8];
        snprintf(models, sizeof models, "%s", v->a[16]);
        for (char *p = models, *tok; p && *p && (tok = strsep(&p, ","));) list[f.n_our_models++] = tok;
        f.our_models = list;
        TB(t, 4096);
        w_prepare_problems(&t, &f);
        COMPARE("SV_PREPARE_PROBLEMS", i, t.p, v->want);
    }
    static const char *const listed[] = {"7.2.19", "7.2.18"};
    for (int i = 0; i < ROWS(SV_COMPAT); i++) {
        const swift_vec *v = &SV_COMPAT[i];
        TB(t, 512);
        if (!strcmp(v->a[0], "std") && v->n == 4) {
            if (!w_standard_account_line(&t, b01(v->a[1]), b01(v->a[2]), b01(v->a[3]))) tb_puts(&t, "nil");
        } else {
            w_short_version(&t, v->a[0]);
            tb_puts(&t, version_listed(v->a[0], listed, 2) ? "|1" : "|0");
        }
        COMPARE("SV_COMPAT", i, t.p, v->want);
    }
}

/* scripted callbacks, as vectors.swift's closures */
typedef struct { const char *script; size_t at; dbuf trace; char used[1024]; } script_ctx;
static char next_answer(script_ctx *s) { return s->script[s->at] ? s->script[s->at++] : '0'; }
static int32_t r_check(void *c) { script_ctx *s = c; dputs(&s->trace, "C"); return next_answer(s) == '1'; }
static void r_release(void *c) { script_ctx *s = c; dputs(&s->trace, "R"); }
static int32_t r_again(void *c) { script_ctx *s = c; dputs(&s->trace, "A"); char a = next_answer(s); return a == '1' || a == 'a' || a == 'x' || a == 'r'; }
static int32_t r_retake(void *c) { script_ctx *s = c; dputs(&s->trace, "T"); return next_answer(s) != '0'; }
static void r_log(void *c, const char *l) { script_ctx *s = c; dputs(&s->trace, "["); dputs(&s->trace, l); dputs(&s->trace, "]"); }

/* settle: answers are pairs (kind, 0|1); a question takes the first unused pair of its kind */
static int ask(script_ctx *s, char kind) {
    size_t n = strlen(s->script);
    for (size_t i = 0; i + 1 < n; i += 2)
        if (s->script[i] == kind && !s->used[i / 2]) { s->used[i / 2] = 1; return s->script[i + 1] == '1'; }
    return 0;
}
static int32_t s_updating(void *c) { script_ctx *s = c; dputs(&s->trace, "u"); return ask(s, 'u'); }
static int32_t s_busy(void *c) { script_ctx *s = c; dputs(&s->trace, "b"); return ask(s, 'b'); }
static int32_t s_open(void *c) { script_ctx *s = c; dputs(&s->trace, "o"); return ask(s, 'o'); }
static void s_pause(void *c, int32_t sec) { script_ctx *s = c; char b[16]; snprintf(b, sizeof b, "p%d", sec); dputs(&s->trace, b); }
static int32_t s_half(void *c, char *id, size_t cap) {
    script_ctx *s = c;
    dputs(&s->trace, "h");
    if (!ask(s, 'h')) return 0;
    snprintf(id, cap, "%s", "uninstall-all");
    return 1;
}
static void s_session(void *c) { script_ctx *s = c; dputs(&s->trace, "s"); }

static void t_watcher(void) {
    for (int i = 0; i < ROWS(SV_WATCHER_DIALOG); i++) {
        const swift_vec *v = &SV_WATCHER_DIALOG[i];
        TB(m, 1024);
        watcher_dialog d;
        watcher_dialog_build(&m, &d, b01(v->a[0]), b01(v->a[1]), "0003", "7.3.0", b01(v->a[2]), b01(v->a[3]), b01(v->a[4]));
        TB(t, 2048);
        TB_TEXT0(&t, d.title);
        tb_puts(&t, "|");
        tb_put(&t, m.p, m.len);
        for (int32_t k = 0; k < d.nbuttons; k++) { tb_puts(&t, "|"); TB_TEXT0(&t, d.buttons[k]); }
        COMPARE("SV_WATCHER_DIALOG", i, t.p, v->want);
    }
    for (int i = 0; i < ROWS(SV_WATCHER_ROUNDS); i++) {
        script_ctx s;
        memset(&s, 0, sizeof s);
        s.script = SV_WATCHER_ROUNDS[i].a[0];
        watcher_round_ops o = {&s, r_check, r_release, r_again, r_retake, r_log};
        int32_t n = watcher_rounds(3, &o);
        dbuf d = {0};
        di(&d, n);
        SEP(&d);
        dputs(&d, s.trace.p ? s.trace.p : "");
        COMPARE("SV_WATCHER_ROUNDS", i, d.p, SV_WATCHER_ROUNDS[i].want);
        free(d.p);
        free(s.trace.p);
    }
    for (int i = 0; i < ROWS(SV_WATCHER_SETTLE); i++) {
        script_ctx s;
        memset(&s, 0, sizeof s);
        s.script = SV_WATCHER_SETTLE[i].a[0];
        watcher_settle_ops o = {&s, s_updating, s_busy, s_open, s_pause, s_half, s_session, r_log};
        int32_t r = watcher_settle(&o);
        dbuf d = {0};
        dputs(&d, r ? "true|" : "false|");
        dputs(&d, s.trace.p ? s.trace.p : "");
        COMPARE("SV_WATCHER_SETTLE", i, d.p, SV_WATCHER_SETTLE[i].want);
        free(d.p);
        free(s.trace.p);
    }
    for (int i = 0; i < ROWS(SV_WATCHER_MISC); i++) {
        const swift_vec *v = &SV_WATCHER_MISC[i];
        TB(t, 512);
        if (!strcmp(v->a[0], "launchd")) tb_puts(&t, watcher_launchd_disabled(v->a[1], strlen(v->a[1]), v->a[2]) ? "1" : "0");
        else if (!strcmp(v->a[0], "off")) tb_puts(&t, watcher_off(b01(v->a[1]), b01(v->a[2]), b01(v->a[3])) ? "1" : "0");
        else if (!strcmp(v->a[0], "tmcleanup"))
            tb_puts(&t, this_mac_needs_cleanup(!strcmp(v->a[1], "avail"), b01(v->a[2]), b01(v->a[3])) ? "1" : "0");
        else if (!strcmp(v->a[0], "tmretry"))
            tb_puts(&t, may_retry_this_mac_deletion(b01(v->a[1]), !strcmp(v->a[2], "avail"), b01(v->a[3]), b01(v->a[4])) ? "1" : "0");
        else watcher_set_run_pattern(&t, v->a[1]);
        COMPARE("SV_WATCHER_MISC", i, t.p, v->want);
    }
}

static void t_state(void) {
    arena a = {0};
    for (int i = 0; i < ROWS(SV_BUSY_MARKER); i++) {
        const swift_vec *v = &SV_BUSY_MARKER[i];
        busy_marker m;
        int r = busy_marker_parse(&a, v->a[0], strlen(v->a[0]), &m);
        dbuf d = {0};
        if (r <= 0) dputs(&d, "nil|");
        else {
            di(&d, m.pid);
            SEP(&d);
            if (m.has_start) di(&d, m.start); else dputs(&d, "nil");
            SEP(&d);
            if (m.has_action) { desc(&d, m.id); SEP(&d); desc(&d, m.label); } else dputs(&d, "nil|nil");
            SEP(&d);
            static const int64_t starts[] = {-1, 1699999999, 1700000000, 1700000001};
            for (int k = 0; k < 4; k++) {
                dputs(&d, busy_marker_live(&m, starts[k], 1, 1700000000) ? "1" : "0");
                dputs(&d, busy_marker_live(&m, starts[k], 0, 0) ? "1" : "0");
            }
        }
        COMPARE("SV_BUSY_MARKER", i, d.p, v->want);
        free(d.p);
        arena_reset(&a);
    }
    for (int i = 0; i < ROWS(SV_BRIDGE_STATE); i++) {
        const swift_vec *v = &SV_BRIDGE_STATE[i];
        bridge_state b;
        int r = bridge_state_parse(&a, v->a[0], strlen(v->a[0]), &b);
        dbuf d = {0};
        if (r <= 0) dputs(&d, "nil|");
        else {
            di(&d, b.pid); SEP(&d); di(&d, b.start); SEP(&d); desc(&d, b.location); SEP(&d); desc(&d, b.state); SEP(&d); desc(&d, b.reason); SEP(&d);
            char loc[512];
            snprintf(loc, sizeof loc, "%.*s", (int)b.location.n, b.location.s);
            dputs(&d, bridge_state_live(&b, "", 2) ? "1" : "0");
            dputs(&d, bridge_state_live(&b, loc, 2) ? "1" : "0");
            dputs(&d, bridge_state_live(&b, loc, 3) ? "1" : "0");
        }
        COMPARE("SV_BRIDGE_STATE", i, d.p, v->want);
        free(d.p);
        arena_reset(&a);
    }
    arena_free(&a);
}

static void t_dialog_texts(void) {
    for (int i = 0; i < ROWS(SV_DIALOG_TEXTS); i++) {
        const swift_vec *v = &SV_DIALOG_TEXTS[i];
        const char *k = v->a[0];
        TB(t, 2048);
        if (!strcmp(k, "install")) {
            int plus = strstr(v->a[1], "Demucs v4") != NULL, cache = strstr(v->a[1], "Stems Cache") != NULL;
            w_install_note(&t, plus, cache, b01(v->a[2]), !nil(v->a[3]), nil(v->a[3]) ? 0 : strtoll(v->a[3], NULL, 10), nil(v->a[4]) ? NULL : v->a[4]);
        } else if (!strcmp(k, "uninstall")) w_uninstall_note(&t, strstr(v->a[1], "Stems Cache") != NULL);
        else if (!strcmp(k, "all")) w_uninstall_all_message(&t, b01(v->a[1]), b01(v->a[2]), v->a[3], nil(v->a[4]) ? NULL : v->a[4], b01(v->a[5]));
        else if (!strcmp(k, "fate")) {
            const char *left[2] = {"x.", "y."};
            w_own_files_removed(&t, "About.", atoi(v->a[1]), b01(v->a[2]), b01(v->a[3]), left, *v->a[4] ? 2 : 0);
        } else if (!strcmp(k, "removal")) w_location_removal_note(&t, nil(v->a[1]) ? NULL : v->a[1], b01(v->a[2]), v->a[3]);
        else if (!strcmp(k, "switch")) w_switch_message(&t, "GLS", !nil(v->a[1]), nil(v->a[1]) ? 0 : strtoll(v->a[1], NULL, 10), strtoll(v->a[2], NULL, 10), nil(v->a[3]) ? NULL : v->a[3]);
        else if (!strcmp(k, "thismac")) { w_use_this_mac_message(&t, nil(v->a[1]) ? NULL : v->a[1]); tb_puts(&t, "|"); w_clear_location_message(&t, "GLS"); }
        else if (!strcmp(k, "refusal")) w_location_refusal(&t, atoi(v->a[1]), "My \"Drive\"");
        else if (!strcmp(k, "update")) w_update_failed_message(&t, update_failure_reason(v->a[1], strlen(v->a[1])));
        else if (!strcmp(k, "root")) {
            core_text_id title, body;
            int32_t code = atoi(v->a[1]);
            if (root_problem(code, &title, &body)) { TB_TEXT0(&t, title); tb_puts(&t, "|"); TB_TEXT0(&t, body); }
            else tb_puts(&t, "nil");
            tb_puts(&t, "|");
            w_cache_failure_reason(&t, 1, code, NULL);
        } else if (!strcmp(k, "removal-ok")) { w_cache_failure_reason(&t, 0, 0, NULL); tb_puts(&t, "|"); w_cache_failure_reason(&t, 0, 0, NULL); }
        else {
            core_text_id status, title;
            root_run_texts(v->a[1], &status, &title);
            TB_TEXT0(&t, status);
            tb_puts(&t, "|");
            TB_TEXT0(&t, title);
        }
        COMPARE("SV_DIALOG_TEXTS", i, t.p, v->want);
    }
}

void swift_vector_tests(int *passed, int *failed);
void swift_vector_tests(int *passed, int *failed) {
    vp = passed;
    vf = failed;
    t_dialog_texts();
    t_mask_unicode();
    t_mask_more();
    t_mask();
    t_lines();
    t_estimates();
    t_words();
    t_lights();
    t_location();
    t_problems();
    t_watcher();
    t_state();
}
