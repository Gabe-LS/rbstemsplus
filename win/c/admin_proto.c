/* The app's and the worker's shared protocol (admin_proto.h). Pure C11. */
#include "admin_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const verbs[ADMIN_VERB_COUNT] = {"install", "uninstall", "restore", "install-worker", "remove-root-folder"};

const char *admin_verb_name(int verb) { return verb >= 0 && verb < ADMIN_VERB_COUNT ? verbs[verb] : NULL; }

int admin_verb_of(const char *word) {
    for (int i = 0; word && i < ADMIN_VERB_COUNT; i++)
        if (!strcmp(word, verbs[i])) return i;
    return -1;
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_hex(int c) { return is_digit(c) || (c >= 'a' && c <= 'f'); }
#if defined(_WIN32)
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
#endif

int admin_version_ok(const char *v) {
    if (!v) return 0;
    for (int part = 0; part < 3; part++) {
        int k = 0;
        while (is_digit((unsigned char)*v)) v++, k++;
        if (k < 1 || k > 9) return 0;
        if (part < 2 && *v++ != '.') return 0;
    }
    return *v == 0;
}

static const char pipe_prefix[] = "\\\\.\\pipe\\rbstemsplus-admin-";

int admin_pipe_name_ok(const char *name) {
    size_t p = sizeof pipe_prefix - 1;
    if (!name || strncmp(name, pipe_prefix, p)) return 0;
    for (int i = 0; i < 32; i++)
        if (!is_hex((unsigned char)name[p + (size_t)i])) return 0;
    return name[p + 32] == 0;
}

void admin_pipe_name(const unsigned char rnd[16], char out[ADMIN_PIPE_MAX]) {
    static const char hex[] = "0123456789abcdef";
    size_t p = sizeof pipe_prefix - 1;
    memcpy(out, pipe_prefix, p);
    for (int i = 0; i < 16; i++) {
        out[p + 2 * (size_t)i] = hex[rnd[i] >> 4];
        out[p + 2 * (size_t)i + 1] = hex[rnd[i] & 15];
    }
    out[p + 32] = 0;
}

int admin_sha12_ok(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < 12; i++)
        if (!is_hex((unsigned char)name[i])) return 0;
    return name[12] == 0;
}

#if defined(_WIN32)
#  define SEP '\\'
#else
#  define SEP '/'
#endif

/* one component: not empty, ".", ".."; nothing Windows refuses in a name */
static int component_ok(const char *s, size_t n) {
    if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || c == '"') return 0;
#if defined(_WIN32)
        if (c == '<' || c == '>' || c == '|' || c == '?' || c == '*' || c == ':' || c == '/') return 0;
#endif
    }
#if defined(_WIN32)
    if (s[n - 1] == '.' || s[n - 1] == ' ') return 0;   /* Windows drops them: another name than written */
#endif
    return 1;
}

int admin_abs_path_ok(const char *p) {
    if (!p) return 0;
    size_t l = strlen(p);
    if (l >= ADMIN_PATH_MAX) return 0;
#if defined(_WIN32)
    if (l < 4 || !((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) || p[1] != ':' || p[2] != '\\') return 0;
    size_t i = 3;
#else
    if (l < 2 || p[0] != '/') return 0;
    size_t i = 1;
#endif
    while (i < l) {
        const char *s = strchr(p + i, SEP);
        size_t e = s ? (size_t)(s - p) : l;
        if (!component_ok(p + i, e - i)) return 0;
        if (s && e + 1 == l) return 0;   /* a trailing separator: an empty last component */
        i = e + 1;
    }
    return 1;
}

int admin_plain_name_ok(const char *name) {
    if (!name) return 0;
    size_t n = strlen(name);
    if (n == 0 || n > 128) return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..") || name[n - 1] == '.' || name[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c > 0x7e || strchr("\\/:<>|?*\"", c)) return 0;
    }
    return 1;
}

static int copy_value(char *dst, size_t cap, const char *v) {
    size_t l = strlen(v);
    if (l >= cap) return 0;
    memcpy(dst, v, l + 1);
    return 1;
}

