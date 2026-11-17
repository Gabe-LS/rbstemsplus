/* The helper's control rules (helper_proc.h). Pure C11. */
#include "helper_proc.h"

#include "win_rules.h"

#include <stdlib.h>
#include <string.h>

void helper_state_init(helper_state *s, int run) {
    memset(s, 0, sizeof *s);
    s->run = run != 0;
    s->stdin_open = 1;
}

unsigned helper_ops(helper_state *s, int sig) {
    unsigned ops = 0;
    if (s->ended) return 0;
    switch (sig) {
    case HELPER_SIG_PAUSE:
        if (!s->paused) ops |= HELPER_OP_SUSPEND, s->paused = 1;
        break;
    case HELPER_SIG_RESUME:
        if (s->paused) ops |= HELPER_OP_RESUME, s->paused = 0;
        break;
    case HELPER_SIG_STOP:
        if (!s->run) {
            ops |= HELPER_OP_TERMINATE;   /* I63: any call that isn't a run, at once */
            s->paused = 0;
            break;
        }
        if (s->stdin_open) ops |= HELPER_OP_CLOSE_STDIN, s->stdin_open = 0;
        if (s->paused) ops |= HELPER_OP_RESUME, s->paused = 0;   /* after the EOF: once, so it sees it */
        break;
    default: break;
    }
    return ops;
}

/* "NAME=value\0" into out at *at */
static int put_var(const char *name, const char *value, uint16_t *out, size_t cap, size_t *at) {
    size_t nl = strlen(name), vl = strlen(value);
    if (!vl || memchr(value, '=', vl)) return -1;
    if (*at + nl + 1 > cap) return -1;
    for (size_t i = 0; i < nl; i++) out[(*at)++] = (uint16_t)(unsigned char)name[i];
    out[(*at)++] = '=';
    long w = wr_utf8_to_16(value, vl, out + *at, cap - *at);
    if (w < 0 || *at + (size_t)w + 1 > cap) return -1;
    *at += (size_t)w + 1;   /* wr_utf8_to_16 wrote its NUL */
    return 0;
}

long helper_env_block(const char *system_root, const char *temp, uint16_t *out, size_t cap) {
    size_t at = 0;
    /* sorted case-blind, as CreateProcessW's block must be: SystemRoot, TEMP, TMP */
    if (put_var("SystemRoot", system_root, out, cap, &at) || put_var("TEMP", temp, out, cap, &at) || put_var("TMP", temp, out, cap, &at)) return -1;
    if (at + 1 > cap) return -1;
    out[at++] = 0;
    return (long)at;
}

/* ---------------------------------------------------------------- stderr's tail */

void helper_tail_init(helper_tail *t) { memset(t, 0, sizeof *t); }

void helper_tail_feed(helper_tail *t, const char *p, size_t n) {
    if (!n) return;
    if (n >= HELPER_ERR_TAIL) {
        if (n > HELPER_ERR_TAIL || t->len) t->cut = 1;
        memcpy(t->buf, p + (n - HELPER_ERR_TAIL), HELPER_ERR_TAIL);
        t->len = HELPER_ERR_TAIL;
        return;
    }
    if (t->len + n > HELPER_ERR_TAIL) {
        size_t drop = t->len + n - HELPER_ERR_TAIL;
        memmove(t->buf, t->buf + drop, t->len - drop);
        t->len -= drop;
        t->cut = 1;
    }
    memcpy(t->buf + t->len, p, n);
    t->len += n;
}

const char *helper_tail_get(const helper_tail *t, size_t *len) {
    size_t at = 0;
    if (t->cut)
        while (at < t->len && at < 3 && ((unsigned char)t->buf[at] & 0xc0) == 0x80) at++;
    *len = t->len - at;
    return t->buf + at;
}

/* ---------------------------------------------------------------- stdout's lines */

void helper_lines_init(helper_lines *l, size_t max) {
    memset(l, 0, sizeof *l);
    l->max = max ? max : HELPER_LINE_MAX;
}

void helper_lines_free(helper_lines *l) {
    free(l->buf);
    l->buf = NULL;
    l->len = l->cap = 0;
}

