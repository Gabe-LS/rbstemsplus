/* The live model and its snapshots (mac-core §3.4, §3.10).
 *
 * Sections, tables and rows are each their own counted allocation, shared between the model and
 * every snapshot that holds them. The model writes an object in place only while it is the only
 * holder (refs == 1); otherwise it copies it first (copy on write). So a new estimate line copies
 * one row, plus the table's row-pointer array once per commit, never the other rows. Counts are
 * atomic: snapshots may be released on any thread. Only main writes the model. */
#include "internal.h"

static const char empty_str[1] = "";

/* ---------------------------------------------------------------- rows */

static void row_release(row_obj *r) {
    if (r && atomic_fetch_sub_explicit(&r->refs, 1, memory_order_acq_rel) == 1) {
        LIVE_DEC();
        free(r);
    }
}

static row_obj *row_retain(row_obj *r) {
    atomic_fetch_add_explicit(&r->refs, 1, memory_order_relaxed);
    return r;
}

/* An empty row of table t (every string "", every number 0). */
static row_obj *row_empty(uint8_t t, uint64_t rev) {
    uint16_t n = core_table_meta[t].ncols;
    row_obj *r = calloc(1, sizeof *r + n * sizeof(cell));
    if (!r) return NULL;
    LIVE_INC();
    atomic_init(&r->refs, 1);
    r->rev = rev;
    r->ncells = n;
    for (int c = 0; c < CORE_COLUMN_COUNT; c++) {
        if (core_column_meta[c].table != t) continue;
        cell *x = &r->cells[core_column_meta[c].index];
        x->kind = core_column_meta[c].kind;
        if (x->kind == K_STR) x->v.s = empty_str;
    }
    return r;
}

/* A copy of src with cell ci replaced by *nv, its strings packed after the cells: one malloc. */
static row_obj *row_with(const row_obj *src, uint16_t ci, const cell *nv, uint64_t rev) {
    size_t bytes = 0;
    for (uint16_t i = 0; i < src->ncells; i++) {
        const cell *x = i == ci ? nv : &src->cells[i];
        if (x->kind == K_STR && x->len) bytes += (size_t)x->len + 1;
    }
    size_t head = sizeof(row_obj) + src->ncells * sizeof(cell);
    row_obj *r = malloc(head + bytes);
    if (!r) return NULL;
    LIVE_INC();
    atomic_init(&r->refs, 1);
    r->rev = rev;
    r->ncells = src->ncells;
    char *w = (char *)r + head;
    for (uint16_t i = 0; i < src->ncells; i++) {
        const cell *x = i == ci ? nv : &src->cells[i];
        r->cells[i] = *x;
        if (x->kind == K_STR) {
            if (x->len) {
                memcpy(w, x->v.s, x->len);
                w[x->len] = 0;
                r->cells[i].v.s = w;
                w += x->len + 1;
            } else r->cells[i].v.s = empty_str;
        }
    }
    return r;
}

/* ---------------------------------------------------------------- tables */

static table_obj *tab_retain(table_obj *t) {
    atomic_fetch_add_explicit(&t->refs, 1, memory_order_relaxed);
    return t;
}

static void tab_release(table_obj *t) {
    if (!t || atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) != 1) return;
    for (int32_t i = 0; i < t->n; i++) row_release(t->rows[i]);
    free(t->rows);
    free(t);
    LIVE_DEC();
}

static table_obj *tab_new(uint8_t table, uint64_t list_gen, int32_t cap) {
    table_obj *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    LIVE_INC();
    atomic_init(&t->refs, 1);
    t->table = table;
    t->list_gen = list_gen;
    t->cap = cap > 0 ? cap : 0;
    if (t->cap && !(t->rows = malloc((size_t)t->cap * sizeof *t->rows))) {
        free(t);
        LIVE_DEC();
        return NULL;
    }
    return t;
}

static int tab_grow(table_obj *t, int32_t need) {
    if (need <= t->cap) return 0;
    int64_t cap = t->cap ? t->cap : 16;
    while (cap < need) cap *= 2;
    if (cap > INT32_MAX) return -1;
    row_obj **r = realloc(t->rows, (size_t)cap * sizeof *r);
    if (!r) return -1;
    t->rows = r;
    t->cap = (int32_t)cap;
    return 0;
}

