/* update.h's pure rules (no file, no network): addresses, the versions and records, quoting,
 * ranges. Linked alone into the Windows test exe and the gates. */
#include "update.h"

#include <stdio.h>
#include <string.h>

static int older(const char *a, const char *b) { return payload_version_cmp(a, b) == -1; }

static void copy_str(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src); }

int update_is_version(const char *s) { return s && payload_is_release_version(s); }

/* ------------------------------------------------------------------ addresses */

static int plain_name(const char *s) {
    size_t n = strlen(s);
    if (!n || n > PAYLOAD_NAME_MAX || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return 0;
    }
    return 1;
}

int update_url(const char *base, const char *tag, const char *name, char *out, size_t n) {
    if (!base) base = UPDATE_BASE;
    if (!name || !plain_name(name) || (tag && !update_is_version(tag)) || strncmp(base, "https://", 8)) return -1;
    int k = tag ? snprintf(out, n, "%s/download/v%s/%s", base, tag, name) : snprintf(out, n, "%s/latest/download/%s", base, name);
    return k < 0 || (size_t)k >= n ? -1 : 0;
}

/* ------------------------------------------------------------------ versions and records */

int update_shown_parse(const void *data, size_t n, update_shown *out) {
    if (!data || n >= 64) return 0;
    char buf[64];
    memcpy(buf, data, n);
    buf[n] = 0;
    if (strlen(buf) != n) return 0;
    char *nl1 = strchr(buf, '\n');
    if (!nl1) return 0;
    char *nl2 = strchr(nl1 + 1, '\n');
    if (!nl2 || nl2[1]) return 0;
    *nl1 = *nl2 = 0;
    if (!update_is_version(buf) || !update_is_version(nl1 + 1) || older(buf, nl1 + 1)) return 0;
    copy_str(out->shown, sizeof out->shown, buf);
    copy_str(out->app, sizeof out->app, nl1 + 1);
    return 1;
}

size_t update_shown_format(const update_shown *r, char *out, size_t n) {
    int k = snprintf(out, n, "%s\n%s\n", r->shown, r->app);
    return k < 0 || (size_t)k >= n ? 0 : (size_t)k;
}

int update_next_shown(const char *own, const update_shown *before, const char *seen_payload, const char *seen_app, update_shown *out) {
    if (!update_is_version(own)) return 0;
    const update_shown *kept = before && !strcmp(before->app, own) && update_is_version(before->shown) && !older(before->shown, own) ? before : NULL;
    int take = seen_payload && seen_app && !strcmp(seen_app, own) && update_is_version(seen_payload) && !older(seen_payload, own) &&
               (!kept || older(kept->shown, seen_payload));
    if (!take) {
        if (kept && kept != out) *out = *kept;
        return kept != NULL;
    }
    copy_str(out->shown, sizeof out->shown, seen_payload);
    copy_str(out->app, sizeof out->app, own);
    return 1;
}

const char *update_shown_version(const char *own, const update_shown *record) {
    update_shown r;
    return update_next_shown(own, record, NULL, NULL, &r) ? record->shown : own;
}

int update_newer(const payload_manifest *m, const char *own) {
    return m && m->has_windows && payload_version_cmp(m->win_app.version, own) == 1;
}

/* a decimal number of at most digits digits, no sign, no leading zero (but "0") */
static int decimal(const char *s, int digits, uint64_t *v) {
    size_t n = strlen(s);
    if (!n || n > (size_t)digits || (n > 1 && s[0] == '0')) return 0;
    uint64_t x = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        x = x * 10 + (uint64_t)(s[i] - '0');
    }
    *v = x;
    return 1;
}

