/* The ids hash (D7): FNV-1a over every .def row this build keeps: its table, name, kind, OS tag,
 * owner (section, table or parent menu) and position. Included by ids.c (core_ids_hash) and by
 * tools/gen_ids.c, which writes the same value into the shells' generated ids, so the two can't
 * be computed differently. Needs core.h. */
#ifndef RBSP_CORE_IDS_HASH_H
#define RBSP_CORE_IDS_HASH_H

#include <stdint.h>

static inline uint64_t ids_fnv_bytes(uint64_t h, const void *p, unsigned long n) {
    const unsigned char *b = (const unsigned char *)p;
    for (unsigned long i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static inline uint64_t ids_fnv_str(uint64_t h, const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return ids_fnv_bytes(h, s, n + 1);   /* the NUL separates the fields */
}

static inline uint64_t ids_row(uint64_t h, const char *table, const char *name, const char *kind, const char *os,
                               const char *owner, long long position) {
    unsigned char pos[8];
    for (int i = 0; i < 8; i++) pos[i] = (unsigned char)((unsigned long long)position >> (8 * i));
    h = ids_fnv_str(h, table);
    h = ids_fnv_str(h, name);
    h = ids_fnv_str(h, kind);
    h = ids_fnv_str(h, os);
    h = ids_fnv_str(h, owner);
    return ids_fnv_bytes(h, pos, 8);
}

/* events.def is left out on purpose: events cross the boundary by name, never by a positional id,
 * and a renamed event is caught at run time (CORE_E_UNKNOWN). */
static inline uint64_t ids_hash_compute(void) {
    uint64_t h = 0xcbf29ce484222325ULL;
#define SECTION(id, doc) h = ids_row(h, "SECTION", #id, "", "", "", id);
#define TABLE(id, section, os, doc) CORE_KEEP_##os(h = ids_row(h, "TABLE", #id, "", #os, #section, id);)
#define FIELD(id, section, kind, os, doc) CORE_KEEP_##os(h = ids_row(h, "FIELD", #id, #kind, #os, #section, id);)
#define COLUMN(id, table, kind, os, doc) CORE_KEEP_##os(h = ids_row(h, "COLUMN", #id, #kind, #os, #table, id);)
#include "schema.def"
#define SLOT(name, os) CORE_KEEP_##os(h = ids_row(h, "SLOT", #name, "", #os, "", CORE_SLOT_##name);)
#define TEXT(id, os, mac, win) CORE_KEEP_##os(h = ids_row(h, "TEXT", #id, "", #os, "", id);)
#include "texts.def"
#define MENU(id, os, menu, kind, title, key_mac, key_win, mods_mac, mods_win, event) \
    CORE_KEEP_##os(h = ids_row(h, "MENU", #id, #kind, #os, #menu, id);)
#include "menus.def"
#define FACT(id, kind, os, doc) CORE_KEEP_##os(h = ids_row(h, "FACT", #id, #kind, #os, "", id);)
#include "facts.def"
    return h;
}

#endif