/* the model's table t, writable: copied first when a snapshot holds it */
static table_obj *tab_w(model *m, core_table t) {
    table_obj *o = m->tab[t];
    if (atomic_load_explicit(&o->refs, memory_order_acquire) == 1) return o;
    table_obj *c = tab_new(o->table, o->list_gen, o->n);
    if (!c) return NULL;
    for (int32_t i = 0; i < o->n; i++) c->rows[i] = row_retain(o->rows[i]);
    c->n = o->n;
    tab_release(o);
    return m->tab[t] = c;
}

/* ---------------------------------------------------------------- sections */

static section_obj *sec_retain(section_obj *s) {
    atomic_fetch_add_explicit(&s->refs, 1, memory_order_relaxed);
    return s;
}

static void sec_release(section_obj *s) {
    if (!s || atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) != 1) return;
    arena_free(&s->strs);
    free(s);
    LIVE_DEC();
}

static section_obj *sec_new(uint8_t section) {
    uint16_t n = core_section_meta[section].nfields;
    section_obj *s = calloc(1, sizeof *s + n * sizeof(cell));
    if (!s) return NULL;
    LIVE_INC();
    atomic_init(&s->refs, 1);
    s->section = section;
    for (int f = 0; f < CORE_FIELD_COUNT; f++) {
        if (core_field_meta[f].section != section) continue;
        cell *x = &s->cells[core_field_meta[f].index];
        x->kind = core_field_meta[f].kind;
        if (x->kind == K_STR) x->v.s = empty_str;
    }
    return s;
}

/* a copy of s with its strings compacted into a fresh arena */
static section_obj *sec_clone(const section_obj *s) {
    uint16_t n = core_section_meta[s->section].nfields;
    section_obj *c = calloc(1, sizeof *c + n * sizeof(cell));
    if (!c) return NULL;
    LIVE_INC();
    atomic_init(&c->refs, 1);
    c->section = s->section;
    for (uint16_t i = 0; i < n; i++) {
        c->cells[i] = s->cells[i];
        if (s->cells[i].kind == K_STR && s->cells[i].len) {
            char *p = arena_strdup(&c->strs, s->cells[i].v.s, s->cells[i].len);
            if (!p) {
                arena_free(&c->strs);
                free(c);
                LIVE_DEC();
                return NULL;
            }
            c->cells[i].v.s = p;
        }
    }
    return c;
}

/* the model's section, writable: copied first when a snapshot holds it or it holds too much garbage.
 * The old section comes back in *old (or NULL), for the caller to release after the write: the
 * value written may point into it (C13). */
static section_obj *sec_w(model *m, uint8_t section, section_obj **old) {
    section_obj *o = m->sec[section];
    *old = NULL;
    if (atomic_load_explicit(&o->refs, memory_order_acquire) == 1 && o->garbage < 64 * 1024) return o;
    section_obj *c = sec_clone(o);
    if (!c) return NULL;
    *old = o;
    return m->sec[section] = c;
}

/* ---------------------------------------------------------------- the model */

int model_init(model *m) {
    memset(m, 0, sizeof *m);
    for (int s = 0; s < CORE_SECTION_COUNT; s++)
        if (!(m->sec[s] = sec_new((uint8_t)s))) goto fail;
    for (int t = 0; t < CORE_TABLE_COUNT; t++)
        if (!(m->tab[t] = tab_new((uint8_t)t, ++m->counter, 0))) goto fail;
    for (int s = 0; s < CORE_SECTION_COUNT; s++) m->rev[s] = ++m->counter;
    return CORE_OK;
fail:
    model_free(m);
    return CORE_E_NOMEM;
}

void model_free(model *m) {
    for (int s = 0; s < CORE_SECTION_COUNT; s++) sec_release(m->sec[s]);
    for (int t = 0; t < CORE_TABLE_COUNT; t++) {
        tab_release(m->tab[t]);
        free(m->changed[t]);
        free(m->seen[t]);
    }
    memset(m, 0, sizeof *m);
}

static int field_ok(core_field f, int kind) {
    if ((unsigned)f >= (unsigned)CORE_FIELD_COUNT) return 0;
    int k = core_field_meta[f].kind;
    return k == kind || (kind == K_I64 && k == K_FLAG);
}

