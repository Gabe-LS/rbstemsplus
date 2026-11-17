/* admin_proto.c's rules: the command line both ways, the --version pattern, the protocol
 * mismatch, the pipe's name and lines, the worker's names and marker. */
#include "../../c/tests/check.h"

#include "../../c/admin_proto.h"

void test_proto(void);

static const char PIPE[] = "\\\\.\\pipe\\rbstemsplus-admin-0123456789abcdef0123456789abcdef";

static int parse(const char *verb, const char *proto, const char *version, const char *source, const char *force, const char *pipe, admin_args *a) {
    const char *argv[12] = {"RB Stems Plus.exe", verb, "--proto", proto, "--version", version, "--source", source, "--force", force, "--pipe", pipe};
    char why[160];
    return admin_parse_args(12, argv, a, why, sizeof why);
}

/* CommandLineToArgvW's rules (the CRT's are the same for these), for the round trip */
static int split_cmdline(const char *s, char out[12][1100]) {
    int n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (n == 12) return -1;
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

void test_proto(void) {
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

    CHECK(parse("install", "1", "7.2.19", "/src", "0", PIPE, &a) == ADMIN_EXIT_OK && a.verb == ADMIN_INSTALL && a.proto == 1 && !strcmp(a.version, "7.2.19")
              && !strcmp(a.source, "/src") && !a.force && !strcmp(a.pipe, PIPE),
          "a good command line");
    CHECK(parse("install", "1", "7.2.19.0342", "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "a four-part version: refused before any agreement check");
    CHECK(parse("install", "1", "7.2.19", "", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "install needs --source");
    CHECK(parse("uninstall", "1", "7.2.19", "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "uninstall takes no --source");
    CHECK(parse("uninstall", "1", "", "", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "uninstall needs --version");
    CHECK(parse("remove-root-folder", "1", "", "", "0", PIPE, &a) == ADMIN_EXIT_OK, "remove-root-folder: no version");
    CHECK(parse("install", "1", "7.2.19", "/src", "2", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "--force 0 or 1");
    CHECK(parse("install", "1", "7.2.19", "/src", "1", "\\\\host\\pipe\\x", &a) == ADMIN_EXIT_CHECK_FAILED, "a remote pipe: refused");
    CHECK(parse("install", "01", "7.2.19", "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED && parse("install", "-1", "7.2.19", "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED,
          "--proto: plain digits");
    CHECK(parse("install", "2", "7.2.19", "/src", "0", PIPE, &a) == ADMIN_EXIT_PROTOCOL, "another protocol: its own exit code");
    CHECK(parse("restore", "2", "7.2.19", "", "0", PIPE, &a) == ADMIN_EXIT_PROTOCOL, "for every verb");
    CHECK(parse("install-worker", "7", "", "/src", "0", PIPE, &a) == ADMIN_EXIT_OK && a.proto == 7, "install-worker: never refused by the protocol (I75)");
    CHECK(parse("install", "2", "7.2", "/src", "0", PIPE, &a) == ADMIN_EXIT_CHECK_FAILED, "a malformed line is malformed whatever the protocol");
    {
        const char *argv[12] = {"x", "install", "--proto", "1", "--proto", "1", "--source", "/s", "--force", "0", "--pipe", PIPE};
        CHECK(admin_parse_args(12, argv, &a, NULL, 0) == ADMIN_EXIT_CHECK_FAILED, "an option twice");
        const char *argv2[12] = {"x", "install", "--pipe", PIPE, "--force", "0", "--source", "/s", "--version", "7.2.19", "--proto", "1"};
        CHECK(admin_parse_args(12, argv2, &a, NULL, 0) == ADMIN_EXIT_OK, "options in any order");
        CHECK(admin_parse_args(11, argv2, &a, NULL, 0) == ADMIN_EXIT_CHECK_FAILED, "one missing");
    }

    /* the round trip through the quoting, spaces and a trailing backslash included */
    memset(&a, 0, sizeof a);
    a.verb = ADMIN_INSTALL;
    a.proto = ADMIN_PROTO;
    snprintf(a.version, sizeof a.version, "7.2.19");
    snprintf(a.source, sizeof a.source, "/Users/Jane Doe/");
    snprintf(a.pipe, sizeof a.pipe, "%s", PIPE);
    char params[2048];
    long pl = admin_build_params(&a, params, sizeof params);
    CHECK(pl < 0, "a trailing separator isn't a path the worker takes");
    /* a backslash before the closing quote (a Windows path's separator; on the Mac a plain byte) */
    snprintf(a.source, sizeof a.source, "/Users/Jane Doe/App Data\\x\\");
    pl = admin_build_params(&a, params, sizeof params);
    char back[12][1100];
    int n = pl > 0 ? split_cmdline(params, back) : -1;
    CHECK(n == 11 && !strcmp(back[0], "install") && !strcmp(back[6], a.source) && !strcmp(back[10], PIPE), "quoted and read back: %s", params);
    if (n == 11) {
        const char *argv[12] = {"x"};
        for (int i = 0; i < 11; i++) argv[i + 1] = back[i];
        admin_args b;
        CHECK(admin_parse_args(12, argv, &b, NULL, 0) == ADMIN_EXIT_OK && !strcmp(b.source, a.source), "and parsed as given");
    }
    CHECK(admin_build_params(&a, params, 20) < 0 && params[0] == 0, "too small a buffer: -1, empty");

    /* the marker */
    const char bin[] = "MZ\0\0junk rbsp-admin worker: proto=x\n rbsp-admin worker: proto=3 minimum=1.5.0\n tail";
    int proto = 0;
    char min[ADMIN_VERSION_MAX];
    CHECK(admin_marker_find(bin, sizeof bin - 1, &proto, min) && proto == 3 && !strcmp(min, "1.5.0"), "the marker: the first well-formed one");
    CHECK(!admin_marker_find("rbsp-admin worker: proto=1 minimum=1.5.0", 40, &proto, min), "cut before its end: none");
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
