/* fake-prepare-win: tests/harness/fake-prepare.c's scripted stand-in for rbsp-prepare on Windows
 * (app.md W10/W11; mac-core.md §2.3). A TEST shell over a test-hooks core DLL starts it instead of
 * the real helper when probe.conf names it (helper=, core_win.h's core_test_win_fake_helper); a
 * release build can't (no such export). win/shell/build.sh --test-app builds it for x64 and arm64.
 *
 * The same scripts, directives and argv log as the Mac fake (its header), in <its folder>\script\
 * and <its folder>\log\argv.log, with Windows' stop instead of SIGTERM (helper_proc.h): a run
 * ("--keep" without "--estimate" or "--quick") stops when its stdin reaches EOF (the script's
 * @onterm part, then it exits; without one, status 143); Pause and Resume are the kernel's
 * (NtSuspendProcess); any other call is ended by its Job object. Untested until the VM's W10 round. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static volatile LONG term;
static wchar_t dir[MAX_PATH * 2];

static void sleep_ms(long ms) {
    while (ms > 0 && !term) {   /* in short steps, so the stop is seen soon */
        long step = ms > 5 ? 5 : ms;
        Sleep((DWORD)step);
        ms -= step;
    }
}

/* the stop of a run: stdin's EOF */
static DWORD WINAPI watch_stdin(void *arg) {
    (void)arg;
    char b[256];
    DWORD n;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    while (ReadFile(in, b, sizeof b, &n, NULL) && n > 0) {}
    InterlockedExchange(&term, 1);
    return 0;
}

static char *utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = n > 0 ? malloc((size_t)n) : NULL;
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* <dir>\<sub>\<name> as a wide path */
static FILE *open_in(const char *sub, const char *name, const wchar_t *mode) {
    wchar_t *wn = wide(name), p[MAX_PATH * 3];
    if (!wn) return NULL;
    _snwprintf(p, sizeof p / sizeof *p, L"%ls\\%hs\\%ls", dir, sub, wn);
    p[sizeof p / sizeof *p - 1] = 0;
    free(wn);
    return _wfopen(p, mode);
}

static int exists_in(const char *sub, const char *name) {
    wchar_t *wn = wide(name), p[MAX_PATH * 3];
    if (!wn) return 0;
    _snwprintf(p, sizeof p / sizeof *p, L"%ls\\%hs\\%ls", dir, sub, wn);
    p[sizeof p / sizeof *p - 1] = 0;
    free(wn);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static const char *arg_after(int argc, char **argv, const char *flag) {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], flag)) return argv[i + 1];
    return NULL;
}
static int has(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], flag)) return 1;
    return 0;
}

static FILE *open_script(const char *name, const char *kind, char *used, size_t n) {
    char f[600];
    snprintf(f, sizeof f, "%s.jsonl", name);
    FILE *s = open_in("script", f, L"rb");
    if (s) { snprintf(used, n, "%s", name); return s; }
    snprintf(f, sizeof f, "%s-default.jsonl", kind);
    s = open_in("script", f, L"rb");
    if (s) snprintf(used, n, "%s-default", kind);
    return s;
}

