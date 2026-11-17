/* The stateful model and delta fuzz (C29): a byte string drives a sequence of model operations
 * (field and cell writes, self-aliasing writes, resets, appends, renders, snapshots held and
 * released out of order). A mirror keeps what the model should hold, and a "shell" keeps only what
 * it has applied from each render's delta. After every render the shell must equal the mirror, and
 * every held snapshot must still read what it read when it was taken.
 *
 *   int dfuzz_run(const uint8_t *d, size_t n)   0, or the source line of the first mismatch
 */
#include "../internal.h"
#include "dfuzz.h"
#include <stdio.h>

enum { MAXR = 64, HELD = 4 };
#define T_A T_PREP_TRACKS
#define T_B T_PREP_OUTLINE

typedef struct {
    char line[2100];           /* F_STATUS_LINE */
    char notice[2100];         /* F_NOTICE */
    int spinner;               /* F_SPINNER */
    int na, nb;
    char a[MAXR][24];          /* T_A, C_TRK_NAME */
    int64_t b[MAXR];           /* T_B, C_OUT_DEPTH */
} view;

typedef struct { core_snap *s; view v; } held_snap;

static void read_view(const core_snap *s, view *v) {
    memset(v, 0, sizeof *v);
    snprintf(v->line, sizeof v->line, "%s", core_str(s, F_STATUS_LINE, NULL));
    snprintf(v->notice, sizeof v->notice, "%s", core_str(s, F_NOTICE, NULL));
    v->spinner = core_flag(s, F_SPINNER);
    v->na = core_rows(s, T_A);
    v->nb = core_rows(s, T_B);
    for (int r = 0; r < v->na && r < MAXR; r++) snprintf(v->a[r], sizeof v->a[r], "%s", core_cell_str(s, C_TRK_NAME, r, NULL));
    for (int r = 0; r < v->nb && r < MAXR; r++) v->b[r] = core_cell_i64(s, C_OUT_DEPTH, r);
}

static int same(const view *x, const view *y) {
    if (strcmp(x->line, y->line) || strcmp(x->notice, y->notice) || x->spinner != y->spinner || x->na != y->na || x->nb != y->nb) return 0;
    for (int r = 0; r < x->na; r++) if (strcmp(x->a[r], y->a[r])) return 0;
    for (int r = 0; r < x->nb; r++) if (x->b[r] != y->b[r]) return 0;
    return 1;
}

/* the shell: applies a render's delta to what it has, row by row */
static int apply(view *sh, const core_snap *s) {
    const core_delta *d = &s->delta;
    view now;
    read_view(s, &now);
    if (core_delta_sections(d) & (1u << S_APP)) {   /* fields: read only when their section moved */
        snprintf(sh->line, sizeof sh->line, "%s", now.line);
        snprintf(sh->notice, sizeof sh->notice, "%s", now.notice);
        sh->spinner = now.spinner;
    }
    const core_table ts[2] = {T_A, T_B};
    for (int k = 0; k < 2; k++) {
        core_table t = ts[k];
        int rows = core_rows(s, t), old = core_delta_old_rows(d, t), n;
        const int32_t *ch = core_delta_changed(d, t, &n);
        if (core_delta_reload(d, t)) old = 0;
        else if (old > rows) return 0;
        for (int i = 0; i < n; i++) {
            if (ch[i] < 0 || ch[i] >= old || (i && ch[i] <= ch[i - 1])) return 0;
            if (t == T_A) snprintf(sh->a[ch[i]], sizeof sh->a[0], "%s", now.a[ch[i]]);
            else sh->b[ch[i]] = now.b[ch[i]];
        }
        for (int r = old; r < rows; r++) {   /* appended, or everything on reload */
            if (t == T_A) snprintf(sh->a[r], sizeof sh->a[0], "%s", now.a[r]);
            else sh->b[r] = now.b[r];
        }
        if (t == T_A) sh->na = rows; else sh->nb = rows;
    }
    return 1;
}

#define FAIL() do { line = __LINE__; goto done; } while (0)

