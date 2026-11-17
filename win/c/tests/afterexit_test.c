/* afterexit.c on the Mac (release.md T11): the copy's command line and its parsing, the folder rules,
 * the temp name, cmd's and RunOnce's lines, and ax_run's order on a fake OS over a real folder
 * tree (the UsageLogs files through plat_mac.c). */
#include "check.h"

#include "../afterexit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void cmdline_and_parse(void) {
    char o[512];
    CHECK(!ax_cmdline("/T/rbsp-uninstall-00/RB Stems Plus.exe", 0x1a4, "/U/Local", o, sizeof o)
              && !strcmp(o, "\"/T/rbsp-uninstall-00/RB Stems Plus.exe\" --after-exit 1a4 \"/U/Local\""),
          "the copy's line: %s", o);
    CHECK(ax_cmdline("/T/x\".exe", 1, "/U", o, sizeof o) == -1 && ax_cmdline("/T/x.exe", 1, "/U\"", o, sizeof o) == -1, "a quote refused");
    CHECK(ax_cmdline("/T/x.exe", 0, "/U", o, sizeof o) == -1 && ax_cmdline("/T/x.exe", 1, "U", o, sizeof o) == -1, "no handle, a relative local");
    CHECK(ax_cmdline("/T/x.exe", 1, "/U", o, 10) == -1, "too long");

    ax_args a;
    CHECK(!ax_parse("--after-exit 1a4 \"/U/Local\"", &a) && a.parent == 0x1a4 && !strcmp(a.local, "/U/Local")
              && !strcmp(a.apps, "/U/Local/Programs/RB Stems Plus"),
          "parsed: %llx %s %s", (unsigned long long)a.parent, a.local, a.apps);
    CHECK(!ax_parse("  --after-exit\tFFFFFFFFFFFFFFFF  \"/U/L/\"  ", &a) && a.parent == UINT64_MAX && !strcmp(a.local, "/U/L"),
          "spaces, a 16-digit handle, a trailing separator trimmed: %s", a.local);
    static const char *const bad[] = {
        "",
        "--after-exit",
        "--after-exitx 1 \"/U\"",
        "--watcher 1 \"/U\"",
        "--after-exit 0 \"/U\"",
        "--after-exit 1FFFFFFFFFFFFFFFF \"/U\"",
        "--after-exit 1g \"/U\"",
        "--after-exit 1 /U",
        "--after-exit 1 \"/U",
        "--after-exit 1 \"U\"",
        "--after-exit 1 \"\"",
        "--after-exit 1 \"/U/../V\"",
        "--after-exit 1 \"/U\" extra",
        "--after-exit \"/U\"",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(ax_parse(bad[i], &a) == -1 && a.parent == 0 && !a.apps[0], "refused: %s", bad[i]);
    }
    CHECK(ax_parse(NULL, &a) == -1, "NULL refused");
}

