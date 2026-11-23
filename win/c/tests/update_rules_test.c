/* update_rules.c and stub_rules.c: the updater's and the stub's pure rules, on the Mac and in the
 * Windows test exe (no file, no network). The shown-version rule and the result keys are also held
 * to today's Swift by the shared fixtures (fixtures_test.c). */
#include "check.h"

#include "../stub.h"
#include "../update.h"

#include <errno.h>
#include <string.h>
#include <wchar.h>

/* CommandLineToArgvW's reading of the arguments (the UCRT's rules): what update_quote_arg must
 * round-trip through */
static int argv_parse(const char *s, char out[][128], int max) {
    int argc = 0;
    size_t i = 0;
    for (;;) {
        while (s[i] == ' ' || s[i] == '\t') i++;
        if (!s[i] || argc == max) break;
        size_t k = 0;
        int quoted = 0;
        char *a = out[argc++];
        while (s[i] && (quoted || (s[i] != ' ' && s[i] != '\t'))) {
            size_t bs = 0;
            while (s[i] == '\\') bs++, i++;
            if (s[i] == '"') {
                for (size_t j = 0; j < bs / 2; j++) a[k++] = '\\';
                if (bs % 2) {
                    a[k++] = '"';
                    i++;
                } else if (quoted && s[i + 1] == '"') {
                    a[k++] = '"';
                    i += 2;
                } else {
                    quoted = !quoted;
                    i++;
                }
            } else {
                for (size_t j = 0; j < bs; j++) a[k++] = '\\';
                if (s[i] && (quoted || (s[i] != ' ' && s[i] != '\t'))) a[k++] = s[i++];
            }
        }
        a[k] = 0;
    }
    return argc;
}

