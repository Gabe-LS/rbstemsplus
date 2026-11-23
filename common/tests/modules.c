/* The shared modules' unit tests (bridge.md §0a T1a): cfg_ini.c and marker.c against the fixtures the
 * bridge and the app share (tests/config-fixtures.txt, tests/marker-fixtures.txt: the oracle),
 * path_rules.c's rule, and entries.c's layout and size rule, on a scratch folder.
 *
 *   modules-test SCRATCH FIXTURES_DIR     exits 1 when a check fails */
#include "../cfg_ini.h"
#include "../entries.h"
#include "../marker.h"
#include "../path_rules.h"
#include "check.h"

#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[1024];
#define ID "0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F"

/* the fixture file's lines: name, TAB, expected, TAB, content (none: an empty file) */
typedef void (*fixture_fn)(const char *name, const char *expected, const char *content, long n);
static int each_fixture(const char *file, fixture_fn fn) {
    FILE *f = fopen(file, "rb");
    if (!f) { CHECK(0, "can't read %s", file); return 0; }
    static char line[1 << 17], content[1 << 17];
    int count = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (l && line[l - 1] == '\n') line[--l] = 0;
        if (!l || line[0] == '#') continue;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1) { CHECK(0, "a fixture line without its expected column: %.40s", line); continue; }
        *t1 = 0;
        if (t2) *t2 = 0;
        long n = fixture_decode(t2 ? t2 + 1 : "", content, sizeof content);
        CHECK(n >= 0, "fixture %s: its content doesn't decode", line);
        if (n >= 0) fn(line, t1 + 1, content, n);
        count++;
    }
    fclose(f);
    return count;
}

/* the value of key=... in an expected column ("" when absent), %20 and the escapes decoded */
static void expected(const char *col, const char *key, char *out, size_t cap) {
    char k[32];
    snprintf(k, sizeof k, "%s=", key);
    out[0] = 0;
    for (const char *p = col; p && *p; p = strchr(p, ' '), p = p ? p + 1 : NULL)
        if (!strncmp(p, k, strlen(k))) {
            char raw[4096], dec[4096];
            const char *v = p + strlen(k), *e = strchr(v, ' ');
            size_t l = e ? (size_t)(e - v) : strlen(v), o = 0;
            snprintf(raw, sizeof raw, "%.*s", (int)l, v);
            long n = fixture_decode(raw, dec, sizeof dec - 1);
            if (n < 0) return;
            dec[n] = 0;
            for (const char *c = dec; *c && o + 1 < cap; c++) {
                if (!strncmp(c, "%20", 3)) { out[o++] = ' '; c += 2; }
                else out[o++] = *c;
            }
            out[o] = 0;
            return;
        }
}

/* ---------------------------------------------------------------- config.ini */

static void config_fixture(const char *name, const char *col, const char *content, long n) {
    /* the bytes as a file, read as the bridge reads config.ini (whole, at most CFG_MAX), then parsed */
    char path[1100], v[1024];
    snprintf(path, sizeof path, "%s/config.ini", root);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(content, 1, (size_t)n, f) != (size_t)n) { CHECK(0, "fixture %s: can't write it", name); if (f) fclose(f); return; }
    fclose(f);
    if (!strcmp(name, "none")) unlink(path);
    cfg_config c = {.enabled = 1, .rekordbox_model = 0, .near_hits = 0, .max_bytes = 20LL << 30, .max_days = 60};
    char *b = NULL;
    size_t len = 0;
    int e = plat_read_file(path, CFG_MAX, &b, &len);
    int unreadable = e == ENOENT ? 0 : e ? 1 : cfg_parse(b, len, &c, NULL) != 0;
    free(b);
    expected(col, "location", v, sizeof v);
    if (!strcmp(v, "unreadable")) { CHECK(unreadable, "fixture %s: unreadable", name); return; }
    CHECK(!unreadable, "fixture %s: read", name);
    const char *loc = c.location == 0 ? "thismac" : c.location == 1 ? "chosen" : "malformed";
    CHECK(!strcmp(v, loc), "fixture %s: location %s, expected %s", name, loc, v);
    expected(col, "enabled", v, sizeof v);
    CHECK(c.enabled == atoi(v), "fixture %s: enabled=%d, expected %s", name, c.enabled, v);
    expected(col, "max_gb", v, sizeof v);
    CHECK(c.max_bytes == (long long)(atof(v) * (1LL << 30)), "fixture %s: max_bytes %lld, expected %s GB", name, c.max_bytes, v);
    expected(col, "max_days", v, sizeof v);
    CHECK(fabs(c.max_days - atof(v)) < 1e-9, "fixture %s: max_days %g, expected %s", name, c.max_days, v);
    if (c.location == 1) {
        expected(col, "dir", v, sizeof v);
        CHECK(!strcmp(c.dir, v) && cfg_uuid_ok(c.id), "fixture %s: dir %s, expected %s", name, c.dir, v);
    }
}

