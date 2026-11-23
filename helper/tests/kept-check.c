/* Check a helper-written set with the Windows bridge's parser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "entries.h"
#include "plat_helper.h"

#include "kept-parser.inc"

int main(int argc, char **argv) {
#ifdef _WIN32
    if (plat_exe_start()) return 1;
#endif
    if (argc != 4) {
        fprintf(stderr, "usage: %s FILE UNTIL COUNT\n", argv[0]);
        return 2;
    }

    plat_file f;
    if (plat_file_open_path(argv[1], &f)) return 1;
    char *buf = malloc(KEPT_FILE_MAX + 1);
    size_t n = 0, got = 0, count = 0;
    long long until;
    int e = 0;
    if (buf) {
        do {
            e = plat_read(f, buf + n, KEPT_FILE_MAX + 1 - n, &got);
            n += got;
        } while (!e && got && n <= KEPT_FILE_MAX);
    }
    plat_file_close(f);
    int ok = buf && !e && n <= KEPT_FILE_MAX && !parse_kept(buf, n, &until, NULL, &count) &&
             until == strtoll(argv[2], NULL, 10) && count == strtoull(argv[3], NULL, 10);
    free(buf);
    if (!ok) fprintf(stderr, "bridge parser: unexpected kept file: %s\n", argv[1]);
    return !ok;
}
