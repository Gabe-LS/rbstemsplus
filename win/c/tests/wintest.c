/* win/c's test exe for Windows (x64 and arm64; app.md §7's rbsp-wintests.exe starts here): the
 * shared fixtures from today's Swift, decided by sig.c, payload.c and update_rules.c with the real
 * BCrypt (sig_bcrypt.c), and the updater's and the stub's pure rules with Windows' 16-bit wchar_t.
 * Built by run.sh, which gates its import table; run in the VM by the main session (the fixtures
 * file copied next to it):
 *
 *   winc-wintest.exe FIXTURES */
#include "check.h"

int checks, failures;

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s FIXTURES\n", argv[0]);
        return 2;
    }
    test_fixtures(argv[1]);
    test_update_rules();
    test_afterexit_win();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
