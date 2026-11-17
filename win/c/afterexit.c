/* afterexit.c: the after-exit's pure part (afterexit.h): its command line, its paths and the order
 * of ax_run over an ops table, the UsageLogs files through common/plat.h. Tested on the Mac
 * (tests/afterexit_test.c). */
#include "afterexit.h"

#include "plat.h"
#include "update.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int is_sep(char c) { return c == '/' || c == '\\'; }

/* whether p is absolute here: a drive letter and a separator, or \\server\share, on Windows; '/'
 * elsewhere (the tests on the Mac) */
static int absolute(const char *p) {
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && is_sep(p[2])) return 1;
    return is_sep(p[0]) && is_sep(p[1]) && p[2] && !is_sep(p[2]);
#else
    return p[0] == '/';
#endif
}

/* whether p has a ".." part (any separator) */
static int has_dotdot(const char *p) {
    for (const char *s = p; *s;) {
        const char *e = s;
        while (*e && !is_sep(*e)) e++;
        if (e - s == 2 && s[0] == '.' && s[1] == '.') return 1;
        s = *e ? e + 1 : e;
    }
    return 0;
}

int ax_cmdline(const char *exe, uint64_t parent, const char *local, char *out, size_t n) {
    if (!exe || !local || !exe[0] || !absolute(local) || strchr(exe, '"') || strchr(local, '"') || !parent) return -1;
    int k = snprintf(out, n, "\"%s\" " AX_ARG " %llx \"%s\"", exe, (unsigned long long)parent, local);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

int ax_parse(const char *args, ax_args *out) {
    memset(out, 0, sizeof *out);
    const char *p = args;
    if (!p) return -1;
    while (*p == ' ' || *p == '\t') p++;
    size_t al = strlen(AX_ARG);
    if (strncmp(p, AX_ARG, al) || (p[al] != ' ' && p[al] != '\t')) return -1;
    p += al;
    while (*p == ' ' || *p == '\t') p++;
    uint64_t h = 0;
    int digits = 0;
    for (; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'); p++) {
        if (++digits > 16) return -1;
        int c = upper((unsigned char)*p);
        h = h * 16 + (uint64_t)(c <= '9' ? c - '0' : c - 'A' + 10);
    }
    if (!digits || !h || (*p != ' ' && *p != '\t')) return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '"') return -1;
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= sizeof out->local) return -1;
    memcpy(out->local, p, (size_t)(end - p));
    out->local[end - p] = 0;
    for (p = end + 1; *p == ' ' || *p == '\t'; p++) {
    }
    if (*p || !out->local[0] || !absolute(out->local) || has_dotdot(out->local)) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    size_t ln = strlen(out->local);
    while (ln > 1 && is_sep(out->local[ln - 1]) && !(ln == 3 && out->local[1] == ':')) out->local[--ln] = 0;
    int k = snprintf(out->apps, sizeof out->apps, "%s" AX_SEP AX_APPS_REL, out->local);
    if (k < 0 || (size_t)k >= sizeof out->apps) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    out->parent = h;
    return 0;
}

/* whether s, from its start, is t ignoring ASCII case and reading every separator as one */
static size_t match_ci(const char *s, const char *t) {
    size_t i = 0;
    for (; t[i]; i++) {
        if (is_sep(t[i])) {
            if (!is_sep(s[i])) return 0;
        } else if (upper((unsigned char)s[i]) != upper((unsigned char)t[i])) {
            return 0;
        }
    }
    return i;
}

int ax_local_of(const char *apps, char *out, size_t n) {
    static const char tail[] = "\\Programs\\RB Stems Plus";
    size_t an = apps ? strlen(apps) : 0, tn = sizeof tail - 1;
    while (an > 0 && is_sep(apps[an - 1])) an--;
    if (an <= tn || !match_ci(apps + an - tn, tail) || an - tn + 1 > n) return -1;
    memcpy(out, apps, an - tn);
    out[an - tn] = 0;
    return absolute(out) ? 0 : -1;
}