int admin_parse_args(int argc, const char *const *argv, admin_args *out, char *why, size_t n) {
    static const char *const opts[5] = {"--proto", "--version", "--source", "--force", "--pipe"};
    const char *val[5] = {NULL, NULL, NULL, NULL, NULL};
    memset(out, 0, sizeof *out);
    out->verb = -1;
    if (n) why[0] = 0;
#define BAD(...)                                     \
    do {                                             \
        if (n) snprintf(why, n, __VA_ARGS__);        \
        return ADMIN_EXIT_CHECK_FAILED;              \
    } while (0)
    if (argc != 12 || !argv) BAD("expected a verb and five options (%d arguments)", argc - 1);
    for (int i = 0; i < argc; i++)
        if (!argv[i]) BAD("a missing argument");
    if ((out->verb = admin_verb_of(argv[1])) < 0) BAD("unknown verb");
    for (int i = 2; i < argc; i += 2) {
        int k = -1;
        for (int j = 0; j < 5; j++)
            if (!strcmp(argv[i], opts[j])) k = j;
        if (k < 0) BAD("unknown option");
        if (val[k]) BAD("%s given twice", opts[k]);
        val[k] = argv[i + 1];
    }
    /* --proto: 1 to 4 digits, no sign, no leading zero */
    const char *p = val[0];
    size_t pl = strlen(p);
    if (pl < 1 || pl > 4 || (pl > 1 && p[0] == '0')) BAD("--proto isn't a number");
    for (size_t i = 0; i < pl; i++)
        if (!is_digit((unsigned char)p[i])) BAD("--proto isn't a number");
    out->proto = atoi(p);
    int needs_version = out->verb == ADMIN_INSTALL || out->verb == ADMIN_UNINSTALL || out->verb == ADMIN_RESTORE;
    if (needs_version ? !admin_version_ok(val[1]) : (val[1][0] && !admin_version_ok(val[1])))
        BAD("--version isn't x.y.z");
    if (!copy_value(out->version, sizeof out->version, val[1])) BAD("--version is too long");
    int needs_source = out->verb == ADMIN_INSTALL || out->verb == ADMIN_INSTALL_WORKER;
    if (needs_source ? !admin_abs_path_ok(val[2]) : val[2][0] != 0) BAD("--source isn't an absolute folder path");
    if (!copy_value(out->source, sizeof out->source, val[2])) BAD("--source is too long");
    if (strcmp(val[3], "0") && strcmp(val[3], "1")) BAD("--force isn't 0 or 1");
    out->force = val[3][0] == '1';
    if (!admin_pipe_name_ok(val[4])) BAD("--pipe isn't this app's local pipe");
    memcpy(out->pipe, val[4], strlen(val[4]) + 1);
#undef BAD
    if (out->proto != ADMIN_PROTO && out->verb != ADMIN_INSTALL_WORKER) {
        if (n) snprintf(why, n, "the app speaks protocol %d, this worker %d", out->proto, ADMIN_PROTO);
        return ADMIN_EXIT_PROTOCOL;
    }
    return ADMIN_EXIT_OK;
}

/* appends one value in double quotes, as CommandLineToArgvW reads it back: backslashes are
 * literal unless a quote follows, so those before the closing quote are doubled */
static int put_quoted(char *out, size_t cap, size_t *at, const char *v) {
    size_t i = *at;
#define PUT(c)                       \
    do {                             \
        if (i + 1 >= cap) return 0;  \
        out[i++] = (c);              \
    } while (0)
    if (i) PUT(' ');
    PUT('"');
    for (const char *s = v; *s; s++) {
        if (*s == '"') return 0;   /* never in a value (admin_abs_path_ok, the pipe name) */
        PUT(*s);
    }
    for (const char *s = v + strlen(v); s > v && s[-1] == '\\'; s--) PUT('\\');
    PUT('"');
#undef PUT
    out[i] = 0;
    *at = i;
    return 1;
}

long admin_build_params(const admin_args *a, char *out, size_t cap) {
    if (!cap) return -1;
    out[0] = 0;
    const char *verb = admin_verb_name(a->verb);
    if (!verb) return -1;
    /* the same rules as the parse, for this app's own protocol */
    const char *argv[12];
    char proto[8], force[2] = {a->force ? '1' : '0', 0};
    snprintf(proto, sizeof proto, "%d", a->proto);
    argv[0] = "RB Stems Plus.exe";
    argv[1] = verb;
    argv[2] = "--proto", argv[3] = proto;
    argv[4] = "--version", argv[5] = a->version;
    argv[6] = "--source", argv[7] = a->source;
    argv[8] = "--force", argv[9] = force;
    argv[10] = "--pipe", argv[11] = a->pipe;
    admin_args check;
    int r = admin_parse_args(12, argv, &check, NULL, 0);
    if (r != ADMIN_EXIT_OK && r != ADMIN_EXIT_PROTOCOL) return -1;
    size_t at = 0;
    for (int i = 1; i < 12; i++)
        if (!put_quoted(out, cap, &at, argv[i])) {
            out[0] = 0;
            return -1;
        }
    return (long)at;
}

int admin_marker_find(const void *buf, size_t n, int *proto, char min[ADMIN_VERSION_MAX]) {
    const char *b = buf, *pre = ADMIN_MARKER_PREFIX;
    size_t pl = strlen(pre);
    for (size_t i = 0; n >= pl && i <= n - pl; i++) {
        if (memcmp(b + i, pre, pl)) continue;
        const char *s = b + i + pl, *end = b + n;
        if (end - s < 7 || memcmp(s, "proto=", 6)) continue;
        s += 6;
        int p = 0, k = 0;
        while (s < end && is_digit((unsigned char)*s) && k < 4) p = p * 10 + (*s++ - '0'), k++;
        if (!k || end - s < 9 || memcmp(s, " minimum=", 9)) continue;
        s += 9;
        char v[ADMIN_VERSION_MAX];
        size_t vl = 0;
        while (s < end && *s != '\n' && vl < sizeof v - 1) v[vl++] = *s++;
        v[vl] = 0;
        if (s >= end || *s != '\n' || !admin_version_ok(v)) continue;
        *proto = p;
        memcpy(min, v, vl + 1);
        return 1;
    }
    return 0;
}

