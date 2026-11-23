/* Force the dump tool's 16-bit conversion allocation to fail after the track loads. */
#include <assert.h>
#include <stdlib.h>

static void *fail_malloc(size_t size) {
    assert(size == 2 * sizeof(float));
    return NULL;
}

#define main decode_dump_main
#define malloc fail_malloc
#include "decode_dump.c"
#undef malloc
#undef main

static int freed;

int load_tracks(const char *path, const char *app, tracks *ts, char *why, size_t whyn) {
    (void)path; (void)app; (void)why; (void)whyn;
    static int16_t sample = 1;
    *ts = (tracks){.count = 1, .t = {{.p16 = &sample, .ch = 1, .n = 1}}};
    return 0;
}

void tracks_free(tracks *ts) {
    assert(ts->count == 1);
    freed++;
}

void chunk_at(const track *t, size_t i, float *c) { (void)t; (void)i; (void)c; abort(); }

int main(void) {
    char *argv[] = {"decode_dump", "rekordbox", ".", "fixture.m4a", NULL};
    int rc = decode_dump_main(4, argv);
    assert(rc == 1 && freed == 1);
    return 0;
}
