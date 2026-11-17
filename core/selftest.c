/* core_selftest(): what `--swap` runs first (G1.4). Pure C, no core, no host, no globals: the
 * tables, the texts, the event parser, the arenas and one snapshot round trip. A nonzero result
 * is the number of the first failed check. */
#include "internal.h"

static int tables_ok(void) {
    /* each field and column has its own index inside its section or table, and the counts agree */
    uint32_t seen_f[CORE_SECTION_COUNT][8] = {{0}};
    for (int f = 0; f < CORE_FIELD_COUNT; f++) {
        const field_meta *m = &core_field_meta[f];
        if (!m->name || m->section >= CORE_SECTION_COUNT || m->kind > K_F64) return 0;
        if (m->index >= core_section_meta[m->section].nfields || m->index >= 256) return 0;
        uint32_t *w = &seen_f[m->section][m->index / 32], bit = 1u << (m->index % 32);
        if (*w & bit) return 0;
        *w |= bit;
    }
    int nf = 0;
    for (int s = 0; s < CORE_SECTION_COUNT; s++) nf += core_section_meta[s].nfields;
    if (nf != CORE_FIELD_COUNT) return 0;
    int nc = 0;
    for (int t = 0; t < CORE_TABLE_COUNT; t++) {
        if (!core_table_meta[t].name || core_table_meta[t].section >= CORE_SECTION_COUNT) return 0;
        nc += core_table_meta[t].ncols;
    }
    if (nc != CORE_COLUMN_COUNT) return 0;
    for (int c = 0; c < CORE_COLUMN_COUNT; c++) {
        const column_meta *m = &core_column_meta[c];
        if (!m->name || m->table >= CORE_TABLE_COUNT || m->index >= core_table_meta[m->table].ncols || m->kind > K_F64) return 0;
    }
    for (int f = 0; f < CORE_FACT_COUNT; f++)
        if (!core_fact_meta[f].name || core_fact_meta[f].kind == K_F64) return 0;
    return 1;
}

static int menus_ok(void) {
    for (int32_t i = 0; i < core_menu_rows_n; i++) {
        const menu_row *r = &core_menu_rows[i];
        if (r->id != i || r->parent >= i) return 0;   /* parents first: no cycles, any depth */
        if (r->parent >= 0) {   /* the parent is an item, present for every OS the child is */
            const menu_row *p = &core_menu_rows[r->parent];
            if (p->kind != CORE_MENU_ITEM || (r->os & ~p->os)) return 0;
        }
        if (r->title >= CORE_TEXT_COUNT || (r->kind == CORE_MENU_ITEM && r->title < 0)) return 0;
        if (!r->key_mac || !r->key_win || !r->event || !r->os) return 0;
    }
    return core_menu_count(CORE_OS_MAC) > 0;
}

static int text_ok(void) {
    char b[256];
    size_t n = 0;
    core_slot s = {CORE_SLOT_engine, 0, "{drive}", CORE_NTS};
    if (core_text(CORE_OS_MAC, T_CACHE_ENGINE, &s, 1, b, sizeof b, &n) != CORE_OK) return 0;
    static const char want[] = "Stems Cache doesn't support STEMS Engine {drive} yet. When an RB Stems Plus update supports it, click Reinstall Stems Cache.";
    if (n != sizeof want - 1 || strcmp(b, want)) return 0;
    if (core_text(CORE_OS_MAC, T_CACHE_ENGINE, NULL, 0, b, sizeof b, &n) != CORE_E_ARG) return 0;   /* a slot not given */
    if (core_text(CORE_OS_MAC, T_OK, &s, 1, b, sizeof b, &n) != CORE_E_ARG) return 0;              /* a slot not used */
    if (core_text(CORE_OS_MAC, T_CHECKING, NULL, 0, b, 4, &n) != CORE_E_SPACE || n != strlen("Checking…") || strlen(b) != 3) return 0;
    return 1;
}

