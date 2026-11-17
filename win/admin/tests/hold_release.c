/* admin.c built as released (no ADMIN_TEST_HOOKS): a bin\ worker with no hold callback is
 * refused, "a check failed" (4), and never elevated (review NT1). win/admin/tests/run.sh runs it.
 *   hold-release SCRATCH      exits 1 when the worker is elevated */
#include "../../c/admin.h"
#include "plat.h"

#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int elevations;

static int elevate(void *ctx, const char *exe, const char *params, const char *pipe, void (*line)(void *, const char *, size_t), void *lctx,
                   void (*started)(void *), void *sctx, int *code) {
    (void)ctx, (void)exe, (void)params, (void)pipe, (void)line, (void)lctx;
    elevations++;
    if (started) started(sctx);
    *code = 0;
    return 0;
}

static void say(void *ctx, const char *s, size_t n) {
    (void)ctx;
    printf("  %.*s\n", (int)n, s);
}

static void rnd(void *ctx, unsigned char out[16]) {
    (void)ctx;
    memset(out, 1, 16);
}

int main(int argc, char **argv) {
    static const char w[] = "MZ one rbsp-admin worker: proto=1 minimum=1.5.0\n";
    char scratch[1024], root[1100], dir[1200], exe[1300], sha[65];
    unsigned char h[32];
    if (argc != 2 || !realpath(argv[1], scratch)) return 2;
    CC_SHA256(w, (CC_LONG)strlen(w), h);
    for (int i = 0; i < 32; i++) snprintf(sha + 2 * i, 3, "%02x", h[i]);
    snprintf(root, sizeof root, "%s/RB Stems Plus", scratch);
    snprintf(dir, sizeof dir, "%s/bin/%.12s", root, sha);
    snprintf(exe, sizeof exe, "%s/RB Stems Plus.exe", dir);
    plat_mkdirs(dir);
    FILE *f = fopen(exe, "wb");
    if (!f) return 2;
    fputs(w, f);
    fclose(f);
    admin_client c;
    memset(&c, 0, sizeof c);
    c.root = root;
    c.app_copy = "app copy";
    c.source = "";
    c.own_sha256 = sha;
    c.elevate = elevate;
    c.random = rnd;
    c.log = say;
    admin_result res;
    int x = admin_client_run(&c, ADMIN_UNINSTALL, "7.2.19", 0, &res);
    printf("a bin\\ worker with no hold, as released: %d, %d elevations\n", x, elevations);
    return x == ADMIN_EXIT_CHECK_FAILED && !elevations ? 0 : 1;
}