static void folders(void) {
    char o[256];
    CHECK(!ax_local_of("/U/Local/Programs/RB Stems Plus", o, sizeof o) && !strcmp(o, "/U/Local"), "local from apps: %s", o);
    CHECK(!ax_local_of("/U/L/programs/rb stems plus/", o, sizeof o) && !strcmp(o, "/U/L"), "case and a trailing separator: %s", o);
    CHECK(ax_local_of("/U/L/Programs/RB Stems Plus2", o, sizeof o) == -1 && ax_local_of("/U/L/Other/RB Stems Plus", o, sizeof o) == -1
              && ax_local_of("Programs/RB Stems Plus", o, sizeof o) == -1 && ax_local_of("/Programs/RB Stems Plus", o, sizeof o) == -1
              && ax_local_of(NULL, o, sizeof o) == -1,
          "anything else refused");
    CHECK(ax_local_of("/U/Local/Programs/RB Stems Plus", o, 4) == -1, "too long");

    const char *apps = "C:\\U\\AppData\\Local\\Programs\\RB Stems Plus";
    CHECK(ax_under("C:\\U\\AppData\\Local\\Programs\\RB Stems Plus\\1.5.0\\RB Stems Plus.exe", apps), "the app under the folder");
    CHECK(ax_under("c:\\u\\appdata\\local\\programs\\rb stems plus\\1.5.0\\rbsp-watch-x64.exe", apps), "ASCII case ignored");
    CHECK(ax_under("\\\\?\\C:\\U\\AppData\\Local\\Programs\\RB Stems Plus\\1.5.0\\x.exe", apps), "a \\\\?\\ prefix ignored");
    CHECK(ax_under("C:/U/AppData/Local/Programs/RB Stems Plus/x.exe", "C:\\U\\AppData\\Local\\Programs\\RB Stems Plus\\\\"), "'/' and a trailing separator");
    CHECK(!ax_under("C:\\U\\AppData\\Local\\Programs\\RB Stems Plus2\\x.exe", apps), "a sibling with the same start");
    CHECK(!ax_under(apps, apps) && !ax_under("C:\\U\\AppData\\Local\\Programs\\RB Stems Plus\\", apps), "the folder itself");
    CHECK(!ax_under("D:\\Other\\RB Stems Plus.exe", apps) && !ax_under("C:\\U\\AppData\\Local\\Programs\\RB Stems Plus.exe", apps),
          "the same image name elsewhere: never ours (I78, by full path)");
    CHECK(!ax_under("C:\\U\\AppData\\Local\\Programs\\RB Stems Plus\\\\x.exe", apps) && !ax_under(NULL, apps) && !ax_under("x", ""),
          "an empty part, NULL, an empty folder");

    /* the %LOCALAPPDATA% given against plat_home's (review UN11) */
    CHECK(ax_same_dir("C:\\Users\\x\\AppData\\Local", "c:\\users\\X\\appdata\\local\\") && ax_same_dir("C:/Users/x/AppData/Local", "C:\\Users\\x\\AppData\\Local")
              && ax_same_dir("/U/L", "/U/L//"),
          "the same folder: case, '/' and trailing separators ignored");
    CHECK(!ax_same_dir("C:\\Users\\x\\AppData\\Local", "C:\\Users\\y\\AppData\\Local") && !ax_same_dir("C:\\Users\\x\\AppData\\Local2", "C:\\Users\\x\\AppData\\Local")
              && !ax_same_dir("D:\\Users\\x\\AppData\\Local", "C:\\Users\\x\\AppData\\Local") && !ax_same_dir("/U/L", "/U/L/x") && !ax_same_dir(NULL, "/U")
              && !ax_same_dir("/U", "") && !ax_same_dir("C:\\Users\\x\\AppData\\Local", "C:\\Users\\x\\AppData\\Locál"),
          "another folder: never the same");

    unsigned char rnd[8] = {0x00, 0x01, 0xab, 0xcd, 0xef, 0x10, 0x20, 0xff};
    CHECK(!ax_temp_name(rnd, o, sizeof o) && !strcmp(o, "rbsp-uninstall-0001abcdef1020ff"), "the temp name: %s", o);
    CHECK(ax_temp_name(rnd, o, 10) == -1, "too long");
    CHECK(!ax_temp_name(rnd, o, sizeof o) && ax_is_temp_name(o), "the temp name is one (review UN1)");
    static const char *const not_temp[] = {
        "rbsp-uninstall-0001ABCDEF1020FF", "rbsp-uninstall-0001abcdef1020f", "rbsp-uninstall-0001abcdef1020ff0", "rbsp-uninstall-0001abcdef1020fg",
        "rbsp-uninstall-", "Rbsp-uninstall-0001abcdef1020ff", "RB Stems Plus", "x\\rbsp-uninstall-0001abcdef1020ff", "", NULL,
    };
    for (size_t i = 0; i < sizeof not_temp / sizeof not_temp[0]; i++)
        CHECK(!ax_is_temp_name(not_temp[i]), "not the temp name: %s", not_temp[i] ? not_temp[i] : "NULL");
}

