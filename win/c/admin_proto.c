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

/* --proto's value: 1 to 4 digits, no sign, no leading zero; the number, or -1 */
static int proto_value(const char *p) {
    size_t pl = strlen(p);
    if (pl < 1 || pl > 4 || (pl > 1 && p[0] == '0')) return -1;
    for (size_t i = 0; i < pl; i++)
        if (!is_digit((unsigned char)p[i])) return -1;
    return atoi(p);
}

static int is_sha256(const char *s) {
    for (int i = 0; i < 64; i++)
        if (!is_hex((unsigned char)s[i])) return 0;
    return s[64] == 0;
}

/* the line's shape, whatever its --proto (admin_parse_args and admin_build_params) */
static int parse_shape(int argc, const char *const *argv, admin_args *out, char *why, size_t n) {
    static const char *const opts[6] = {"--proto", "--targets", "--manifest", "--source", "--force", "--pipe"};
    const char *val[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
    memset(out, 0, sizeof *out);
    out->verb = -1;
    if (n) why[0] = 0;
#define BAD(...)                                     \
    do {                                             \
        if (n) snprintf(why, n, __VA_ARGS__);        \
        return ADMIN_EXIT_CHECK_FAILED;              \
    } while (0)
    if (argc != ADMIN_ARGC || !argv) BAD("expected a verb and six options (%d arguments)", argc - 1);
    for (int i = 0; i < argc; i++)
        if (!argv[i]) BAD("a missing argument");
    if ((out->verb = admin_verb_of(argv[1])) < 0) BAD("unknown verb");
    for (int i = 2; i < argc; i += 2) {
        int k = -1;
        for (int j = 0; j < 6; j++)
            if (!strcmp(argv[i], opts[j])) k = j;
        if (k < 0) BAD("unknown option");
        if (val[k]) BAD("%s given twice", opts[k]);
        val[k] = argv[i + 1];
    }
    if ((out->proto = proto_value(val[0])) < 0) BAD("--proto isn't a number");
    /* --targets: "*" for the verbs that sweep, else the checked scope */
    int names = out->verb == ADMIN_INSTALL || out->verb == ADMIN_RESTORE;
    if (strlen(val[1]) >= ADMIN_TARGETS_ARG_MAX || admin_targets_parse(val[1], out) || out->all == names)
        BAD(names ? "--targets isn't a list of X.Y.Z:<16 hex>" : "--targets isn't *");
    int needs_source = out->verb == ADMIN_INSTALL || out->verb == ADMIN_INSTALL_WORKER;
    if (needs_source ? !is_sha256(val[2]) : strcmp(val[2], "-") != 0) BAD(needs_source ? "--manifest isn't a sha256" : "--manifest isn't -");
    if (needs_source) memcpy(out->manifest, val[2], 65);
    if (needs_source ? !admin_abs_path_ok(val[3]) : val[3][0] != 0) BAD("--source isn't an absolute folder path");
    if (!copy_value(out->source, sizeof out->source, val[3])) BAD("--source is too long");
    if (strcmp(val[4], "0") && strcmp(val[4], "1")) BAD("--force isn't 0 or 1");
    out->force = val[4][0] == '1';
    if (!admin_pipe_name_ok(val[5])) BAD("--pipe isn't this app's local pipe");
    memcpy(out->pipe, val[5], strlen(val[5]) + 1);
#undef BAD
    return ADMIN_EXIT_OK;
}

int admin_parse_args(int argc, const char *const *argv, admin_args *out, char *why, size_t n) {
    /* another protocol's line is that protocol's, whatever its shape and argument count, for
     * every verb: said as such on its pipe when it names a valid one (D13, review finding 9).
     * The one exception is install-worker's protocol-2 shape (I75), parsed below */
    int verb = argc >= 2 && argv && argv[1] ? admin_verb_of(argv[1]) : -1;
    if (verb >= 0)
        for (int i = 2; i + 1 < argc; i += 2) {
            int p;
            if (argv[i] && argv[i + 1] && !strcmp(argv[i], "--proto") && (p = proto_value(argv[i + 1])) >= 0 && p != ADMIN_PROTO) {
                if (verb == ADMIN_INSTALL_WORKER && parse_shape(argc, argv, out, NULL, 0) == ADMIN_EXIT_OK) break;
                memset(out, 0, sizeof *out);
                out->verb = verb;
                out->proto = p;
                for (int j = 2; j + 1 < argc; j += 2)
                    if (argv[j] && argv[j + 1] && !strcmp(argv[j], "--pipe") && admin_pipe_name_ok(argv[j + 1]) && strlen(argv[j + 1]) < sizeof out->pipe)
                        memcpy(out->pipe, argv[j + 1], strlen(argv[j + 1]) + 1);
                if (n) snprintf(why, n, "the app speaks protocol %d, this worker %d", p, ADMIN_PROTO);
                return ADMIN_EXIT_PROTOCOL;
            }
        }
    int r = parse_shape(argc, argv, out, why, n);
    if (r != ADMIN_EXIT_OK) return r;
    if (out->proto != ADMIN_PROTO && out->verb != ADMIN_INSTALL_WORKER) {
        if (n) snprintf(why, n, "the app speaks protocol %d, this worker %d", out->proto, ADMIN_PROTO);
        return ADMIN_EXIT_PROTOCOL;
    }
    return ADMIN_EXIT_OK;
}

int admin_start(int argc, const char *const *argv, const char *marker, int (*connect)(void *ctx, const char *pipe), void (*say)(void *ctx, const char *line),
                void *ctx, admin_args *out) {
    char why[160] = "";
    int r = argv && argc >= 0 ? admin_parse_args(argc, argv, out, why, sizeof why) : ADMIN_EXIT_CHECK_FAILED;
    if (!argv || argc < 0) memset(out, 0, sizeof *out);
    /* nothing is said, and nothing done, without the app's own pipe: another protocol's line
     * keeps its own code all the same, so the app offers the update */
    if (!out->pipe[0] || connect(ctx, out->pipe)) return r == ADMIN_EXIT_PROTOCOL ? r : ADMIN_EXIT_CHECK_FAILED;
    say(ctx, marker);
    if (r != ADMIN_EXIT_OK) {
        char line[ADMIN_LINE_MAX];
        snprintf(line, sizeof line, "%s: nothing changed", why);
        say(ctx, line);
    }
    return r;
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

/* one X.Y.Z part at v: its digits' start, their count in *len */
static const char *part(const char *v, size_t *len) {
    const char *s = v;
    while (is_digit((unsigned char)*v)) v++;
    *len = (size_t)(v - s);
    return s;
}

int admin_version_cmp(const char *a, const char *b) {
    for (int i = 0; i < 3; i++) {
        size_t al, bl;
        const char *x = part(a, &al), *y = part(b, &bl);
        a = x + al + (i < 2 && x[al] == '.');
        b = y + bl + (i < 2 && y[bl] == '.');
        while (al > 1 && *x == '0') x++, al--;   /* admin_version_ok allows "07": the same as "7" */
        while (bl > 1 && *y == '0') y++, bl--;
        if (al != bl) return al < bl ? -1 : 1;
        int c = memcmp(x, y, al);
        if (c) return c < 0 ? -1 : 1;
    }
    return 0;
}

static int hex16_ok(const char *h) {
    for (int i = 0; i < 16; i++)
        if (!is_hex((unsigned char)h[i])) return 0;
    return h[16] == 0;
}

int admin_targets_parse(const char *s, admin_args *out) {
    out->all = 0;
    out->nt = 0;
    memset(out->t, 0, sizeof out->t);
    if (!s || strlen(s) >= ADMIN_TARGETS_ARG_MAX) return -1;
    if (!strcmp(s, "*")) {
        out->all = 1;
        return 0;
    }
    int nt = 0;
    for (;;) {
        const char *comma = strchr(s, ',');
        size_t l = comma ? (size_t)(comma - s) : strlen(s);
        const char *colon = memchr(s, ':', l);
        if (nt == ADMIN_TARGETS_MAX || !colon || (size_t)(colon - s) >= ADMIN_VERSION_MAX || l - (size_t)(colon - s) - 1 != 16) break;
        admin_target *t = &out->t[nt];
        memcpy(t->version, s, (size_t)(colon - s));
        t->version[colon - s] = 0;
        memcpy(t->exe16, colon + 1, 16);
        t->exe16[16] = 0;
        if (!admin_version_ok(t->version) || !hex16_ok(t->exe16) || (nt && admin_version_cmp(out->t[nt - 1].version, t->version) >= 0)) break;
        nt++;
        if (!comma) {
            out->nt = nt;
            return 0;
        }
        s = comma + 1;
    }
    memset(out->t, 0, sizeof out->t);
    return -1;
}

long admin_targets_format(const admin_target *t, int n, char *out, size_t cap) {
    if (!cap) return -1;
    out[0] = 0;
    if (!t || n < 1 || n > ADMIN_TARGETS_MAX) return -1;
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        if (!admin_version_ok(t[i].version) || !hex16_ok(t[i].exe16) || (i && admin_version_cmp(t[i - 1].version, t[i].version) >= 0)) {
            out[0] = 0;
            return -1;
        }
        int k = snprintf(out + at, cap - at, "%s%s:%s", i ? "," : "", t[i].version, t[i].exe16);
        if (k < 0 || (size_t)k >= cap - at || at + (size_t)k >= ADMIN_TARGETS_ARG_MAX) {
            out[0] = 0;
            return -1;
        }
        at += (size_t)k;
    }
    return (long)at;
}

long admin_build_params(const admin_args *a, char *out, size_t cap) {
    if (!cap) return -1;
    out[0] = 0;
    const char *verb = admin_verb_name(a->verb);
    if (!verb) return -1;
    /* the same rules as the parse, whatever the protocol */
    char proto[8], force[2] = {a->force ? '1' : '0', 0}, targets[ADMIN_TARGETS_ARG_MAX];
    if (a->all) snprintf(targets, sizeof targets, "*");
    else if (admin_targets_format(a->t, a->nt, targets, sizeof targets) < 0) return -1;
    snprintf(proto, sizeof proto, "%d", a->proto);
    const char *argv[ADMIN_ARGC] = {"RB Stems Plus.exe", verb,       "--proto",  proto,     "--targets", targets,
                                    "--manifest",        a->manifest[0] ? a->manifest : "-", "--source", a->source, "--force",
                                    force,               "--pipe",   a->pipe};
    admin_args check;
    if (parse_shape(ADMIN_ARGC, argv, &check, NULL, 0) != ADMIN_EXIT_OK) return -1;
    size_t at = 0;
    for (int i = 1; i < ADMIN_ARGC; i++)
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
        {"target", ADMIN_LINE_TARGET, 1},
        {"root-kept", ADMIN_LINE_ROOT_KEPT, 1},
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

int admin_target_value(const char *val, char *version, int *code, char *what) {
    static const char *const words[] = {ADMIN_TARGET_WHAT_INSTALLED, ADMIN_TARGET_WHAT_ALREADY, ADMIN_TARGET_WHAT_PUT_BACK,
                                        ADMIN_TARGET_WHAT_KEPT,      ADMIN_TARGET_WHAT_NOT_STARTED, ADMIN_TARGET_WHAT_REFUSED};
    const char *sp = val ? strchr(val, ' ') : NULL, *sp2 = sp ? strchr(sp + 1, ' ') : NULL;
    if (!sp || !sp2 || (size_t)(sp - val) >= ADMIN_VERSION_MAX) return -1;
    char v[ADMIN_VERSION_MAX];
    memcpy(v, val, (size_t)(sp - val));
    v[sp - val] = 0;
    if (!admin_version_ok(v)) return -1;
    const char *c = sp + 1;
    size_t cl = (size_t)(sp2 - c);
    int k = -1;
    if (cl != 1 || c[0] != '-') {
        if (cl < 1 || cl > 4 || (cl > 1 && c[0] == '0')) return -1;
        k = 0;
        for (size_t i = 0; i < cl; i++) {
            if (!is_digit((unsigned char)c[i])) return -1;
            k = k * 10 + (c[i] - '0');
        }
    }
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (!strcmp(sp2 + 1, words[i])) {
            memcpy(version, v, strlen(v) + 1);
            *code = k;
            memcpy(what, words[i], strlen(words[i]) + 1);
            return 0;
        }
    return -1;
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