int update_pending_parse(const void *data, size_t n, update_pending *out) {
    if (!data || n >= UPDATE_RECORD_MAX) return 0;
    char buf[UPDATE_RECORD_MAX], *line[4];
    memcpy(buf, data, n);
    buf[n] = 0;
    if (strlen(buf) != n || !n || buf[n - 1] != '\n') return 0;
    char *p = buf;
    for (int i = 0; i < 4; i++) {
        char *nl = strchr(p, '\n');
        if (!nl) return 0;
        *nl = 0;
        line[i] = p;
        p = nl + 1;
    }
    uint64_t pid, started;
    if (*p || !update_is_version(line[0]) || !update_is_version(line[1]) || !decimal(line[2], 10, &pid) || pid > UINT32_MAX ||
        !decimal(line[3], 18, &started))
        return 0;
    copy_str(out->from, sizeof out->from, line[0]);
    copy_str(out->to, sizeof out->to, line[1]);
    out->pid = (uint32_t)pid;
    out->started = (int64_t)started;
    return 1;
}

size_t update_pending_format(const update_pending *p, char *out, size_t n) {
    int k = snprintf(out, n, "%s\n%s\n%u\n%lld\n", p->from, p->to, (unsigned)p->pid, (long long)(p->started < 0 ? 0 : p->started));
    return k < 0 || (size_t)k >= n ? 0 : (size_t)k;
}

int update_current_parse(const void *data, size_t n, char out[PAYLOAD_VERSION_MAX]) {
    if (!data) return 0;
    const char *b = data;
    size_t k = n;
    if (k && b[k - 1] == '\n') k--;
    if (k && b[k - 1] == '\r') k--;
    if (!k || k >= PAYLOAD_VERSION_MAX) return 0;
    char v[PAYLOAD_VERSION_MAX];
    memcpy(v, b, k);
    v[k] = 0;
    if (strlen(v) != k || !update_is_version(v)) return 0;
    copy_str(out, PAYLOAD_VERSION_MAX, v);
    return 1;
}

static const char *const fail_lines[] = {"failed", "failed app", "failed download", "failed staging", "failed copy", "failed copy-check", "failed swap"};

const char *update_fail_line(int key) {
    return key >= 0 && key < (int)(sizeof fail_lines / sizeof fail_lines[0]) ? fail_lines[key] : NULL;
}