static void cmd_lines(void) {
    char o[1024];
    const char *n = "rbsp-uninstall-0001abcdef1020ff";
    CHECK(!ax_cleanup_args("/W/System32", n, o, sizeof o)
              && !strcmp(o, "/d /e:on /v:off /q /s /c \"for /l %i in (1,1,60) do @(del /f /q \"rbsp-uninstall-0001abcdef1020ff\\RB Stems Plus.exe\" "
                            "\"rbsp-uninstall-0001abcdef1020ff\\rbsp-core.dll\" 2>nul & rd /q \"rbsp-uninstall-0001abcdef1020ff\" 2>nul & (if not exist "
                            "\"rbsp-uninstall-0001abcdef1020ff\\\" exit /b 0) & \"/W/System32\\PING.EXE\" -n 2 -w 1000 127.0.0.1 >nul)\""),
          "cmd's arguments (reviews UN1, UN6, UN14): the relative name, the two files by name, no /s for rd, PING.EXE by its path: %s", o);
    CHECK(!strstr(o, "rd /s") && strstr(o, "/e:on /v:off"), "no recursive rd; extensions on, delayed expansion off");
    CHECK(strstr(o, "(if not exist") && strstr(o, "exit /b 0) & "),
          "the IF grouped: the PING waits on every try, the exit only once the folder is gone (review UQ1)");
    CHECK(!ax_cleanup_args("/W/Sys tem (x86)&", n, o, sizeof o), "spaces, & and brackets in sys are quoted: fine");
    static const char *const bad_sys[] = {"/W/a%b", "/W/a\"b", "/W/a!b", "/W/a^b", "/W/a\nb", "W/rel", "", NULL};
    for (size_t i = 0; i < sizeof bad_sys / sizeof bad_sys[0]; i++)
        CHECK(ax_cleanup_args(bad_sys[i], n, o, sizeof o) == -1, "sys refused: %s", bad_sys[i] ? bad_sys[i] : "NULL");
    static const char *const bad_name[] = {"/T/rbsp-uninstall-0001abcdef1020ff", "rbsp-uninstall-0001abcdef1020f", "..", "a\" & del x & \"", "", NULL};
    for (size_t i = 0; i < sizeof bad_name / sizeof bad_name[0]; i++)
        CHECK(ax_cleanup_args("/W/System32", bad_name[i], o, sizeof o) == -1, "name refused: %s", bad_name[i] ? bad_name[i] : "NULL");
    CHECK(ax_cleanup_args("/W/System32", n, o, 40) == -1, "too long");
    CHECK(!ax_runonce_data("C:\\Windows\\System32\\cmd.exe", "/T/x", o, sizeof o)
              && !strcmp(o, "\"C:\\Windows\\System32\\cmd.exe\" /d /q /s /c \"rd /s /q \"/T/x\"\""),
          "RunOnce's data: %s", o);
    CHECK(!ax_runonce_name("rbsp-uninstall-0001abcdef1020ff", o, sizeof o) && !strcmp(o, "RB Stems Plus cleanup 0001abcdef1020ff"),
          "RunOnce's value name, one per temp folder (review UN13): %s", o);
    CHECK(ax_runonce_name("rbsp-uninstall-0001abcdef1020f", o, sizeof o) == -1 && ax_runonce_name(NULL, o, sizeof o) == -1
              && ax_runonce_name("rbsp-uninstall-0001abcdef1020ff", o, 30) == -1,
          "the value name: only for a temp folder's name, and it must fit");
    /* review UQ5: the 259-unit cap and the UNC working folder, as pure rules */
    char cap[600];
    memset(cap, 'a', 259);
    cap[259] = 0;
    CHECK(ax_runonce_fits(cap), "259 units: runs");
    cap[259] = 'a';
    cap[260] = 0;
    CHECK(!ax_runonce_fits(cap), "260 units: never runs");
    memset(cap, 0, sizeof cap);
    for (int i = 0; i < 129; i++) memcpy(cap + 4 * i, "\xf0\x9f\x98\x80", 4);   /* 129 surrogate pairs: 258 units */
    CHECK(ax_runonce_fits(cap), "258 units of surrogate pairs: runs");
    memcpy(cap + 4 * 129, "\xf0\x9f\x98\x80", 4);
    CHECK(!ax_runonce_fits(cap), "260 units of surrogate pairs: never runs");
    memset(cap, 0, sizeof cap);
    for (int i = 0; i < 259; i++) memcpy(cap + 2 * i, "\xc3\xa9", 2);   /* 259 two-byte characters: 259 units */
    CHECK(ax_runonce_fits(cap), "259 two-byte characters: runs (units, not bytes)");
    CHECK(!ax_runonce_fits("\xff") && !ax_runonce_fits("\xc3") && !ax_runonce_fits(NULL), "not UTF-8: refused");
    /* review U52: strict UTF-8 */
    CHECK(!ax_runonce_fits("\xc0\x80") && !ax_runonce_fits("\xc1\xbf") && !ax_runonce_fits("\xe0\x80\x80") && !ax_runonce_fits("\xed\xa0\x80")
              && !ax_runonce_fits("\xf0\x80\x80\x80") && !ax_runonce_fits("\xf4\x90\x80\x80") && !ax_runonce_fits("\xf5\x80\x80\x80"),
          "overlong, surrogate, past U+10FFFF: refused");
    CHECK(ax_runonce_fits("\xc2\x80") && ax_runonce_fits("\xe0\xa0\x80") && ax_runonce_fits("\xed\x9f\xbf") && ax_runonce_fits("\xee\x80\x80")
              && ax_runonce_fits("\xf0\x90\x80\x80") && ax_runonce_fits("\xf4\x8f\xbf\xbf"),
          "the first and last of each range: runs");
    CHECK(!ax_cmd_cwd_ok("\\\\server\\share\\t\\") && !ax_cmd_cwd_ok("//server/share/") && !ax_cmd_cwd_ok("") && !ax_cmd_cwd_ok(NULL),
          "a UNC parent (or none): RunOnce instead of cmd (review UQ4)");
    CHECK(ax_cmd_cwd_ok("C:\\Users\\x\\AppData\\Local\\Temp\\") && ax_cmd_cwd_ok("/T/"), "a local parent: cmd");
    CHECK(ax_runonce_data("C:\\c\"md.exe", "/T/x", o, sizeof o) == -1 && ax_runonce_data("cmd.exe", "/T/%x", o, sizeof o) == -1
              && ax_runonce_data("cmd.exe", "/T/!x", o, sizeof o) == -1,
          "refused (a %%TEMP%% with \" %% ! ^ gets no RunOnce fallback: documented)");
}

