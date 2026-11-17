/* Arenas: chunked bump allocation, freed all at once (mac-core §3.10: per-event scratch and a
 * section's own contents only). */
#include "internal.h"

struct arena_chunk {
    arena_chunk *next;
    size_t cap, used;
    _Alignas(16) unsigned char data[];
};

enum { ARENA_CHUNK = 4096 - sizeof(arena_chunk) };

void *arena_alloc(arena *a, size_t n) {
    size_t need = (n + 15) & ~(size_t)15;
    if (need < n) return NULL;
    arena_chunk *h = a->head;
    if (!h || h->cap - h->used < need) {
        size_t cap = need > ARENA_CHUNK ? need : ARENA_CHUNK;
        if (cap > SIZE_MAX - sizeof(arena_chunk)) return NULL;
        arena_chunk *c = malloc(sizeof(arena_chunk) + cap);
        if (!c) return NULL;
        c->cap = cap;
        c->used = 0;
        /* a big one-off block goes behind the current chunk, so the current one keeps filling */
        if (h && need > ARENA_CHUNK) {
            c->next = h->next;
            h->next = c;
        } else {
            c->next = h;
            a->head = c;
        }
        h = c;
    }
    void *p = h->data + h->used;
    h->used += need;
    a->used_total += need;
    return p;
}

char *arena_strdup(arena *a, const char *s, size_t n) {
    if (n == SIZE_MAX) return NULL;
    char *p = arena_alloc(a, n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void arena_reset(arena *a) {
    arena_chunk *h = a->head;
    if (!h) return;
    arena_chunk *keep = NULL;
    while (h) {
        arena_chunk *next = h->next;
        if (!keep && h->cap == ARENA_CHUNK) keep = h;
        else free(h);
        h = next;
    }
    if (keep) {
        keep->next = NULL;
        keep->used = 0;
    }
    a->head = keep;
    a->used_total = 0;
}

void arena_free(arena *a) {
    arena_chunk *h = a->head;
    while (h) {
        arena_chunk *next = h->next;
        free(h);
        h = next;
    }
    a->head = NULL;
    a->used_total = 0;
}