static int logged;
static void count_log(const char *fmt, ...) { (void)fmt; logged++; }

static void t_config(const char *fixtures) {
    char file[1100];
#if defined(PATHS_WIN_FORM)
    /* cfg_ini.c with Windows' path rule (the Windows builds'): Windows' own fixtures */
    snprintf(file, sizeof file, "%s/config-fixtures-win.txt", fixtures);
#else
    snprintf(file, sizeof file, "%s/config-fixtures.txt", fixtures);
#endif
    int n = each_fixture(file, config_fixture);
    CHECK(n > 0, "config fixtures: %d ran", n);
    printf("config fixtures (%s): %d\n", file + strlen(fixtures) + 1, n);
    /* the notes go to the log given, never elsewhere */
    cfg_config c = {.enabled = 1, .max_bytes = 20LL << 30, .max_days = 60};
    const char *t = "max_gb=zero\nenabled=maybe\nmax_days=99999\n";
    CHECK(cfg_parse(t, strlen(t), &c, count_log) == 0 && logged == 3, "three ignored values, three log lines (%d)", logged);
    CHECK(c.max_bytes == 20LL << 30 && c.enabled == 1 && c.max_days == 60, "ignored values leave the defaults");
    CHECK(cfg_parse(t, strlen(t), &c, NULL) == 0, "no log: silent");
    CHECK(cfg_yes_no("On") == 1 && cfg_yes_no("FALSE") == 0 && cfg_yes_no("2") == -1, "yes_no");
}

/* ---------------------------------------------------------------- the marker */

static void marker_fixture(const char *name, const char *col, const char *content, long n) {
    char dir[1100], path[1200];
    snprintf(dir, sizeof dir, "%s/mk-%s", root, name);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%s", dir, MARKER_NAME);
    FILE *f = fopen(path, "wb");
    if (!f) { CHECK(0, "marker fixture %s: can't write it", name); return; }
    fwrite(content, 1, (size_t)n, f);
    fclose(f);
    plat_dir d;
    CHECK(plat_dir_open(dir, &d) == 0, "marker fixture %s: its folder", name);
    const char *why = marker_check_at(d, ID);
    plat_dir_close(d);
    int valid = !strncmp(col, "valid", 5);
    CHECK(valid ? why == NULL : why && !strcmp(why, "marker"), "marker fixture %s: %s, got %s", name, col, why ? why : "ours");
    CHECK(n > MARKER_MAX || marker_ok(content, (size_t)n, ID) == valid, "marker fixture %s: marker_ok agrees", name);
}