/* ------------------------------------------------------------------ ax_run on a fake OS */

typedef struct {
    char calls[4096];
    int not_self;           /* self_ok answers 0 */
    const char *home;       /* home's answer (run: the local given when NULL); "-" none */
    int parent_exits;       /* wait_parent: 0 exited */
    int running;            /* running_under's answer while now < running_until */
    uint64_t running_until; /* ms */
    int remove_rc, run_rc, records_left, clean_rc;
    int logon_rc;           /* clean_at_logon's answer: 1 registered when 0 */
    uint64_t now;
    char usage[1200];       /* our CLR_v4.0 UsageLogs entry: remove notes whether it is still there */
    char lines[8192];
} fake;

static void add(fake *f, const char *s) {
    strncat(f->calls, s, sizeof f->calls - strlen(f->calls) - 1);
    strncat(f->calls, ";", sizeof f->calls - strlen(f->calls) - 1);
}
static const char *base_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static int exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}
static int f_self(void *ctx) {
    fake *f = ctx;
    add(f, "self");
    return !f->not_self;
}
static int f_home(void *ctx, char *out, size_t n) {
    fake *f = ctx;
    if (!f->home || !strcmp(f->home, "-")) return -1;
    snprintf(out, n, "%s", f->home);
    return 0;
}
static int f_wait(void *ctx, uint64_t parent, uint32_t ms) {
    fake *f = ctx;
    char b[64];
    snprintf(b, sizeof b, "wait %llx %u", (unsigned long long)parent, ms);
    add(f, b);
    return f->parent_exits;
}
static int f_stop(void *ctx, const char *dir) {
    fake *f = ctx;
    char b[300];
    snprintf(b, sizeof b, "stop %.250s", base_of(dir));
    add(f, b);
    return !strcmp(base_of(dir), "1.6.0");
}
static int f_running(void *ctx, const char *dir) {
    fake *f = ctx;
    (void)dir;
    if (!strstr(f->calls, "running;")) add(f, "running");
    return f->now < f->running_until ? f->running : 0;
}
static int f_remove(void *ctx, const char *path) {
    fake *f = ctx;
    char b[300];
    snprintf(b, sizeof b, "remove %s%s", base_of(path), f->usage[0] && exists(f->usage) ? " (UsageLogs entry there)" : "");
    add(f, b);
    return f->remove_rc;
}
static int f_clean(void *ctx) {
    fake *f = ctx;
    add(f, "clean");
    return f->clean_rc;
}
static int f_run(void *ctx) {
    fake *f = ctx;
    add(f, "run");
    return f->run_rc;
}
static int f_records(void *ctx) {
    fake *f = ctx;
    add(f, "records");
    return f->records_left;
}
static uint64_t f_now(void *ctx) { return ((fake *)ctx)->now; }
static void f_sleep(void *ctx, uint32_t ms) { ((fake *)ctx)->now += ms; }
static void f_log(void *ctx, const char *line) {
    fake *f = ctx;
    strncat(f->lines, line, sizeof f->lines - strlen(f->lines) - 1);
    strncat(f->lines, "\n", sizeof f->lines - strlen(f->lines) - 1);
}

static void put(const char *path, const char *text) {
    FILE *fp = fopen(path, "w");
    if (fp) {
        fputs(text, fp);
        fclose(fp);
    }
}

/* scratch/local: Programs/RB Stems Plus/{current, 1.5.0/, 1.6.0/}, the two UsageLogs folders */
static void tree(const char *local) {
    char p[1024];
    mkdir(local, 0755);
    snprintf(p, sizeof p, "%s/Programs", local);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Programs/RB Stems Plus", local);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Programs/RB Stems Plus/1.5.0", local);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Programs/RB Stems Plus/1.6.0", local);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Programs/RB Stems Plus/current", local);
    put(p, "1.6.0\n");
    snprintf(p, sizeof p, "%s/Microsoft", local);
    mkdir(p, 0755);
    for (int i = 0; i < 2; i++) {
        const char *clr = i ? "CLR_v4.0_32" : "CLR_v4.0";
        snprintf(p, sizeof p, "%s/Microsoft/%s", local, clr);
        mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/Microsoft/%s/UsageLogs", local, clr);
        mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/Microsoft/%s/UsageLogs/Other.exe.log", local, clr);
        put(p, "theirs");
    }
    snprintf(p, sizeof p, "%s/Microsoft/CLR_v4.0/UsageLogs/RB Stems Plus.exe.log", local);
    put(p, "ours");
    /* CLR_v4.0_32's is a link: never followed, left */
    char target[1024];
    snprintf(target, sizeof target, "%s/Microsoft/CLR_v4.0_32/UsageLogs/Other.exe.log", local);
    snprintf(p, sizeof p, "%s/Microsoft/CLR_v4.0_32/UsageLogs/RB Stems Plus.exe.log", local);
    unlink(p);
    (void)symlink(target, p);
}