int update_fail_parse(const void *data, size_t n) {
    if (!data) return UPDATE_FAIL_NONE;
    /* two words: "failed" and a key (whitespace around and between, as Swift splits it) */
    const unsigned char *b = data;
    size_t w[2][2], nw = 0, i = 0;
    while (i < n) {
        while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r' || b[i] == '\v' || b[i] == '\f')) i++;
        if (i >= n) break;
        size_t a = i;
        while (i < n && !(b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r' || b[i] == '\v' || b[i] == '\f')) i++;
        if (nw == 2) return UPDATE_FAIL_GENERIC;
        w[nw][0] = a;
        w[nw][1] = i;
        nw++;
    }
    if (nw != 2 || w[0][1] - w[0][0] != 6 || memcmp(b + w[0][0], "failed", 6)) return UPDATE_FAIL_GENERIC;
    for (int k = UPDATE_FAIL_APP; k <= UPDATE_FAIL_SWAP; k++) {
        const char *key = fail_lines[k] + 7;
        if (strlen(key) == w[1][1] - w[1][0] && !memcmp(b + w[1][0], key, strlen(key))) return k;
    }
    return UPDATE_FAIL_GENERIC;
}

size_t update_watcher_event_name(uint32_t pid, char *out, size_t n) {
    int k = snprintf(out, n, "Local\\io.github.rbstemsplus.watcher.stop.%u", (unsigned)pid);
    return k < 0 || (size_t)k >= n ? 0 : (size_t)k;
}

int update_watcher_action(int mine_known, uint32_t mine, int theirs_known, uint32_t theirs) {
    return mine_known && theirs_known && mine == theirs ? UPDATE_WATCHER_STOP : UPDATE_WATCHER_LEAVE;
}

int update_stop_result(int listed, int stopped, int left) {
    return !listed || left ? -1 : stopped ? 1 : 0;
}

/* a and b's first n bytes equal, ASCII case ignored */
static int ascii_eq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

int update_image_kind(const char *name) {
    static const char app[] = "RB Stems Plus.exe", watch[] = "rbsp-watch";
    if (!name) return UPDATE_IMAGE_OTHER;
    size_t n = strlen(name);
    if (n == sizeof app - 1 && ascii_eq_n(name, app, n)) return UPDATE_IMAGE_APP;
    if (n >= sizeof watch - 1 && ascii_eq_n(name, watch, sizeof watch - 1)) return UPDATE_IMAGE_WATCHER;   /* as watcher_in's match */
    return UPDATE_IMAGE_OTHER;
}

int update_open_skips(unsigned long err) {
    return err == UPDATE_ERR_ACCESS_DENIED || err == UPDATE_ERR_INVALID_PARAMETER;
}

int update_scan_result(int found, int unknown, int ended) {
    return found ? 1 : unknown || !ended ? -1 : 0;
}

int update_quote_arg(const char *arg, char *out, size_t n) {
    size_t at = strlen(out);
#define PUT(c)                         \
    do {                               \
        if (at + 1 >= n) return -1;    \
        out[at++] = (c);               \
        out[at] = 0;                   \
    } while (0)
    if (*arg && !strpbrk(arg, " \t\n\v\"")) {
        for (const char *p = arg; *p; p++) PUT(*p);
        return 0;
    }
    PUT('"');
    for (const char *p = arg;; p++) {
        size_t bs = 0;
        while (*p == '\\') bs++, p++;
        if (!*p) {
            for (size_t i = 0; i < 2 * bs; i++) PUT('\\');
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < 2 * bs + 1; i++) PUT('\\');
        } else {
            for (size_t i = 0; i < bs; i++) PUT('\\');
        }
        PUT(*p);
    }
    PUT('"');
#undef PUT
    return 0;
}

int64_t update_range_start(int64_t from, int status, const char *content_range, int64_t content_length, int64_t *total, int64_t *end) {
    *total = *end = -1;
    if (status == 200) {
        *total = *end = content_length >= 0 ? content_length : -1;
        return 0;
    }
    if (status != 206 || from <= 0 || !content_range) return -1;
    /* "bytes A-B/T" or "bytes A-B/ *" */
    const char *p = content_range;
    if (strncmp(p, "bytes ", 6)) return -1;
    p += 6;
    int64_t v[3] = {0, 0, -1};
    for (int k = 0; k < 3; k++) {
        if (k == 2 && *p == '*') {
            p++;
            break;
        }
        if (*p < '0' || *p > '9') return -1;
        int64_t x = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 18) return -1;
            x = x * 10 + (*p++ - '0');
        }
        v[k] = x;
        if (k < 2 && *p++ != (k == 0 ? '-' : '/')) return -1;
    }
    if (*p || v[0] != from || v[1] < v[0] || (v[2] >= 0 && v[1] != v[2] - 1)) return -1;
    *total = v[2];
    *end = v[1] + 1;
    return v[0];
}


/* ------------------------------------------------------------------ the shell's entries, pure */

int update_apps_of(const char *app, char *out, size_t n) {
    if (!app || !out || !n) return -1;
    size_t k = strlen(app);
    while (k > 0 && app[k - 1] == '\\') k--;
    size_t cut = k;
    while (cut > 0 && app[cut - 1] != '\\') cut--;
    if (cut <= 1 || cut - 1 >= n) return -1;   /* no parent, or "\\x" */
    memcpy(out, app, cut - 1);
    out[cut - 1] = 0;
    return 0;
}

const char *update_arch_of(uint16_t native_machine, int built_arm64) {
    if (native_machine == 0xaa64) return "arm64";   /* IMAGE_FILE_MACHINE_ARM64 */
    if (native_machine == 0x8664) return "x64";     /* IMAGE_FILE_MACHINE_AMD64 */
    return built_arm64 ? "arm64" : "x64";
}

int update_swap_answer(int r) { return r == 0 ? 0 : r == 1 ? 1 : 2; }
