/* location-win-test's command line (location_args.h) on the Mac: the forms it takes and the ones
 * it refuses, by path_rules.c's Windows rule. Built and run by common/tests/win.sh.
 *
 *   location-args-test       exits 1 when a check fails */
#include "location_args.h"
#include "check.h"

#include <stdarg.h>

#define G "\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f}\\"
#define GU "\\\\?\\Volume{0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F}\\"
#define F PATHS_CACHE_FOLDER

/* parse the arguments given (the program's name first), its result */
static int parse(loc_args *o, int n, ...) {
    char *a[8] = {"location-win-test"};
    va_list ap;
    va_start(ap, n);
    for (int i = 1; i < n && i < 8; i++) a[i] = va_arg(ap, char *);
    va_end(ap);
    return loc_args_parse(n, a, o);
}

int main(void) {
    loc_args o;
    /* the scratch run: a drive path only */
    CHECK(parse(&o, 2, "C:\\rbtest\\loc") == 0 && o.mode == LOC_SCRATCH && !strcmp(o.path, "C:\\rbtest\\loc"), "scratch: a drive path");
    CHECK(parse(&o, 2, G "loc") == -1, "scratch: a GUID path refused (the scratch run needs a drive)");
    CHECK(parse(&o, 2, "/tmp/loc") == -1 && parse(&o, 2, "rbtest\\loc") == -1, "scratch: a Mac or a relative path refused");
    CHECK(parse(&o, 1) == -1, "nothing: refused");
    /* --check: a location in Windows' form, through a drive, a mount folder or a GUID path */
    CHECK(parse(&o, 4, "--check", "S:\\" F, "0") == 0 && o.mode == LOC_CHECK && o.any == 0 && !strcmp(o.path, "S:\\" F), "check: a drive's own folder, any 0");
    CHECK(parse(&o, 4, "--check", "C:\\mnt\\x\\" F, "1") == 0 && o.any == 1, "check: through a mount folder, any 1");
    CHECK(parse(&o, 4, "--check", G F, "0") == 0, "check: a GUID path");
    CHECK(parse(&o, 4, "--check", GU F, "0") == 0, "check: a GUID path in upper case");
    CHECK(parse(&o, 4, "--check", "C:\\rbtest\\jx\\" F, "0") == 0, "check: a path with a junction on the way (refused by location_check, not here)");
    CHECK(parse(&o, 4, "--check", "S:\\other", "0") == -1, "check: a folder that isn't ours refused");
    CHECK(parse(&o, 4, "--check", "S:/" F, "0") == -1, "check: '/' separators refused");
    CHECK(parse(&o, 4, "--check", "\\\\?\\S:\\" F, "0") == -1, "check: \\\\?\\X:\\ refused");
    CHECK(parse(&o, 4, "--check", "\\\\host\\share\\" F, "0") == -1, "check: a UNC path refused");
    CHECK(parse(&o, 4, "--check", "\\\\?\\Volume{0f8c2d3a}\\" F, "0") == -1, "check: a short GUID refused");
    CHECK(parse(&o, 4, "--check", "S:\\" F, "2") == -1 && parse(&o, 4, "--check", "S:\\" F, "") == -1, "check: ANY other than 0 or 1 refused");
    CHECK(parse(&o, 4, "--check", "S:\\" F, "01") == -1 && parse(&o, 4, "--check", "S:\\" F, "1x") == -1
              && parse(&o, 4, "--gone", G, "01") == -1 && parse(&o, 4, "--gone", G, "1x") == -1,
          "ANY \"01\" or \"1x\" refused (check and gone)");
    CHECK(parse(&o, 3, "--check", "S:\\" F) == -1, "check: without ANY refused");
    /* --gone: a root alone */
    CHECK(parse(&o, 4, "--gone", G, "0") == 0 && o.mode == LOC_GONE && !o.any && !strcmp(o.path, G), "gone: a GUID root");
    CHECK(parse(&o, 4, "--gone", GU, "1") == 0 && o.any && !strcmp(o.path, GU), "gone: a GUID root in upper case, any 1");
    CHECK(parse(&o, 4, "--gone", "S:\\", "0") == 0 && !strcmp(o.path, "S:\\"), "gone: a drive root");
    CHECK(parse(&o, 4, "--gone", G F, "0") == -1 && parse(&o, 4, "--gone", "S:", "0") == -1, "gone: more than a root, or no root, refused");
    /* --make: a location */
    CHECK(parse(&o, 3, "--make", G F) == 0 && o.mode == LOC_MAKE && !strcmp(o.path, G F), "make: a GUID path");
    CHECK(parse(&o, 3, "--make", "S:\\x") == -1 && parse(&o, 4, "--make", "S:\\" F, "0") == -1, "make: not ours, or too many arguments, refused");
    CHECK(parse(&o, 4, "--other", "S:\\" F, "0") == -1, "an unknown option refused");
    printf("location_args: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