static void f_kept(void *ctx) { add(ctx, "kept dialog"); }
static int f_logon(void *ctx) {
    add(ctx, "logon");
    return ((fake *)ctx)->logon_rc ? ((fake *)ctx)->logon_rc : 1;
}
/* how many times the kept dialog was shown */
static int kept_shown(const fake *f) {
    int n = 0;
    for (const char *p = f->calls; (p = strstr(p, "kept dialog;")); p++) n++;
    return n;
}
static int run(fake *f, const char *local, ax_args *a) {
    char line[1200];
    snprintf(line, sizeof line, "--after-exit 2c \"%s\"", local);
    if (ax_parse(line, a)) return -1;
    if (!f->home) f->home = local;
    snprintf(f->usage, sizeof f->usage, "%s/Microsoft/CLR_v4.0/UsageLogs/RB Stems Plus.exe.log", local);
    ax_os os = {f_self, f_home, f_wait, f_stop, f_running, f_remove, f_clean, f_run, f_records, f_now, f_sleep, f_log, f_logon, f_kept, f};
    return ax_run(&os, a);
}

/* the calls' positions, in this order: 1 when each is there after the one before */
static int calls_in_order(const fake *f, int n, ...) {
    va_list ap;
    va_start(ap, n);
    const char *p = f->calls;
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        const char *c = va_arg(ap, const char *);
        const char *q = strstr(p, c);
        if (!q) ok = 0;
        else p = q + strlen(c);
    }
    va_end(ap);
    return ok;
}