static void log_argv(int argc, char **argv, const char *name) {
    wchar_t p[MAX_PATH * 3];
    _snwprintf(p, sizeof p / sizeof *p, L"%ls\\log", dir);
    p[sizeof p / sizeof *p - 1] = 0;
    CreateDirectoryW(p, NULL);
    FILE *f = open_in("log", "argv.log", L"ab");
    if (!f) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = (((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000 - 11644473600000ULL;
    fprintf(f, "%llu.%03llu\t%lu\t%s", t / 1000, t % 1000, (unsigned long)GetCurrentProcessId(), name);
    for (int i = 1; i < argc; i++) fprintf(f, "\t%s", argv[i]);
    fputc('\n', f);
    fclose(f);
}

static void print_repeat(const char *tmpl, long i) {
    for (const char *s = tmpl; *s; s++) {
        if (!strncmp(s, "{i}", 3)) { printf("%ld", i); s += 2; }
        else if (!strncmp(s, "{n}", 3)) { printf("%ld", i + 1); s += 2; }
        else putchar(*s);
    }
    putchar('\n');
    fflush(stdout);
}

/* one whole line of f into *buf (a heap buffer of *cap bytes, doubled until the line fits, as the
 * Mac fake's getline), its \r\n cut; 0 at EOF with nothing read, or when the buffer can't grow */
static int next_line(FILE *f, char **buf, size_t *cap) {
    if (!*buf) {
        *cap = 4096;
        if (!(*buf = malloc(*cap))) return 0;
    }
    size_t n = 0;
    for (;;) {
        if (!fgets(*buf + n, (int)(*cap - n), f)) {
            if (n == 0) return 0;
            break;   /* EOF after a last line without '\n' */
        }
        n += strlen(*buf + n);
        if (n > 0 && (*buf)[n - 1] == '\n') break;
        if (n + 1 < *cap) continue;   /* fgets stopped short of a full buffer: EOF next */
        if (*cap > INT_MAX / 2) return 0;
        char *b = realloc(*buf, *cap * 2);
        if (!b) return 0;
        *buf = b;
        *cap *= 2;
    }
    char *line = *buf;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    return 1;
}

int wmain(int wargc, wchar_t **wargv) {
    wchar_t exe[MAX_PATH * 2];
    DWORD k = GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    if (!k || k >= MAX_PATH * 2) return 2;
    wcscpy(dir, exe);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    char **argv = calloc((size_t)wargc + 1, sizeof *argv);
    if (!argv) return 2;
    for (int i = 0; i < wargc; i++)
        if (!(argv[i] = utf8(wargv[i]))) return 2;
    int argc = wargc;

    char name[512], kind[32];
    const char *pl = arg_after(argc, argv, "--playlist"), *tr = arg_after(argc, argv, "--tracks"), *keep = arg_after(argc, argv, "--keep");
    int estimate = has(argc, argv, "--estimate") || has(argc, argv, "--quick");
    if (has(argc, argv, "--playlists")) { snprintf(name, sizeof name, "playlists"); snprintf(kind, sizeof kind, "playlists"); }
    else if (tr) { snprintf(name, sizeof name, "tracks-%s", tr); snprintf(kind, sizeof kind, "tracks"); }
    else if (estimate && pl) { snprintf(name, sizeof name, "estimate-%s", pl); snprintf(kind, sizeof kind, "estimate"); }
    else if (estimate && keep) { snprintf(name, sizeof name, "estimate-keep-%s", keep); snprintf(kind, sizeof kind, "estimate-keep"); }
    else if (pl) { snprintf(name, sizeof name, "run-%s", pl); snprintf(kind, sizeof kind, "run"); }
    else { snprintf(name, sizeof name, "unknown"); snprintf(kind, sizeof kind, "unknown"); }
    /* a run's stop is its stdin's EOF (helper_proc.h); any other call is ended by its Job */
    if (keep && !estimate) CloseHandle(CreateThread(NULL, 0, watch_stdin, NULL, 0, NULL));

    char used[512] = "";
    FILE *f = open_script(name, kind, used, sizeof used);
    char logged[1100];
    snprintf(logged, sizeof logged, "%s%s%s", name, f ? " <- " : " (no script)", f ? used : "");
    log_argv(argc, argv, logged);
    if (!f) { fprintf(stderr, "fake-prepare: no script for %s\n", name); return 1; }

    char *line = NULL;
    size_t cap = 0;
    long pace = 0;
    int status = 0;
    while (!term && next_line(f, &line, &cap)) {
        if (line[0] == 0 || line[0] == '#') continue;
        if (line[0] != '@') {
            if (pace) sleep_ms(pace);
            if (!term) { puts(line); fflush(stdout); }
            continue;
        }
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = 0; else arg = "";
        if (!strcmp(line, "@onterm")) break;
        else if (!strcmp(line, "@pace")) pace = atol(arg);
        else if (!strcmp(line, "@sleep")) sleep_ms(atol(arg));
        else if (!strcmp(line, "@exit")) status = atoi(arg);
        else if (!strcmp(line, "@stderr")) fprintf(stderr, "%s\n", arg);
        else if (!strcmp(line, "@touch")) { FILE *t = open_in("script", arg, L"ab"); if (t) fclose(t); }
        else if (!strcmp(line, "@wait")) { while (!term && !exists_in("script", arg)) sleep_ms(5); }
        else if (!strcmp(line, "@repeat")) {
            long count = atol(arg);
            char *tmpl = strchr(arg, ' ');
            if (!tmpl) continue;
            tmpl++;
            for (long i = 0; i < count && !term; i++) { if (pace) sleep_ms(pace); if (!term) print_repeat(tmpl, i); }
        }
    }
    if (!term) return status;
    /* the stop: the script's @onterm part (as the real helper ends its chunk, writes its set and
     * its last line), or 143 without one */
    rewind(f);
    int found = 0;
    status = 143;
    while (next_line(f, &line, &cap)) {
        if (!found) { if (!strcmp(line, "@onterm")) { found = 1; status = 0; } continue; }
        if (!strncmp(line, "@exit ", 6)) status = atoi(line + 6);
        else if (!strncmp(line, "@stderr ", 8)) fprintf(stderr, "%s\n", line + 8);
        else if (line[0] && line[0] != '@' && line[0] != '#') {
            if (pace) Sleep((DWORD)pace);
            puts(line);
            fflush(stdout);
        }
    }
    return status;
}