int admin_self_in_bin(const char *root, const char *self, char sha12[13]) {
    size_t rl = strlen(root);
    if (!rl || strlen(self) <= rl) return 0;
    for (size_t i = 0; i < rl; i++) {
#if defined(_WIN32)
        if (lower((unsigned char)root[i]) != lower((unsigned char)self[i])) return 0;
#else
        if (root[i] != self[i]) return 0;
#endif
    }
    const char *s = self + rl;
    if (*s++ != SEP) return 0;
#if defined(_WIN32)
    if (lower((unsigned char)s[0]) != 'b' || lower((unsigned char)s[1]) != 'i' || lower((unsigned char)s[2]) != 'n') return 0;
#else
    if (strncmp(s, "bin", 3)) return 0;
#endif
    s += 3;
    if (*s++ != SEP) return 0;
    char d[13];
    memcpy(d, s, 12);
    d[12] = 0;
    if (strlen(s) < 13 || !admin_sha12_ok(d) || s[12] != SEP || strcmp(s + 13, ADMIN_WORKER_NAME)) return 0;
    memcpy(sha12, d, 13);
    return 1;
}

int admin_line_kind(const char *line, size_t n, char *val, size_t cap) {
    static const struct { const char *word; int kind; int has_value; } recs[] = {
        {"still-closing", ADMIN_LINE_STILL_CLOSING, 0},
        {"restart", ADMIN_LINE_RESTART, 0},
        {"bridge", ADMIN_LINE_BRIDGE, 1},
        {"floor", ADMIN_LINE_FLOOR, 1},
        {"closed", ADMIN_LINE_CLOSED, 0},
    };
    if (cap) val[0] = 0;
    if (!n || line[0] != '@') return ADMIN_LINE_TEXT;
    for (size_t i = 0; i < sizeof recs / sizeof recs[0]; i++) {
        size_t w = strlen(recs[i].word);
        if (n - 1 < w || memcmp(line + 1, recs[i].word, w)) continue;
        const char *rest = line + 1 + w;
        size_t rn = n - 1 - w;
        if (!recs[i].has_value) {
            if (rn == 0) return recs[i].kind;
            continue;
        }
        if (rn < 2 || rest[0] != ' ' || rn - 1 >= cap) continue;
        memcpy(val, rest + 1, rn - 1);
        val[rn - 1] = 0;
        return recs[i].kind;
    }
    return ADMIN_LINE_UNKNOWN;
}

void admin_lines_init(admin_lines *l) { memset(l, 0, sizeof *l); }

/* the length of the valid UTF-8 sequence at s (n bytes left), or 0 */
static size_t utf8_len(const unsigned char *s, size_t n) {
    unsigned c = s[0];
    size_t k;
    unsigned cp;
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) k = 2, cp = c & 0x1f;
    else if (c >= 0xe0 && c <= 0xef) k = 3, cp = c & 0x0f;
    else if (c >= 0xf0 && c <= 0xf4) k = 4, cp = c & 0x07;
    else return 0;
    if (n < k) return 0;
    for (size_t i = 1; i < k; i++) {
        if ((s[i] & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    if ((k == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (k == 4 && (cp < 0x10000 || cp > 0x10ffff))) return 0;
    return k;
}

static void emit(admin_lines *l, void (*fn)(void *ctx, const char *line, size_t len), void *ctx) {
    size_t n = l->n;
    if (n && l->buf[n - 1] == '\r') n--;
    /* replace what isn't printable ASCII, a tab or valid UTF-8 */
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)l->buf[i];
        if (c >= 0x80) {
            size_t k = utf8_len((unsigned char *)l->buf + i, n - i);
            if (k) {
                i += k;
                continue;
            }
            l->buf[i++] = '?';
            continue;
        }
        if ((c < 0x20 && c != '\t') || c == 0x7f) l->buf[i] = '?';
        i++;
    }
    l->buf[n] = 0;
    fn(ctx, l->buf, n);
    l->n = 0;
}

void admin_lines_feed(admin_lines *l, const void *data, size_t n, int at_end, void (*fn)(void *ctx, const char *line, size_t len),
                      void *ctx) {
    const char *d = data;
    for (size_t i = 0; i < n; i++) {
        char c = d[i];
        if (c == '\n') {
            if (l->dropping) l->dropping = 0;
            else emit(l, fn, ctx);
            continue;
        }
        if (l->dropping) continue;
        if (l->n == sizeof l->buf - 1) {   /* too long: cut here, drop the rest of it */
            emit(l, fn, ctx);
            l->dropping = 1;
            continue;
        }
        l->buf[l->n++] = c;
    }
    if (at_end) {
        if (l->n && !l->dropping) emit(l, fn, ctx);
        l->n = 0;
        l->dropping = 0;
    }
}
