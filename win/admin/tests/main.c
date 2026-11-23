/* The worker's and the app's admin side's Mac-native tests (win/admin/tests/run.sh):
 *
 *   admin-test SCRATCH PE    SCRATCH: an empty folder; PE: win/c's PE fixtures (its run.sh builds them) */
#include "../../c/tests/check.h"

#include <sys/stat.h>

int checks, failures;

void test_proto(void);
void test_worker(const char *scratch, const char *pedir);
void test_client(const char *scratch);
void test_receipt(const char *scratch);

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s SCRATCH PE\n", argv[0]);
        return 2;
    }
    char sub[1024];
    test_proto();
    snprintf(sub, sizeof sub, "%s/receipt", argv[1]);
    mkdir(sub, 0755);
    test_receipt(sub);
    snprintf(sub, sizeof sub, "%s/worker", argv[1]);
    mkdir(sub, 0755);
    test_worker(sub, argv[2]);
    snprintf(sub, sizeof sub, "%s/client", argv[1]);
    mkdir(sub, 0755);
    test_client(sub);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