int model_set_str(model *m, core_field f, const char *v, size_t n) {
    CORE_CHECK(field_ok(f, K_STR));
    if (!field_ok(f, K_STR) || (!v && n)) return CORE_E_ARG;
    if (n == CORE_NTS) n = strlen(v);
    if (n > UINT32_MAX - 1) return CORE_E_ARG;
    const field_meta *fm = &core_field_meta[f];
    const cell *old = &m->sec[fm->section]->cells[fm->index];
    if (old->len == n && (!n || !memcmp(old->v.s, v, n))) return CORE_OK;   /* the same value: nothing moves */
    section_obj *prev, *s = sec_w(m, fm->section, &prev);
    if (!s) return CORE_E_NOMEM;
    cell *x = &s->cells[fm->index];
    const char *p = empty_str;
    if (n) p = arena_strdup(&s->strs, v, n);   /* v may point into prev: released only after this copy */
    sec_release(prev);
    if (!p) return CORE_E_NOMEM;
    s->garbage += x->len ? x->len + 1 : 0;
    x->v.s = p;
    x->len = (uint32_t)n;
    m->dirty_sections |= 1u << fm->section;
    return CORE_OK;
}

static int set_num(model *m, core_field f, int kind, int64_t i, double d) {
    const field_meta *fm = &core_field_meta[f];
    const cell *old = &m->sec[fm->section]->cells[fm->index];
    if (kind == K_F64 ? old->v.f == d && (d != 0 || 1 / old->v.f == 1 / d) : old->v.i == i) return CORE_OK;
    section_obj *prev, *s = sec_w(m, fm->section, &prev);
    if (!s) return CORE_E_NOMEM;
    sec_release(prev);
    if (kind == K_F64) s->cells[fm->index].v.f = d;
    else s->cells[fm->index].v.i = i;
    m->dirty_sections |= 1u << fm->section;
    return CORE_OK;
}

int model_set_i64(model *m, core_field f, int64_t v) {
    CORE_CHECK(field_ok(f, K_I64));
    if (!field_ok(f, K_I64)) return CORE_E_ARG;
    if (core_field_meta[f].kind == K_FLAG) v = v != 0;
    return set_num(m, f, K_I64, v, 0);
}

int model_set_f64(model *m, core_field f, double v) {
    CORE_CHECK(field_ok(f, K_F64));
    if (!field_ok(f, K_F64)) return CORE_E_ARG;
    return set_num(m, f, K_F64, 0, v);
}

void model_touch(model *m, core_field f) {
    if ((unsigned)f < (unsigned)CORE_FIELD_COUNT) m->dirty_sections |= 1u << core_field_meta[f].section;
}

int64_t model_get_i64(const model *m, core_field f) {
    if (!field_ok(f, K_I64)) return 0;
    const field_meta *fm = &core_field_meta[f];
    return m->sec[fm->section]->cells[fm->index].v.i;
}

/* the baseline is the last render, not dirty_sections alone: core_snap_current freezes (and so
 * consumes) those, and a table change moves its section's rev */
int model_needs_render(const model *m) {
    if (!m->rendered || m->dirty_sections) return 1;
    for (int s = 0; s < CORE_SECTION_COUNT; s++)
        if (m->rev[s] != m->last_rev[s]) return 1;
    return 0;
}

/* forget the rows tracked as changed since the last render (after a render or a reload) */
static void untrack(model *m, core_table t) {
    for (int32_t i = 0; i < m->nchanged[t]; i++) {
        int32_t r = m->changed[t][i];
        if (r < m->seencap[t] * 8) m->seen[t][r >> 3] &= (uint8_t)~(1u << (r & 7));
    }
    m->nchanged[t] = 0;
}

static int track(model *m, core_table t, int32_t row) {
    /* tracked only below the row count of the last render, within its list_gen: rows after it
     * are appended ones, and a new list_gen reloads everything */
    if (!m->rendered || m->tab[t]->list_gen != m->last_gen[t] || row >= m->last_rows[t]) return 0;
    if (row >= m->seencap[t] * 8) {
        int32_t cap = (m->last_rows[t] + 7) / 8;
        uint8_t *s = realloc(m->seen[t], (size_t)cap);
        if (!s) return -1;
        memset(s + m->seencap[t], 0, (size_t)(cap - m->seencap[t]));
        m->seen[t] = s;
        m->seencap[t] = cap;
    }
    uint8_t bit = (uint8_t)(1u << (row & 7));
    if (m->seen[t][row >> 3] & bit) return 0;
    if (m->nchanged[t] == m->capchanged[t]) {
        int32_t cap = m->capchanged[t] ? m->capchanged[t] * 2 : 64;
        int32_t *c = realloc(m->changed[t], (size_t)cap * sizeof *c);
        if (!c) return -1;
        m->changed[t] = c;
        m->capchanged[t] = cap;
    }
    m->changed[t][m->nchanged[t]++] = row;
    m->seen[t][row >> 3] |= bit;
    return 0;
}

