/* The helper's track rules on its modules (helper.md §1, §4, §5; T13), with whichever rules the
 * build has: the Mac's, or Windows' (on Windows, and on the Mac with PATHS_WIN_TRACKS):
 *  - library.c's count invariant: a playlist's count (library_list's "tracks": its local rows,
 *    duplicates included) >= the distinct local paths library_tracks returns for it, equal without
 *    duplicates; on a fixture with a duplicated track, two tracks sharing a FolderPath, the same
 *    file in another form (Windows: c:\ vs C:/), rows that aren't local files, a deleted row, a
 *    folder and an empty playlist;
 *  - manifest.c's comparator: the same file recorded as c:/... and looked up as C:\... (Windows:
 *    found, kept, written once; the Mac: two files).
 *
 *   win-rules-test DIR       a scratch folder; exits 1 on a failure
 *
 * Built with a dummy library key (never rekordbox's): helper/tests/win-helper.sh on the Mac (both
 * rules), helper/build-win.sh as rbsp-win-rules-test.exe (Windows' rules, the VM's run). Rows
 * that aren't local files are only counted as excluded: none is ever printed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "library.h"
#include "manifest.h"
#include "path_rules.h"
#include "plat_helper.h"

int sqlite3_key(sqlite3 *db, const void *key, int n);

#if defined(_WIN32) || defined(PATHS_WIN_TRACKS)
#define WIN_RULES 1
#define RULES "Windows'"
#else
#define WIN_RULES 0
#define RULES "the Mac's"
#endif

static int fails;
#define CHECK(c, ...)                                                   \
    do {                                                                \
        if (c) printf("ok   ");                                         \
        else { printf("FAIL "); fails++; }                              \
        printf(__VA_ARGS__);                                            \
        printf("\n");                                                   \
    } while (0)

/* the dummy key's one byte as library.c decodes it: LIBKEY {0}, 0 - 0x5a */
static const unsigned char DUMMY_KEY[1] = {0xa6};

static const char *const SCHEMA =
    "CREATE TABLE djmdPlaylist(ID TEXT, ParentID TEXT, Name TEXT, Attribute INTEGER, Seq INTEGER, rb_local_deleted INTEGER);"
    "CREATE TABLE djmdSongPlaylist(PlaylistID TEXT, ContentID TEXT, TrackNo INTEGER, rb_local_deleted INTEGER);"
    "CREATE TABLE djmdContent(ID TEXT, FolderPath TEXT);"
    "INSERT INTO djmdPlaylist VALUES ('1', 'root', 'Mix', 0, 1, 0), ('2', 'root', 'Crate', 1, 2, 0), ('3', '2', 'Empty', 0, 1, 0),"
    "  ('4', 'root', 'Plain', 0, 3, 0);"
    /* Windows' local files: 10 twice (a duplicated track), 11 sharing 10's FolderPath, 12 the same
     * file as c:\, 13 another drive with \, 14 UNC; the Mac's: 16 twice and 18 sharing its path;
     * neither's: 15 (not a file on any computer) and 19 (relative); 17's row deleted */
    "INSERT INTO djmdContent VALUES ('10', 'C:/Music/a.mp3'), ('11', 'C:/Music/a.mp3'), ('12', 'c:\\Music\\a.mp3'),"
    "  ('13', 'D:\\Music\\b.flac'), ('14', '//nas/share/c.wav'), ('15', 'x-service:tracks:123'), ('16', '/Users/dj/d.mp3'),"
    "  ('17', 'C:/Music/e.mp3'), ('18', '/Users/dj/d.mp3'), ('19', 'Music/f.mp3'), ('20', 'C:/Music/g.mp3'), ('21', '/Users/dj/h.mp3');"
    "INSERT INTO djmdSongPlaylist VALUES ('1', '10', 1, 0), ('1', '10', 2, 0), ('1', '11', 3, 0), ('1', '12', 4, 0), ('1', '13', 5, 0),"
    "  ('1', '14', 6, 0), ('1', '15', 7, 0), ('1', '16', 8, 0), ('1', '16', 9, 0), ('1', '18', 10, 0), ('1', '17', 11, 1),"
    "  ('1', '19', 12, 0), ('4', '20', 1, 0), ('4', '21', 2, 0);";

static int make_fixture(const char *path) {
    sqlite3 *db = NULL;
    remove(path);
    int rc = sqlite3_open(path, &db);
    if (rc == SQLITE_OK) rc = sqlite3_key(db, DUMMY_KEY, (int)sizeof DUMMY_KEY);
    if (rc == SQLITE_OK) rc = sqlite3_exec(db, SCHEMA, NULL, NULL, NULL);
    if (rc != SQLITE_OK) printf("fixture: %s\n", db ? sqlite3_errmsg(db) : "out of memory");
    sqlite3_close(db);
    return rc != SQLITE_OK;
}

/* playlist ID's "tracks" in library_list's output, -1 when it isn't there */
static const char *scratch;

static long listed_count(library *l, const char *id) {
    char why[512], line[1024], want[64], path[1100];
    snprintf(path, sizeof path, "%s/list.jsonl", scratch);
    FILE *f = fopen(path, "w+b");
    if (!f) return -2;
    long n = -1;
    if (!library_list(l, f, why, sizeof why)) {
        rewind(f);
        snprintf(want, sizeof want, "{\"playlist\":\"%s\",", id);
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, want, strlen(want))) {
                const char *t = strstr(line, "\"tracks\":");
                n = t ? atol(t + 9) : -1;
            }
    } else printf("library_list: %s\n", why);
    fclose(f);
    return n;
}

