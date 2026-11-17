/* The Prepare and Status tabs' words (Prepare.swift, PrepareView.swift, Location.swift,
 * LocationActions.swift, State.swift), and numbers written as Swift writes them. */
#include "internal.h"
#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ numbers */

/* The digits of x (no point, no sign) and the decimal exponent of the first one, shortest that
 * reads back, as Swift's description picks them. *nd digits into d (at most 17). */
static void shortest(double x, char d[24], int *nd, int *exp10) {
    char buf[64];
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, x);
        /* digits, then the exponent after 'e'; the locale's decimal point is never read */
        int n = 0, e = 0, neg = 0;
        const char *q = buf;
        for (; *q && *q != 'e'; q++)
            if (*q >= '0' && *q <= '9' && n < 23) d[n++] = *q;
        if (*q == 'e') {
            q++;
            if (*q == '-') neg = 1;
            if (*q == '-' || *q == '+') q++;
            for (; *q >= '0' && *q <= '9'; q++) e = e * 10 + (*q - '0');
        }
        if (neg) e = -e;
        char back[64];
        memcpy(back, d, (size_t)n);
        snprintf(back + n, sizeof back - (size_t)n, "e%d", e - (n - 1));
        if (strtod(back, NULL) == x || p == 17) {
            while (n > 1 && d[n - 1] == '0') n--;
            *nd = n;
            *exp10 = e;
            return;
        }
    }
}