int ax_same_dir(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) return 0;
    size_t an = strlen(a), bn = strlen(b);
    while (an > 1 && is_sep(a[an - 1])) an--;
    while (bn > 1 && is_sep(b[bn - 1])) bn--;
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++) {
        if (is_sep(a[i]) || is_sep(b[i])) {
            if (!is_sep(a[i]) || !is_sep(b[i])) return 0;
        } else if (upper((unsigned char)a[i]) != upper((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

int ax_under(const char *path, const char *dir) {
    if (!path || !dir) return 0;
    if (!strncmp(path, "\\\\?\\", 4)) path += 4;
    if (!strncmp(dir, "\\\\?\\", 4)) dir += 4;
    size_t dn = strlen(dir);
    while (dn > 0 && is_sep(dir[dn - 1])) dn--;
    if (!dn || strlen(path) <= dn + 1) return 0;
    for (size_t i = 0; i < dn; i++) {
        if (is_sep(dir[i])) {
            if (!is_sep(path[i])) return 0;
        } else if (upper((unsigned char)path[i]) != upper((unsigned char)dir[i])) {
            return 0;
        }
    }
    return is_sep(path[dn]) && path[dn + 1] && !is_sep(path[dn + 1]);
}

int ax_temp_name(const unsigned char rnd[8], char *out, size_t n) {
    int k = snprintf(out, n, AX_DIR_PREFIX "%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

int ax_is_temp_name(const char *name) {
    size_t pn = sizeof AX_DIR_PREFIX - 1;
    if (!name || strncmp(name, AX_DIR_PREFIX, pn)) return 0;
    for (size_t i = 0; i < 16; i++) {
        char c = name[pn + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return name[pn + 16] == 0;
}

/* whether cmd would read nothing in p: no " % ! ^ and no control character; absolute */
static int cmd_safe(const char *p) {
    if (!p || !p[0] || !absolute(p)) return 0;
    for (const unsigned char *q = (const unsigned char *)p; *q; q++)
        if (*q < 0x20 || *q == 0x7f || *q == '"' || *q == '%' || *q == '!' || *q == '^') return 0;
    return 1;
}

int ax_cleanup_args(const char *sys, const char *name, char *out, size_t n) {
    if (!ax_is_temp_name(name) || !cmd_safe(sys)) return -1;
    int k = snprintf(out, n,
                     "/d /e:on /v:off /q /s /c \"for /l %%i in (1,1,60) do @(del /f /q \"%s\\" AX_EXE "\" \"%s\\" AX_DLL "\" 2>nul & "
                     "rd /q \"%s\" 2>nul & (if not exist \"%s\\\" exit /b 0) & \"%s\\PING.EXE\" -n 2 -w 1000 127.0.0.1 >nul)\"",
                     name, name, name, name, sys);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

int ax_runonce_fits(const char *utf8) {
    if (!utf8) return 0;
    size_t units = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p;) {
        unsigned c = *p;
        size_t len = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!len) return 0;
        /* the second byte's range: no overlong form, no surrogate, nothing past U+10FFFF (review
         * U52); a NUL is below every lo, so a cut sequence never reads past its end */
        unsigned lo = c == 0xe0 ? 0xa0 : c == 0xf0 ? 0x90 : 0x80, hi = c == 0xed ? 0x9f : c == 0xf4 ? 0x8f : 0xbf;
        if (len > 1 && (p[1] < lo || p[1] > hi)) return 0;
        for (size_t i = 2; i < len; i++)
            if ((p[i] & 0xc0) != 0x80) return 0;
        units += len == 4 ? 2 : 1;   /* a character past U+FFFF is a surrogate pair */
        p += len;
    }
    return units <= 259;
}

int ax_cmd_cwd_ok(const char *parent8) {
    if (!parent8 || !parent8[0]) return 0;
    return !(is_sep(parent8[0]) && is_sep(parent8[1]));
}

int ax_runonce_name(const char *name, char *out, size_t n) {
    if (!ax_is_temp_name(name)) return -1;
    int k = snprintf(out, n, AX_RUNONCE_NAME " %s", name + sizeof AX_DIR_PREFIX - 1);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

int ax_runonce_data(const char *cmd, const char *dir, char *out, size_t n) {
    if (!cmd_safe(dir) || !cmd || !cmd[0] || strchr(cmd, '"')) return -1;
    int k = snprintf(out, n, "\"%s\" /d /q /s /c \"rd /s /q \"%s\"\"", cmd, dir);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

/* ------------------------------------------------------------------ ax_run */

/* one log line: a, then b and c when given (fixed parts and names: no format string from data) */
static void say(const ax_os *os, const char *a, const char *b, const char *c) {
    if (!os->log) return;
    char line[AX_LINE_MAX];
    snprintf(line, sizeof line, "%s%s%s", a, b ? b : "", c ? c : "");
    os->log(os->ctx, line);
}

typedef struct {
    const ax_os *os;
    const char *apps;
    int failed;   /* the folder list couldn't be read, or a folder's path didn't fit */
} watchers_walk;

static int stop_in(void *ctx, const char *name, const plat_stat *st) {
    watchers_walk *w = ctx;
    if (st->kind != PLAT_DIR) return 0;
    char dir[AX_PATH_MAX];
    int k = snprintf(dir, sizeof dir, "%s" AX_SEP "%s", w->apps, name);
    if (k < 0 || (size_t)k >= sizeof dir) {
        w->failed = 1;   /* not checked: counted, as native_steps.c's unload_each does (review UN16) */
        return 0;
    }
    int r = w->os->stop_watchers(w->os->ctx, dir);
    if (r > 0) say(w->os, "after-exit: the watcher in ", name, " stopped");
    if (r < 0) w->failed = 1;
    return 0;
}

/* the CLR's UsageLogs entry of our exe, in local's Microsoft\CLR_v4.0 and CLR_v4.0_32: each folder
 * opened from local's handle without following a link (a link along the way: left), the entry a
 * plain file only, unlinked relative to UsageLogs' handle (review UN3). 0 gone (or never there),
 * else an errno */
static int usage_logs(const ax_os *os, const char *local) {
    static const char *const clr[] = {"CLR_v4.0", "CLR_v4.0_32"};
    plat_dir l, m;
    int e = plat_dir_open(local, &l);
    if (e == ENOENT) return 0;
    if (e) {
        say(os, "after-exit: LocalAppData couldn't be opened: the UsageLogs entries left", NULL, NULL);   /* review UQ3 */
        return e;
    }
    e = plat_dir_open_at(l, "Microsoft", &m);
    plat_dir_close(l);
    if (e == ENOENT) return 0;
    if (e) {
        say(os, "after-exit: ", "Microsoft", e == ELOOP ? " is a link: the UsageLogs entries left" : " couldn't be opened: the UsageLogs entries left");
        return e == ELOOP ? 0 : e;
    }
    int first = 0;
    for (size_t i = 0; i < sizeof clr / sizeof clr[0]; i++) {
        plat_dir c, u;
        e = plat_dir_open_at(m, clr[i], &c);
        if (!e) {
            e = plat_dir_open_at(c, "UsageLogs", &u);
            plat_dir_close(c);
        }
        if (e == ENOENT) continue;   /* no such folder: nothing to delete */
        if (e) {
            say(os, "after-exit: ", clr[i], e == ELOOP ? "'s UsageLogs folder is under a link: left" : "'s UsageLogs folder couldn't be opened");
            if (e != ELOOP && !first) first = e;
            continue;
        }
        plat_stat st;
        if (!plat_stat_at(u, AX_EXE ".log", &st)) {
            if (st.kind != PLAT_FILE) {
                say(os, "after-exit: ", clr[i], "'s UsageLogs entry isn't a plain file: left");
            } else {
                e = plat_unlink_at(u, AX_EXE ".log");
                if (e && e != ENOENT && !first) first = e;
                if (!e) say(os, "after-exit: ", clr[i], "'s UsageLogs entry deleted");
            }
        }
        plat_dir_close(u);
    }
    plat_dir_close(m);
    return first;
}

/* a definite answer that the entry isn't there (or isn't a plain file): anything else (an ACL,
 * EBUSY, ENOMEM) can't tell (review UF3) */
static int gone(int e) { return e == ENOENT || e == ENOTDIR || e == PLAT_EFTYPE; }

/* whether the install still starts after a partial delete: the stub, `current` and current's
 * <version>\RB Stems Plus.exe still there, each a plain file, read through a freshly opened apps
 * handle (no link followed). 1, else 0 (review GH6); 1 also when the check can't tell (review UF3):
 * only a missing entry, a kind other than a file or a `current` that doesn't parse gives 0 */
static int still_starts(const char *apps) {
    plat_dir d, v;
    int e = plat_dir_open(apps, &d);
    if (e) return !gone(e);
    plat_stat st;
    plat_file f;
    int64_t size = 0;
    char b[PAYLOAD_VERSION_MAX + 2], cur[PAYLOAD_VERSION_MAX];
    int ok = 1;
    if ((e = plat_stat_at(d, AX_EXE, &st))) ok = !gone(e);
    else if (st.kind != PLAT_FILE) ok = 0;
    else if ((e = plat_file_open_at(d, "current", (int64_t)sizeof b, &size, &f))) ok = !gone(e);
    else {
        if (plat_read_all(f, b, (size_t)size)) ok = 1;   /* can't tell: the records stay */
        else if (!update_current_parse(b, (size_t)size, cur)) ok = 0;
        else if ((e = plat_dir_open_at(d, cur, &v))) ok = !gone(e);
        else {
            if ((e = plat_stat_at(v, AX_EXE, &st))) ok = !gone(e);
            else ok = st.kind == PLAT_FILE;
            plat_dir_close(v);
        }
        plat_file_close(f);
    }
    plat_dir_close(d);
    return ok;
}

int ax_run(const ax_os *os, const ax_args *a) {
    char check[AX_PATH_MAX], home[AX_PATH_MAX];
    if (!os || !a || !a->parent || !a->local[0] || ax_local_of(a->apps, check, sizeof check) || strcmp(check, a->local)) return AX_BAD_ARGS;
    /* this copy's own folder (UN1) and this account's LocalAppData (UN11): else nothing at all, not
     * even the temp folder's cleanup */
    if (!os->self_ok(os->ctx)) {
        say(os, "after-exit: not run from its own temp folder: nothing done", NULL, NULL);
        return AX_BAD_ARGS;
    }
    if (os->home(os->ctx, home, sizeof home) || !ax_same_dir(a->local, home)) {
        say(os, "after-exit: ", a->local, " isn't this account's LocalAppData: nothing done");
        return AX_BAD_ARGS;
    }
    say(os, "after-exit: waiting for RB Stems Plus to quit", NULL, NULL);
    if (os->wait_parent(os->ctx, a->parent, AX_PARENT_WAIT_MS)) {
        say(os, "after-exit: RB Stems Plus didn't quit: nothing deleted", NULL, NULL);
        int c = os->clean_later(os->ctx);
        if (c < 0) say(os, "after-exit: this folder couldn't be set to go", NULL, NULL);
        return AX_PARENT_RUNS;
    }
    int result = AX_DONE;
    /* the Run value first: a logon meanwhile starts no watcher (release.md §7's order, UN4) */
    if (os->remove_run(os->ctx)) {
        say(os, "after-exit: the Run value couldn't be removed", NULL, NULL);
        result = AX_LEFT;
    }
    /* the watchers, each version folder's, then whatever else runs from the apps folder */
    plat_dir d;
    int oe = plat_dir_open(a->apps, &d);
    if (!oe) {
        watchers_walk w = {os, a->apps, 0};
        if (plat_dir_each(d, stop_in, &w)) w.failed = 1;
        plat_dir_close(d);
        if (w.failed) say(os, "after-exit: the watchers couldn't all be checked", NULL, NULL);
    } else if (oe != ENOENT) {
        say(os, "after-exit: the watchers couldn't all be checked: ", a->apps, " couldn't be opened");
    }
    /* confirmed: nothing runs from the apps folder any more. A list that can't be read is waited
     * on as a program that runs (UN4) */
    int confirmed = 0, unreadable = 0, kept_running = 0;
    uint64_t t0 = os->now_ms(os->ctx);
    for (;;) {
        int r = os->running_under(os->ctx, a->apps);
        if (r == 0) {
            confirmed = 1;
            break;
        }
        if (r < 0 && !unreadable) {
            unreadable = 1;
            say(os, "after-exit: the process list couldn't be read: waiting", NULL, NULL);
        }
        if (os->now_ms(os->ctx) - t0 >= AX_OTHERS_WAIT_MS) {
            if (r < 0) say(os, "after-exit: the process list still can't be read: the app's files, the Apps & features entry and the UsageLogs entry kept", NULL, NULL);
            else say(os, "after-exit: a program still runs from ", a->apps, ": its files, the Apps & features entry and the UsageLogs entry kept; uninstall again once it is closed");
            result = AX_LEFT;
            kept_running = 1;
            break;
        }
        os->sleep_ms(os->ctx, AX_POLL_MS);
    }
    /* Only once nothing runs from the apps folder: the UsageLogs entry (T11: a live .NET process
     * writes it again), the folder, the other records. One still running after the wait, or a list
     * that still can't be read, keeps all three, so the uninstall can be run again from Apps &
     * features (owner 2026-11-13) */
    if (confirmed) {
        if (usage_logs(os, a->local)) result = AX_LEFT;
        int e = os->remove_tree(os->ctx, a->apps), kept = 0;
        if (e == 0) say(os, "after-exit: ", a->apps, " deleted");
        else if (e != ENOENT) {
            say(os, "after-exit: ", a->apps, " couldn't all be deleted");
            result = AX_LEFT;
            /* the app still starts (a partial delete): its Apps & features entry and the other
             * records stay, so the uninstall can be run again from there (review GH6, owner
             * 2026-11-13) */
            if (still_starts(a->apps)) {
                say(os, "after-exit: the app still starts: the Apps & features entry kept", NULL, NULL);
                kept = 1;
            }
        }
        /* the other records last: a watcher's toast meanwhile can't make the notification
         * settings again (UN4) */
        if (!kept && os->remove_records(os->ctx)) {
            say(os, "after-exit: some of the Windows records couldn't be removed", NULL, NULL);
            result = AX_LEFT;
        }
    }
    /* kept for a program still running (or a list still unknown): the one dialog, with the fix
     * only (owner 2026-11-16; the path is in the log above). Before the temp folder's cleanup,
     * whose cmd.exe gives up about a minute after it starts: the dialog waits for the user */
    if (kept_running && os->kept_dialog) {
        /* the dialog says to restart Windows, and a restart ends this process inside it, before
         * clean_later: the logon-time cleanup is set first (review undefined1) */
        if (os->clean_at_logon && os->clean_at_logon(os->ctx) != 1)
            say(os, "after-exit: this folder couldn't be set to go at the next logon", NULL, NULL);
        say(os, "after-exit: telling the user to restart Windows, then uninstall again", NULL, NULL);
        os->kept_dialog(os->ctx);
    }
    int c = os->clean_later(os->ctx);
    if (c == 1) say(os, "after-exit: this folder goes at the next logon (RunOnce)", NULL, NULL);
    else if (c < 0) say(os, "after-exit: this folder couldn't be set to go", NULL, NULL);
    return result;
}
