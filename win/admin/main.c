/* rbsp-admin.exe (shipped as admin\RB Stems Plus.exe, installed as bin\<sha12>\RB Stems Plus.exe;
 * app.md §5.5, I78): the elevated worker's start. x64 only, -mwindows -municode, a
 * requireAdministrator manifest, no per-release version resource (I83).
 *
 *   RB Stems Plus.exe install|uninstall|restore|install-worker|remove-root-folder --proto N
 *       --version V --source DIR --force 0|1 --pipe NAME
 *
 * Its first call is SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32): bcrypt, wintrust,
 * crypt32 (and user32) are loaded from System32 only, never from the folder it runs from. The
 * exit code is admin_proto.h's. */
#include "plat.h"
#include "plat_win.h"
#include "win_rules.h"

#include "worker_win.h"

#include <stdlib.h>
#include <string.h>

#include "admin_keys.h"   /* the build's (win/admin/build.sh): admin_key_texts[], from keys/release.pub.pem and keys/backup.pub.pem */

/* The compiled minimum (J18): the floor before the floor file exists. A constant in this source,
 * raised only with a worker code or protocol change, never derived from VERSION. */
#define ADMIN_COMPILED_MINIMUM "1.5.0"

/* The marker the app reads a bin\ worker's protocol from (admin_marker_find); said in the log, so
 * the linker keeps it. */
static const char marker[] = ADMIN_MARKER_PREFIX "proto=1 minimum=" ADMIN_COMPILED_MINIMUM "\n";
_Static_assert(ADMIN_PROTO == 1, "the marker's proto= follows ADMIN_PROTO");

static int utf8_of(const wchar_t *w, char *out, size_t cap) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out, (int)cap, NULL, NULL);
    return n > 0 ? 0 : -1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    (void)inst, (void)prev, (void)cmd, (void)show;
    int argc = __argc;
    wchar_t **wargv = __wargv;
    if (argc != 12 || !wargv) return ADMIN_EXIT_CHECK_FAILED;
    static char buf[12][ADMIN_PATH_MAX];
    const char *argv[12];
    for (int i = 0; i < 12; i++) {
        if (utf8_of(wargv[i], buf[i], sizeof buf[i])) return ADMIN_EXIT_CHECK_FAILED;
        argv[i] = buf[i];
    }
    admin_args a;
    char why[160];
    int r = admin_parse_args(12, argv, &a, why, sizeof why);
    static worker_win ww;
    worker_win_init(&ww);
    /* nothing is said, and nothing done, without the app's own pipe */
    if (!a.pipe[0] || worker_win_connect(&ww, a.pipe)) return ADMIN_EXIT_CHECK_FAILED;
    char line[ADMIN_LINE_MAX];
    snprintf(line, sizeof line, "%.*s", (int)(sizeof marker - 2), marker);
    ww.os.say(&ww, line);
    if (r != ADMIN_EXIT_OK) {
        snprintf(line, sizeof line, "%s: nothing changed", why);
        ww.os.say(&ww, line);
        return r;
    }
    if (!worker_win_elevated()) {
        ww.os.say(&ww, "not running as an administrator: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    if (worker_win_owner_admins()) {
        ww.os.say(&ww, "couldn't make Administrators the owner of what this run makes: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    enum { NKEYS = sizeof admin_key_texts / sizeof admin_key_texts[0] };
    sig_pubkey keys[NKEYS];
    size_t nkeys = 0;
    for (size_t i = 0; i < NKEYS; i++)
        if (!sig_key_from_text(&admin_key_texts[i], &keys[nkeys])) nkeys++;
    if (!nkeys) {
        ww.os.say(&ww, "no signing key compiled in: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    /* the places: under Windows' own Program Files, on whichever drive (plat_program_files, the
     * one rule the core DLL, the bridge and the watcher read too; a test build alone takes
     * RBSP_TEST_PROGRAMS through it, app.md §7: a fake tree one name under a drive root, e.g.
     * C:\rbtest-programs) */
    static char root[ADMIN_PATH_MAX], rekordbox[ADMIN_PATH_MAX];
    if (plat_program_files(WR_PF_ROOT, root, sizeof root) || plat_program_files(WR_PF_REKORDBOX, rekordbox, sizeof rekordbox)) {
        ww.os.say(&ww, "couldn't read Windows' Program Files folder: nothing changed");
        return ADMIN_EXIT_CHECK_FAILED;
    }
    admin_env e = {root, rekordbox, keys, nkeys, ADMIN_COMPILED_MINIMUM, 90000, 1000};
    return admin_run(&e, &ww.os, &a);
}