static void t_marker(const char *fixtures) {
    char file[1100];
    snprintf(file, sizeof file, "%s/marker-fixtures.txt", fixtures);
    int n = each_fixture(file, marker_fixture);
    CHECK(n > 0, "marker fixtures: %d ran", n);
    printf("marker fixtures: %d\n", n);
    /* the verdicts for what opening or reading it can fail with */
    CHECK(!strcmp(marker_why(EACCES), "no-permission") && !strcmp(marker_why(EPERM), "no-permission"), "no permission");
    CHECK(!strcmp(marker_why(EIO), "io-error") && !strcmp(marker_why(ENXIO), "io-error") && !strcmp(marker_why(ENODEV), "io-error"), "io");
    CHECK(!*marker_why(EMFILE) && !*marker_why(ENFILE) && !*marker_why(ENOMEM) && !*marker_why(EINTR) && !*marker_why(EAGAIN),
          "no verdict for a shortage");
    CHECK(!*marker_why(EBUSY), "no verdict for a marker held open elsewhere (EBUSY, I83)");
    CHECK(!strcmp(marker_why(ENOENT), "marker") && !strcmp(marker_why(PLAT_EFTYPE), "marker") && !strcmp(marker_why(ELOOP), "marker"),
          "missing, not a file, a link: marker");
    /* on disk: missing, a folder, a link, unreadable */
    char dir[1100], p[1200], q[1200];
    plat_dir d;
    snprintf(dir, sizeof dir, "%s/mk-cases", root);
    mkdir(dir, 0755);
    plat_dir_open(dir, &d);
    const char *w = marker_check_at(d, ID);
    CHECK(w && !strcmp(w, "marker"), "no marker: marker");
    snprintf(p, sizeof p, "%s/%s", dir, MARKER_NAME);
    mkdir(p, 0755);
    w = marker_check_at(d, ID);
    CHECK(w && !strcmp(w, "marker"), "a folder named like it: marker");
    rmdir(p);
    snprintf(q, sizeof q, "%s/mk-written/%s", root, MARKER_NAME);
    symlink(q, p);
    w = marker_check_at(d, ID);
    CHECK(w && !strcmp(w, "marker"), "a link to a valid marker: marker (never followed)");
    unlink(p);
    FILE *f = fopen(p, "wb");
    if (f) { fputs("id=" ID "\nformat=" MARKER_FORMAT "\n", f); fclose(f); }
    CHECK(marker_check_at(d, ID) == NULL, "ours");
    w = marker_check_at(d, "1F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F");
    CHECK(w && !strcmp(w, "marker"), "another location's id: marker");
    chmod(p, 0);
    w = marker_check_at(d, ID);
    CHECK(geteuid() == 0 || (w && !strcmp(w, "no-permission")), "unreadable: no-permission (%s)", w ? w : "ours");
    chmod(p, 0644);
    plat_dir_close(d);
}

/* ---------------------------------------------------------------- paths */