static void pure(void) {
    /* versions */
    static const char *const yes[] = {"1.4.0", "08.09.10", "123456789.1.1"};
    static const char *const no[] = {"", "1.4", "v1.4.0", "1.5.0-beta.1", "1.5.0-beta.10", "1.4.0-beta", "1.4.0-rc.1", "1.4.0 ", "1234567890.1.1",
                                     "1.4.0.1"};
    for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++) {
        CHECK(update_is_version(yes[i]) && stub_version_ok(yes[i]), "a version: %s", yes[i]);
    }
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) {
        CHECK(!update_is_version(no[i]) && !stub_version_ok(no[i]), "not a version: \"%s\"", no[i]);
    }

    /* the shown version (the Swift fixtures cover the rule; here its records) */
    update_shown r, out;
    CHECK(update_next_shown("1.5.0", NULL, "1.5.1", "1.5.0", &out) && !strcmp(out.shown, "1.5.1"), "a release carrying this app: shown");
    snprintf(r.shown, sizeof r.shown, "1.5.1"), snprintf(r.app, sizeof r.app, "1.5.0");
    CHECK(update_next_shown("1.5.0", &r, "1.6.0", "1.6.0", &out) && !strcmp(out.shown, "1.5.1"), "a release with a new app: stays");
    CHECK(update_next_shown("1.5.0", &r, "1.5.0", "1.5.0", &out) && !strcmp(out.shown, "1.5.1"), "never steps back");
    CHECK(!update_next_shown("1.5.0", NULL, "1.5.1-beta.1", "1.5.0", &out), "a suffixed payload_version: never shown");
    CHECK(!strcmp(update_shown_version("1.6.0", &r), "1.6.0"), "a record for another app: the own version");
    char buf[64];
    size_t n = update_shown_format(&r, buf, sizeof buf);
    CHECK(n && update_shown_parse(buf, n, &out) && !strcmp(out.shown, r.shown) && !strcmp(out.app, r.app), "the record round-trips");

    /* newer: windows.app.version only */
    static payload_manifest m;
    memset(&m, 0, sizeof m);
    snprintf(m.payload_version, sizeof m.payload_version, "1.6.0");
    snprintf(m.win_app.version, sizeof m.win_app.version, "1.5.0");
    CHECK(!update_newer(&m, "1.5.0"), "no windows block: not newer");
    m.has_windows = 1;
    CHECK(!update_newer(&m, "1.5.0") && update_newer(&m, "1.4.9") && !update_newer(&m, "1.5.0-beta.3") && !update_newer(&m, "1.5.1"),
          "by windows.app.version, never payload_version");

    /* addresses */
    char url[UPDATE_URL_MAX];
    CHECK(!update_url(NULL, NULL, "payload.json", url, sizeof url) && !strcmp(url, UPDATE_BASE "/latest/download/payload.json"), "latest's");
    CHECK(!update_url(NULL, "1.5.2", "rbstemsplus-windows.zip", url, sizeof url) &&
              !strcmp(url, UPDATE_BASE "/download/v1.5.2/rbstemsplus-windows.zip"),
          "a tag's");
    CHECK(update_url(NULL, NULL, "../x", url, sizeof url) && update_url(NULL, NULL, "a/b", url, sizeof url) && update_url(NULL, "v1", "a", url, sizeof url) &&
              update_url(NULL, "1.5.0-beta.2", "a", url, sizeof url) &&
              update_url("http://example.com", NULL, "a", url, sizeof url) && update_url(NULL, NULL, "a", url, 20),
          "names, tags and bases refused");

    /* records */
    update_pending p = {"1.5.2", "1.5.3", 4242, 1794177745}, q;
    char rec[UPDATE_RECORD_MAX];
    n = update_pending_format(&p, rec, sizeof rec);
    CHECK(n && !strcmp(rec, "1.5.2\n1.5.3\n4242\n1794177745\n"), "pending's form");
    CHECK(update_pending_parse(rec, n, &q) && !strcmp(q.from, p.from) && !strcmp(q.to, p.to) && q.pid == p.pid && q.started == p.started, "read back");
    static const char *const badp[] = {"1.4.0\n1.5.0\n1\n", "1.4.0\n1.5.0\n1\n2", "1.4.0\n1.5.0\n01\n2\n", "1.4.0\n1.5.0\n4294967296\n2\n",
                                       "1.4.0\nx\n1\n2\n", "1.4.0\n1.5.0\n1\n2\n\n", "1.4.0\n1.5.0\n-1\n2\n"};
    for (size_t i = 0; i < sizeof badp / sizeof badp[0]; i++) CHECK(!update_pending_parse(badp[i], strlen(badp[i]), &q), "pending refused: %zu", i);
    char v[PAYLOAD_VERSION_MAX];
    wchar_t wv[STUB_VERSION_MAX];
    static const char *const cur_ok[] = {"1.5.0\n", "1.5.0", "1.5.0\r\n"};
    static const char *const cur_no[] = {"", "\n", "1.5.0\n\n", " 1.5.0\n", "1.5.0 \n", "v1.5.0\n", "1.5.0\n1.4.0\n", "..\n", "1.5.0\r", "1.5.0-beta.3\n"};
    for (size_t i = 0; i < sizeof cur_ok / sizeof cur_ok[0]; i++)
        CHECK(update_current_parse(cur_ok[i], strlen(cur_ok[i]), v) && stub_current(cur_ok[i], strlen(cur_ok[i]), wv) && !strncmp(v, "1.5.0", 5) &&
                  wv[0] == L'1' && wcslen(wv) == strlen(v),
              "current read alike by both: %zu", i);
    for (size_t i = 0; i < sizeof cur_no / sizeof cur_no[0]; i++) {
        int a = update_current_parse(cur_no[i], strlen(cur_no[i]), v), b = stub_current(cur_no[i], strlen(cur_no[i]), wv);
        CHECK(a == b, "current refused alike by both: %zu (%d %d)", i, a, b);
    }
    CHECK(!update_current_parse("1.5.0\0x", 7, v) && !stub_current("1.5.0\0x", 7, wv), "a NUL inside: refused");

    for (int k = UPDATE_FAIL_GENERIC; k <= UPDATE_FAIL_SWAP; k++) {
        const char *l = update_fail_line(k);
        CHECK(l && update_fail_parse(l, strlen(l)) == k, "the result line of key %d reads back", k);
    }
    CHECK(update_fail_parse(NULL, 0) == UPDATE_FAIL_NONE && update_fail_line(UPDATE_FAIL_NONE) == NULL, "no result");

    char ev[96];
    CHECK(update_watcher_event_name(4242, ev, sizeof ev) && !strcmp(ev, "Local\\io.github.rbstemsplus.watcher.stop.4242"), "the stop event's name");
    CHECK(!update_watcher_event_name(4242, ev, 10), "too small: 0");

    /* review UN7: only a watcher in this session is asked to stop (or ended); one in another
     * session, or whose session (or ours) couldn't be read, is left running and reported */
    CHECK(update_watcher_action(1, 2, 1, 2) == UPDATE_WATCHER_STOP, "a watcher in our session: stopped");
    CHECK(update_watcher_action(1, 2, 1, 3) == UPDATE_WATCHER_LEAVE, "a watcher in another session: left");
    CHECK(update_watcher_action(1, 3, 1, 2) == UPDATE_WATCHER_LEAVE, "another session, the other way: left");
    CHECK(update_watcher_action(0, 2, 1, 2) == UPDATE_WATCHER_LEAVE, "our session unknown: left");
    CHECK(update_watcher_action(1, 2, 0, 2) == UPDATE_WATCHER_LEAVE, "its session unknown: left");
    CHECK(update_watcher_action(0, 0, 0, 0) == UPDATE_WATCHER_LEAVE, "both unknown (zeros): left");
    CHECK(update_stop_result(1, 0, 0) == 0, "none found: 0");
    CHECK(update_stop_result(1, 1, 0) == 1, "one stopped: 1");
    CHECK(update_stop_result(1, 1, 1) == -1 && update_stop_result(1, 0, 1) == -1, "one left running: -1 (its files stay)");
    CHECK(update_stop_result(0, 0, 0) == -1, "no process list: -1");
    /* review UQ's GPT re-run, G2: a process that can't be inspected is unknown only by our names;
     * a list cut short is unknown; one found wins */
    CHECK(update_image_kind("RB Stems Plus.exe") == UPDATE_IMAGE_APP && update_image_kind("rb stems plus.EXE") == UPDATE_IMAGE_APP,
          "the app's exe name, any case");
    CHECK(update_image_kind("rbsp-watch-x64.exe") == UPDATE_IMAGE_WATCHER && update_image_kind("RBSP-WATCH-arm64.exe") == UPDATE_IMAGE_WATCHER
              && update_image_kind("rbsp-watch") == UPDATE_IMAGE_WATCHER,
          "a watcher's name, any case (watcher_in's prefix)");
    /* review K2F3: a ShortPath launch shortens the exe name too */
    CHECK(update_image_kind("RBSP-W~1.EXE") == UPDATE_IMAGE_WATCHER && update_image_kind("rbsp-w~2.exe") == UPDATE_IMAGE_WATCHER
              && update_image_kind("RBSTEM~1.EXE") == UPDATE_IMAGE_APP,
          "our exe names' 8.3 forms, any case");
    CHECK(update_image_kind("RBSP-W~") == UPDATE_IMAGE_OTHER && update_image_kind("RBSTE~1.EXE") == UPDATE_IMAGE_OTHER,
          "a bare 8.3 stem or another stem: another program's");
    CHECK(update_image_kind("csrss.exe") == UPDATE_IMAGE_OTHER && update_image_kind("RB Stems Plus.exe.old") == UPDATE_IMAGE_OTHER
              && update_image_kind("RB Stems Plus") == UPDATE_IMAGE_OTHER && update_image_kind("rbsp-watc") == UPDATE_IMAGE_OTHER
              && update_image_kind("") == UPDATE_IMAGE_OTHER && update_image_kind(NULL) == UPDATE_IMAGE_OTHER
              && update_image_kind("R\xc3\x9f Stems Plus.exe") == UPDATE_IMAGE_OTHER,
          "anything else: another program's");
    /* review GH1: a refused open is skipped for ERROR_ACCESS_DENIED (another account's, the
     * system's) and ERROR_INVALID_PARAMETER (gone); any other error leaves it to the name */
    CHECK(update_open_skips(UPDATE_ERR_ACCESS_DENIED) && update_open_skips(5), "ERROR_ACCESS_DENIED (5): skipped");
    CHECK(update_open_skips(UPDATE_ERR_INVALID_PARAMETER) && update_open_skips(87), "ERROR_INVALID_PARAMETER (87): skipped");
    CHECK(!update_open_skips(0) && !update_open_skips(6) && !update_open_skips(8) && !update_open_skips(299) && !update_open_skips(1450)
              && !update_open_skips(0x80000005ul),
          "another error (none, ERROR_INVALID_HANDLE, NOT_ENOUGH_MEMORY, PARTIAL_COPY, NO_SYSTEM_RESOURCES, 5 in a high word): not skipped");
    /* review K2: one of this account's in another logon session (session 0's password-logon task)
     * is refused with ERROR_ACCESS_DENIED, as another account's is. Skipped by the error alone,
     * running_under missed it and the uninstall deleted the app; its path from the system's list
     * now decides */
    {
        const int W = UPDATE_IMAGE_WATCHER, A = UPDATE_IMAGE_APP, O = UPDATE_IMAGE_OTHER;
        CHECK(update_listed_verdict(0, 5, 1, 1, W, 0) == UPDATE_LISTED_FOUND && update_listed_verdict(0, 5, 1, 1, W, 1) == UPDATE_LISTED_FOUND,
              "K2: refused (5), its path in the apps folder: found, for running_under and stop_watchers");
        CHECK(update_listed_verdict(0, 5, 1, 1, O, 0) == UPDATE_LISTED_FOUND, "refused, any name, its path in the folder: found");
        CHECK(update_listed_verdict(0, 5, 1, 0, W, 0) == UPDATE_LISTED_SKIP && update_listed_verdict(0, 5, 1, 0, A, 0) == UPDATE_LISTED_SKIP,
              "GH1: refused (5), our name, its path elsewhere (another account's own copy): never counts");
        CHECK(update_listed_verdict(0, 5, -1, 0, A, 0) == UPDATE_LISTED_UNKNOWN && update_listed_verdict(0, 5, -1, 0, W, 1) == UPDATE_LISTED_UNKNOWN,
              "refused (5), its path unreadable, our name: unknown (the files kept)");
        CHECK(update_listed_verdict(0, 5, -1, 0, O, 0) == UPDATE_LISTED_SKIP && update_listed_verdict(0, 5, -1, 0, A, 1) == UPDATE_LISTED_SKIP,
              "refused, its path unreadable, not our name (an app's name for stop_watchers): skipped");
        CHECK(update_listed_verdict(0, 5, 0, 0, A, 0) == UPDATE_LISTED_SKIP && update_listed_verdict(0, 87, -1, 1, A, 0) == UPDATE_LISTED_SKIP
                  && update_listed_verdict(0, 87, 1, 1, A, 0) == UPDATE_LISTED_SKIP,
              "gone (its path 0, or ERROR_INVALID_PARAMETER): skipped");
        CHECK(update_listed_verdict(0, 8, 1, 0, A, 0) == UPDATE_LISTED_UNKNOWN && update_listed_verdict(0, 8, 1, 1, O, 0) == UPDATE_LISTED_FOUND
                  && update_listed_verdict(0, 8, 1, 0, O, 0) == UPDATE_LISTED_SKIP,
              "another error: in the folder found, else unknown by our name (as before)");
        CHECK(update_listed_verdict(1, 0, 1, 1, O, 0) == UPDATE_LISTED_FOUND && update_listed_verdict(1, 0, 1, 0, A, 0) == UPDATE_LISTED_SKIP,
              "opened: by its path");
        CHECK(update_listed_verdict(1, 0, -1, 0, A, 0) == UPDATE_LISTED_UNKNOWN && update_listed_verdict(1, 0, 0, 0, A, 0) == UPDATE_LISTED_UNKNOWN
                  && update_listed_verdict(1, 0, -1, 0, O, 0) == UPDATE_LISTED_SKIP,
              "opened, its path unreadable: unknown by our name");
        /* review K2's undefined1, undefined2: when a mismatch proves the image is elsewhere */
        CHECK(update_nt_path(1, 1, 1, 0) == 1 && update_nt_path(1, 1, 0, 0) == 1 && update_nt_path(1, 1, 0, -1) == 1,
              "a match: the folder's, with or without the canonical form");
        CHECK(update_nt_path(1, 0, 1, 0) == 0, "a mismatch against the canonical form, no 8.3 component: elsewhere (GH1)");
        CHECK(update_nt_path(1, 0, 0, 0) == -1 && update_nt_path(1, 0, 0, 1) == -1,
              "no canonical form (the folder's final path couldn't be had): a mismatch proves nothing");
        CHECK(update_nt_path(1, 0, 1, -1) == -1, "an 8.3 component that couldn't be resolved: a mismatch proves nothing");
        CHECK(update_nt_path(1, 1, 1, 1) == 1, "an 8.3 component resolved, its final path the folder's: a match");
        CHECK(update_nt_path(1, 0, 1, 1) == 0, "an 8.3 component resolved, its final path elsewhere: elsewhere (GH1)");
        CHECK(update_nt_path(0, 0, 1, 0) == -1 && update_nt_path(0, 1, 1, 0) == -1, "not read: can't be told");
        CHECK(update_listed_verdict(0, 5, update_nt_path(1, 0, 0, 0), 0, W, 1) == UPDATE_LISTED_UNKNOWN
                  && update_listed_verdict(0, 5, update_nt_path(1, 0, 1, -1), 0, A, 0) == UPDATE_LISTED_UNKNOWN,
              "refused (5), a mismatch that proves nothing, our name: unknown (the files kept)");
    }
    CHECK(update_scan_result(0, 0, 1) == 0, "nothing found, nothing unknown, the list read to its end: 0");
    CHECK(update_scan_result(1, 0, 1) == 1 && update_scan_result(1, 1, 0) == 1, "one found: 1");
    CHECK(update_scan_result(0, 1, 1) == -1, "one of ours couldn't be inspected: -1");
    CHECK(update_scan_result(0, 0, 0) == -1, "the list cut short (not ERROR_NO_MORE_FILES): -1");

    /* quoting, read back as Windows reads it */
    static const char *const args[] = {"plain", "with space", "", "a\"b", "a\\b", "a\\\\\"b", "trail\\", "C:\\Program Files\\RB Stems Plus\\",
                                       "tab\there", "--swap", "\"", "\\\\", "x\\\"y"};
    char line[1024] = "";
    for (size_t i = 0; i < sizeof args / sizeof args[0]; i++) {
        if (i) strcat(line, " ");
        CHECK(!update_quote_arg(args[i], line, sizeof line), "quoted: %s", args[i]);
    }
    char back[16][128];
    int argc = argv_parse(line, back, 16);
    int same_all = argc == (int)(sizeof args / sizeof args[0]);
    for (int i = 0; same_all && i < argc; i++) same_all = !strcmp(back[i], args[i]);
    CHECK(same_all, "every argument read back as it was (%d): %s", argc, line);
    char tiny[8] = "";
    CHECK(update_quote_arg("much too long", tiny, sizeof tiny) == -1, "too long: -1");

    /* ranges */
    int64_t total, end;
    CHECK(update_range_start(0, 200, NULL, 1234, &total, &end) == 0 && total == 1234 && end == 1234, "200: from the start");
    CHECK(update_range_start(100, 200, NULL, -1, &total, &end) == 0 && total == -1 && end == -1, "200 to a range request: from scratch");
    CHECK(update_range_start(100, 206, "bytes 100-999/1000", -1, &total, &end) == 100 && total == 1000 && end == 1000, "206: from where it was asked");
    CHECK(update_range_start(100, 206, "bytes 100-999/*", -1, &total, &end) == 100 && total == -1 && end == 1000, "206 with the size unknown: its end known");
    static const char *const badr[] = {"bytes 50-999/1000", "bytes 100-998/1000", "bytes 100-99/1000", "100-999/1000", "bytes 100-999/1000x", "bytes -999/1000", ""};
    for (size_t i = 0; i < sizeof badr / sizeof badr[0]; i++) CHECK(update_range_start(100, 206, badr[i], -1, &total, &end) == -1, "range refused: %s", badr[i]);
    CHECK(update_range_start(0, 206, "bytes 0-9/10", -1, &total, &end) == -1 && update_range_start(100, 416, NULL, -1, &total, &end) == -1,
          "a 206 never asked for, a 416");
}