static void lines_drop(helper_lines *l, void (*too_long)(void *ctx), void *ctx) {
    l->len = 0;
    if (!l->warned) {
        l->warned = 1;
        if (too_long) too_long(ctx);
    }
}

/* n more bytes of the line coming: 0, or -1 when it would pass max + 1 (a '\r' may follow max
 * bytes) or the buffer can't grow. The buffer keeps one byte more for a NUL. */
static int lines_append(helper_lines *l, const char *p, size_t n) {
    if (!n) return 0;
    if (n > l->max + 1 - l->len) return -1;
    size_t need = l->len + n + 1;
    if (need > l->cap) {
        size_t c = l->cap ? l->cap : 256;
        while (c < need) c *= 2;
        if (c > l->max + 2) c = l->max + 2;
        char *b = realloc(l->buf, c);
        if (!b) {
            l->oom = 1;
            return -1;
        }
        l->buf = b;
        l->cap = c;
    }
    memcpy(l->buf + l->len, p, n);
    l->len += n;
    return 0;
}

/* a line has ended ('\n', or the end of stdout) */
static void lines_emit(helper_lines *l, helper_line_out out, void (*too_long)(void *ctx), void *ctx) {
    size_t n = l->len;
    if (n && l->buf[n - 1] == '\r') n--;
    if (n > l->max) {
        lines_drop(l, too_long, ctx);
        return;
    }
    if (l->buf) l->buf[n] = 0;
    out(ctx, l->buf ? l->buf : "", n);
    l->len = 0;
}

void helper_lines_feed(helper_lines *l, const char *p, size_t n, int at_end, helper_line_out out, void (*too_long)(void *ctx), void *ctx) {
    while (n) {
        const char *nl = memchr(p, '\n', n);
        size_t seg = nl ? (size_t)(nl - p) : n;
        if (!l->discarding && lines_append(l, p, seg)) {
            l->discarding = 1;
            lines_drop(l, too_long, ctx);
        }
        if (nl) {
            if (l->discarding) {
                l->discarding = 0;
                l->len = 0;
            } else
                lines_emit(l, out, too_long, ctx);
            seg++;
        }
        p += seg;
        n -= seg;
    }
    if (at_end) {
        if (!l->discarding && l->len) lines_emit(l, out, too_long, ctx);
        l->discarding = 0;
        l->len = 0;
    }
}

/* prepare.c's options that take a value (besides --ort and --rekordbox): the value is copied as
 * it is, whatever it looks like */
static int takes_value(const char *a) {
    static const char *const v[] = {"--model", "--cache", "--max-gb", "--threads", "--list", "--playlist", "--first", "--tracks",
                                    "--wait-read", "--library", "--keep", "--keep-days", "--until", "--kept-dir"};
    for (size_t i = 0; i < sizeof v / sizeof *v; i++)
        if (!strcmp(a, v[i])) return 1;
    return 0;
}

int helper_program_args(const char *const *argv, int argc, const char *ort, const char *rekordbox, const char **out, int *has_rb) {
    if (has_rb) *has_rb = 0;
    int i = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        out[i] = a;
        if (strncmp(a, "--", 2)) break;   /* the first operand: the rest is copied as it is */
        int is_ort = !strcmp(a, "--ort"), is_rb = !strcmp(a, "--rekordbox");
        if (is_ort || is_rb) {
            if (i + 1 >= argc) return -1;
            out[++i] = is_ort ? (ort ? ort : "") : (rekordbox ? rekordbox : "");
            if (is_rb && has_rb) *has_rb = 1;
        } else if (takes_value(a) && i + 1 < argc) {
            i++;
            out[i] = argv[i];
        }
    }
    for (; i < argc; i++) out[i] = argv[i];
    return 0;
}

int helper_is_run(const char *const *argv, int argc) {
    int keep = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--", 2)) break;
        if (!strcmp(a, "--estimate") || !strcmp(a, "--quick")) return 0;
        if (!strcmp(a, "--keep")) keep = 1;
        if (takes_value(a) || !strcmp(a, "--ort") || !strcmp(a, "--rekordbox")) i++;
    }
    return keep;
}

int helper_run_refused(int has_rb, const char *rekordbox, int is_run) { return has_rb && (!rekordbox || !rekordbox[0]) && is_run; }
