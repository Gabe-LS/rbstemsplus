/* rbsp-admin.exe (shipped as admin\RB Stems Plus.exe, installed as bin\<sha12>\RB Stems Plus.exe;
 * app.md §5.5, I78): the elevated worker's start. x64 only, -mwindows -municode, a
 * requireAdministrator manifest, no per-release version resource (I83).
 *
 *   RB Stems Plus.exe install|uninstall|restore|install-worker|remove-root-folder --proto N
 *       --targets T --manifest M --source DIR --force 0|1 --pipe NAME (admin_proto.h, protocol 2)
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
static const char marker[] = ADMIN_MARKER_PREFIX "proto=" ADMIN_PROTO_TEXT " minimum=" ADMIN_COMPILED_MINIMUM "\n";
_Static_assert(ADMIN_PROTO == 2, "the marker's proto= (ADMIN_PROTO_TEXT) follows ADMIN_PROTO");

/* w as UTF-8 (malloc'd), or NULL when it isn't valid UTF-16 */
static char *utf8_of(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, NULL, 0, NULL, NULL);
    char *out = n > 0 ? malloc((size_t)n) : NULL;
    if (out && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out, n, NULL, NULL) != n) {
        free(out);
        out = NULL;
    }
    return out;
}

static int connect_pipe(void *ctx, const char *pipe) { return worker_win_connect(ctx, pipe); }

#ifdef ADMIN_TEST_HOLD
/* test builds only (win/admin/build.sh --test-key): two holds, each 1 to 120 seconds from a
 * variable, said first and its end said after. The VM's sign-out cases (stems-cache-uac) need a
 * worker that runs for seconds, so that Windows' block screen shows; here the worker ends in
 * under one. An elevated process gets a new environment, so the suite sets them in the user's
 * HKCU\Environment. Anything else in one, or none, holds nothing there:
 *   RBSP_TEST_WORKER_HOLD     before the worker does anything ("hold over"): WC1, WC2, where the
 *                             worker then finishes
 *   RBSP_TEST_WORKER_MIDHOLD  once its work has begun changing files (worker.h's
 *                             admin_test_mid_hold; "mid-work hold over"): WC3, WC10, where the
 *                             session's end then lands mid-work */
static int test_secs(const wchar_t *name) {
    wchar_t v[8];
    DWORD k = GetEnvironmentVariableW(name, v, 8);
    if (!k || k >= 4) return 0;
    int s = 0;
    for (DWORD i = 0; i < k; i++) {
        if (v[i] < L'0' || v[i] > L'9') return 0;
        s = s * 10 + (v[i] - L'0');
    }
    return s >= 1 && s <= 120 ? s : 0;
}

static void test_hold(worker_win *ww) {
    int s = test_secs(L"RBSP_TEST_WORKER_HOLD");
    if (!s) return;
    char line[96];
    snprintf(line, sizeof line, "test build: held %d s before the work (RBSP_TEST_WORKER_HOLD)", s);
    ww->os.say(ww, line);
    Sleep((DWORD)s * 1000);
    ww->os.say(ww, "test build: hold over");
}

static int test_target_matches(const char *name, const char *version, int unset) {
    char target[COMPAT_VERSION_MAX];
    DWORD n = GetEnvironmentVariableA(name, target, sizeof target);
    return n ? version && n < sizeof target && !strcmp(target, version) : unset;
}

int admin_test_fail_target(const admin_os *os, const char *version) {
    if (!test_target_matches("RBSP_TEST_WORKER_FAIL_TARGET", version, 0)) return 0;
    char line[ADMIN_LINE_MAX];
    snprintf(line, sizeof line, "test build: failing rekordbox %s's load check (RBSP_TEST_WORKER_FAIL_TARGET)", version);
    os->say(os->ctx, line);
    return 1;
}

void admin_test_mid_hold(const admin_os *os, const char *where, const char *version) {
    static int held;   /* once per run */
    int s = test_secs(L"RBSP_TEST_WORKER_MIDHOLD");
    if (!s || !test_target_matches("RBSP_TEST_WORKER_MIDHOLD_TARGET", version, 1) || held++) return;
    char line[ADMIN_LINE_MAX];
    snprintf(line, sizeof line, "test build: held %d s mid-work: %s (RBSP_TEST_WORKER_MIDHOLD)", s, where);
    os->say(os->ctx, line);
    Sleep((DWORD)s * 1000);
    os->say(os->ctx, "test build: mid-work hold over");
}
#endif

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    (void)inst, (void)prev, (void)cmd, (void)show;
    int argc = __argc;
    wchar_t **wargv = __wargv;
    /* any count: another protocol's line is refused as such (12), with or without its pipe, never
     * as malformed (review finding 9, re-review 9); admin_parse_args checks this protocol's
     * count. An argument that isn't valid UTF-16 is NULL there (malformed) */
    if (argc < 1 || !wargv) return ADMIN_EXIT_CHECK_FAILED;
    const char **argv = calloc((size_t)argc, sizeof *argv);
    if (!argv) return ADMIN_EXIT_CHECK_FAILED;
    for (int i = 0; i < argc; i++) argv[i] = utf8_of(wargv[i]);
    admin_args a;
    static worker_win ww;
    worker_win_init(&ww);
    char line[ADMIN_LINE_MAX];
    snprintf(line, sizeof line, "%.*s", (int)(sizeof marker - 2), marker);
    int r = admin_start(argc, argv, line, connect_pipe, ww.os.say, &ww, &a);
    for (int i = 0; i < argc; i++) free((void *)argv[i]);
    free(argv);
    if (r != ADMIN_EXIT_OK) return r;
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
#ifdef ADMIN_TEST_HOLD
    test_hold(&ww);
#endif
    admin_env e = {root, rekordbox, keys, nkeys, ADMIN_COMPILED_MINIMUM, 90000, 1000};
    return admin_run(&e, &ww.os, &a);
}
