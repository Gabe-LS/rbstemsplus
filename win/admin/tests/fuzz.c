/* The admin side's fuzz driver (win/admin/tests/run.sh, ASan+UBSan): random and mutated input to
 * the parsers both programs run on what the other wrote (the command line, the pipe's lines, the
 * marker) and the worker's pure rules. A parse never reads out of bounds, never accepts what it
 * would build differently, and a parsed command line builds and parses back the same.
 *
 *   admin-fuzz ITERATIONS SEED    SEED 0: from the clock (printed) */
#include "../../c/admin_proto.h"
#include "../worker.h"
#include "../../c/admin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t rng;
static uint32_t next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static const char *const seeds[] = {
    "install", "uninstall", "restore", "install-worker", "remove-root-folder", "--proto", "--targets", "--manifest", "--source", "--force", "--pipe",
    "--version", "1", "2", "0", "7.2.19", "7.2", "*", "-", "7.2.19:0123456789abcdef", "7.2.9:0123456789abcdef,7.2.19:fedcba9876543210",
    "7.2.19:0123456789abcdef,7.2.9:0123456789abcdef", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "/src", "/a b/c",
    "\\\\.\\pipe\\rbstemsplus-admin-0123456789abcdef0123456789abcdef", "", "\"", "\\", ",", ":",
};

static void mutate(char *s, size_t cap) {
    size_t n = strlen(s);
    int k = (int)(next() % 4);
    for (int i = 0; i < k; i++) {
        uint32_t op = next() % 3;
        if (op == 0 && n) s[next() % n] = (char)(next() % 256 ? next() % 256 : 'x');
        else if (op == 1 && n + 1 < cap) {
            size_t at = n ? next() % n : 0;
            memmove(s + at + 1, s + at, n - at + 1);
            s[at] = (char)(32 + next() % 95);
            n++;
        } else if (op == 2 && n) {
            size_t at = next() % n;
            memmove(s + at, s + at + 1, n - at);
            n--;
        }
    }
    s[strnlen(s, cap - 1)] = 0;
}

/* CommandLineToArgvW's rules, to read admin_quote_argv's line back */
static int split(const char *s, char out[4][256]) {
    int n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (n == 4) return -1;
        char *o = out[n++];
        size_t k = 0;
        int inq = 0;
        for (;;) {
            size_t bs = 0;
            while (*s == '\\') bs++, s++;
            if (*s == '"') {
                for (size_t i = 0; i < bs / 2; i++) o[k++] = '\\';
                if (bs % 2) o[k++] = '"';
                else inq = !inq;
                s++;
                continue;
            }
            for (size_t i = 0; i < bs; i++) o[k++] = '\\';
            if (!*s || (!inq && *s == ' ')) break;
            o[k++] = *s++;
        }
        o[k] = 0;
    }
    return n;
}