int dfuzz_run(const uint8_t *d, size_t n) {
    model m;
    if (model_init(&m)) return __LINE__;
    view mir, sh;
    memset(&mir, 0, sizeof mir);
    memset(&sh, 0, sizeof sh);
    held_snap held[HELD];
    memset(held, 0, sizeof held);
    static const char *const words[] = {"", "a", "bb", "Track", "Ready", "To prepare", "é✓"};
    char big[2048];
    int line = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t op = d[i] % 12, arg = d[i + 1];
        const char *w = words[arg % 7];
        switch (op) {
        case 0:
            if (arg & 0x80) {   /* a long value: fills the section's arena, so it compacts */
                memset(big, 'a' + arg % 26, sizeof big - 1);
                big[sizeof big - 1 - arg] = 0;
                w = big;
            }
            if (model_set_str(&m, F_STATUS_LINE, w, CORE_NTS)) FAIL();
            snprintf(mir.line, sizeof mir.line, "%s", w);
            break;
        case 1: {   /* self-aliasing: one field set from the live section's own string (C13) */
            if (arg & 0x80)   /* with no snapshot held, so the model owns the section alone (R6) */
                for (int k = 0; k < HELD; k++) {
                    core_snap_release(held[k].s);
                    held[k].s = NULL;
                }
            const field_meta *fm = &core_field_meta[F_STATUS_LINE];
            const cell *c = &m.sec[fm->section]->cells[fm->index];
            if (model_set_str(&m, F_NOTICE, c->v.s, c->len)) FAIL();
            snprintf(mir.notice, sizeof mir.notice, "%s", mir.line);
            break;
        }
        case 2:
            if (model_set_i64(&m, F_SPINNER, arg & 1)) FAIL();
            mir.spinner = arg & 1;
            break;
        case 3: {
            int rows = arg % 40;
            core_table t = arg & 0x80 ? T_B : T_A;
            if (model_table_reset(&m, t, rows)) FAIL();
            if (t == T_A) { mir.na = rows; memset(mir.a, 0, sizeof mir.a); }
            else { mir.nb = rows; memset(mir.b, 0, sizeof mir.b); }
            break;
        }
        case 4: {
            core_table t = arg & 0x80 ? T_B : T_A;
            int *nr = t == T_A ? &mir.na : &mir.nb, k = arg % 5;
            if (*nr + k > MAXR) break;
            if (model_table_append(&m, t, k)) FAIL();
            for (int r = *nr; r < *nr + k; r++) { if (t == T_A) mir.a[r][0] = 0; else mir.b[r] = 0; }
            *nr += k;
            break;
        }
        case 5:
        case 6:
            if (!mir.na) break;
            if (model_cell_str(&m, C_TRK_NAME, arg % mir.na, w, CORE_NTS)) FAIL();
            snprintf(mir.a[arg % mir.na], sizeof mir.a[0], "%s", w);
            break;
        case 7:
            if (!mir.nb) break;
            if (model_cell_i64(&m, C_OUT_DEPTH, arg % mir.nb, arg)) FAIL();
            mir.b[arg % mir.nb] = arg;
            break;
        case 8:
        case 9: {   /* a render: the shell applies its delta and must equal the mirror */
            core_snap *s = model_freeze(&m, 1);
            if (!s || !apply(&sh, s)) { core_snap_release(s); FAIL(); }
            if (!same(&sh, &mir)) { core_snap_release(s); FAIL(); }
            int slot = arg % HELD;
            core_snap_release(held[slot].s);   /* released out of order */
            held[slot].s = s;
            held[slot].v = mir;
            break;
        }
        case 10: {   /* a snapshot outside the render path (the probe) */
            core_snap *s = model_freeze(&m, 0);
            view v;
            if (!s) FAIL();
            read_view(s, &v);
            core_snap_release(s);
            if (!same(&v, &mir)) FAIL();
            break;
        }
        default:   /* every held snapshot still reads what it read */
            for (int k = 0; k < HELD; k++) {
                if (!held[k].s) continue;
                view v;
                read_view(held[k].s, &v);
                if (!same(&v, &held[k].v)) FAIL();
            }
            break;
        }
    }
done:
    if (line) fprintf(stderr, "dfuzz: mismatch at dfuzz.c:%d\n", line);
    model_free(&m);   /* the model before the snapshots it shares objects with */
    for (int k = 0; k < HELD; k++) core_snap_release(held[k].s);
    return line;
}