void fmt_swift_double(double x, char out[32]) {
    if (isnan(x)) { strcpy(out, "nan"); return; }
    if (isinf(x)) { strcpy(out, x < 0 ? "-inf" : "inf"); return; }
    char *w = out;
    if (signbit(x)) { *w++ = '-'; x = -x; }
    if (x == 0) { strcpy(w, "0.0"); return; }
    char d[24];
    int nd, e;
    shortest(x, d, &nd, &e);
    if (e >= -4 && x <= 9007199254740992.0) {   /* Swift: exponential beyond 2^53 and under 1e-4 */
        if (e >= 0) {
            for (int i = 0; i <= e; i++) *w++ = i < nd ? d[i] : '0';
            *w++ = '.';
            if (nd > e + 1) for (int i = e + 1; i < nd; i++) *w++ = d[i];
            else *w++ = '0';
        } else {
            *w++ = '0';
            *w++ = '.';
            for (int i = 0; i < -e - 1; i++) *w++ = '0';
            for (int i = 0; i < nd; i++) *w++ = d[i];
        }
        *w = 0;
        return;
    }
    *w++ = d[0];
    if (nd > 1) {
        *w++ = '.';
        for (int i = 1; i < nd; i++) *w++ = d[i];
    }
    snprintf(w, 12, "e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
}

void fmt_config_number(double x, char out[32]) {
    /* Swift's String(Int(x)) traps beyond Int's range: here those take String(x) */
    if (x == round(x) && fabs(x) < 9.2e18) { snprintf(out, 32, "%lld", (long long)x); return; }
    fmt_swift_double(x, out);
}

void fmt_fixed(double x, int decimals, char out[64]) {
    snprintf(out, 64, "%.*f", decimals, x);
    for (char *p = out; *p; p++)
        if (*p == ',') *p = '.';   /* a locale's decimal comma: String(format:) writes a point */
}

/* Int(x) for a finite double, saturating where Swift would trap */
static int64_t int_of(double x) {
    if (x != x) return 0;
    if (x >= 9223372036854775807.0) return INT64_MAX;
    if (x <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)x;
}

static void tb_n(tbuf *b, core_text_id id, int64_t n) {
    char s[24];
    snprintf(s, sizeof s, "%lld", (long long)n);
    core_slot sl = SLOT_S(n, s);
    tb_text(b, id, &sl, 1);
}

static int pstr_is(const pstr *s, const char *lit) { return s->n == strlen(lit) && !memcmp(s->s, lit, s->n); }

static int pstr_has(const pstr *s, const char *lit) {
    size_t k = strlen(lit);
    if (k > s->n) return 0;
    for (size_t i = 0; i + k <= s->n; i++)
        if (!memcmp(s->s + i, lit, k)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ sizes and durations */

void w_duration(tbuf *b, double s) {
    if (s < 60) { TB_TEXT0(b, T_DUR_UNDER_MINUTE); return; }
    int64_t m = int_of(round(s / 60));
    if (m < 60) { tb_n(b, T_DUR_MIN, m); return; }
    /* String(format: "%d h %02d min", m / 60, m % 60): %d reads an Int's low 32 bits */
    char h[24], mm[24];
    snprintf(h, sizeof h, "%d", (int)(int32_t)(uint32_t)(uint64_t)(m / 60));
    snprintf(mm, sizeof mm, "%02d", (int)(int32_t)(uint32_t)(uint64_t)(m % 60));
    core_slot sl[2] = {SLOT_S(n, h), SLOT_S(m, mm)};
    tb_text(b, T_DUR_H_MIN, sl, 2);
}

void w_size(tbuf *b, double gb) {
    if (gb < 1) {
        int64_t mb = int_of(round(gb * 1024));
        tb_n(b, T_SIZE_MB, mb > 1 ? mb : 1);
        return;
    }
    char g[64];
    fmt_fixed(gb, 1, g);
    core_slot sl = SLOT_S(gb, g);
    tb_text(b, T_SIZE_GB, &sl, 1);
}

void w_about_size(tbuf *b, int64_t bytes) {
    if (bytes <= 0) return;
    double gb = (double)bytes / 1073741824.0;
    if (gb >= 0.1) {
        char g[64];
        fmt_fixed(gb, 1, g);
        core_slot sl = SLOT_S(gb, g);
        tb_text(b, T_ABOUT_GB, &sl, 1);
        return;
    }
    int64_t mb = bytes / 1048576;
    tb_n(b, T_ABOUT_MB, mb > 1 ? mb : 1);
}

void w_stems_size(tbuf *b, int32_t kind, int64_t bytes) {
    switch (kind) {
    case STEMS_MEASURING: TB_TEXT0(b, T_SIZE_MEASURING); break;
    case STEMS_MEASURED: w_about_size(b, bytes); break;
    case STEMS_FAILED: TB_TEXT0(b, T_SIZE_FAILED); break;
    default: TB_TEXT0(b, T_SIZE_NOT_CONNECTED); break;
    }
}

void w_size_words(tbuf *b, int64_t bytes) {
    double gb = (double)bytes / 1073741824.0;
    if (gb >= 0.1) {
        char g[64];
        fmt_fixed(gb, 1, g);
        core_slot sl = SLOT_S(gb, g);
        tb_text(b, T_SIZE_GB, &sl, 1);
        return;
    }
    int64_t mb = bytes / 1048576;
    tb_n(b, T_SIZE_MB, mb > 1 ? mb : 1);
}

/* a text with one slot that is itself built (a size or a duration): built into a scratch buffer */
#define SUB(name, build) \
    char name##_buf[256]; tbuf name##_tb; tb_init(&name##_tb, b->os, name##_buf, sizeof name##_buf); build; \
    if (name##_tb.err) b->err = name##_tb.err

/* ------------------------------------------------------------------ an estimate */

void w_estimate_summary(tbuf *b, const prep_estimate *e) {
    int parts = 0;
    if (e->ready > 0) {
        tb_n(b, e->ready == 1 ? T_EST_READY_ONE : T_EST_READY_MANY, e->ready);
        parts++;
    }
    if (e->prepare > 0) {
        double music = 0;
        for (int32_t i = 0; i < e->ntracks; i++)
            if (pstr_is(&e->tracks[i].result, "prepare")) music += e->tracks[i].seconds;
        if (parts++) tb_puts(b, ", ");
        SUB(t, w_duration(&t_tb, music));
        char n[24];
        snprintf(n, sizeof n, "%lld", (long long)e->prepare);
        core_slot sl[2] = {SLOT_S(n, n), SLOT_N(time, t_buf, tb_len(&t_tb))};
        tb_text(b, T_EST_TO_PREPARE, sl, 2);
    }
    if (e->skipped > 0) {
        if (parts++) tb_puts(b, ", ");
        tb_n(b, T_EST_SKIPPED, e->skipped);
    }
    if (parts) tb_puts(b, ".");
    else TB_TEXT0(b, T_EST_NO_TRACKS);
    if (e->prepare > 0) {
        SUB(s, w_size(&s_tb, e->need_gb));
        SUB(t, w_duration(&t_tb, e->time_s));
        core_slot sl[2] = {SLOT_N(size, s_buf, tb_len(&s_tb)), SLOT_N(time, t_buf, tb_len(&t_tb))};
        tb_text(b, T_EST_ABOUT, sl, 2);
    }
    SUB(s, w_size(&s_tb, e->kept_gb + e->need_gb));
    SUB(sh, w_size(&sh_tb, e->share_gb));
    char lim[32];
    fmt_config_number(e->max_gb, lim);
    core_slot sl[3] = {SLOT_N(size, s_buf, tb_len(&s_tb)), SLOT_N(share, sh_buf, tb_len(&sh_tb)), SLOT_S(limit, lim)};
    tb_text(b, T_EST_SHARE, sl, 3);
}

void w_doesnt_fit(tbuf *msg, const prep_estimate *e, double free_gb, fit_choices *out) {
    tbuf *b = msg;
    double need = ceil(e->max_gb_needed * 10) / 10;
    memset(out, 0, sizeof *out);
    if (e->fit_files > 0) { out->first_n = e->fit_files; out->nbuttons++; }
    if (need > e->max_gb && (need - e->max_gb) <= free_gb) { out->has_raise = 1; out->raise_to = need; out->nbuttons++; }
    out->nbuttons++;   /* Cancel */
    char lim[32];
    fmt_config_number(e->max_gb, lim);
    SUB(sh, w_size(&sh_tb, e->share_gb));
    SUB(s, w_size(&s_tb, e->kept_gb + e->need_gb));
    core_slot sl[3] = {SLOT_S(limit, lim), SLOT_N(share, sh_buf, tb_len(&sh_tb)), SLOT_N(size, s_buf, tb_len(&s_tb))};
    tb_text(b, T_FIT_MSG, sl, 3);
    if (!out->has_raise) TB_TEXT0(b, T_FIT_NO_ROOM);
}

void w_fit_button(tbuf *b, const fit_choices *c, int32_t i) {
    if (c->first_n > 0 && i-- == 0) { tb_n(b, c->first_n == 1 ? T_FIT_FIRST_ONE : T_FIT_FIRST_MANY, c->first_n); return; }
    if (c->has_raise && i-- == 0) {
        char r[32];
        fmt_config_number(c->raise_to, r);
        core_slot sl = SLOT_S(limit, r);
        tb_text(b, T_FIT_RAISE, &sl, 1);
        return;
    }
    if (i == 0) TB_TEXT0(b, T_CANCEL);
}

/* ------------------------------------------------------------------ a track's row */

void w_result_text(tbuf *b, const prep_track *t, int32_t checking) {
    const pstr *r = &t->result;
    if (!r->n) { TB_TEXT0(b, checking ? T_CHECKING : T_NOT_CHECKED); return; }
    if (pstr_is(r, "ready")) TB_TEXT0(b, T_READY);
    else if (pstr_is(r, "prepare")) TB_TEXT0(b, T_TO_PREPARE);
    else if (pstr_is(r, "skipped")) TB_TEXT0(b, T_RES_SKIPPED);
    else if (pstr_is(r, "prepared")) TB_TEXT0(b, T_RES_PREPARED);
    else if (pstr_is(r, "stopped")) TB_TEXT0(b, T_RES_STOPPED);
    else if (pstr_is(r, "unchecked")) TB_TEXT0(b, T_NOT_CHECKED);
    else tb_put(b, r->s, r->n);
}

void w_prepared_note(tbuf *b, const prep_track *t) {
    if (t->written == 0 && t->already == 0) { TB_TEXT0(b, t->refused > 0 ? T_NOTE_REFUSED : T_NOTE_SILENT); return; }
    TB_TEXT0(b, t->written == 0 ? T_NOTE_ALREADY : t->already > 0 ? T_NOTE_PARTLY : T_NOTE_NOW);
    if (t->refused > 0) TB_TEXT0(b, T_NOTE_SOME_PARTS);
}

void w_track_note(tbuf *b, const prep_track *t) {
    if (t->why.n) { tb_put(b, t->why.s, t->why.n); return; }
    if (pstr_is(&t->result, "prepare")) {
        if (t->missing > 0) tb_n(b, T_NOTE_REMOVED, t->missing);
        else w_duration(b, t->seconds);
    } else if (pstr_is(&t->result, "prepared")) w_prepared_note(b, t);
}

/* ------------------------------------------------------------------ sets and runs */

void w_set_state(tbuf *b, const prep_estimate *e) {
    if (!e) { TB_TEXT0(b, T_SET_UNCHECKED_ERR); return; }
    if (e->has_refused) { tb_put(b, e->refused.s, e->refused.n); return; }
    if (e->ready + e->prepare + e->skipped + e->unchecked == 0) { TB_TEXT0(b, T_SET_NO_TRACKS); return; }
    if (e->prepare == 0 && e->skipped == 0) {
        if (e->unchecked > 0) tb_n(b, T_SET_READY_UNCHECKED, e->unchecked);
        else TB_TEXT0(b, T_READY);
        return;
    }
    if (e->prepare > 0) tb_n(b, e->prepare == 1 ? T_SET_TO_PREPARE_ONE : T_SET_TO_PREPARE_MANY, e->prepare);
    if (e->skipped > 0) {
        if (e->prepare > 0) tb_puts(b, ", ");
        tb_n(b, T_SET_UNREADABLE, e->skipped);
    }
}

void w_last_run_line(tbuf *b, int64_t files, int64_t skipped, const pstr *stopped, int32_t has_error) {
    tb_n(b, files == 1 ? T_LAST_ONE : T_LAST_MANY, files);
    if (skipped > 0) tb_n(b, T_PART_SKIPPED, skipped);
    if (stopped)
        TB_TEXT0(b, pstr_is(stopped, "stopped") ? T_LAST_STOPPED : pstr_has(stopped, "75%") ? T_LAST_STOPPED_SHARE : T_LAST_STOPPED_EARLY);
    if (has_error) TB_TEXT0(b, T_LAST_NOT_SAVED);
    tb_puts(b, ".");
}

void w_run_end_title(tbuf *b, pstr name, int32_t status, int32_t got_done, const pstr *stopped, int64_t finished,
                     int64_t skipped, int32_t has_error, int32_t partial) {
    core_slot sl = SLOT_N(playlist, name.s, name.n);
    core_text_id id;
    if (status != 0 || !got_done) id = T_RUN_FAILED;
    else if (!stopped && skipped == 0 && !has_error && !partial) id = T_RUN_PREPARED;
    else id = finished == 0 ? T_RUN_FAILED : T_RUN_PARTIAL;
    tb_text(b, id, &sl, 1);
}

void w_run_summary(tbuf *b, pstr name, int32_t paused, double fraction) {
    char n[24];
    snprintf(n, sizeof n, "%lld", (long long)int_of(fraction * 100));
    core_slot sl[2] = {SLOT_N(playlist, name.s, name.n), SLOT_S(n, n)};
    tb_text(b, paused ? T_RUN_PAUSED : T_RUN_PREPARING, sl, 2);
}

void w_run_done_line(tbuf *b, int64_t done, int64_t skipped, double gb, const pstr *stopped, const pstr *error) {
    tb_n(b, done == 1 ? T_RUN_DONE_ONE : T_RUN_DONE_MANY, done);
    if (skipped > 0) tb_n(b, T_PART_SKIPPED, skipped);
    SUB(s, w_size(&s_tb, gb));
    core_slot sl = SLOT_N(size, s_buf, tb_len(&s_tb));
    tb_text(b, T_RUN_USE, &sl, 1);
    if (stopped) {
        if (pstr_is(stopped, "stopped")) TB_TEXT0(b, T_RUN_STOPPED_USER);
        else {
            core_slot w = SLOT_N(why, stopped->s, stopped->n);
            tb_text(b, T_RUN_STOPPED_WHY, &w, 1);
        }
    }
    if (error) {
        tb_puts(b, " ");
        tb_put(b, error->s, error->n);
    }
}

int w_kept_notice(tbuf *b, const prep_estimate *e) {
    int64_t n = e->prepare + e->skipped;
    if (n <= 0) return 0;
    tb_n(b, n == 1 ? T_W_KEPT_ONE : T_W_KEPT_MANY, n);
    return 1;
}

/* noPaths (Prepare.swift): a log line never holds a path; each becomes <path> */
void w_no_paths(tbuf *b, const char *s) {
    static const char *const heads[] = {"~", "/Volumes", "/Users", "/private", "/tmp", "/var", "/Applications", "/Library"};
    while (*s) {
        size_t k = 0;
        for (size_t i = 0; i < sizeof heads / sizeof *heads && !k; i++)
            if (!strncmp(s, heads[i], strlen(heads[i]))) k = strlen(heads[i]);
        if (!k) {
            tb_put(b, s, 1);
            s++;
            continue;
        }
        s += k;
        if (*s == '/') {
            s++;
            while (*s && !strchr("\n:;,)", *s)) s++;
        }
        tb_puts(b, "<path>");
    }
}

/* notReadyTracks (Watcher.swift): the files of the tracks to prepare or skipped, in order (an
 * unchecked one wasn't looked at); how many, at most cap into out */
int32_t w_kept_not_ready(const prep_estimate *e, pstr *out, int32_t cap) {
    int32_t k = 0;
    for (int32_t i = 0; i < e->ntracks && k < cap; i++) {
        const pstr *r = &e->tracks[i].result;
        if ((r->n == 7 && !memcmp(r->s, "prepare", 7)) || (r->n == 7 && !memcmp(r->s, "skipped", 7))) out[k++] = e->tracks[i].file;
    }
    return k;
}

/* newlyNotReady (Watcher.swift): 1 when a file of now isn't among noticed (Set(now).isSubset(of:)) */
static int kept_cmp(const void *a, const void *b) {
    const pstr *x = a, *y = b;
    if (x->n != y->n) return x->n < y->n ? -1 : 1;
    return x->n ? memcmp(x->s, y->s, x->n) : 0;
}
void w_kept_sort(pstr *v, int32_t m) { if (m > 1) qsort(v, (size_t)m, sizeof *v, kept_cmp); }
int w_kept_newly(const pstr *now, int32_t n, const pstr *noticed, int32_t m) {
    for (int32_t i = 0; i < n; i++)
        if (m <= 0 || !bsearch(&now[i], noticed, (size_t)m, sizeof *noticed, kept_cmp)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ the free-space floor */

int64_t cache_floor_gb(int64_t floor) { return (floor + 500000000) / 1000000000; }

static void paused_text(tbuf *b, core_text_id drive_id, core_text_id disk_id, int64_t floor, const char *drive) {
    char g[24];
    snprintf(g, sizeof g, "%lld", (long long)cache_floor_gb(floor));
    core_slot sl[2] = {SLOT_S(gb, g), SLOT_S(drive, drive ? drive : "")};
#ifdef CORE_WIN_POLICY
    if (TB_NAMELESS(b, drive)) { tb_text(b, nameless_text(drive_id), sl, 1); return; }
#endif
    if (drive) tb_text(b, drive_id, sl, 2);
    else tb_text(b, disk_id, sl, 1);
}

void w_cache_paused_tip(tbuf *b, int64_t floor, const char *drive) { paused_text(b, T_CACHE_PAUSED, T_CACHE_PAUSED_DISK, floor, drive); }

void w_cache_paused_at_install(tbuf *b, int64_t floor, const char *drive) {
    paused_text(b, T_CACHE_PAUSED_AT_INSTALL, T_CACHE_PAUSED_AT_INSTALL_DISK, floor, drive);
}