static int append_rows(model *m, table_obj *o, int32_t rows) {
    if (rows <= 0) return CORE_OK;
    if (rows > INT32_MAX - o->n || tab_grow(o, o->n + rows)) return CORE_E_NOMEM;
    row_obj *e = row_empty(o->table, o->list_gen);   /* one empty row, shared by every new row */
    if (!e) return CORE_E_NOMEM;
    for (int32_t i = 0; i < rows; i++) o->rows[o->n + i] = i ? row_retain(e) : e;
    o->n += rows;
    (void)m;
    return CORE_OK;
}

int model_table_reset(model *m, core_table t, int32_t rows) {
    if ((unsigned)t >= (unsigned)CORE_TABLE_COUNT || rows < 0) return CORE_E_ARG;
    table_obj *o = tab_new((uint8_t)t, ++m->counter, rows);
    if (!o) return CORE_E_NOMEM;
    if (append_rows(m, o, rows)) {
        tab_release(o);
        return CORE_E_NOMEM;
    }
    tab_release(m->tab[t]);
    m->tab[t] = o;
    untrack(m, t);
    m->dirty_sections |= 1u << core_table_meta[t].section;
    return CORE_OK;
}

int model_table_append(model *m, core_table t, int32_t rows) {
    if ((unsigned)t >= (unsigned)CORE_TABLE_COUNT || rows < 0) return CORE_E_ARG;
    if (!rows) return CORE_OK;
    table_obj *o = tab_w(m, t);
    if (!o) return CORE_E_NOMEM;
    int r = append_rows(m, o, rows);
    if (r) return r;
    m->dirty_sections |= 1u << core_table_meta[t].section;
    return CORE_OK;
}

static int set_cell(model *m, core_column col, int32_t row, const cell *nv) {
    const column_meta *cm = &core_column_meta[col];
    table_obj *o = m->tab[cm->table];
    if (row < 0 || row >= o->n) return CORE_E_ARG;
    const cell *old = &o->rows[row]->cells[cm->index];
    if (nv->kind == K_STR ? old->len == nv->len && (!nv->len || !memcmp(old->v.s, nv->v.s, nv->len))
                          : nv->kind == K_F64 ? old->v.f == nv->v.f : old->v.i == nv->v.i)
        return CORE_OK;   /* the same value: the row's rev doesn't move */
    if (!(o = tab_w(m, cm->table))) return CORE_E_NOMEM;
    row_obj *r = row_with(o->rows[row], cm->index, nv, ++m->counter);
    if (!r) return CORE_E_NOMEM;
    if (track(m, cm->table, row)) {
        row_release(r);
        return CORE_E_NOMEM;
    }
    row_release(o->rows[row]);
    o->rows[row] = r;
    m->dirty_sections |= 1u << core_table_meta[cm->table].section;
    return CORE_OK;
}

static int column_ok(core_column col, int kind) {
    if ((unsigned)col >= (unsigned)CORE_COLUMN_COUNT) return 0;
    int k = core_column_meta[col].kind;
    return k == kind || (kind == K_I64 && k == K_FLAG);
}

int model_cell_str(model *m, core_column col, int32_t row, const char *v, size_t n) {
    CORE_CHECK(column_ok(col, K_STR));
    if (!column_ok(col, K_STR) || (!v && n)) return CORE_E_ARG;
    if (n == CORE_NTS) n = strlen(v);
    if (n > UINT32_MAX - 1) return CORE_E_ARG;
    cell nv = {.kind = K_STR, .len = (uint32_t)n, .v.s = n ? v : empty_str};
    return set_cell(m, col, row, &nv);
}

