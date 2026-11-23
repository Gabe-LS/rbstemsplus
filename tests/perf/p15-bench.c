/* P15 (mac-core §2.1): the helper-line parse time per line, on the host: the C core's parser
 * (core/helper_lines.c's prep_run_line_parse, which took over Prepare.swift's parseRunLine), on
 * the same lines as tests/perf/p15-bench.swift, each line's strings in an arena reset per line as
 * the core's reader does. Built by tests/perf/p15.sh with the core's sources (-O2, as the core);
 * prints "c <ns per line>". */
#include "internal.h"
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILES 2000
#define LINES (2 * FILES + 1)

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(void) {
    static char *lines[LINES];
    static size_t lens[LINES];
    char buf[512];
    int n = 0;
    for (int i = 0; i < FILES; i++) {
        lens[n] = (size_t)snprintf(buf, sizeof buf, "{\"at\":%d,\"files\":2000,\"chunk\":3,\"chunks\":21}", i);
        lines[n++] = strdup(buf);
        lens[n] = (size_t)snprintf(buf, sizeof buf, "{\"file\":\"/Users/dj/Music/Harness/track-%d.mp3\",\"result\":\"prepare\",\"seconds\":241.5,\"entries\":22,\"gb\":0.0625,\"missing\":0}", i);
        lines[n++] = strdup(buf);
    }
    lens[n] = (size_t)snprintf(buf, sizeof buf, "{\"done\":2000,\"runs\":44000,\"run_s\":110000,\"kept\":\"7\",\"until\":1790000000,\"kept_gb\":12.5}");
    lines[n++] = strdup(buf);

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    arena a = {0};
    double per_line[30];
    long sink = 0;
    for (int b = 0; b < 30; b++) {
        uint64_t s = mach_absolute_time();
        for (int i = 0; i < n; i++) {
            prep_run_line l;
            int r = prep_run_line_parse(&a, lines[i], lens[i], &l);
            if (r < 0) { fprintf(stderr, "out of memory\n"); return 1; }
            sink += r;
            arena_reset(&a);
        }
        per_line[b] = (double)((mach_absolute_time() - s) * tb.numer / tb.denom) / n;
    }
    arena_free(&a);
    qsort(per_line, 30, sizeof per_line[0], cmp_double);
    printf("c %.1f\n", per_line[15]);
    printf("# ns per line, median of 30 batches of %d lines (min %.1f, max %.1f); parsed %ld\n", n, per_line[0], per_line[29], sink);
    return 0;
}