static void t_paths(void) {
    CHECK(paths_location_ok("/Volumes/GLS/RB Stems Plus Cache.noindex"), "a drive's folder");
    CHECK(!paths_location_ok("/RB Stems Plus Cache.noindex") && !paths_location_ok("RB Stems Plus Cache.noindex"),
          "the root's or a relative one");
    CHECK(!paths_location_ok("/Volumes/GLS/../RB Stems Plus Cache.noindex") && !paths_location_ok("/Volumes//RB Stems Plus Cache.noindex"),
          "dot-dot, empty component");
    CHECK(!paths_location_ok("/Volumes/G\"LS/RB Stems Plus Cache.noindex") && !paths_location_ok("/Volumes/G\x7fLS/RB Stems Plus Cache.noindex"),
          "a quote, DEL");
    char p[800];
    int k = snprintf(p, sizeof p, "/Volumes/");
    while (k < PATHS_LOCATION_MAX - (int)strlen("/" PATHS_CACHE_FOLDER)) p[k++] = 'a';
    snprintf(p + k, sizeof p - (size_t)k, "/" PATHS_CACHE_FOLDER);
    CHECK(strlen(p) == PATHS_LOCATION_MAX && paths_location_ok(p), "700 bytes");
    memmove(p + 10, p + 9, strlen(p + 9) + 1);
    CHECK(!paths_location_ok(p), "701 bytes");

    /* a track's path (the helper): the Mac's forms, then Windows' (pure strings, tested here too) */
    CHECK(paths_local_file("/Users/a/x.mp3") && !paths_local_file("x.mp3") && !paths_local_file("") && !paths_local_file("https://a/b"),
          "a local file: absolute");
    CHECK(paths_same("/a/b.mp3", "/a/b.mp3") && !paths_same("/a/b.mp3", "/A/b.mp3") && !paths_same("/a/b.mp3", "/a/b.mp3 "),
          "the same track: the same bytes");
    CHECK(paths_other_drive("/Volumes/GLS/m/x.mp3") == strlen("/Volumes/GLS") && paths_other_drive("/Volumes/GLS") == strlen("/Volumes/GLS")
          && paths_other_drive("/Volumes/") == 9 && !paths_other_drive("/Users/a/x.mp3") && !paths_other_drive("/VolumesX/a"),
          "another drive: under /Volumes/, its root");
    CHECK(paths_local_file_win("C:/m/x.mp3") && paths_local_file_win("c:\\m\\x.mp3") && paths_local_file_win("//srv/share/x.mp3")
          && paths_local_file_win("\\\\srv\\share\\x.mp3"), "Windows: a drive letter with / or \\, UNC");
    CHECK(!paths_local_file_win("C:x.mp3") && !paths_local_file_win("CD:/x") && !paths_local_file_win("/m/x.mp3")
          && !paths_local_file_win("x.mp3") && !paths_local_file_win("") && !paths_local_file_win("1:/x"), "Windows: everything else isn't");
    CHECK(paths_same_win("c:/m/x.mp3", "C:\\m\\x.mp3") && paths_same_win("//s/x", "\\\\s\\x") && paths_same_win("C:/a", "C:/a"),
          "Windows: the drive letter's case and / against \\ folded");
    CHECK(!paths_same_win("C:/M/x.mp3", "C:/m/x.mp3") && !paths_same_win("C:/a", "D:/a") && !paths_same_win("C:/a", "C:/ab")
          && !paths_same_win("ca:/x", "Ca:/x"), "Windows: nothing else folded");
#if defined(PATHS_WIN_FORM)
    /* Windows' another drive (T13): the system drive C: */
    CHECK(!paths_other_drive_win("C:/Users/a/x.mp3", 'C') && !paths_other_drive_win("c:\\x.mp3", 'C') && !paths_other_drive_win("C:", 'c')
              && !paths_other_drive_win("\\\\?\\C:\\x.mp3", 'C'),
          "Windows: the system drive, either case and separator, behind \\\\?\\ too, isn't another drive");
    CHECK(paths_other_drive_win("D:/Music/x.mp3", 'C') == 3 && paths_other_drive_win("e:\\x", 'C') == 3 && paths_other_drive_win("D:", 'C') == 2
              && paths_other_drive_win("\\\\?\\D:\\x", 'C') == 7 && paths_other_drive_win("C:/x", 0) == 3,
          "Windows: another letter is, its root \"D:\\\\\" (and every drive when the system's isn't known)");
    CHECK(paths_other_drive_win("//nas/share/m/x.mp3", 'C') == strlen("//nas/share") && paths_other_drive_win("\\\\nas\\share", 'C') == strlen("\\\\nas\\share")
              && paths_other_drive_win("\\\\?\\UNC\\nas\\share\\x", 'C') == strlen("\\\\?\\UNC\\nas\\share")
              && paths_other_drive_win("//nas", 'C') == 5,
          "Windows: UNC is, its root \\\\server\\share (behind \\\\?\\UNC\\ too)");
    CHECK(paths_other_drive_win("\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f}\\m\\x", 'C') == 49, "Windows: a volume GUID path is");
    CHECK(!paths_other_drive_win("/Volumes/GLS/x", 'C') && !paths_other_drive_win("Music/x.mp3", 'C') && !paths_other_drive_win("", 'C')
              && !paths_other_drive_win("C:x.mp3", 'D') && !paths_other_drive_win("1:/x", 'C'),
          "Windows: a path with no root isn't (the Mac's, relative, empty, drive-relative)");
    /* the UNC ruling: track input only; a chosen cache location never takes UNC */
    CHECK(!paths_location_ok_win("\\\\nas\\share\\" PATHS_CACHE_FOLDER) && !paths_location_ok_win("//nas/share/" PATHS_CACHE_FOLDER)
              && !paths_location_ok_win("\\\\?\\UNC\\nas\\share\\" PATHS_CACHE_FOLDER) && !paths_root_win("\\\\?\\UNC\\nas\\share\\"),
          "Windows: a cache location on a share is refused, in each UNC form");
    CHECK(paths_location_ok_os("E:\\" PATHS_CACHE_FOLDER) && !paths_location_ok_os("/Volumes/GLS/" PATHS_CACHE_FOLDER), "the build's form: Windows'");
#else
    CHECK(!paths_location_ok_os("E:\\" PATHS_CACHE_FOLDER) && paths_location_ok_os("/Volumes/GLS/" PATHS_CACHE_FOLDER), "the build's form: the Mac's");
#endif
}