static void stub(void) {
    CHECK(!wcscmp(stub_args(L"\"C:\\Programs\\RB Stems Plus\\RB Stems Plus.exe\" --watcher"), L"--watcher"), "a quoted name");
    CHECK(!wcscmp(stub_args(L"stub.exe  a \"b c\""), L"a \"b c\""), "an unquoted name, the rest as given");
    CHECK(!wcscmp(stub_args(L"\"a b\"x  y"), L"y") && !wcscmp(stub_args(L"stub.exe"), L"") && !wcscmp(stub_args(L""), L""), "edges");
    CHECK(stub_mode(L"--watcher") == STUB_WATCHER && stub_mode(L"--watcher  ") == STUB_WATCHER, "the watcher");
    CHECK(stub_mode(L"") == STUB_APP && stub_mode(L"--watcherx") == STUB_APP && stub_mode(L"--watcher x") == STUB_APP && stub_mode(L"x --watcher") == STUB_APP,
          "anything else: the app");
    CHECK(stub_mode(L"--after-exit 1a \"C:\\U\"") == STUB_AFTER_EXIT && stub_mode(L"--after-exit\t1") == STUB_AFTER_EXIT, "the after-exit copy");
    CHECK(stub_after_exit_dir(L"rbsp-uninstall-0001abcdef1020ff"), "the temp folder's name");
    CHECK(!stub_after_exit_dir(L"rbsp-uninstall-0001ABCDEF1020FF") && !stub_after_exit_dir(L"rbsp-uninstall-0001abcdef1020f")
              && !stub_after_exit_dir(L"rbsp-uninstall-0001abcdef1020ff0") && !stub_after_exit_dir(L"rbsp-uninstall-0001abcdef1020fg")
              && !stub_after_exit_dir(L"RB Stems Plus") && !stub_after_exit_dir(L"Rbsp-uninstall-0001abcdef1020ff") && !stub_after_exit_dir(L"")
              && !stub_after_exit_dir(NULL),
          "anything else: no after-exit (review UN1)");
    CHECK(stub_mode(L"--after-exit") == STUB_APP && stub_mode(L"--after-exitx 1") == STUB_APP && stub_mode(L" --after-exit 1") == STUB_APP,
          "--after-exit alone, glued or not first: the app");
    wchar_t t[512], line[512];
    CHECK(!stub_target(L"C:\\P\\RB Stems Plus", L"1.5.0", STUB_APP, L"x64", t, 512) && !wcscmp(t, L"C:\\P\\RB Stems Plus\\1.5.0\\RB Stems Plus.exe"), "the app");
    CHECK(!stub_target(L"C:\\P", L"1.5.0", STUB_WATCHER, L"arm64", t, 512) && !wcscmp(t, L"C:\\P\\1.5.0\\rbsp-watch-arm64.exe"), "the watcher of the arch");
    CHECK(stub_target(L"C:\\P", L"1.5.0", STUB_APP, L"x64", t, 20) == -1, "too long: -1");
    CHECK(!stub_cmdline(L"C:\\a b\\x.exe", L"--swap \"q r\"", line, 512) && !wcscmp(line, L"\"C:\\a b\\x.exe\" --swap \"q r\""), "the child's line");
    CHECK(!stub_cmdline(L"C:\\x.exe", L"", line, 512) && !wcscmp(line, L"\"C:\\x.exe\""), "no arguments");
    CHECK(stub_cmdline(L"C:\\x\".exe", L"", line, 512) == -1 && stub_cmdline(L"C:\\x.exe", L"a", line, 10) == -1, "a quote in the path, too long");
}