static int json_ok(arena *a) {
    static const char *const good[] = {
        "{\"ev\":\"click\",\"id\":\"install_cache\"}", " { \"ev\" : \"tab\" , \"tab\" : 2 } ",
        "{\"a\":[1,-2,{\"b\":null}],\"c\":true,\"d\":\"\\u00e9\\ud83d\\ude00\\n\"}", "[]", "-9223372036854775808", "9223372036854775808", "{\"a\":-1.5e-3,\"b\":1E+2,\"c\":0.25}", "{\"ev\":\"mount\",\"path\":\"D:\\\\Music\\\\\"}",
    };
    static const char *const bad[] = {
        "", "{", "{\"a\":1,}", "{\"a\":1,\"a\":2}", "{\"a\":01}", "{\"a\":1.}", "{\"a\":1e}", "{\"a\":-}", "{\"a\":\"\\u0000\"}",
        "{\"a\":\"\\ud83d\"}", "{\"a\":\"\x80\"}", "{\"a\":\"\x01\"}", "{} x", "[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]", ".5", "1.5.2",
    };
    jval v;
    for (size_t i = 0; i < sizeof good / sizeof *good; i++)
        if (json_parse(a, good[i], strlen(good[i]), &v) != CORE_OK) return 0;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        if (json_parse(a, bad[i], strlen(bad[i]), &v) == CORE_OK) return 0;
    if (json_parse(a, good[2], strlen(good[2]), &v)) return 0;
    const jval *d = json_get(&v, "d");
    if (!d || d->type != J_STR || d->len != 7 || memcmp(d->s, "\xc3\xa9\xf0\x9f\x98\x80\n", 7)) return 0;
    return core_event_lookup("activated", 9) == EV_ACTIVATED && core_event_lookup("nope", 4) < 0;
}

static int arena_ok(void) {
    arena a = {0};
    char *p = arena_alloc(&a, 10), *q = arena_alloc(&a, 100000);
    char *r = arena_strdup(&a, "abc", 3);
    int ok = p && q && r && !((uintptr_t)p & 15) && !((uintptr_t)q & 15) && !strcmp(r, "abc");
    if (ok) memset(q, 1, 100000);
    arena_reset(&a);
    ok = ok && a.used_total == 0 && arena_alloc(&a, 1);
    arena_free(&a);
    return ok && !a.head;
}

static int snapshot_ok(void) {
    model m;
    if (model_init(&m)) return 0;
    int ok = 1;
    ok &= !model_set_str(&m, F_STATUS_LINE, "one", CORE_NTS) && !model_table_reset(&m, T_PREP_TRACKS, 3);
    ok &= !model_cell_str(&m, C_TRK_NAME, 1, "track b", CORE_NTS);
    core_snap *a = ok ? model_freeze(&m, 1) : NULL;
    ok &= a != NULL;
    ok &= !model_set_str(&m, F_STATUS_LINE, "two", CORE_NTS) && !model_cell_str(&m, C_TRK_NAME, 2, "track c", CORE_NTS);
    core_snap *b = ok ? model_freeze(&m, 1) : NULL;
    ok &= b != NULL;
    if (ok) {
        size_t n;
        int32_t nc;
        const int32_t *ch = core_delta_changed(&b->delta, T_PREP_TRACKS, &nc);
        ok &= !strcmp(core_str(a, F_STATUS_LINE, &n), "one") && n == 3 && !strcmp(core_str(b, F_STATUS_LINE, NULL), "two");
        ok &= !strcmp(core_cell_str(a, C_TRK_NAME, 2, NULL), "") && !strcmp(core_cell_str(b, C_TRK_NAME, 2, NULL), "track c");
        ok &= !strcmp(core_cell_str(b, C_TRK_NAME, 1, NULL), "track b");
        ok &= core_rows(b, T_PREP_TRACKS) == 3 && core_list_gen(a, T_PREP_TRACKS) == core_list_gen(b, T_PREP_TRACKS);
        ok &= core_delta_reload(&a->delta, T_PREP_TRACKS) && !core_delta_reload(&b->delta, T_PREP_TRACKS);
        ok &= nc == 1 && ch && ch[0] == 2 && core_delta_old_rows(&b->delta, T_PREP_TRACKS) == 3;
        ok &= core_snap_rev(a, S_APP) != core_snap_rev(b, S_APP) && core_snap_rev(a, S_STATUS) == core_snap_rev(b, S_STATUS);
        ok &= (core_delta_sections(&b->delta) & (1u << S_STATUS)) == 0 && (core_delta_sections(&b->delta) & (1u << S_PREPARE));
        ok &= a->tab[T_PREP_TRACKS]->rows[1] == b->tab[T_PREP_TRACKS]->rows[1];   /* shared, not copied */
    }
    core_snap_release(a);   /* out of order: the older one first */
    model_free(&m);         /* the model goes before the last snapshot */
    core_snap_release(b);
    return ok;
}

int32_t core_selftest(void) {
    if (core_api_version() != CORE_API_VERSION) return 1;
    if (!tables_ok()) return 2;
    if (core_text_check_all()) return 3;
    if (!text_ok()) return 4;
    if (!menus_ok()) return 5;
    arena a = {0};
    int j = json_ok(&a);
    arena_free(&a);
    if (!j) return 6;
    if (!arena_ok()) return 7;
    if (!snapshot_ok()) return 8;
#ifdef CORE_SELFTEST_BROKEN
    /* core/build.sh lib --test-hooks --broken-selftest only (G1.4's test: a TEST build whose
     * self-test fails is refused by --swap); never in a release archive */
    return 9;
#endif
    return 0;
}