int model_cell_i64(model *m, core_column col, int32_t row, int64_t v) {
    CORE_CHECK(column_ok(col, K_I64));
    if (!column_ok(col, K_I64)) return CORE_E_ARG;
    uint8_t k = core_column_meta[col].kind;
    cell nv = {.kind = k, .v.i = k == K_FLAG ? v != 0 : v};
    return set_cell(m, col, row, &nv);
}

int model_cell_f64(model *m, core_column col, int32_t row, double v) {
    CORE_CHECK(column_ok(col, K_F64));
    if (!column_ok(col, K_F64)) return CORE_E_ARG;
    cell nv = {.kind = K_F64, .v.f = v};
    return set_cell(m, col, row, &nv);
}

static int cmp_i32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

core_snap *model_freeze(model *m, int for_render) {
    for (int s = 0; s < CORE_SECTION_COUNT; s++)
        if (m->dirty_sections & (1u << s)) m->rev[s] = ++m->counter;
    m->dirty_sections = 0;
    core_snap *snap = calloc(1, sizeof *snap);
    if (!snap) return NULL;
    LIVE_INC();
    atomic_init(&snap->refs, 1);
    for (int s = 0; s < CORE_SECTION_COUNT; s++) {
        snap->sec[s] = sec_retain(m->sec[s]);
        snap->rev[s] = m->rev[s];
    }
    for (int t = 0; t < CORE_TABLE_COUNT; t++) snap->tab[t] = tab_retain(m->tab[t]);
    if (!for_render) return snap;

    core_delta *d = &snap->delta;
    for (int s = 0; s < CORE_SECTION_COUNT; s++)
        if (!m->rendered || m->rev[s] != m->last_rev[s]) d->sections |= 1u << s;
    for (int t = 0; t < CORE_TABLE_COUNT; t++) {
        table_obj *o = m->tab[t];
        int reload = !m->rendered || o->list_gen != m->last_gen[t];
        d->reload[t] = (uint8_t)reload;
        d->old_rows[t] = reload ? 0 : m->last_rows[t];
        if (!reload && m->nchanged[t]) {
            int32_t *c = malloc((size_t)m->nchanged[t] * sizeof *c);
            if (!c) {
                d->reload[t] = 1;   /* out of memory: a full reload is still correct */
                d->old_rows[t] = 0;
            } else {
                memcpy(c, m->changed[t], (size_t)m->nchanged[t] * sizeof *c);
                qsort(c, (size_t)m->nchanged[t], sizeof *c, cmp_i32);
                d->changed[t] = c;
                d->nchanged[t] = m->nchanged[t];
            }
        }
        untrack(m, t);
        m->last_gen[t] = o->list_gen;
        m->last_rows[t] = o->n;
        if (m->seencap[t] * 8 < o->n) {   /* room for every row of this render's count */
            int32_t cap = (o->n + 7) / 8;
            uint8_t *s = realloc(m->seen[t], (size_t)cap);
            if (s) {
                memset(s + m->seencap[t], 0, (size_t)(cap - m->seencap[t]));
                m->seen[t] = s;
                m->seencap[t] = cap;
            }
        }
    }
    for (int s = 0; s < CORE_SECTION_COUNT; s++) m->last_rev[s] = m->rev[s];
    m->rendered = 1;
    return snap;
}

/* ---------------------------------------------------------------- snapshots: the API */

void core_snap_retain(core_snap *s) {
    if (s) atomic_fetch_add_explicit(&s->refs, 1, memory_order_relaxed);
}

void core_snap_release(core_snap *s) {
    if (!s || atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) != 1) return;
    for (int i = 0; i < CORE_SECTION_COUNT; i++) sec_release(s->sec[i]);
    for (int t = 0; t < CORE_TABLE_COUNT; t++) {
        tab_release(s->tab[t]);
        free(s->delta.changed[t]);
    }
    free(s);
    LIVE_DEC();
}

uint64_t core_snap_rev(const core_snap *s, core_section section) {
    CORE_CHECK(s && (unsigned)section < (unsigned)CORE_SECTION_COUNT);
    if (!s || (unsigned)section >= (unsigned)CORE_SECTION_COUNT) return 0;
    return s->rev[section];
}

static const cell *field_cell(const core_snap *s, core_field f, int kind) {
    CORE_CHECK(s && field_ok(f, kind));
    if (!s || !field_ok(f, kind)) return NULL;
    const field_meta *fm = &core_field_meta[f];
    return &s->sec[fm->section]->cells[fm->index];
}