/* Windows' form (T3c): the root, then the rule; tests/config-fixtures-win.txt has the reader's cases */
static void t_paths_win(void) {
    static const char g[] = "\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1A2B3C4D5E6F}\\";
    CHECK(paths_root_win("E:\\x") == 3 && paths_root_win("e:\\") == 3 && paths_root_win(g) == 49, "roots: a drive, a GUID path");
    CHECK(!paths_root_win("E:/x") && !paths_root_win("E:x") && !paths_root_win("\\\\?\\E:\\x") && !paths_root_win("\\\\server\\share\\")
              && !paths_root_win("\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f}") && !paths_root_win("/Volumes/x") && !paths_root_win("")
              && !paths_root_win("\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6}\\") && !paths_root_win("1:\\x"),
          "not roots: '/', drive-relative, \\\\?\\X:, UNC, a GUID without its '\\' or a digit, the Mac's, empty, a digit for a letter");
    char p[800];
    /* the GUID's closing brace is checked: ')' in its place refuses the root and the path (TC8) */
    static const char paren[] = "\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f)\\x";
    snprintf(p, sizeof p, "%.47s)\\" PATHS_CACHE_FOLDER, g);
    CHECK(!paths_root_win(paren) && !paths_location_ok_win(p), "a GUID path closed by ')': refused (%s)", p);
    snprintf(p, sizeof p, "%s" PATHS_CACHE_FOLDER, g);
    CHECK(paths_location_ok_win(p), "a GUID path's own folder");
    CHECK(paths_location_ok_win("E:\\" PATHS_CACHE_FOLDER) && paths_location_ok_win("E:\\Sets\\DJ's #1\\" PATHS_CACHE_FOLDER),
          "a drive's folder, at its root or below");
    CHECK(!paths_location_ok_win("E:\\Sets\\" PATHS_CACHE_FOLDER "\\") && !paths_location_ok_win("E:\\Sets\\x" PATHS_CACHE_FOLDER)
              && !paths_location_ok_win(PATHS_CACHE_FOLDER) && !paths_location_ok_win("/Volumes/GLS/" PATHS_CACHE_FOLDER),
          "a trailing '\\', another name, relative, the Mac's form");
    static const char *const bad[] = {"a\\..\\", "a\\.\\", "a\\\\", "a.\\", "a \\", "a:b\\", "a<b\\", "a>b\\", "a\"b\\", "a/b\\",
                                      "a|b\\", "a?b\\", "a*b\\", "a\tb\\", "a\x7f" "b\\", "a\x01" "b\\"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        snprintf(p, sizeof p, "E:\\%s" PATHS_CACHE_FOLDER, bad[i]);
        CHECK(!paths_location_ok_win(p), "refused: %s", p);
    }
    CHECK(paths_location_ok_win("E:\\a b.c\\M\xc3\xbasica\\" PATHS_CACHE_FOLDER), "inner dots and spaces, UTF-8");
    int k = snprintf(p, sizeof p, "E:\\");
    while (k < PATHS_LOCATION_MAX - (int)strlen("\\" PATHS_CACHE_FOLDER)) p[k++] = 'a';
    snprintf(p + k, sizeof p - (size_t)k, "\\" PATHS_CACHE_FOLDER);
    CHECK(strlen(p) == PATHS_LOCATION_MAX && paths_location_ok_win(p), "700 bytes");
    memmove(p + 4, p + 3, strlen(p + 3) + 1);
    CHECK(!paths_location_ok_win(p), "701 bytes");
}

/* ---------------------------------------------------------------- the cache's layout and sizes */

#define K "ab11111111111111111111111111111111111111111111111111111111111111"
#define M "435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a"

