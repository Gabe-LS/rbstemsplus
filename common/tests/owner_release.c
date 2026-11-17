/* plat_owner_check built as released (no PLAT_TEST_OWNER): a file the running account owns, with
 * accept_self set, is still refused (common/tests/run.sh, review Q33).
 *   owner-release SCRATCH      exits 1 when it is accepted */
#include "../plat.h"

#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    char dir[1100], f[1200], bad[1200];
    if (argc != 2) return 2;
    snprintf(dir, sizeof dir, "%s/lib/x", argv[1]);
    plat_mkdirs(dir);
    snprintf(f, sizeof f, "%s/x.dylib", dir);
    FILE *o = fopen(f, "wb");
    if (!o) return 2;
    fputs("x", o);
    fclose(o);
    chmod(f, 0644);
    int r = plat_owner_check(f, 2, 1, bad, sizeof bad);
    printf("owner check of the account's own file, accept_self set: %d (%s)\n", r, bad);
    return r == PLAT_OWNER_FILE ? 0 : 1;
}
