/* T3c's tests for the Windows VM (bridge.md §1, §4): a chosen location's checks (location.c) on
 * plat_win.c, the volume calls with a volume GUID path and with a path past MAX_PATH, and
 * cfg_ini.c's reader taking Windows' form of cache_dir. Built by common/tests/win.sh for x64 and
 * arm64; run in the VM, unelevated:
 *
 *   location-win-test.exe SCRATCH     SCRATCH: an empty folder on the system volume's NTFS
 *                                     ("C:\rbtest\loc"); exits 1 when a check fails
 *   location-win-test.exe --make DIR | --check DIR ANY | --gone ROOT ANY
 *                                     one location on another volume (T3c case 3, a VHDX driven
 *                                     by tests/win/suites/volumes.suite): DIR made with our
 *                                     marker; location_check's verdict; location_gone's (ENOENT).
 *                                     One "verdict V" line (and "mount M" when ok); exit 0, or 2
 *                                     on a usage location_args.h refuses or a --make that fails
 *
 * RBSP_TEST_NET_DRIVE=Z: (a network drive, e.g. `net use Z: \\Mac\Home` in Parallels) adds the
 * network case: "not-local". Another volume (a VHDX: its drive and GUID paths, a mount folder,
 * read-only, detached) is tests/win/suites/volumes.suite's, through the one-location modes. Not
 * run on the Mac: it needs Windows (its command line is: common/tests/location_args.c). */
#include "../cfg_ini.h"
#include "../location.h"
#include "../marker.h"
#include "../path_rules.h"
#include "../plat_win.h"
#include "check.h"
#include "location_args.h"

#include <shellapi.h>

#define ID "0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F"
static char root[1024];

/* a UTF-8 path as "\\?\"-prefixed UTF-16 (a drive path; a GUID path as it is), for the setup's
 * own calls past MAX_PATH */
static wchar_t *wide(const char *p) {
    static wchar_t w[2][4096];
    static int i;
    wchar_t *o = w[i ^= 1];
    int pre = p[1] == ':';
    if (pre) wcscpy(o, L"\\\\?\\");
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, o + (pre ? 4 : 0), 4096 - 4)) o[0] = 0;
    return o;
}
static int mk(const char *p) { return CreateDirectoryW(wide(p), NULL) || GetLastError() == ERROR_ALREADY_EXISTS; }
static void put(const char *p, const char *text) {
    HANDLE h = CreateFileW(wide(p), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n = 0;
    if (h == INVALID_HANDLE_VALUE || !WriteFile(h, text, (DWORD)strlen(text), &n, NULL)) { fprintf(stderr, "can't write %s\n", p); exit(2); }
    CloseHandle(h);
}
static int run_cmd(const char *cmd) {
    wchar_t w[3000];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, cmd, -1, w, 3000)) return -1;
    return _wsystem(w);
}
static const char *check_loc(const char *dir, int any, location_pin *pin) {
    const char *w = location_check(dir, ID, any, pin);
    return w ? w : "ok";
}

static void t_config(void) {
    cfg_config c = {.enabled = 1, .max_bytes = 20LL << 30, .max_days = 60};
    const char *t = "cache_dir=\"\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f}\\RB Stems Plus Cache.noindex\"\ncache_id=" ID "\n";
    CHECK(cfg_parse(t, strlen(t), &c, NULL) == 0 && c.location == 1 && !strcmp(c.dir, "\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f}\\RB Stems Plus Cache.noindex"),
          "config: a GUID path is a chosen location here (%d)", c.location);
    cfg_config m = {.enabled = 1, .max_bytes = 20LL << 30, .max_days = 60};
    t = "cache_dir=\"/Volumes/GLS/RB Stems Plus Cache.noindex\"\ncache_id=" ID "\n";
    CHECK(cfg_parse(t, strlen(t), &m, NULL) == 0 && m.location == -1, "config: the Mac's form is malformed here (%d)", m.location);
}

