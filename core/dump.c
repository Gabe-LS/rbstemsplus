/* A snapshot as JSON, by names: for tests, traces and the report, never the hot path (§3.4). */
#include "internal.h"
#include <stdio.h>

typedef struct { char *buf; size_t cap, n; } out;

static void put(out *o, const char *s, size_t n) {
    if (o->n < o->cap) {
        size_t room = o->cap - o->n;
        memcpy(o->buf + o->n, s, n < room ? n : room);
    }
    o->n += n;
}

static void puts_(out *o, const char *s) { put(o, s, strlen(s)); }

static void put_str(out *o, const char *s, size_t n) {
    static const char hexd[] = "0123456789abcdef";
    put(o, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; put(o, e, 2); }
        else if (c < 0x20) { char e[6] = {'\\', 'u', '0', '0', hexd[c >> 4], hexd[c & 15]}; put(o, e, 6); }
        else put(o, (const char *)&c, 1);
    }
    put(o, "\"", 1);
}

static void put_i64(out *o, int64_t v) {
    char b[24];
    int n = snprintf(b, sizeof b, "%lld", (long long)v);
    put(o, b, (size_t)n);
}

static void put_f64(out *o, double v) {
    char b[40];
    if (v != v || v - v != 0) { puts_(o, "null"); return; }   /* NaN and infinities aren't JSON */
    int n = snprintf(b, sizeof b, "%.17g", v);
    for (int i = 0; i < n; i++) if (b[i] == ',') b[i] = '.';   /* whatever the C locale */
    put(o, b, (size_t)n);
}

static void put_cell(out *o, const cell *c) {
    switch (c->kind) {
    case K_STR: put_str(o, c->v.s, c->len); break;
    case K_FLAG: puts_(o, c->v.i ? "true" : "false"); break;
    case K_I64: put_i64(o, c->v.i); break;
    default: put_f64(o, c->v.f); break;
    }
}

int32_t core_snap_dump(const core_snap *s, char *buf, size_t cap, size_t *len) {
    if (len) *len = 0;
    if (!s || (cap && !buf)) return CORE_E_ARG;
    out o = {buf, cap ? cap - 1 : 0, 0};
    puts_(&o, "{\"sections\":{");
    for (int sec = 0; sec < CORE_SECTION_COUNT; sec++) {
        if (sec) puts_(&o, ",");
        put_str(&o, core_section_meta[sec].name, strlen(core_section_meta[sec].name));
        puts_(&o, ":{\"rev\":");
        put_i64(&o, (int64_t)s->rev[sec]);
        for (int f = 0; f < CORE_FIELD_COUNT; f++) {
            if (core_field_meta[f].section != sec) continue;
            puts_(&o, ",");
            put_str(&o, core_field_meta[f].name, strlen(core_field_meta[f].name));
            puts_(&o, ":");
            put_cell(&o, &s->sec[sec]->cells[core_field_meta[f].index]);
        }
        puts_(&o, "}");
    }
    puts_(&o, "},\"tables\":{");
    for (int t = 0; t < CORE_TABLE_COUNT; t++) {
        const table_obj *tab = s->tab[t];
        if (t) puts_(&o, ",");
        put_str(&o, core_table_meta[t].name, strlen(core_table_meta[t].name));
        puts_(&o, ":{\"list_gen\":");
        put_i64(&o, (int64_t)tab->list_gen);
        puts_(&o, ",\"rows\":[");
        for (int32_t r = 0; r < tab->n; r++) {
            puts_(&o, r ? ",{\"rev\":" : "{\"rev\":");
            put_i64(&o, (int64_t)tab->rows[r]->rev);
            for (int c = 0; c < CORE_COLUMN_COUNT; c++) {
                if (core_column_meta[c].table != t) continue;
                puts_(&o, ",");
                put_str(&o, core_column_meta[c].name, strlen(core_column_meta[c].name));
                puts_(&o, ":");
                put_cell(&o, &tab->rows[r]->cells[core_column_meta[c].index]);
            }
            puts_(&o, "}");
        }
        puts_(&o, "]}");
    }
    puts_(&o, "}}");
    if (cap) buf[o.n < o.cap ? o.n : o.cap] = 0;
    if (len) *len = o.n;
    return o.n < cap ? CORE_OK : CORE_E_SPACE;
}
