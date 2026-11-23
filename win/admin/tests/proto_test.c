/* admin_proto.c's rules: the command line both ways (protocol 2: --targets and --manifest), the
 * version pattern, the protocol mismatch, the pipe's name and lines (@target, @root-kept), the
 * worker's names and marker. */
#include "../../c/tests/check.h"

#include "../../c/admin_proto.h"

void test_proto(void);

static const char PIPE[] = "\\\\.\\pipe\\rbstemsplus-admin-0123456789abcdef0123456789abcdef";
static const char MAN[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char T19[] = "7.2.19:0123456789abcdef";

static int parse(const char *verb, const char *proto, const char *targets, const char *manifest, const char *source, const char *force, const char *pipe,
                 admin_args *a) {
    const char *argv[ADMIN_ARGC] = {"RB Stems Plus.exe", verb, "--proto", proto, "--targets", targets, "--manifest", manifest,
                                    "--source", source, "--force", force, "--pipe", pipe};
    char why[160];
    return admin_parse_args(ADMIN_ARGC, argv, a, why, sizeof why);
}

/* CommandLineToArgvW's rules (the CRT's are the same for these), for the round trip */
static int split_cmdline(const char *s, char out[14][1100]) {
    int n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (n == 14) return -1;
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

static int lines_n;
static char lines[8][ADMIN_LINE_MAX];
static void take(void *ctx, const char *line, size_t n) {
    (void)ctx;
    if (lines_n < 8) {
        memcpy(lines[lines_n], line, n + 1);
        lines_n++;
    }
}

/* admin_start's fake pipe (main.c's worker_win_connect and the pipe's say) */
typedef struct {
    int connect_ok, connects;
    char said[2048];
} start_io;
static int start_connect(void *ctx, const char *pipe) {
    start_io *io = ctx;
    (void)pipe;
    io->connects++;
    return io->connect_ok ? 0 : -1;
}
static void start_say(void *ctx, const char *line) {
    start_io *io = ctx;
    size_t n = strlen(io->said);
    snprintf(io->said + n, sizeof io->said - n, "%s\n", line);
}
static int start(int argc, const char *const *argv, int connect_ok, start_io *io, admin_args *a) {
    memset(io, 0, sizeof *io);
    io->connect_ok = connect_ok;
    return admin_start(argc, argv, "marker", start_connect, start_say, io, a);
}

/* re-review finding 9: the entry point's codes. Another protocol's recognised line is 12 whether
 * or not its pipe can be used, at any argument count; this protocol's refusals are 4 without a pipe */
static void test_start(void) {
    admin_args a;
    start_io io;
    const char *v1[12] = {"x", "uninstall", "--proto", "1", "--version", "7.2.19", "--source", "", "--force", "0", "--pipe", PIPE};
    CHECK(start(12, v1, 1, &io, &a) == ADMIN_EXIT_PROTOCOL && strstr(io.said, "marker") && strstr(io.said, "protocol 1"), "another protocol's line: 12, said on its pipe");
    CHECK(start(12, v1, 0, &io, &a) == ADMIN_EXIT_PROTOCOL, "its pipe unusable: still 12");
    const char *nopipe[10] = {"x", "uninstall", "--proto", "1", "--version", "7.2.19", "--source", "", "--force", "0"};
    CHECK(start(10, nopipe, 1, &io, &a) == ADMIN_EXIT_PROTOCOL && !io.connects, "no pipe named: 12, nothing said");
    const char *many[40];
    for (int i = 0; i < 40; i++) many[i] = "--x";
    many[0] = "x", many[1] = "install", many[2] = "--proto", many[3] = "3", many[38] = "--pipe", many[39] = PIPE;
    CHECK(start(40, many, 1, &io, &a) == ADMIN_EXIT_PROTOCOL && strstr(io.said, "protocol 3"), "a line of 40 arguments: recognised, 12");
    CHECK(start(40, many, 0, &io, &a) == ADMIN_EXIT_PROTOCOL, "and without its pipe");
    many[3] = "2";
    CHECK(start(40, many, 1, &io, &a) == ADMIN_EXIT_CHECK_FAILED, "this protocol's line that long: malformed, 4");
    const char *bad[ADMIN_ARGC] = {"x", "install", "--proto", "2", "--targets", "nope", "--manifest", MAN, "--source", "/s", "--force", "0", "--pipe", PIPE};
    CHECK(start(ADMIN_ARGC, bad, 0, &io, &a) == ADMIN_EXIT_CHECK_FAILED, "malformed, no pipe: 4");
    CHECK(start(ADMIN_ARGC, bad, 1, &io, &a) == ADMIN_EXIT_CHECK_FAILED && !io.connects, "malformed: 4, its pipe never taken");
    const char *good[ADMIN_ARGC] = {"x", "install", "--proto", "2", "--targets", T19, "--manifest", MAN, "--source", "/s", "--force", "0", "--pipe", PIPE};
    CHECK(start(ADMIN_ARGC, good, 0, &io, &a) == ADMIN_EXIT_CHECK_FAILED, "a good line without its pipe: 4, nothing done");
    CHECK(start(ADMIN_ARGC, good, 1, &io, &a) == ADMIN_EXIT_OK && a.verb == ADMIN_INSTALL && strstr(io.said, "marker"), "a good line: go on, the marker said");
    const char *holes[ADMIN_ARGC] = {"x", "uninstall", "--proto", "1", NULL, "7.2.19", "--source", NULL, "--force", "0", "--pipe", PIPE, "--a", "b"};
    CHECK(start(ADMIN_ARGC, holes, 1, &io, &a) == ADMIN_EXIT_PROTOCOL, "an argument that couldn't be read (NULL) doesn't hide another protocol");
}

void test_proto(void) {
    test_start();
    admin_args a;
    CHECK(admin_verb_of("install") == ADMIN_INSTALL && admin_verb_of("remove-root-folder") == ADMIN_REMOVE_ROOT_FOLDER && admin_verb_of("Install") < 0,
          "the verbs, exactly");
    CHECK(admin_version_ok("7.2.19") && admin_version_ok("0.0.0") && !admin_version_ok("7.2") && !admin_version_ok("7.2.19.0342")
              && !admin_version_ok("7.2.19 ") && !admin_version_ok("v7.2.19") && !admin_version_ok("7..19") && !admin_version_ok("1234567890.1.1")
              && !admin_version_ok(""),
          "--version: ^\\d+\\.\\d+\\.\\d+$");
    CHECK(admin_pipe_name_ok(PIPE) && !admin_pipe_name_ok("\\\\server\\pipe\\rbstemsplus-admin-0123456789abcdef0123456789abcdef")
              && !admin_pipe_name_ok("\\\\.\\pipe\\rbstemsplus-admin-0123456789ABCDEF0123456789abcdef")
              && !admin_pipe_name_ok("\\\\.\\pipe\\rbstemsplus-admin-0123456789abcdef0123456789abcde"),
          "the pipe: local, our prefix, 32 lowercase hex");
    unsigned char rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (unsigned char)(i * 17);
    char pn[ADMIN_PIPE_MAX];
    admin_pipe_name(rnd, pn);
    CHECK(admin_pipe_name_ok(pn), "a made pipe name passes: %s", pn);
    CHECK(admin_sha12_ok("0123456789ab") && !admin_sha12_ok("0123456789aB") && !admin_sha12_ok("0123456789abc") && !admin_sha12_ok("0123456789a"),
          "bin\\'s folder names: exactly 12 lowercase hex (J7)");
    CHECK(admin_plain_name_ok("rbstemsplus-bridge-win-x64.dll") && !admin_plain_name_ok("..") && !admin_plain_name_ok("a\\b") && !admin_plain_name_ok("a/b")
              && !admin_plain_name_ok("a:b") && !admin_plain_name_ok("a.") && !admin_plain_name_ok(""),
          "the payload's file names: one plain component");
    CHECK(admin_abs_path_ok("/Users/x/Library/a b") && !admin_abs_path_ok("relative") && !admin_abs_path_ok("/a/../b") && !admin_abs_path_ok("/a//b")
              && !admin_abs_path_ok("/a/") && !admin_abs_path_ok("/a\"b") && !admin_abs_path_ok("/a\nb"),
          "absolute paths (the Mac's form here; Windows': X:\\...)");

    CHECK(parse("install", "2", T19, MAN, "/src", "0", PIPE, &a) == ADMIN_EXIT_OK && a.verb == ADMIN_INSTALL && a.proto == 2 && a.nt == 1 && !a.all
              && !strcmp(a.t[0].version, "7.2.19") && !strcmp(a.t[0].exe16, "0123456789abcdef") && !strcmp(a.manifest, MAN) && !strcmp(a.source, "/src")
              && !a.force && !strcmp(a.pipe, PIPE),
          "a good command line");
    CHECK(parse("install", "2", "7.2.19.0342:0123456789abcdef", MAN, "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "a four-part version: refused");
    CHECK(parse("install", "2", T19, MAN, "", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "install needs --source");
    CHECK(parse("install", "2", T19, "-", "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "install needs a manifest");
    CHECK(parse("install", "2", T19, "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef", "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED,
          "a manifest: 64 lowercase hex");
    CHECK(parse("install", "2", "*", MAN, "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "install names its targets: * refused");
    CHECK(parse("uninstall", "2", "*", "-", "", "0", PIPE, &a) == ADMIN_EXIT_OK && a.all && !a.nt && !a.manifest[0], "uninstall: *, -");
    CHECK(parse("uninstall", "2", T19, "-", "", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "uninstall sweeps: a list refused");
    CHECK(parse("uninstall", "2", "*", MAN, "", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "uninstall takes no manifest");
    CHECK(parse("uninstall", "2", "*", "-", "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "uninstall takes no --source");
    CHECK(parse("restore", "2", "7.2.19:0123456789abcdef,7.2.20:fedcba9876543210", "-", "", "0", PIPE, &a) == ADMIN_EXIT_OK && a.nt == 2
              && !strcmp(a.t[1].version, "7.2.20"),
          "restore: its targets");
    CHECK(parse("remove-root-folder", "2", "*", "-", "", "0", PIPE, &a) == ADMIN_EXIT_OK, "remove-root-folder: *");
    CHECK(parse("install-worker", "2", "*", MAN, "/src", "0", PIPE, &a) == ADMIN_EXIT_OK, "install-worker: *, its manifest");
    CHECK(parse("install", "2", T19, MAN, "/src", "2", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "--force 0 or 1");
    CHECK(parse("install", "2", T19, MAN, "/src", "1", "\\\\host\\pipe\\x", &a) == ADMIN_EXIT_CHECK_FAILED, "a remote pipe: refused");
    CHECK(parse("install", "02", T19, MAN, "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED && parse("install", "-1", T19, MAN, "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED,
          "--proto: plain digits");
    CHECK(parse("install", "1", T19, MAN, "/src", "0", PIPE, &a) == ADMIN_EXIT_PROTOCOL && a.proto == 1, "another protocol: its own exit code");
    CHECK(parse("restore", "3", T19, "-", "", "0", PIPE, &a) == ADMIN_EXIT_PROTOCOL, "for every verb");
    CHECK(parse("install-worker", "7", "*", MAN, "/src", "0", PIPE, &a) == ADMIN_EXIT_OK && a.proto == 7, "install-worker: never refused by the protocol (I75)");
    CHECK(parse("install-worker", "1", "*", "-", "/src", "0", PIPE, &a) == ADMIN_EXIT_PROTOCOL,
          "install-worker: only this protocol's shape is taken; another protocol's line is 12 (review finding 9)");
    CHECK(parse("install-worker", "2", "*", "-", "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "install-worker of this protocol: well-formed or 4");
    {
        /* a protocol-1 app's line (--version, 12 arguments): refused as another protocol's, not as malformed (D13) */
        const char *v1[12] = {"x", "uninstall", "--proto", "1", "--version", "7.2.19", "--source", "", "--force", "0", "--pipe", PIPE};
        CHECK(admin_parse_args(12, v1, &a, NULL, 0) == ADMIN_EXIT_PROTOCOL, "a protocol-1 line: 12, update the app first");
        /* review finding 9: refused at the entry point whatever its shape, every verb, with its
         * pipe so the worker can say why (main.c parses before any argument count check) */
        CHECK(!strcmp(a.pipe, PIPE), "another protocol's line: its pipe kept, to say why");
        const char *w1[12] = {"x", "install-worker", "--proto", "1", "--version", "-", "--source", "/s", "--force", "0", "--pipe", PIPE};
        CHECK(admin_parse_args(12, w1, &a, NULL, 0) == ADMIN_EXIT_PROTOCOL && a.verb == ADMIN_INSTALL_WORKER && !strcmp(a.pipe, PIPE),
              "a protocol-1 install-worker line: 12 too (only protocol 2's install-worker shape is I75's)");
        const char *w3[16] = {"x", "install", "--proto", "3", "--targets", T19, "--manifest", MAN, "--source", "/s", "--force", "0", "--pipe", PIPE, "--new", "1"};
        CHECK(admin_parse_args(16, w3, &a, NULL, 0) == ADMIN_EXIT_PROTOCOL, "a newer protocol's longer line: 12");
        const char *bad[12] = {"x", "uninstall", "--proto", "1", "--version", "7.2.19", "--source", "", "--force", "0", "--pipe", "\\\\host\\pipe\\x"};
        CHECK(admin_parse_args(12, bad, &a, NULL, 0) == ADMIN_EXIT_PROTOCOL && !a.pipe[0], "never another host's pipe");
        const char *v2[12] = {"x", "uninstall", "--proto", "2", "--version", "7.2.19", "--source", "", "--force", "0", "--pipe", PIPE};
        CHECK(admin_parse_args(12, v2, &a, NULL, 0) == ADMIN_EXIT_CHECK_FAILED, "--version with this protocol: malformed");
        const char *v3[ADMIN_ARGC] = {"x", "install", "--proto", "2", "--version", "7.2.19", "--manifest", MAN, "--source", "/s", "--force", "0", "--pipe", PIPE};
        CHECK(admin_parse_args(ADMIN_ARGC, v3, &a, NULL, 0) == ADMIN_EXIT_CHECK_FAILED, "--version in place of --targets: unknown option");
    }
    {
        const char *argv[ADMIN_ARGC] = {"x", "install", "--proto", "2", "--proto", "2", "--targets", T19, "--source", "/s", "--force", "0", "--pipe", PIPE};
        CHECK(admin_parse_args(ADMIN_ARGC, argv, &a, NULL, 0) == ADMIN_EXIT_CHECK_FAILED, "an option twice");
        const char *argv2[ADMIN_ARGC] = {"x", "install", "--pipe", PIPE, "--force", "0", "--source", "/s", "--targets", T19, "--manifest", MAN, "--proto", "2"};
        CHECK(admin_parse_args(ADMIN_ARGC, argv2, &a, NULL, 0) == ADMIN_EXIT_OK, "options in any order");
        CHECK(admin_parse_args(ADMIN_ARGC - 1, argv2, &a, NULL, 0) == ADMIN_EXIT_CHECK_FAILED, "one missing");
    }

    /* --targets */
    CHECK(!admin_targets_parse("7.2.9:0123456789abcdef,7.2.19:0123456789abcdef,7.2.20:aaaaaaaaaaaaaaaa", &a) && a.nt == 3 && !a.all
              && !strcmp(a.t[0].version, "7.2.9"),
          "sorted by version, numerically (7.2.9 < 7.2.19 < 7.2.20)");
    CHECK(admin_targets_parse("7.2.19:0123456789abcdef,7.2.9:0123456789abcdef", &a) == -1 && !a.nt, "unsorted: refused");
    CHECK(admin_targets_parse("7.2.19:0123456789abcdef,7.2.19:fedcba9876543210", &a) == -1, "a version twice: refused");
    CHECK(admin_targets_parse("7.2.19:0123456789abcde", &a) == -1 && admin_targets_parse("7.2.19:0123456789abcdef0", &a) == -1
              && admin_targets_parse("7.2.19:0123456789ABCDEF", &a) == -1 && admin_targets_parse("7.2.19", &a) == -1,
          "16 lowercase hex exactly");
    CHECK(admin_targets_parse("", &a) == -1 && admin_targets_parse(",", &a) == -1 && admin_targets_parse("7.2.19:0123456789abcdef,", &a) == -1
              && admin_targets_parse("**", &a) == -1 && admin_targets_parse(NULL, &a) == -1,
          "empty items: refused");
    {
        char many[1024] = "";
        for (int i = 0; i <= ADMIN_TARGETS_MAX; i++) snprintf(many + strlen(many), sizeof many - strlen(many), "%s7.2.%d:0123456789abcdef", i ? "," : "", i);
        CHECK(admin_targets_parse(many, &a) == -1, "over ADMIN_TARGETS_MAX: refused");
        *strrchr(many, ',') = 0;
        CHECK(!admin_targets_parse(many, &a) && a.nt == ADMIN_TARGETS_MAX, "ADMIN_TARGETS_MAX: taken");
        char back[ADMIN_TARGETS_ARG_MAX];
        CHECK(admin_targets_format(a.t, a.nt, back, sizeof back) == (long)strlen(many) && !strcmp(back, many), "and written back the same");
        CHECK(admin_targets_format(a.t, a.nt, back, 20) == -1 && !back[0], "too small: -1, empty");
        admin_target sw[2] = {a.t[1], a.t[0]};
        CHECK(admin_targets_format(sw, 2, back, sizeof back) == -1 && admin_targets_format(a.t, 0, back, sizeof back) == -1, "unsorted, or none: -1");
    }
    CHECK(admin_version_cmp("7.2.9", "7.2.19") < 0 && admin_version_cmp("7.10.0", "7.9.99") > 0 && admin_version_cmp("07.2.1", "7.2.1") == 0, "the version order");

    /* the round trip through the quoting, spaces and a trailing backslash included */
    memset(&a, 0, sizeof a);
    a.verb = ADMIN_INSTALL;
    a.proto = ADMIN_PROTO;
    admin_targets_parse(T19, &a);
    memcpy(a.manifest, MAN, sizeof MAN);
    snprintf(a.source, sizeof a.source, "/Users/Jane Doe/");
    snprintf(a.pipe, sizeof a.pipe, "%s", PIPE);
    char params[2048];
    long pl = admin_build_params(&a, params, sizeof params);
    CHECK(pl < 0, "a trailing separator isn't a path the worker takes");
    /* a backslash before the closing quote (a Windows path's separator; on the Mac a plain byte) */
    snprintf(a.source, sizeof a.source, "/Users/Jane Doe/App Data\\x\\");
    pl = admin_build_params(&a, params, sizeof params);
    char back[14][1100];
    int n = pl > 0 ? split_cmdline(params, back) : -1;
    CHECK(n == ADMIN_ARGC - 1 && !strcmp(back[0], "install") && !strcmp(back[4], T19) && !strcmp(back[8], a.source) && !strcmp(back[12], PIPE),
          "quoted and read back: %s", params);
    if (n == ADMIN_ARGC - 1) {
        const char *argv[ADMIN_ARGC] = {"x"};
        for (int i = 0; i < ADMIN_ARGC - 1; i++) argv[i + 1] = back[i];
        admin_args b;
        CHECK(admin_parse_args(ADMIN_ARGC, argv, &b, NULL, 0) == ADMIN_EXIT_OK && !strcmp(b.source, a.source) && b.nt == 1 && !strcmp(b.manifest, MAN),
              "and parsed as given");
    }
    {
        admin_args u;
        memset(&u, 0, sizeof u);
        u.verb = ADMIN_UNINSTALL;
        u.proto = ADMIN_PROTO;
        u.all = 1;
        snprintf(u.pipe, sizeof u.pipe, "%s", PIPE);
        CHECK(admin_build_params(&u, params, sizeof params) > 0 && strstr(params, "\"--targets\" \"*\" \"--manifest\" \"-\""), "uninstall: * and -: %s", params);
        u.all = 0;
        CHECK(admin_build_params(&u, params, sizeof params) < 0, "uninstall with no targets: -1");
    }
    CHECK(admin_build_params(&a, params, 20) < 0 && params[0] == 0, "too small a buffer: -1, empty");

    /* the marker */
    const char bin[] = "MZ\0\0junk rbsp-admin worker: proto=x\n rbsp-admin worker: proto=3 minimum=1.5.0\n tail";
    int proto = 0;
    char min[ADMIN_VERSION_MAX];
    CHECK(admin_marker_find(bin, sizeof bin - 1, &proto, min) && proto == 3 && !strcmp(min, "1.5.0"), "the marker: the first well-formed one");
    CHECK(!admin_marker_find("rbsp-admin worker: proto=2 minimum=1.5.0", 40, &proto, min), "cut before its end: none");
    char sha12[13];
    CHECK(admin_self_in_bin("/R", "/R/bin/0123456789ab/RB Stems Plus.exe", sha12) && !strcmp(sha12, "0123456789ab"), "self in bin\\");
    CHECK(!admin_self_in_bin("/R", "/R/bin/0123456789ab/RB Stems Plus.exe.1.del", sha12) && !admin_self_in_bin("/R", "/app/admin/RB Stems Plus.exe", sha12)
              && !admin_self_in_bin("/R", "/R/bin/0123456789AB/RB Stems Plus.exe", sha12) && !admin_self_in_bin("/R", "/Rx/bin/0123456789ab/RB Stems Plus.exe", sha12),
          "not in bin\\ otherwise");

    /* the pipe's lines */
    char val[80];
    CHECK(admin_line_kind("@still-closing", 14, val, sizeof val) == ADMIN_LINE_STILL_CLOSING, "@still-closing");
    CHECK(admin_line_kind("@closed", 7, val, sizeof val) == ADMIN_LINE_CLOSED && admin_line_kind("@closed now", 11, val, sizeof val) == ADMIN_LINE_UNKNOWN,
          "@closed exactly");
    CHECK(admin_line_kind("@restart", 8, val, sizeof val) == ADMIN_LINE_RESTART && admin_line_kind("@restarts", 9, val, sizeof val) == ADMIN_LINE_UNKNOWN,
          "@restart exactly");
    CHECK(admin_line_kind("@bridge abc", 11, val, sizeof val) == ADMIN_LINE_BRIDGE && !strcmp(val, "abc"), "@bridge with its value");
    CHECK(admin_line_kind("@bridge", 7, val, sizeof val) == ADMIN_LINE_UNKNOWN && admin_line_kind("@floor ", 7, val, sizeof val) == ADMIN_LINE_UNKNOWN,
          "a record without its value: unknown");
    CHECK(admin_line_kind("rekordbox 7.2.19 checked", 24, val, sizeof val) == ADMIN_LINE_TEXT, "text");
    CHECK(admin_line_kind("@target 7.2.19 0 installed", 26, val, sizeof val) == ADMIN_LINE_TARGET && !strcmp(val, "7.2.19 0 installed"), "@target");
    CHECK(admin_line_kind("@root-kept bridge-remains", 25, val, sizeof val) == ADMIN_LINE_ROOT_KEPT && !strcmp(val, "bridge-remains"), "@root-kept");
    CHECK(admin_line_kind("@target", 7, val, sizeof val) == ADMIN_LINE_UNKNOWN && admin_line_kind("@targets x", 10, val, sizeof val) == ADMIN_LINE_UNKNOWN,
          "@target without a value, or another word: unknown");
    {
        char v[ADMIN_VERSION_MAX], w[ADMIN_TARGET_WHAT_MAX];
        int code = 99;
        CHECK(!admin_target_value("7.2.20 - not-started", v, &code, w) && !strcmp(v, "7.2.20") && code == -1 && !strcmp(w, "not-started"), "a not-started target");
        CHECK(!admin_target_value("7.2.20 16 put-back", v, &code, w) && code == 16 && !strcmp(w, "put-back"), "a code");
        CHECK(admin_target_value("7.2.20 6 installed extra", v, &code, w) == -1 && admin_target_value("7.2 0 installed", v, &code, w) == -1
                  && admin_target_value("7.2.20 06 installed", v, &code, w) == -1 && admin_target_value("7.2.20 0 done", v, &code, w) == -1
                  && admin_target_value("7.2.20 0", v, &code, w) == -1 && admin_target_value("7.2.20 99999 kept", v, &code, w) == -1,
              "anything else: -1");
    }
    admin_lines l;
    admin_lines_init(&l);
    lines_n = 0;
    admin_lines_feed(&l, "one\r\ntw", 7, 0, take, NULL);
    admin_lines_feed(&l, "o\n\x01three\xff\xc3\xa9", 11, 1, take, NULL);
    CHECK(lines_n == 3 && !strcmp(lines[0], "one") && !strcmp(lines[1], "two") && !strcmp(lines[2], "?three?\xc3\xa9"), "lines: split, CR dropped, bad bytes replaced (%d)",
          lines_n);
    char longl[3000];
    memset(longl, 'x', sizeof longl);
    longl[2000] = '\n';
    lines_n = 0;
    admin_lines_feed(&l, longl, 2001, 0, take, NULL);
    admin_lines_feed(&l, "next\n", 5, 0, take, NULL);
    CHECK(lines_n == 2 && strlen(lines[0]) == ADMIN_LINE_MAX - 1 && !strcmp(lines[1], "next"), "a long line cut, its rest dropped");
}
