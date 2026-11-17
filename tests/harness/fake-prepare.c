/* fake-prepare: the harness's scripted stand-in for rbsp-prepare (the private Windows plan's mac-core
 * §2.3). A TEST build runs it instead of the real helper when the probe's probe.conf names it
 * (helper=, app/Probe/Probe.swift); a release build can't.
 *
 *   clang -O2 -o fake-prepare tests/harness/fake-prepare.c
 *
 * It prints scripted JSON lines with scripted delays, honours SIGSTOP and SIGCONT (the kernel's)
 * and SIGTERM (the script's @onterm part, as the real helper writes its set and its last line),
 * and records its argv. Its scripts are in <its folder>/script/, one file per call:
 *   --playlists                      playlists.jsonl
 *   --tracks ID                      tracks-ID.jsonl
 *   --estimate … --playlist ID       estimate-ID.jsonl
 *   --estimate … --keep ID (alone)   estimate-keep-ID.jsonl   (a set whose playlist can't be read)
 *   … --playlist ID (a run)          run-ID.jsonl
 * falling back to <kind>-default.jsonl. No script: nothing printed, exit 1.
 * Its argv goes to <its folder>/log/argv.log (one line per call: time, pid, the script's name,
 * the arguments, tab-separated).
 *
 * Script lines: a JSON line is printed as it is (after the pace's delay); directives start with @
 *   @pace MS          wait MS before each following printed line
 *   @sleep MS         wait MS once
 *   @wait FILE        wait until <script>/FILE exists (the scenario says when to go on)
 *   @touch FILE       create <script>/FILE (tells the scenario this point was reached)
 *   @repeat N LINE    print LINE N times; {i} is 0…N-1, {n} 1…N (big playlists without big files)
 *   @stderr TEXT      write TEXT to stderr
 *   @exit N           the exit status at the end (default 0)
 *   @onterm           the lines after it are printed only on SIGTERM, then it exits; without it
 *                     SIGTERM ends the fake at once (status 143)
 * Fixtures hold local-file tracks only (no streaming tracks, ever). */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t term;
static void on_term(int s) { (void)s; term = 1; }

static char dir[PATH_MAX];

static void sleep_ms(long ms) {
    /* in short steps, so SIGTERM is seen soon */
    while (ms > 0 && !term) {
        long step = ms > 5 ? 5 : ms;
        struct timespec ts = {0, step * 1000000L};
        nanosleep(&ts, NULL);
        ms -= step;
    }
}

static const char *arg_after(int argc, char **argv, const char *flag) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], flag)) return argv[i + 1];
    return NULL;
}
static int has(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], flag)) return 1;
    return 0;
}

static FILE *open_script(const char *name, const char *kind, char *used, size_t n) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/script/%s.jsonl", dir, name);
    FILE *f = fopen(p, "r");
    if (f) { snprintf(used, n, "%s", name); return f; }
    snprintf(p, sizeof p, "%s/script/%s-default.jsonl", dir, kind);
    f = fopen(p, "r");
    if (f) snprintf(used, n, "%s-default", kind);
    return f;
}