/* the shell's entries' pure parts (WU11, WU7) */
static void shell_parts(void) {
    char o[64];
    CHECK(!update_apps_of("C:\\U\\Programs\\RB Stems Plus\\1.5.3", o, sizeof o) && !strcmp(o, "C:\\U\\Programs\\RB Stems Plus"), "the apps folder: the parent");
    CHECK(!update_apps_of("C:\\P\\1.5.3\\\\", o, sizeof o) && !strcmp(o, "C:\\P"), "trailing separators trimmed first");
    CHECK(update_apps_of("1.5.3", o, sizeof o) == -1 && update_apps_of("\\1.5.3", o, sizeof o) == -1 && update_apps_of("", o, sizeof o) == -1,
          "no parent: -1");
    CHECK(update_apps_of("C:\\a-long-parent\\1.5.3", o, 8) == -1, "too long: -1");
    CHECK(!strcmp(update_arch_of(0xaa64, 0), "arm64") && !strcmp(update_arch_of(0x8664, 1), "x64"), "the native machine decides, not the build");
    CHECK(!strcmp(update_arch_of(0, 0), "x64") && !strcmp(update_arch_of(0, 1), "arm64") && !strcmp(update_arch_of(0x14c, 1), "arm64"),
          "unknown: the build's own");
    CHECK(update_swap_answer(0) == 0 && update_swap_answer(1) == 1 && update_swap_answer(ETIMEDOUT) == 2, "update_swap_wait's answers");
}

void test_update_rules(void) {
    pure();
    stub();
    shell_parts();
}