static void t_volumes(char *guid_root, size_t n) {
    plat_volume v, g;
    const char *how;
    CHECK(plat_volume_info(root, &v) == 0 && paths_root_win(v.mount) == 49 && strlen(v.mount) == 49, "the scratch's volume: %s", v.mount);
    snprintf(guid_root, n, "%s", v.mount);
    /* the scratch folder as a volume GUID path: the same volume, its free space */
    char gp[1200];
    snprintf(gp, sizeof gp, "%s%s", v.mount, root + paths_root_win(root));
    CHECK(plat_volume_info(gp, &g) == 0 && !_stricmp(g.mount, v.mount) && g.total == v.total, "a GUID path's volume: %s (%s)", g.mount, gp);
    CHECK(plat_free_bytes(gp, 0, &how) > 0, "a GUID path's free space");
    char mount[PLAT_MOUNT_MAX];
    int sys = -1;
    CHECK(plat_volume_holding(gp, mount, sizeof mount, &sys) == 0 && !_stricmp(mount, v.mount) && sys == 1, "a GUID path held: %s, system %d", mount, sys);
    /* past MAX_PATH: GetVolumePathNameW gets it as \\?\X:\... */
    char lp[1200];
    int k = snprintf(lp, sizeof lp, "%s", root);
    for (int i = 0; k < 400; i++) {
        k += snprintf(lp + k, sizeof lp - (size_t)k, "\\long-folder-%02d-abcdefghijklmnopqrstuvwxyz", i);
        CHECK(mk(lp), "made %d bytes", k);
    }
    CHECK(plat_volume_info(lp, &g) == 0 && !_stricmp(g.mount, v.mount), "a path of %d bytes: its volume %s", k, g.mount);
    CHECK(plat_free_bytes(lp, 0, &how) > 0, "a path of %d bytes: its free space", k);
    char lpf[1200];
    snprintf(lpf, sizeof lpf, "%s/x", lp);                  /* '/' separators come in too */
    for (char *c = lpf; *c; c++)
        if (*c == '\\') *c = '/';
    CHECK(plat_volume_info(lpf, &g) == 0 && !_stricmp(g.mount, v.mount), "a long path with '/': its volume");
    CHECK(!plat_win_drive_remote(root) && !plat_win_drive_remote(gp), "not a network drive");
    /* plat_volume_name (review NL1, NL5, NR1): the same name by the drive path, the GUID path and a
     * long path; a volume that isn't there, or a letter no volume has, never answers for another */
    char lab[256], let[8], lab2[256], let2[8];
    CHECK(plat_volume_name(root, lab, sizeof lab, let, sizeof let) == 0 && let[0] == (root[0] & ~0x20) && !strcmp(let + 1, ":"),
          "volume_name by the drive path: \"%s\" %s", lab, let);
    CHECK(plat_volume_name(gp, lab2, sizeof lab2, let2, sizeof let2) == 0 && !strcmp(lab2, lab) && !strcmp(let2, let),
          "volume_name by the GUID path: \"%s\" %s", lab2, let2);
    CHECK(plat_volume_name(lp, lab2, sizeof lab2, let2, sizeof let2) == 0 && !strcmp(lab2, lab) && !strcmp(let2, let),
          "volume_name by a path of %d bytes: \"%s\" %s (NL5)", k, lab2, let2);
    const char *zero = "\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\x";
    wchar_t raw[MAX_PATH + 1];
    DWORD got = GetVolumePathNameW(L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\x", raw, MAX_PATH + 1);
    printf("note: GetVolumePathNameW(zero GUID path): %lu \"%ls\" (%lu)\n", (unsigned long)got, got ? raw : L"", got ? 0UL : (unsigned long)GetLastError());
    CHECK(plat_volume_name(zero, lab2, sizeof lab2, let2, sizeof let2) != 0, "volume_name of a volume that isn't there: an error");
    DWORD drives = GetLogicalDrives();
    int free_letter = 0;
    for (int c = 'Z'; c >= 'D' && !free_letter; c--)
        if (!(drives & (1u << (c - 'A')))) free_letter = c;
    if (free_letter) {
        char q[16];
        wchar_t wq[16];
        snprintf(q, sizeof q, "%c:\\x", free_letter);
        swprintf(wq, 16, L"%c:\\x", free_letter);
        got = GetVolumePathNameW(wq, raw, MAX_PATH + 1);
        printf("note: GetVolumePathNameW(%s): %lu \"%ls\" (%lu)\n", q, (unsigned long)got, got ? raw : L"", got ? 0UL : (unsigned long)GetLastError());
        CHECK(plat_volume_name(q, lab2, sizeof lab2, let2, sizeof let2) != 0, "volume_name of %s (no volume has the letter): an error", q);
    } else CHECK(0, "no unassigned drive letter D: to Z:");
}

static void t_location(const char *guid_root) {
    location_pin pin;
    const char *w;
    char dir[1200], p[1300], gdir[1300];
    snprintf(dir, sizeof dir, "%s\\" PATHS_CACHE_FOLDER, root);
    snprintf(gdir, sizeof gdir, "%s%s\\" PATHS_CACHE_FOLDER, guid_root, root + paths_root_win(root));
    w = check_loc(dir, 1, &pin);
    CHECK(!strcmp(w, "folder"), "no folder: folder (%s)", w);
    CHECK(mk(dir), "the folder");
    w = check_loc(dir, 1, &pin);
    CHECK(!strcmp(w, "marker"), "no marker: marker (%s)", w);
    snprintf(p, sizeof p, "%s\\" MARKER_NAME, dir);
    put(p, "id=" ID "\nformat=" MARKER_FORMAT "\n");
    plat_stat st;
    w = check_loc(dir, 1, &pin);
    CHECK(!strcmp(w, "ok") && plat_stat_path(dir, 0, &st) == 0 && plat_id_same(pin.id, st.id) && !_stricmp(pin.mount, guid_root),
          "ours by its drive path: ok, pinned, %s (%s)", pin.mount, w);
    w = check_loc(gdir, 1, &pin);
    CHECK(!strcmp(w, "ok") && plat_id_same(pin.id, st.id) && !_stricmp(pin.mount, guid_root), "ours by its GUID path: ok (%s)", w);
    w = check_loc(gdir, 0, &pin);
    CHECK(!strcmp(w, "not-connected"), "on the system volume: not-connected (%s)", w);
    w = check_loc("\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\" PATHS_CACHE_FOLDER, 0, &pin);
    CHECK(!strcmp(w, "not-connected"), "a volume that isn't there: not-connected (%s)", w);
    /* a junction on the way, and the folder itself a junction */
    snprintf(p, sizeof p, "%s\\jn", root);
    char cmd[3000];
    snprintf(cmd, sizeof cmd, "mklink /J \"%s\" \"%s\" >NUL", p, root);
    if (run_cmd(cmd) == 0) {
        snprintf(p, sizeof p, "%s\\jn\\" PATHS_CACHE_FOLDER, root);
        w = check_loc(p, 1, &pin);
        CHECK(!strcmp(w, "folder"), "a junction on the way: folder (%s)", w);
    } else CHECK(0, "mklink /J failed");
    snprintf(p, sizeof p, "%s\\jdir", root);
    CHECK(mk(p), "jdir");
    snprintf(p, sizeof p, "%s\\jdir\\" PATHS_CACHE_FOLDER, root);
    snprintf(cmd, sizeof cmd, "mklink /J \"%s\" \"%s\" >NUL", p, dir);
    if (run_cmd(cmd) == 0) {
        w = check_loc(p, 1, &pin);
        CHECK(!strcmp(w, "folder"), "the folder a junction: folder (%s)", w);
    } else CHECK(0, "mklink /J failed");
    /* a long location: past MAX_PATH, under the 700-byte rule */
    char lp[1300];
    int k = snprintf(lp, sizeof lp, "%s", root);
    for (int i = 0; k < 560; i++) {
        k += snprintf(lp + k, sizeof lp - (size_t)k, "\\deep-%02d-abcdefghijklmnopqrstuvwxyz0123456789", i);
        mk(lp);
    }
    k += snprintf(lp + k, sizeof lp - (size_t)k, "\\" PATHS_CACHE_FOLDER);
    CHECK(mk(lp), "the long folder (%d bytes)", k);
    snprintf(p, sizeof p, "%s\\" MARKER_NAME, lp);
    put(p, "id=" ID "\nformat=" MARKER_FORMAT "\n");
    w = check_loc(lp, 1, &pin);
    CHECK(k <= PATHS_LOCATION_MAX && paths_location_ok_win(lp) && !strcmp(w, "ok"), "a %d-byte location: ok (%s)", k, w);
    /* gone */
    CHECK(!strcmp(location_gone(guid_root, 0, ENOENT), "folder") && !strcmp(location_gone(guid_root, 0, EACCES), "no-permission"),
          "gone, its volume still there: the error's reason");
    CHECK(!strcmp(location_gone("\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\", 0, ENOENT), "not-connected")
              && !strcmp(location_gone("\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\", 1, ENOENT), "folder"),
          "gone, its volume unmounted: not-connected (any: the error's)");
    /* a network drive (optional): the table of volumes doesn't list it */
    wchar_t nd[8] = L"";
    if (GetEnvironmentVariableW(L"RBSP_TEST_NET_DRIVE", nd, 8) == 2 && nd[1] == L':') {
        char np[64];
        snprintf(np, sizeof np, "%c:\\x\\" PATHS_CACHE_FOLDER, (char)nd[0]);
        CHECK(plat_win_drive_remote(np), "%c: is a network drive", (char)nd[0]);
        const char *how;
        char nr[8];
        snprintf(nr, sizeof nr, "%c:\\", (char)nd[0]);
        CHECK(plat_free_bytes(nr, 0, &how) > 0, "%s: its free space without a GUID path (TC7)", nr);
        w = check_loc(np, 1, &pin);
        CHECK(!strcmp(w, "not-local"), "a network drive: not-local (%s)", w);
    } else printf("note: no RBSP_TEST_NET_DRIVE: the network drive case skipped\n");
}

/* one location, its verdict on stdout (the volume cases' modes, location_args.h) */
static int one(const loc_args *o) {
    location_pin pin;
    if (o->mode == LOC_MAKE) {
        char p[1300];
        snprintf(p, sizeof p, "%s\\" MARKER_NAME, o->path);
        if (!mk(o->path)) { fprintf(stderr, "can't make %s (%lu)\n", o->path, GetLastError()); return 2; }
        put(p, "id=" ID "\nformat=" MARKER_FORMAT "\n");
        printf("made %s\n", o->path);
        return 0;
    }
    if (o->mode == LOC_GONE) {
        printf("verdict %s\n", location_gone(o->path, o->any, ENOENT));
        return 0;
    }
    const char *w = check_loc(o->path, o->any, &pin);
    printf("verdict %s\n", w);
    if (!strcmp(w, "ok")) printf("mount %s\n", pin.mount);
    return 0;
}

int main(int argc, char **argv) {
    (void)argv;
    int wn = 0;
    wchar_t **wa = CommandLineToArgvW(GetCommandLineW(), &wn);
    /* the arguments as UTF-8 (argv is the ANSI code page's) */
    static char ua[4][1200];
    char *u[4] = {ua[0], ua[1], ua[2], ua[3]};
    int ok = wa && argc == wn && wn >= 2 && wn <= 4;
    for (int i = 0; ok && i < wn; i++) ok = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wa[i], -1, ua[i], sizeof ua[i], NULL, NULL) > 0;
    loc_args o;
    /* a SCRATCH path too long for root is refused, never cut short (review WA8) */
    if (!ok || loc_args_parse(wn, u, &o) != 0 || (o.mode == LOC_SCRATCH && strlen(o.path) >= sizeof root)) {
        fprintf(stderr, "usage: location-win-test SCRATCH (a drive path: C:\\rbtest\\loc)\n"
                        "       location-win-test --make DIR | --check DIR 0|1 | --gone ROOT 0|1\n");
        return 2;
    }
    if (wa) LocalFree(wa);
    if (o.mode != LOC_SCRATCH) return one(&o);
    snprintf(root, sizeof root, "%s", o.path);
    char guid_root[PLAT_MOUNT_MAX] = "";
    t_config();
    t_volumes(guid_root, sizeof guid_root);
    t_location(guid_root);
    printf("location_win: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