static void log_argv(int argc, char **argv, const char *name) {
    char p[PATH_MAX], line[16384];
    snprintf(p, sizeof p, "%s/log", dir);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/log/argv.log", dir);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    int o = snprintf(line, sizeof line, "%lld.%03ld\t%d\t%s", (long long)ts.tv_sec, ts.tv_nsec / 1000000, (int)getpid(), name);
    for (int i = 1; i < argc && o < (int)sizeof line - 2; i++) o += snprintf(line + o, sizeof line - o, "\t%s", argv[i]);
    if (o > (int)sizeof line - 2) o = (int)sizeof line - 2;
    line[o++] = '\n';
    int fd = open(p, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { (void)!write(fd, line, (size_t)o); close(fd); }
}

/* LINE with {i} and {n} replaced */
static void print_repeat(const char *tmpl, long i) {
    for (const char *s = tmpl; *s; s++) {
        if (!strncmp(s, "{i}", 3)) { printf("%ld", i); s += 2; }
        else if (!strncmp(s, "{n}", 3)) { printf("%ld", i + 1); s += 2; }
        else putchar(*s);
    }
    putchar('\n');
}

int main(int argc, char **argv) {
    char exe[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, real)) return 2;
    snprintf(dir, sizeof dir, "%s", real);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);

    char name[512], kind[32];
    const char *pl = arg_after(argc, argv, "--playlist"), *tr = arg_after(argc, argv, "--tracks"), *keep = arg_after(argc, argv, "--keep");
    if (has(argc, argv, "--playlists")) { snprintf(name, sizeof name, "playlists"); snprintf(kind, sizeof kind, "playlists"); }
    else if (tr) { snprintf(name, sizeof name, "tracks-%s", tr); snprintf(kind, sizeof kind, "tracks"); }
    else if (has(argc, argv, "--estimate") && pl) { snprintf(name, sizeof name, "estimate-%s", pl); snprintf(kind, sizeof kind, "estimate"); }
    else if (has(argc, argv, "--estimate") && keep) { snprintf(name, sizeof name, "estimate-keep-%s", keep); snprintf(kind, sizeof kind, "estimate-keep"); }
    else if (pl) { snprintf(name, sizeof name, "run-%s", pl); snprintf(kind, sizeof kind, "run"); }
    else { snprintf(name, sizeof name, "unknown"); snprintf(kind, sizeof kind, "unknown"); }

    char used[512] = "";
    FILE *f = open_script(name, kind, used, sizeof used);
    char logged[1100];
    snprintf(logged, sizeof logged, "%s%s%s", name, f ? " <- " : " (no script)", f ? used : "");
    log_argv(argc, argv, logged);
    if (!f) { fprintf(stderr, "fake-prepare: no script for %s\n", name); return 1; }

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    long pace = 0;
    int status = 0;
    while (!term && (n = getline(&line, &cap, f)) > 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (line[0] == 0 || line[0] == '#') continue;
        if (line[0] != '@') {
            if (pace) sleep_ms(pace);
            if (!term) puts(line);
            continue;
        }
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = 0; else arg = "";
        if (!strcmp(line, "@onterm")) break;
        else if (!strcmp(line, "@pace")) pace = atol(arg);
        else if (!strcmp(line, "@sleep")) sleep_ms(atol(arg));
        else if (!strcmp(line, "@exit")) status = atoi(arg);
        else if (!strcmp(line, "@stderr")) fprintf(stderr, "%s\n", arg);
        else if (!strcmp(line, "@wait") || !strcmp(line, "@touch")) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/script/%s", dir, arg);
            if (line[1] == 't') { int fd = open(p, O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd); }
            else { struct stat st; while (!term && stat(p, &st) != 0) sleep_ms(5); }
        } else if (!strcmp(line, "@repeat")) {
            long count = atol(arg);
            char *tmpl = strchr(arg, ' ');
            if (!tmpl) continue;
            tmpl++;
            for (long i = 0; i < count && !term; i++) { if (pace) sleep_ms(pace); if (!term) print_repeat(tmpl, i); }
        }
    }
    if (!term) return status;
    /* SIGTERM: the script's @onterm part (as the real helper ends its chunk, writes its set and
     * its last line), or 143 without one */
    rewind(f);
    int found = 0;
    status = 143;
    while ((n = getline(&line, &cap, f)) > 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!found) { if (!strcmp(line, "@onterm")) { found = 1; status = 0; } continue; }
        if (!strncmp(line, "@exit ", 6)) status = atoi(line + 6);
        else if (!strncmp(line, "@stderr ", 8)) fprintf(stderr, "%s\n", line + 8);
        else if (line[0] && line[0] != '@' && line[0] != '#') {
            /* the pace holds here too (as the real helper's last lines come one by one) */
            if (pace) { struct timespec ts = {pace / 1000, (pace % 1000) * 1000000L}; nanosleep(&ts, NULL); }
            puts(line);
        }
    }
    return status;
}
