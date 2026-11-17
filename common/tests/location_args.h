/* location-win-test's command line (common/tests/location_win.c, T3c), a pure parse over
 * path_rules.h so the Mac tests it too (common/tests/location_args.c, from common/tests/win.sh):
 *
 *   SCRATCH                  the scratch run: SCRATCH a drive path ("C:\rbtest\loc")
 *   --make DIR               DIR (a location's folder, Windows' form) made with our marker in it
 *   --check DIR ANY          location_check(DIR, ANY): its verdict on one line
 *   --gone ROOT ANY          location_gone(ROOT, ANY, ENOENT): its verdict on one line
 *
 * DIR is a location in Windows' form (paths_location_ok_win: a drive path, a mount folder's path
 * such as "C:\mnt\x\...", or a volume GUID path "\\?\Volume{GUID}\..." in either case); ROOT is
 * a root alone ("X:\" or "\\?\Volume{GUID}\"); ANY is 0 or 1. The volume cases of T3c case 3
 * (tests/win/suites/volumes.suite) drive the last three around a VHDX. */
#ifndef RBSP_LOCATION_ARGS_H
#define RBSP_LOCATION_ARGS_H

#include "../path_rules.h"

#include <string.h>

enum { LOC_SCRATCH, LOC_MAKE, LOC_CHECK, LOC_GONE };
typedef struct {
    int mode;           /* LOC_* */
    const char *path;   /* SCRATCH, DIR or ROOT */
    int any;            /* --check, --gone */
} loc_args;

/* 0 when a (n of them, a[0] the program) is one of the forms above, filled into o; -1 else */
static int loc_args_parse(int n, char **a, loc_args *o) {
    memset(o, 0, sizeof *o);
    if (n == 2 && a[1][0] != '-') {
        o->mode = LOC_SCRATCH;
        o->path = a[1];
        return paths_root_win(a[1]) == 3 ? 0 : -1;
    }
    if (n == 3 && !strcmp(a[1], "--make")) {
        o->mode = LOC_MAKE;
        o->path = a[2];
        return paths_location_ok_win(a[2]) ? 0 : -1;
    }
    if (n == 4 && (!strcmp(a[1], "--check") || !strcmp(a[1], "--gone"))) {
        if (strcmp(a[3], "0") && strcmp(a[3], "1")) return -1;
        o->any = a[3][0] == '1';
        o->path = a[2];
        if (a[1][2] == 'c') {
            o->mode = LOC_CHECK;
            return paths_location_ok_win(a[2]) ? 0 : -1;
        }
        o->mode = LOC_GONE;
        size_t r = paths_root_win(a[2]);
        return r && r == strlen(a[2]) ? 0 : -1;
    }
    return -1;
}

#endif
