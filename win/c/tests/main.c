/* win/c's Mac-native tests (app.md §7): the pure parts of the Windows-only C modules, built for
 * this Mac against the test crypto shim (sig_mac.c) and plat_mac.c, under run.sh's sanitizers.
 *
 *   winc-test FIXTURES SCRATCH PE ZIPS    FIXTURES: fixtures.txt (gen-fixtures.sh); SCRATCH: an
 *                                         empty folder; PE: the PE fixtures run.sh builds
 *                                         (tests/pe/); ZIPS: the zips gen-zips.py makes */
#include "check.h"

#include <sys/stat.h>

int checks, failures;

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s FIXTURES SCRATCH PE ZIPS\n", argv[0]);
        return 2;
    }
    test_fixtures(argv[1]);
    test_sig();
    test_payload();
    char sub[1024];
    snprintf(sub, sizeof sub, "%s/store", argv[2]);
    mkdir(sub, 0755);
    test_store(sub);
    snprintf(sub, sizeof sub, "%s/compat", argv[2]);
    mkdir(sub, 0755);
    test_compat(sub, argv[3]);
    test_update_rules();
    snprintf(sub, sizeof sub, "%s/unzip", argv[2]);
    mkdir(sub, 0755);
    test_unzip(sub, argv[4]);
    snprintf(sub, sizeof sub, "%s/update", argv[2]);
    mkdir(sub, 0755);
    test_update(sub, argv[3]);
    snprintf(sub, sizeof sub, "%s/sha_memo", argv[2]);
    mkdir(sub, 0755);
    test_sha_memo(sub);
    snprintf(sub, sizeof sub, "%s/native_facts", argv[2]);
    mkdir(sub, 0755);
    test_native_facts(sub);
    snprintf(sub, sizeof sub, "%s/native_steps", argv[2]);
    mkdir(sub, 0755);
    test_native_steps(sub);
    snprintf(sub, sizeof sub, "%s/models", argv[2]);
    mkdir(sub, 0755);
    test_models(sub);
    snprintf(sub, sizeof sub, "%s/afterexit", argv[2]);
    mkdir(sub, 0755);
    test_afterexit(sub);
    snprintf(sub, sizeof sub, "%s/native_location", argv[2]);
    mkdir(sub, 0755);
    test_native_location(sub);
    /* against Models.swift: none run only when Swift couldn't write (a sandbox), said on purpose */
    snprintf(sub, sizeof sub, "%s/model_fixtures", argv[2]);
    mkdir(sub, 0755);
    const char *no_write = getenv("RBSP_WINC_NO_SWIFT_WRITE");
    int model_cases = test_model_fixtures(argv[1], sub);
    CHECK(model_cases > 0 || (no_write && !strcmp(no_write, "1")), "models.c compared with Models.swift: no cases (run unsandboxed, or set RBSP_WINC_NO_SWIFT_WRITE=1)");
    /* in the summary, where a waived run can't be missed (review MT11) */
    if (model_cases == 0) printf("note: models.c compared with Models.swift: 0 cases (no Swift write)\n");
    snprintf(sub, sizeof sub, "%s/host_facts", argv[2]);
    mkdir(sub, 0755);
    test_host_facts(sub, argv[3]);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