static void t_entries(void) {
    struct { const char *n; int level, dir, file; const char *parent; int memos, want; } c[] = {
        {M, 1, 1, 0, NULL, 1, ENT_MODEL_DIR},
        {M, 1, 0, 1, NULL, 1, ENT_OTHER},                   /* a file named like a model folder */
        {"435C987855F7EA74FF5F20090541748CBBCE5A2E8E501D07B10F4E703453215A", 1, 1, 0, NULL, 1, ENT_OTHER},
        {"._" M, 1, 0, 1, NULL, 1, ENT_COMP_DIR},
        {"model-12345.sha256.77.tmp", 1, 0, 1, NULL, 1, ENT_MEMO_TMP},
        {"model-12345.sha256.77.tmp", 1, 0, 1, NULL, 0, ENT_OTHER},   /* a chosen location's */
        {"model-x.sha256.77.tmp", 1, 0, 1, NULL, 1, ENT_OTHER},
        {"model-1.sha256.tmp", 1, 0, 1, NULL, 1, ENT_OTHER},
        {"ab", 2, 1, 0, NULL, 1, ENT_PREFIX_DIR},
        {"AB", 2, 1, 0, NULL, 1, ENT_OTHER},
        {"._ab", 2, 0, 1, NULL, 1, ENT_COMP_DIR},
        {"abc", 2, 1, 0, NULL, 1, ENT_OTHER},
        {K ".flac", 3, 0, 1, "ab", 1, ENT_FLAC},
        {K ".desc", 3, 0, 1, "ab", 1, ENT_DESC},
        {K ".flac.123.tmp", 3, 0, 1, "ab", 1, ENT_TMP},
        {K ".desc.9.tmp", 3, 0, 1, "ab", 1, ENT_TMP},
        {K ".flac..tmp", 3, 0, 1, "ab", 1, ENT_OTHER},
        {"._" K ".flac", 3, 0, 1, "ab", 1, ENT_COMP_FLAC},
        {"._" K ".desc", 3, 0, 1, "ab", 1, ENT_COMP_DESC},
        {"._" K ".desc.5.tmp", 3, 0, 1, "ab", 1, ENT_COMP_TMP},
        {K ".flac", 3, 0, 1, "cd", 1, ENT_OTHER},           /* in another prefix's folder */
        {K ".flac", 3, 1, 0, "ab", 1, ENT_OTHER},           /* a folder */
        {K ".flac", 3, 0, 0, "ab", 1, ENT_OTHER},           /* a link */
        {K ".mp3", 3, 0, 1, "ab", 1, ENT_OTHER},
        {".rbstemsplus-cache", 1, 0, 1, NULL, 1, ENT_OTHER},
    };
    for (size_t i = 0; i < sizeof c / sizeof *c; i++) {
        int k = entries_layout(c[i].n, c[i].level, c[i].dir, c[i].file, c[i].parent, c[i].memos);
        CHECK(k == c[i].want, "layout %zu (%.24s at %d): %d, expected %d", i, c[i].n, c[i].level, k, c[i].want);
    }
    char name[80];
    entries_file(name, sizeof name, K, 0);
    CHECK(!strcmp(name, K ".flac"), "file 0");
    entries_file(name, sizeof name, K, 3);
    CHECK(!strcmp(name, "._" K ".desc"), "file 3");
    CHECK(entries_is_hex("09af", 4) && !entries_is_hex("09aF", 4), "hex: lowercase only");

    /* the size rule: regular files only, their allocated space, a missing one 0 */
    char x[1100], p[1200];
    snprintf(x, sizeof x, "%s/ab", root);
    mkdir(x, 0755);
    const char *sizes[] = {K ".flac", "._" K ".flac", "._" K ".desc"};
    long long want = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s/%s", x, sizes[i]);
        FILE *f = fopen(p, "wb");
        for (int j = 0; j < 3000 * (i + 1); j++) fputc(j, f);
        fclose(f);
        struct stat st;
        stat(p, &st);
        want += (long long)st.st_blocks * 512;
    }
    snprintf(p, sizeof p, "%s/%s.desc", x, K);
    symlink("/usr/bin/true", p);                            /* not a regular file: 0 */
    plat_dir d;
    plat_dir_open(x, &d);
    long long got = entries_size_at(d, K);
    CHECK(got == want && got > 0, "entry size: %lld, expected %lld", got, want);
    CHECK(entries_size_at(d, "cd22222222222222222222222222222222222222222222222222222222222222") == 0, "a missing entry: 0");
    plat_dir_close(d);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s SCRATCH FIXTURES_DIR\n", argv[0]); return 2; }
    snprintf(root, sizeof root, "%s", argv[1]);
    t_config(argv[2]);
    t_marker(argv[2]);
    t_paths();
    t_paths_win();
    t_entries();
    printf("modules: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