static void order(const char *scratch) {
    char local[1024], p[1200], q[1200];
    snprintf(local, sizeof local, "%s/local", scratch);
    tree(local);
    ax_args a;
    fake f;

    /* the whole after-exit, in order (review UN4: the Run value first, the other records last) */
    memset(&f, 0, sizeof f);
    int r = run(&f, local, &a);
    CHECK(r == AX_DONE, "done: %d", r);
    CHECK(kept_shown(&f) == 0 && !strstr(f.calls, "logon;"), "a full uninstall: no kept dialog, no logon cleanup: %s", f.calls);
    CHECK(!strncmp(f.calls, "self;wait 2c 120000;run;", 24), "its own folder checked, the app waited for, then the Run value: %s", f.calls);
    CHECK(strstr(f.calls, "stop 1.5.0;") && strstr(f.calls, "stop 1.6.0;") && !strstr(f.calls, "stop current"), "each version folder's watchers: %s", f.calls);
    CHECK(calls_in_order(&f, 6, "run;", "stop ", "running;", "remove RB Stems Plus;", "records;", "clean;"),
          "the Run value, the watchers, the others, the folder (after the UsageLogs entry), the other records, the temp folder: %s", f.calls);
    CHECK(!exists(f.usage), "our UsageLogs entry deleted");
    snprintf(p, sizeof p, "%s/Microsoft/CLR_v4.0/UsageLogs/Other.exe.log", local);
    CHECK(exists(p), "another program's entry kept");
    snprintf(p, sizeof p, "%s/Microsoft/CLR_v4.0_32/UsageLogs/RB Stems Plus.exe.log", local);
    snprintf(q, sizeof q, "%s/Microsoft/CLR_v4.0_32/UsageLogs/Other.exe.log", local);
    CHECK(exists(p) && exists(q), "a link named as our entry: left, its target untouched");
    CHECK(strstr(f.lines, "the watcher in 1.6.0 stopped") != NULL, "the stopped watcher logged: %s", f.lines);

    /* the app doesn't quit: nothing removed, the temp folder still set to go */
    tree(local);
    memset(&f, 0, sizeof f);
    f.parent_exits = 1;
    r = run(&f, local, &a);
    CHECK(r == AX_PARENT_RUNS && !strcmp(f.calls, "self;wait 2c 120000;clean;") && exists(f.usage), "the app still runs: only the temp folder: %s", f.calls);

    /* a program keeps running from the folder: waited for 60 s at most, then the app's files, the
     * Apps & features entry (the records) and the UsageLogs entry all stay, so the uninstall can
     * be run again (owner 2026-11-13); the temp folder still goes */
    memset(&f, 0, sizeof f);
    f.running = 1;
    f.running_until = UINT64_MAX;
    r = run(&f, local, &a);
    CHECK(r == AX_LEFT && f.now >= AX_OTHERS_WAIT_MS && f.now < AX_OTHERS_WAIT_MS + 2 * AX_POLL_MS && exists(f.usage),
          "bounded at %llu ms, the UsageLogs entry kept: %s", (unsigned long long)f.now, f.calls);
    CHECK(!strstr(f.calls, "remove ") && !strstr(f.calls, "records;") && strstr(f.calls, "clean;"),
          "still running after the wait: the folder and the records kept, the temp folder set to go: %s", f.calls);
    CHECK(strstr(f.lines, "the Apps & features entry and the UsageLogs entry kept; uninstall again once it is closed") != NULL,
          "what was kept said: %s", f.lines);
    CHECK(kept_shown(&f) == 1 && calls_in_order(&f, 3, "logon;", "kept dialog;", "clean;"),
          "kept for a program still running: the logon cleanup set, then the kept dialog once, before the temp folder's cleanup (owner 2026-11-16): %s",
          f.calls);
    CHECK(!strstr(f.lines, "at the next logon"), "the logon cleanup set: nothing said: %s", f.lines);
    /* ... the logon cleanup can't be set: said, the dialog and the cleanup still run */
    memset(&f, 0, sizeof f);
    f.running = 1;
    f.running_until = UINT64_MAX;
    f.logon_rc = -1;
    r = run(&f, local, &a);
    CHECK(r == AX_LEFT && strstr(f.lines, "couldn't be set to go at the next logon") && kept_shown(&f) == 1
              && calls_in_order(&f, 3, "logon;", "kept dialog;", "clean;"),
          "no logon cleanup: logged, the dialog and the temp folder's cleanup still run: %s / %s", f.calls, f.lines);
    /* ... one that ends in time */
    memset(&f, 0, sizeof f);
    f.running = 1;
    f.running_until = 1000;
    r = run(&f, local, &a);
    CHECK(r == AX_DONE && f.now >= 1000 && f.now < 1000 + AX_POLL_MS && !exists(f.usage), "waited until it ended: %llu", (unsigned long long)f.now);
    CHECK(kept_shown(&f) == 0, "ended in time: no kept dialog: %s", f.calls);
    /* the list can't be read: waited on as one that runs (UN4) */
    tree(local);
    memset(&f, 0, sizeof f);
    f.running = -1;
    f.running_until = UINT64_MAX;
    r = run(&f, local, &a);
    CHECK(r == AX_LEFT && f.now >= AX_OTHERS_WAIT_MS && exists(f.usage) && strstr(f.lines, "the process list couldn't be read: waiting")
              && strstr(f.lines, "still can't be read"),
          "the list unreadable: the 60 s, AX_LEFT, the UsageLogs entry kept: %d %llu %s", r, (unsigned long long)f.now, f.lines);
    CHECK(!strstr(f.calls, "remove ") && !strstr(f.calls, "records;") && strstr(f.calls, "clean;"),
          "the list still unreadable (an uninspectable process of ours, review UQ's GPT re-run G2): the folder and the records kept: %s", f.calls);
    CHECK(kept_shown(&f) == 1 && calls_in_order(&f, 3, "logon;", "kept dialog;", "clean;"),
          "kept for a list still unknown: the logon cleanup set, then the kept dialog once: %s", f.calls);
    /* ... until it can be read and nothing runs */
    memset(&f, 0, sizeof f);
    f.running = -1;
    f.running_until = 2000;
    r = run(&f, local, &a);
    CHECK(r == AX_DONE && f.now >= 2000 && !exists(f.usage), "the list readable again, nothing running: done");

    /* what stays */
    memset(&f, 0, sizeof f);
    f.remove_rc = EBUSY;
    CHECK(run(&f, local, &a) == AX_LEFT && kept_shown(&f) == 0, "a held file: left, no kept dialog (nothing ran): %s", f.calls);
    /* review GH6: a partial delete that leaves the app able to start (the stub, current and
     * current's exe there) keeps the Apps & features entry and the other records; one that broke
     * it removes them */
    snprintf(p, sizeof p, "%s/Programs/RB Stems Plus/RB Stems Plus.exe", local);
    snprintf(q, sizeof q, "%s/Programs/RB Stems Plus/1.6.0/RB Stems Plus.exe", local);
    put(p, "stub");
    put(q, "app");
    memset(&f, 0, sizeof f);
    f.remove_rc = EBUSY;
    CHECK(run(&f, local, &a) == AX_LEFT && strstr(f.calls, "remove RB Stems Plus;") && !strstr(f.calls, "records;") &&
              strstr(f.lines, "the Apps & features entry kept") && kept_shown(&f) == 0 && !strstr(f.calls, "logon;"),
          "partial delete, the app still starts: the records kept, no kept dialog: %s", f.calls);
    unlink(q);
    memset(&f, 0, sizeof f);
    f.remove_rc = EBUSY;
    CHECK(run(&f, local, &a) == AX_LEFT && strstr(f.calls, "records;") && !strstr(f.lines, "entry kept") && kept_shown(&f) == 0
              && !strstr(f.calls, "logon;"),
          "partial delete, current's exe gone: the records go, no kept dialog: %s", f.calls);
    /* review UF3: a check that can't tell (the apps folder can't be opened: EACCES) keeps them */
    char apps_dir[1100];
    snprintf(apps_dir, sizeof apps_dir, "%s/Programs/RB Stems Plus", local);
    put(q, "app");
    CHECK(chmod(apps_dir, 0) == 0, "the apps folder made unreadable");
    memset(&f, 0, sizeof f);
    f.remove_rc = EBUSY;
    r = run(&f, local, &a);
    chmod(apps_dir, 0755);
    CHECK(r == AX_LEFT && !strstr(f.calls, "records;") && strstr(f.lines, "entry kept") && kept_shown(&f) == 0 && !strstr(f.calls, "logon;"),
          "partial delete, the check can't tell: the records kept, no kept dialog: %s", f.calls);
    unlink(q);
    unlink(p);
    memset(&f, 0, sizeof f);
    f.remove_rc = ENOENT;
    CHECK(run(&f, local, &a) == AX_DONE, "nothing there: done");
    memset(&f, 0, sizeof f);
    f.records_left = 2;
    CHECK(run(&f, local, &a) == AX_LEFT && strstr(f.calls, "remove RB Stems Plus;"), "records left: left, the files still go");
    memset(&f, 0, sizeof f);
    f.run_rc = EACCES;
    CHECK(run(&f, local, &a) == AX_LEFT && strstr(f.calls, "remove RB Stems Plus;") && strstr(f.lines, "the Run value couldn't be removed"),
          "the Run value stays: left, the rest still goes");
    memset(&f, 0, sizeof f);
    f.clean_rc = 1;
    CHECK(run(&f, local, &a) == AX_DONE && strstr(f.lines, "next logon (RunOnce)"), "the RunOnce fallback said: %s", f.lines);
    memset(&f, 0, sizeof f);
    f.clean_rc = -1;
    CHECK(run(&f, local, &a) == AX_DONE && strstr(f.lines, "couldn't be set to go"), "no cleanup said");

    /* not its own temp folder (UN1), not this account's LocalAppData (UN11): nothing at all, not
     * even the cleanup */
    tree(local);
    memset(&f, 0, sizeof f);
    f.not_self = 1;
    CHECK(run(&f, local, &a) == AX_BAD_ARGS && !strcmp(f.calls, "self;") && exists(f.usage), "not its own folder: nothing done: %s", f.calls);
    CHECK(kept_shown(&f) == 0 && !strstr(f.calls, "logon;"), "the temp folder refusal: no kept dialog");
    memset(&f, 0, sizeof f);
    snprintf(p, sizeof p, "%s/other", scratch);
    f.home = p;
    CHECK(run(&f, local, &a) == AX_BAD_ARGS && !strcmp(f.calls, "self;") && exists(f.usage) && strstr(f.lines, "isn't this account's LocalAppData"),
          "a foreign LocalAppData: refused, nothing asked: %s", f.calls);
    CHECK(kept_shown(&f) == 0 && !strstr(f.calls, "logon;"), "the LocalAppData refusal: no kept dialog");
    memset(&f, 0, sizeof f);
    f.home = "-";
    CHECK(run(&f, local, &a) == AX_BAD_ARGS && !strcmp(f.calls, "self;"), "no home: refused");
    memset(&f, 0, sizeof f);
    snprintf(p, sizeof p, "%s/", local);
    f.home = p;
    CHECK(run(&f, local, &a) == AX_DONE, "the home with a trailing separator: the same");

    /* arguments that don't hang together: nothing asked */
    memset(&f, 0, sizeof f);
    f.home = local;
    ax_os os = {f_self, f_home, f_wait, f_stop, f_running, f_remove, f_clean, f_run, f_records, f_now, f_sleep, f_log, f_logon, f_kept, &f};
    ax_args b = a;
    snprintf(b.apps, sizeof b.apps, "%s/Programs/Other", local);
    CHECK(ax_run(&os, &b) == AX_BAD_ARGS && !f.calls[0], "an apps folder that isn't local's: refused, nothing asked");
    b = a;
    b.parent = 0;
    CHECK(ax_run(&os, &b) == AX_BAD_ARGS && !f.calls[0] && ax_run(NULL, &a) == AX_BAD_ARGS, "no handle, no ops");
}