static size_t got_lines;
static void take(void *ctx, const char *line, size_t n) {
    (void)ctx;
    if (strlen(line) != n || n >= ADMIN_LINE_MAX) abort();
    char v[128];
    admin_line_kind(line, n, v, sizeof v);
    got_lines++;
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 20000;
    rng = argc > 2 ? strtoull(argv[2], NULL, 10) : 0;
    if (!rng) rng = (uint64_t)time(NULL) * 2654435761u + 1;
    printf("fuzz: seed %llu\n", (unsigned long long)rng);
    long parsed = 0, round = 0, markers = 0;
    for (long it = 0; it < iters; it++) {
        /* the command line */
        static char vals[ADMIN_ARGC][ADMIN_PATH_MAX + 8];
        const char *av[ADMIN_ARGC];
        int ac = ADMIN_ARGC;
        snprintf(vals[0], sizeof vals[0], "x");
        int vb = (int)(next() % 5);
        snprintf(vals[1], sizeof vals[1], "%s", seeds[vb]);
        /* this verb's well-formed values (protocol 2), each replaced now and then */
        int names = vb == ADMIN_INSTALL || vb == ADMIN_RESTORE, sourced = vb == ADMIN_INSTALL || vb == ADMIN_INSTALL_WORKER;
        static const char *const opts[6] = {"--proto", "--targets", "--manifest", "--source", "--force", "--pipe"};
        const char *good[6] = {next() % 4 ? "2" : "1", names ? (next() % 2 ? "7.2.19:0123456789abcdef" : "7.2.9:0123456789abcdef,7.2.19:fedcba9876543210") : "*",
                               sourced ? "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" : "-", sourced ? "/src/a b" : "", "0",
                               "\\\\.\\pipe\\rbstemsplus-admin-0123456789abcdef0123456789abcdef"};
        for (int i = 0; i < 6; i++) {
            snprintf(vals[2 + 2 * i], sizeof vals[0], "%s", next() % 16 ? opts[i] : seeds[next() % (sizeof seeds / sizeof seeds[0])]);
            snprintf(vals[3 + 2 * i], sizeof vals[0], "%s", next() % 8 ? good[i] : seeds[next() % (sizeof seeds / sizeof seeds[0])]);
        }
        for (int i = 0; i < ADMIN_ARGC; i++) {
            if (next() % 24 == 0) mutate(vals[i], 200);
            av[i] = vals[i];
        }
        if (next() % 10 == 0) ac = (int)(next() % (ADMIN_ARGC + 1));
        admin_args a;
        char why[160];
        int r = admin_parse_args(ac, av, &a, why, sizeof why);
        if (r == ADMIN_EXIT_OK || r == ADMIN_EXIT_PROTOCOL) parsed++;
        if (r == ADMIN_EXIT_OK) {
            /* a parsed line builds back, and that parses as the same */
            char params[4096];
            long pl = admin_build_params(&a, params, sizeof params);
            if (pl < 0) {
                fprintf(stderr, "a parsed line doesn't build back\n");
                return 1;
            }
            char tg[ADMIN_TARGETS_ARG_MAX];
            admin_args b;
            if (!a.all && (admin_targets_format(a.t, a.nt, tg, sizeof tg) < 0 || admin_targets_parse(tg, &b) || b.nt != a.nt || memcmp(b.t, a.t, sizeof a.t))) {
                fprintf(stderr, "parsed targets don't read back the same\n");
                return 1;
            }
            round++;
        }
        if (r != ADMIN_EXIT_OK && r != ADMIN_EXIT_PROTOCOL && r != ADMIN_EXIT_CHECK_FAILED) abort();

        /* the pipe's bytes, in random pieces */
        admin_lines l;
        admin_lines_init(&l);
        unsigned char buf[3000];
        size_t bn = next() % sizeof buf;
        for (size_t i = 0; i < bn; i++) buf[i] = (unsigned char)(next() % 7 == 0 ? '\n' : next() % 5 == 0 ? '@' : next() % 256);
        for (size_t at = 0; at < bn;) {
            size_t k = 1 + next() % 700;
            if (k > bn - at) k = bn - at;
            admin_lines_feed(&l, buf + at, k, at + k == bn, take, NULL);
            at += k;
        }

        /* the marker */
        char mk[256];
        snprintf(mk, sizeof mk, "junk" ADMIN_MARKER_PREFIX "proto=%u minimum=%u.%u.%u\nmore", next() % 20000, next() % 100, next() % 100, next() % 100);
        size_t mn = strlen(mk);
        if (next() % 2) mutate(mk, sizeof mk), mn = strlen(mk);
        int proto;
        char min[ADMIN_VERSION_MAX];
        if (admin_marker_find(mk, next() % (mn + 1), &proto, min)) {
            markers++;
            if (!admin_version_ok(min) || proto < 0 || proto > 9999) abort();
        }

        /* the worker's pure rules */
        int c = admin_classify((int)(next() % 2), (int)(next() % 2), (int)(next() % 2), (int)(next() % 2), (int)(next() % 3) - 1, (int)(next() % 2),
                               (int)(next() % 2));
        if (c < ADMIN_LIVE_PIONEER || c > ADMIN_LIVE_NOTHING) abort();
        admin_ace aces[4];
        for (int i = 0; i < 4; i++) aces[i] = (admin_ace){next() % 3, next() % 4, next() % 2 ? ADMIN_ROOT_FULL : next(), (int)(next() % 4)};
        (void)admin_root_acl_exact((int)(next() % 4), (int)(next() % 2), (int)(next() % 2), aces, next() % 5);

        /* the app's side: the helper's command line reads back as given; the choice stays in range */
        char args[3][64];
        const char *ap[3];
        static const char alphabet[] = "ab \\\"\t/:x";
        for (int i = 0; i < 3; i++) {
            size_t k = next() % 20;
            for (size_t j = 0; j < k; j++) args[i][j] = alphabet[next() % (sizeof alphabet - 1)];
            args[i][k] = 0;
            ap[i] = args[i];
        }
        char cl[512], back[4][256];
        if (admin_quote_argv(ap, 3, cl, sizeof cl) < 0 || split(cl, back) != 3) abort();
        for (int i = 0; i < 3; i++)
            if (strcmp(back[i], args[i])) {
                fprintf(stderr, "quoted [%s] read back as [%s]\n", args[i], back[i]);
                return 1;
            }
        admin_bin_worker bw[4];
        int nb = (int)(next() % 5), pick;
        for (int i = 0; i < nb; i++) {
            memset(&bw[i], 0, sizeof bw[i]);
            snprintf(bw[i].sha256, 65, "%064x", next() % 3);
            bw[i].proto = (int)(next() % 3) - 1;
            snprintf(bw[i].minimum, sizeof bw[i].minimum, "1.%u.0", next() % 3);
            bw[i].mtime = next() % 3;
        }
        char own[65];
        snprintf(own, sizeof own, "%064x", next() % 4);
        int how = admin_choose(bw, nb, next() % 2 ? own : "", 1, (int)(next() % ADMIN_VERB_COUNT), &pick);
        if (how < ADMIN_PICK_APP_COPY || how > ADMIN_PICK_UPDATE_FIRST || pick >= nb || (pick < 0 && (how == ADMIN_PICK_OWN || how == ADMIN_PICK_MATCHING)))
            abort();
    }
    printf("fuzz: %ld iterations, %ld command lines parsed (%ld built back), %zu pipe lines, %ld markers\n", iters, parsed, round, got_lines, markers);
    printf("fuzz: no failures\n");
    return 0;
}