const char *core_str(const core_snap *s, core_field f, size_t *len) {
    const cell *c = field_cell(s, f, K_STR);
    if (len) *len = c ? c->len : 0;
    return c ? c->v.s : empty_str;
}

int32_t core_flag(const core_snap *s, core_field f) {
    CORE_CHECK((unsigned)f < (unsigned)CORE_FIELD_COUNT && core_field_meta[f].kind == K_FLAG);
    const cell *c = field_cell(s, f, K_I64);
    return c ? (int32_t)(c->v.i != 0) : 0;
}

int64_t core_i64(const core_snap *s, core_field f) {
    const cell *c = field_cell(s, f, K_I64);
    return c ? c->v.i : 0;
}

double core_f64(const core_snap *s, core_field f) {
    const cell *c = field_cell(s, f, K_F64);
    return c ? c->v.f : 0;
}

int32_t core_rows(const core_snap *s, core_table t) {
    CORE_CHECK(s && (unsigned)t < (unsigned)CORE_TABLE_COUNT);
    if (!s || (unsigned)t >= (unsigned)CORE_TABLE_COUNT) return 0;
    return s->tab[t]->n;
}

uint64_t core_list_gen(const core_snap *s, core_table t) {
    CORE_CHECK(s && (unsigned)t < (unsigned)CORE_TABLE_COUNT);
    if (!s || (unsigned)t >= (unsigned)CORE_TABLE_COUNT) return 0;
    return s->tab[t]->list_gen;
}

uint64_t core_row_rev(const core_snap *s, core_table t, int32_t row) {
    CORE_CHECK(s && (unsigned)t < (unsigned)CORE_TABLE_COUNT && row >= 0 && row < s->tab[t]->n);
    if (!s || (unsigned)t >= (unsigned)CORE_TABLE_COUNT || row < 0 || row >= s->tab[t]->n) return 0;
    return s->tab[t]->rows[row]->rev;
}

static const cell *row_cell(const core_snap *s, core_column col, int32_t row, int kind) {
    CORE_CHECK(s && column_ok(col, kind));
    if (!s || !column_ok(col, kind)) return NULL;
    const column_meta *cm = &core_column_meta[col];
    const table_obj *t = s->tab[cm->table];
    CORE_CHECK(row >= 0 && row < t->n);
    if (row < 0 || row >= t->n) return NULL;
    return &t->rows[row]->cells[cm->index];
}

const char *core_cell_str(const core_snap *s, core_column col, int32_t row, size_t *len) {
    const cell *c = row_cell(s, col, row, K_STR);
    if (len) *len = c ? c->len : 0;
    return c ? c->v.s : empty_str;
}

int32_t core_cell_flag(const core_snap *s, core_column col, int32_t row) {
    CORE_CHECK((unsigned)col < (unsigned)CORE_COLUMN_COUNT && core_column_meta[col].kind == K_FLAG);
    const cell *c = row_cell(s, col, row, K_I64);
    return c ? (int32_t)(c->v.i != 0) : 0;
}

int64_t core_cell_i64(const core_snap *s, core_column col, int32_t row) {
    const cell *c = row_cell(s, col, row, K_I64);
    return c ? c->v.i : 0;
}

double core_cell_f64(const core_snap *s, core_column col, int32_t row) {
    const cell *c = row_cell(s, col, row, K_F64);
    return c ? c->v.f : 0;
}

uint32_t core_delta_sections(const core_delta *d) { return d ? d->sections : 0; }

int32_t core_delta_reload(const core_delta *d, core_table t) {
    if (!d || (unsigned)t >= (unsigned)CORE_TABLE_COUNT) return 1;
    return d->reload[t];
}

int32_t core_delta_old_rows(const core_delta *d, core_table t) {
    if (!d || (unsigned)t >= (unsigned)CORE_TABLE_COUNT) return 0;
    return d->old_rows[t];
}

const int32_t *core_delta_changed(const core_delta *d, core_table t, int32_t *n) {
    if (!d || (unsigned)t >= (unsigned)CORE_TABLE_COUNT || !d->nchanged[t]) {
        if (n) *n = 0;
        return NULL;
    }
    if (n) *n = d->nchanged[t];
    return d->changed[t];
}