static void t_library(const char *dir) {
    char db[1100], why[512];
    snprintf(db, sizeof db, "%s/master.db", dir);
    if (make_fixture(db)) { CHECK(0, "the library fixture"); return; }
    library *l = library_open(db, why, sizeof why);
    CHECK(l != NULL, "library_open on the fixture (dummy key)%s%s", l ? "" : ": ", l ? "" : why);
    if (!l) return;
    char **p = NULL;
    size_t np = 0;
    long count = listed_count(l, "1");
    int r = library_tracks(l, "1", &p, &np, why, sizeof why);
    CHECK(r == 0, "library_tracks(1)%s%s", r ? ": " : "", r ? why : "");
    /* Windows: rows 10, 10, 11, 12, 13, 14 (6), paths C:/Music/a.mp3, D:\Music\b.flac,
     * //nas/share/c.wav (3: c:\Music\a.mp3 is C:/Music/a.mp3); the Mac (an absolute path): rows 14,
     * 16, 16, 18 (4), paths //nas/share/c.wav and /Users/dj/d.mp3 */
    long want_count = WIN_RULES ? 6 : 4;
    size_t want_paths = WIN_RULES ? 3 : 2;
    CHECK(count == want_count, "%s count: %ld local rows, duplicates included (want %ld)", RULES, count, want_count);
    CHECK(np == want_paths, "%s distinct local paths: %zu (want %zu)", RULES, np, want_paths);
    CHECK(count >= (long)np, "the count invariant: %ld >= %zu", count, np);
    int locals = 1;
    for (size_t i = 0; i < np; i++) locals &= paths_local_file(p[i]);
    CHECK(locals, "every path returned is a local file by paths_local_file");
    if (WIN_RULES && np == 3)
        CHECK(!strcmp(p[0], "C:/Music/a.mp3") && !strcmp(p[1], "D:\\Music\\b.flac") && !strcmp(p[2], "//nas/share/c.wav"),
              "in rekordbox's order, the first form kept, stored strings unchanged");
    if (!WIN_RULES && np == 2) CHECK(!strcmp(p[0], "//nas/share/c.wav") && !strcmp(p[1], "/Users/dj/d.mp3"), "the Mac's paths, in order");
    for (size_t i = 0; i < np; i++) free(p[i]);
    free(p);
    /* no duplicates: the count equals the paths */
    count = listed_count(l, "4");
    r = library_tracks(l, "4", &p, &np, why, sizeof why);
    CHECK(r == 0 && count == 1 && np == 1, "no duplicates: count %ld = paths %zu = 1", count, np);
    for (size_t i = 0; !r && i < np; i++) free(p[i]);
    if (!r) free(p);
    CHECK(listed_count(l, "2") == 0 && listed_count(l, "3") == 0, "a folder and an empty playlist: 0");
    r = library_tracks(l, "2", &p, &np, why, sizeof why);
    CHECK(r == -1, "a folder's tracks are refused");
    library_close(l);
}

static void t_manifest(const char *dir) {
    manifest m = {0}, r;
    plat_finfo fi = {PLAT_FILE, 1234, 5678};
    snprintf(m.helper, sizeof m.helper, "1");
    memset(m.model, 'a', 64);
    mfile *f = manifest_file(&m, "c:/Music/a.mp3", &fi);
    if (f) mfile_key(f, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    char why[512];
    int w = f ? manifest_write(dir, "set", &m, NULL, why, sizeof why) : -1;
    CHECK(w == 0, "manifest written%s%s", w ? ": " : "", w ? why : "");
    if (w || manifest_read(dir, "set", &r)) { CHECK(0, "manifest read back"); manifest_free(&m); return; }
    const char *other = "C:\\Music\\a.mp3";
    CHECK(!!manifest_find(&r, other, &fi) == WIN_RULES, "%s manifest_find: c:/ recorded, C:\\ looked up: %s", RULES,
          manifest_find(&r, other, &fi) ? "found" : "not found");
    CHECK(!!manifest_by_path(&r, other) == WIN_RULES, "%s manifest_by_path: the same", RULES);
    /* a run listing the file as C:\ writes it once, its earlier c:/ record not carried twice */
    manifest now = {0};
    snprintf(now.helper, sizeof now.helper, "1");
    memset(now.model, 'a', 64);
    manifest_file(&now, other, &fi);
    manifest back;
    w = manifest_write(dir, "set2", &now, &r, why, sizeof why);
    CHECK(w == 0 && manifest_read(dir, "set2", &back) == 0 && back.n == (WIN_RULES ? 1u : 2u), "%s carry: %zu file record(s) (want %d)",
          RULES, w ? 0 : back.n, WIN_RULES ? 1 : 2);
    if (!w) manifest_free(&back);
    char *keep[] = {(char *)other};
    manifest_keep_only(&r, keep, 1);
    CHECK(r.n == (WIN_RULES ? 1u : 0u), "%s manifest_keep_only by C:\\: %zu kept", RULES, r.n);
    manifest_free(&r);
    manifest_free(&now);
    manifest_free(&m);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    if (plat_exe_start()) { fprintf(stderr, "plat_exe_start failed\n"); return 1; }
#endif
    if (argc != 2) { fprintf(stderr, "usage: %s SCRATCH-DIR\n", argv[0]); return 2; }
    printf("rules: %s\n", RULES);
    scratch = argv[1];
    t_library(argv[1]);
    t_manifest(argv[1]);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