/* review UN3: a link anywhere on the way to UsageLogs is never followed */
static void usage_links(const char *scratch) {
    char local[1024], p[1200], q[1200], other[1024];
    ax_args a;
    fake f;
    snprintf(other, sizeof other, "%s/elsewhere", scratch);
    mkdir(other, 0755);
    snprintf(p, sizeof p, "%s/UsageLogs", other);
    mkdir(p, 0755);
    snprintf(q, sizeof q, "%s/UsageLogs/RB Stems Plus.exe.log", other);

    /* Microsoft\CLR_v4.0 a link to another folder holding UsageLogs\RB Stems Plus.exe.log */
    snprintf(local, sizeof local, "%s/local-clr-link", scratch);
    tree(local);
    snprintf(p, sizeof p, "%s/Microsoft/CLR_v4.0/UsageLogs/RB Stems Plus.exe.log", local);
    unlink(p);
    snprintf(p, sizeof p, "%s/Microsoft/CLR_v4.0/UsageLogs/Other.exe.log", local);
    unlink(p);
    snprintf(p, sizeof p, "%s/Microsoft/CLR_v4.0/UsageLogs", local);
    rmdir(p);
    snprintf(p, sizeof p, "%s/Microsoft/CLR_v4.0", local);
    rmdir(p);
    (void)symlink(other, p);
    put(q, "not ours to delete");
    memset(&f, 0, sizeof f);
    int r = run(&f, local, &a);
    CHECK(r == AX_DONE && exists(q) && strstr(f.lines, "CLR_v4.0's UsageLogs folder is under a link: left"),
          "a linked CLR_v4.0: the file behind it kept: %d %s", r, f.lines);

    /* Microsoft itself a link */
    snprintf(local, sizeof local, "%s/local-ms-link", scratch);
    mkdir(local, 0755);
    snprintf(p, sizeof p, "%s/CLR_v4.0", other);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/CLR_v4.0/UsageLogs", other);
    mkdir(p, 0755);
    snprintf(q, sizeof q, "%s/CLR_v4.0/UsageLogs/RB Stems Plus.exe.log", other);
    put(q, "not ours either");
    snprintf(p, sizeof p, "%s/Microsoft", local);
    (void)symlink(other, p);
    memset(&f, 0, sizeof f);
    r = run(&f, local, &a);
    CHECK(r == AX_DONE && exists(q) && strstr(f.lines, "Microsoft is a link: the UsageLogs entries left"), "a linked Microsoft: kept: %s", f.lines);
}

/* review UN16: an apps folder that can't be opened, a version folder whose path doesn't fit */
static void watchers_unchecked(const char *scratch) {
    char local[1100], apps[1200];
    ax_args a;
    fake f;
    if (geteuid() != 0) {
        snprintf(local, sizeof local, "%s/local-locked", scratch);
        tree(local);
        snprintf(apps, sizeof apps, "%s/Programs/RB Stems Plus", local);
        chmod(apps, 0);
        memset(&f, 0, sizeof f);
        int r = run(&f, local, &a);
        CHECK(r == AX_DONE && !strstr(f.calls, "stop ") && strstr(f.lines, "the watchers couldn't all be checked: ") && strstr(f.lines, " couldn't be opened"),
              "an apps folder that can't be opened: said: %s", f.lines);
        chmod(apps, 0755);
    }
    /* a local of about 760 bytes, its apps folder holding a version folder whose path is past
     * AX_PATH_MAX */
    char seg[201], name[251], p[1200];
    memset(seg, 'd', 200);
    seg[200] = 0;
    memset(name, 'v', 250);
    name[250] = 0;
    snprintf(local, sizeof local, "%s/ax-long", scratch);
    mkdir(local, 0755);
    while (strlen(local) < 760) {
        size_t l = strlen(local), take = 760 - l < 200 ? 760 - l : 200;
        local[l] = '/';
        memcpy(local + l + 1, seg, take);
        local[l + 1 + take] = 0;
        mkdir(local, 0755);
    }
    tree(local);
    snprintf(p, sizeof p, "%s/Programs/RB Stems Plus", local);
    int fd = open(p, O_RDONLY | O_DIRECTORY);
    int made = fd >= 0 && (!mkdirat(fd, name, 0755) || errno == EEXIST);
    if (fd >= 0) close(fd);
    CHECK(made && strlen(p) + 1 + strlen(name) >= AX_PATH_MAX, "the overlong version folder made (%zu)", strlen(p) + 1 + strlen(name));
    memset(&f, 0, sizeof f);
    int r = run(&f, local, &a);
    CHECK(r == AX_DONE && !strstr(f.calls, "stop vvv") && strstr(f.calls, "stop 1.6.0;") && strstr(f.lines, "the watchers couldn't all be checked"),
          "a version folder whose path doesn't fit: not stopped, said: %d %s", r, f.lines);
}

void test_afterexit(const char *scratch) {
    cmdline_and_parse();
    folders();
    cmd_lines();
    order(scratch);
    usage_links(scratch);
    watchers_unchecked(scratch);
}
