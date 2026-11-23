/* report.h's report itself: the zip, the issue's link and the last problem (Report.swift's
 * buildReport, issueURL, issueDetails, lastProblem, capBytes, knownChecksums, maskLocation), pure or
 * over common/plat.h. The caller supplies the facts and account details, so the Mac tests build
 * whole reports here on a scratch tree. */
#include "report.h"

#include "core.h"
#include "fs_write.h"
#include "plat.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include "win_rules.h"
#  define SEP "\\"
#else
#  define SEP "/"
#endif

/* ------------------------------------------------------------------ a growing buffer */

typedef struct {
    char *p;
    size_t n, cap;
    int err;
} sbuf;

static void sb_add(sbuf *b, const char *s, size_t n) {
    if (b->err) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) {
            b->err = ENOMEM;
            return;
        }
        b->p = q;
        b->cap = c;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_str(sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }
/* the buffer's text, or NULL (freed) on an error; "" when nothing was added */
static char *sb_take(sbuf *b) {
    if (!b->err && !b->p) sb_add(b, "", 0);
    if (b->err) {
        free(b->p);
        return NULL;
    }
    return b->p;
}

static char *dupn(const char *s, size_t n) {
    char *o = malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n);
    o[n] = 0;
    return o;
}

/* the bytes of the UTF-8 sequence starting at s[i] (1 for a byte that doesn't start one) */
static size_t cp_len(const char *s, size_t n, size_t i) {
    unsigned char c = (unsigned char)s[i];
    size_t k = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 1;
    if (i + k > n) return 1;
    for (size_t j = 1; j < k; j++)
        if (((unsigned char)s[i + j] & 0xC0) != 0x80) return 1;
    return k;
}

static const char ELLIPSIS[] = "\xE2\x80\xA6";   /* … */

/* ------------------------------------------------------------------ the zip */

static uint32_t crc_table[256];
static plat_once_t crc_once = PLAT_ONCE_INIT;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

uint32_t report_crc32(uint32_t crc, const void *p, size_t n) {
    plat_once(&crc_once, crc_init);
    const unsigned char *b = p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ b[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void report_zip_init(report_zip *z) { memset(z, 0, sizeof *z); }

void report_zip_free(report_zip *z) {
    free(z->b);
    free(z->cd);
    memset(z, 0, sizeof *z);
}

static int grow(unsigned char **b, size_t *cap, size_t need) {
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 4096;
    while (c < need) {
        if (c > SIZE_MAX / 2) return ENOMEM;
        c *= 2;
    }
    unsigned char *q = realloc(*b, c);
    if (!q) return ENOMEM;
    *b = q;
    *cap = c;
    return 0;
}

static void put16(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void put32(unsigned char *p, uint32_t v) {
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

static int zip_name_ok(const char *name) {
    size_t n = strlen(name);
    if (!n || n > 255 || name[0] == '/') return 0;
    for (size_t i = 0; i < n; i++)
        if (name[i] < 0x20 || name[i] > 0x7E || name[i] == '\\' || name[i] == ':') return 0;
    return 1;
}

int report_zip_add(report_zip *z, const char *name, const void *p, size_t n, const struct tm *when) {
    if (z->err) return z->err;
    size_t nl = strlen(name);
    if (!zip_name_ok(name)) return z->err = EINVAL;
    if (n > 0x7FFFFFFFu || z->n + 30 + nl + n > 0x7FFFFFFFu || z->count >= 0xFFFF) return z->err = EFBIG;
    uint32_t dtime = (uint32_t)(when->tm_hour << 11 | when->tm_min << 5 | when->tm_sec / 2);
    int year = when->tm_year + 1900;
    uint32_t ddate = year < 1980 ? (0u << 9 | 1u << 5 | 1u) : (uint32_t)((year - 1980) << 9 | (when->tm_mon + 1) << 5 | when->tm_mday);
    uint32_t crc = report_crc32(0, p, n);
    size_t off = z->n;
    if (grow(&z->b, &z->cap, z->n + 30 + nl + n) || grow(&z->cd, &z->cdcap, z->cdn + 46 + nl)) return z->err = ENOMEM;
    unsigned char *h = z->b + z->n;
    memset(h, 0, 30);
    put32(h, 0x04034b50u);
    put16(h + 4, 20);            /* version needed: 2.0 */
    put16(h + 8, 0);             /* stored */
    put16(h + 10, dtime);
    put16(h + 12, ddate);
    put32(h + 14, crc);
    put32(h + 18, (uint32_t)n);
    put32(h + 22, (uint32_t)n);
    put16(h + 26, (uint32_t)nl);
    memcpy(h + 30, name, nl);
    if (n) memcpy(h + 30 + nl, p, n);
    z->n += 30 + nl + n;
    unsigned char *c = z->cd + z->cdn;
    memset(c, 0, 46);
    put32(c, 0x02014b50u);
    put16(c + 4, 20);            /* made by: MS-DOS, 2.0 */
    put16(c + 6, 20);
    put16(c + 10, 0);
    put16(c + 12, dtime);
    put16(c + 14, ddate);
    put32(c + 16, crc);
    put32(c + 20, (uint32_t)n);
    put32(c + 24, (uint32_t)n);
    put16(c + 28, (uint32_t)nl);
    put32(c + 42, (uint32_t)off);
    memcpy(c + 46, name, nl);
    z->cdn += 46 + nl;
    z->count++;
    return 0;
}

int report_zip_end(report_zip *z) {
    if (z->err) return z->err;
    if (z->n + z->cdn + 22 > 0x7FFFFFFFu) return z->err = EFBIG;
    if (grow(&z->b, &z->cap, z->n + z->cdn + 22)) return z->err = ENOMEM;
    size_t at = z->n;
    if (z->cdn) memcpy(z->b + z->n, z->cd, z->cdn);
    z->n += z->cdn;
    unsigned char *e = z->b + z->n;
    memset(e, 0, 22);
    put32(e, 0x06054b50u);
    put16(e + 8, (uint32_t)z->count);
    put16(e + 10, (uint32_t)z->count);
    put32(e + 12, (uint32_t)z->cdn);
    put32(e + 16, (uint32_t)at);
    z->n += 22;
    return 0;
}

/* ------------------------------------------------------------------ the issue's text */

char *report_cap_bytes(const char *s, size_t bytes) {
    size_t n = strlen(s);
    if (n <= bytes) return dupn(s, n);
    size_t k = 0, lim = bytes >= 3 ? bytes - 3 : 0;
    while (k < n) {
        size_t l = cp_len(s, n, k);
        if (k + l > lim) break;
        k += l;
    }
    char *o = malloc(k + sizeof ELLIPSIS);
    if (!o) return NULL;
    memcpy(o, s, k);
    memcpy(o + k, ELLIPSIS, sizeof ELLIPSIS);
    return o;
}

static int hexc(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

void report_strings_free(char **list, int n) {
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
}

int report_known_checksums(const char *s, size_t n, char ***list, int *count) {
    for (size_t i = 0; i < n;) {
        if (!hexc(s[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && hexc(s[j])) j++;
        size_t l = j - i;
        if (l == 64 || l == 40) {
            char h[65];
            for (size_t k = 0; k < l; k++) h[k] = (char)(s[i + k] >= 'A' && s[i + k] <= 'F' ? s[i + k] - 'A' + 'a' : s[i + k]);
            h[l] = 0;
            int seen = 0;
            for (int k = 0; k < *count && !seen; k++) seen = !strcmp((*list)[k], h);
            if (!seen) {
                char **q = realloc(*list, sizeof *q * (size_t)(*count + 1));
                if (!q) return -1;
                *list = q;
                if (!(q[*count] = dupn(h, l))) return -1;
                (*count)++;
            }
        }
        i = j;
    }
    return 0;
}

char *report_issue_details(const char *problem, const char *const *facts, int n) {
    sbuf b = {0};
    if (problem && problem[0]) {
        char *c = report_cap_bytes(problem, 800);
        if (!c) return NULL;
        sb_str(&b, c);
        sb_str(&b, "\n\n");
        free(c);
    }
    for (int i = 0; i < n; i++) {
        char *one = dupn(facts[i], strlen(facts[i]));
        if (!one) b.err = ENOMEM;
        else {
            for (char *p = one; *p; p++)
                if (*p == '\n') *p = ' ';
            char *c = report_cap_bytes(one, 300);
            if (!c) b.err = ENOMEM;
            else {
                if (i) sb_str(&b, "\n");
                sb_str(&b, c);
            }
            free(c);
        }
        free(one);
    }
    return sb_take(&b);
}

static void enc(sbuf *b, const char *s, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~')
            sb_add(b, (const char *)&c, 1);
        else {
            char e[3] = {'%', hex[c >> 4], hex[c & 15]};
            sb_add(b, e, 3);
        }
    }
}

/* the link with the first `len` bytes of details */
static char *url_of(const char *title, const char *details, size_t len) {
    static const char head[] = "### What happened?\n\n\n\n### Technical details\n"
                               "Please also drag in the report zip that RB Stems Plus just showed in Explorer (it has the full logs).\n\n```\n";
    char *t = report_cap_bytes(title, 120);
    if (!t) return NULL;
    sbuf b = {0};
    sb_str(&b, REPORT_ISSUES_URL "?title=");
    enc(&b, t, strlen(t));
    sb_str(&b, "&body=");
    enc(&b, head, sizeof head - 1);
    enc(&b, details, len);
    enc(&b, "\n```", 4);
    free(t);
    return sb_take(&b);
}

char *report_issue_url(const char *title, const char *details) {
    size_t len = strlen(details);
    for (;;) {
        char *u = url_of(title, details, len);
        if (!u || strlen(u) <= REPORT_URL_MAX || len == 0) return u;
        free(u);
        /* the last line left out (components(separatedBy: "\n"), then removeLast) */
        size_t k = len;
        while (k > 0 && details[k - 1] != '\n') k--;
        len = k ? k - 1 : 0;
    }
}

typedef struct {
    const report_env *e;
    report_zip *z;
    const char *top;   /* the zip's folder */
    const struct tm *when;
    const char *const *keep;
    int nkeep;
} adder;

/* linkMask: the location's masking, then redact */
static char *link_mask(const adder *a, const char *s) {
    const report_env *e = a->e;
    char *loc = report_mask_location(s, e->location_dir, e->sysdrive, e->drive_names, e->ndrive_names);
    size_t len = 0;
    char *masked = loc ? e->mask(e->ctx, loc, strlen(loc), a->keep, a->nkeep, &len) : NULL;
    free(loc);
    return masked;
}

/* ------------------------------------------------------------------ the last problem */

static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t report_local_s(int64_t t) {
    struct tm tm;
    plat_localtime(t, &tm);
    return days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

/* "yyyy-MM-dd HH:mm:ss" at s: 0 with *t (as report_local_s), else -1 */
static int stamp_of(const char *s, int64_t *t) {
    static const char shape[] = "dddd-dd-dd dd:dd:dd";
    for (int i = 0; i < 19; i++)
        if (shape[i] == 'd' ? !(s[i] >= '0' && s[i] <= '9') : s[i] != shape[i]) return -1;
    int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0'), mo = (s[5] - '0') * 10 + (s[6] - '0'),
        d = (s[8] - '0') * 10 + (s[9] - '0'), h = (s[11] - '0') * 10 + (s[12] - '0'), mi = (s[14] - '0') * 10 + (s[15] - '0'),
        se = (s[17] - '0') * 10 + (s[18] - '0');
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 59) return -1;
    *t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    return 0;
}

typedef struct {
    const char *stamp;   /* 19 bytes */
    int64_t date;
    const char *msg;     /* NUL-terminated (in the lines' copy) */
} lp_line;

/* the log's lines of the window, oldest first, into *out (malloc'd, *n); the messages point into
 * *copy (malloc'd). 0, or -1 out of memory */
static int recent(const char *log, size_t ln, int64_t now, lp_line **out, int *n, char **copy) {
    *out = NULL;
    *n = 0;
    *copy = dupn(log ? log : "", log ? ln : 0);
    if (!*copy) return -1;
    char *s = *copy;
    size_t total = ln, cap = 0;
    for (size_t i = 0; i < total;) {
        size_t j = i;
        while (j < total && s[j] != '\n') j++;
        s[j < total ? j : total] = 0;
        int64_t d;
        if (j - i > 19 && !stamp_of(s + i, &d) && d <= now + 60 && now - d <= REPORT_PROBLEM_WINDOW_S) {
            char *m = s + i + 19, *e = s + j;
            while (m < e && (*m == ' ' || *m == '\t')) m++;
            while (e > m && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
            *e = 0;
            if ((size_t)*n == cap) {
                cap = cap ? cap * 2 : 64;
                lp_line *q = realloc(*out, sizeof *q * cap);
                if (!q) return -1;
                *out = q;
            }
            (*out)[(*n)++] = (lp_line){s + i, d, m};
        }
        i = j + 1;
    }
    return 0;
}

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

static int word_c(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }

/* \b(failed|refused|couldn't|could not|cannot|error)\b, case ignored */
static int failed_in(const char *m) {
    static const char *const words[] = {"failed", "refused", "couldn't", "could not", "cannot", "error"};
    size_t n = strlen(m);
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && word_c((unsigned char)m[i - 1])) continue;
        for (size_t w = 0; w < sizeof words / sizeof *words; w++) {
            size_t l = strlen(words[w]);
            if (i + l > n) continue;
            size_t k = 0;
            while (k < l && (m[i + k] | 0x20) == words[w][k]) k++;
            if (k == l && (i + l == n || !word_c((unsigned char)m[i + l]))) return 1;
        }
    }
    return 0;
}

/* ^admin run \((.+)\), attempt \d+$: the name (malloc'd), or NULL */
static char *admin_start(const char *m) {
    static const char head[] = "admin run (", mid[] = "), attempt ";
    if (!starts(m, head)) return NULL;
    size_t n = strlen(m), digits = 0;
    while (digits < n && m[n - 1 - digits] >= '0' && m[n - 1 - digits] <= '9') digits++;
    size_t ml = sizeof mid - 1, hl = sizeof head - 1;
    if (!digits || n - digits < hl + 1 + ml || strncmp(m + n - digits - ml, mid, ml)) return NULL;
    return dupn(m + hl, n - digits - ml - hl);
}

enum { FIX_NEVER, FIX_ADMIN, FIX_PLUS, FIX_CACHE };
typedef struct {
    int index;
    char *msg, *title;   /* malloc'd */
    const char *stamp;
    int64_t date;
    int fix;
    const char *name;    /* FIX_ADMIN's run */
} lp_problem;

static void problems_free(lp_problem *p, int n) {
    for (int i = 0; i < n; i++) {
        free(p[i].msg);
        free(p[i].title);
    }
    free(p);
}

/* s cut to n code points, "…" in place of the last when cut */
static void cut_chars(sbuf *b, const char *s, size_t n) {
    size_t len = strlen(s), k = 0, count = 0, at = 0;
    for (; k < len; count++) k += cp_len(s, len, k);
    if (count <= n) {
        sb_str(b, s);
        return;
    }
    for (count = 0; count + 1 < n; count++) at += cp_len(s, len, at);
    sb_add(b, s, at);
    sb_str(b, ELLIPSIS);
}

/* Mask complete names and paths before the cut. */
static void cut_link(sbuf *b, const char *s, size_t n, const adder *a) {
    if (!a) {
        cut_chars(b, s, n);
        return;
    }
    char *masked = link_mask(a, s);
    if (masked) cut_chars(b, masked, n);
    else b->err = ENOMEM;
    free(masked);
}

typedef struct {
    const lp_problem *first, *last;
    int count;
} lp_group;

static int collapse(const lp_problem *p, int n, lp_group *g) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k && !strcmp(g[k - 1].first->msg, p[i].msg)) {
            g[k - 1].count++;
            g[k - 1].last = &p[i];
        } else g[k++] = (lp_group){&p[i], &p[i], 1};
    }
    return k;
}

static void render(sbuf *b, const lp_group *g, const adder *a) {
    sbuf one = {0};
    sb_add(&one, g->first->stamp, 19);
    sb_str(&one, "  ");
    sb_str(&one, g->first->msg);
    if (one.err) b->err = one.err;
    else cut_link(b, one.p, 200, a);
    free(one.p);
    if (g->count > 1) {
        char t[64];
        snprintf(t, sizeof t, " (%d times, last at %.5s)", g->count, g->last->stamp + 11);
        sb_str(b, t);
    }
}

static int add_problem(lp_problem **v, int *n, int *cap, lp_problem p) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        lp_problem *q = realloc(*v, sizeof *q * (size_t)*cap);
        if (!q) {
            free(p.msg);
            free(p.title);
            return -1;
        }
        *v = q;
    }
    (*v)[(*n)++] = p;
    return 0;
}

static int last_problem(const char *app, size_t an, const char *bridge, size_t bn, int64_t now, const adder *a, char **title, char **text) {
    *title = *text = NULL;
    lp_line *al = NULL, *bl = NULL;
    char *ac = NULL, *bc = NULL;
    char **ok_run = NULL;   /* line index: the name of an admin run that succeeded there */
    char *run = NULL;
    lp_problem *found = NULL, *kept = NULL, *bp = NULL;
    int nal = 0, nbl = 0, nfound = 0, capf = 0, nkept = 0, nbp = 0, capb = 0, fail = 0;
    lp_group *ag = NULL, *bg = NULL;
    if (recent(app, an, now, &al, &nal, &ac) || recent(bridge, bn, now, &bl, &nbl, &bc)) fail = 1;
    if (!fail && nal && !(ok_run = calloc((size_t)nal, sizeof *ok_run))) fail = 1;
    for (int i = 0; i < nal && !fail; i++) {
        const char *m = al[i].msg;
        char *name = admin_start(m);
        if (name) {
            free(run);
            run = name;
            continue;
        }
        if (run && starts(m, "ok:")) {
            ok_run[i] = run;
            run = NULL;
            continue;
        }
        if (run && starts(m, "FAILED")) {
            sbuf msg = {0}, t = {0};
            sb_str(&msg, "admin run (");
            sb_str(&msg, run);
            sb_str(&msg, ") ");
            sb_str(&msg, m);
            sb_str(&t, "admin run (");
            sb_str(&t, run);
            sb_str(&t, ") failed");
            /* the run's name lives on in the problem (freed with it below, FIX_ADMIN's name) */
            lp_problem p = {i, sb_take(&msg), sb_take(&t), al[i].stamp, al[i].date, FIX_ADMIN, run};
            if (!p.msg || !p.title) {
                free(p.msg);
                free(p.title);
                free(run);
                fail = 1;
            } else if (add_problem(&found, &nfound, &capf, p)) {
                free(run);
                fail = 1;
            }
            run = NULL;
            continue;
        }
        int plus = starts(m, "Demucs v4 not installed") || starts(m, "Stems Plus not installed"), cache = starts(m, "Stems Cache not installed");
        if (starts(m, "report:") || !(plus || cache || failed_in(m))) continue;
        lp_problem p = {i, dupn(m, strlen(m)), dupn(m, strlen(m)), al[i].stamp, al[i].date, plus ? FIX_PLUS : cache ? FIX_CACHE : FIX_NEVER, NULL};
        if (!p.msg || !p.title) {
            free(p.msg);
            free(p.title);
            fail = 1;
        } else if (add_problem(&found, &nfound, &capf, p)) fail = 1;
    }
    free(run);
    /* fixed by a later line (ok_run is complete only now) */
    if (!fail && nfound && !(kept = malloc(sizeof *kept * (size_t)nfound))) fail = 1;
    for (int k = 0; k < nfound && !fail; k++) {
        const lp_problem *p = &found[k];
        int fixed = 0;
        for (int j = p->index + 1; j < nal && !fixed; j++) {
            if (p->fix == FIX_ADMIN) fixed = ok_run[j] && !strcmp(ok_run[j], p->name);
            else if (p->fix == FIX_PLUS)
                fixed = starts(al[j].msg, "Demucs v4: installed") || starts(al[j].msg, "Demucs v4: already in place") || starts(al[j].msg, "Stems Plus: installed")
                        || starts(al[j].msg, "Stems Plus: already in place");
            else if (p->fix == FIX_CACHE) fixed = ok_run[j] && (!strcmp(ok_run[j], "install") || !strcmp(ok_run[j], "reinstall"));
        }
        if (!fixed) kept[nkept++] = *p;   /* shallow: found owns the strings */
    }
    /* bridge.log's ERROR lines, unless a later load or session shows them fixed */
    for (int i = 0; i < nbl && !fail; i++) {
        const char *m = bl[i].msg;
        if (!starts(m, "ERROR")) continue;
        const char *fix = starts(m, "ERROR real library") || starts(m, "ERROR no real ONNX Runtime") || starts(m, "ERROR the home folder") ? "bridge loaded"
                          : starts(m, "ERROR cannot read")                                                                                   ? "Demucs session opened"
                                                                                                                                               : NULL;
        int fixed = 0;
        for (int j = i + 1; fix && j < nbl && !fixed; j++) fixed = starts(bl[j].msg, fix);
        if (fixed) continue;
        const char *rest = m + 5;
        while (*rest == ' ' || *rest == '\t') rest++;
        sbuf t = {0};
        sb_str(&t, "Stems Cache: ");
        sb_str(&t, rest);
        lp_problem p = {i, dupn(m, strlen(m)), sb_take(&t), bl[i].stamp, bl[i].date, FIX_NEVER, NULL};
        if (!p.msg || !p.title) {
            free(p.msg);
            free(p.title);
            fail = 1;
        } else if (add_problem(&bp, &nbp, &capb, p)) fail = 1;
    }
    sbuf out = {0};
    const char *t = NULL;
    if (!fail && ((nkept && !(ag = malloc(sizeof *ag * (size_t)nkept))) || (nbp && !(bg = malloc(sizeof *bg * (size_t)nbp))))) fail = 1;
    if (!fail) {
        int na = collapse(kept, nkept, ag), nb = collapse(bp, nbp, bg);
        int a0 = na > 5 ? na - 5 : 0, b0 = nb > 3 ? nb - 3 : 0;
        if (na > a0) t = ag[na - 1].first->title;
        else if (nb > b0) t = bg[nb - 1].first->title;
        if (t) {
            sb_str(&out, "Last problem:\n");
            int first = 1;
            for (int i = a0; i < na; i++, first = 0) {
                if (!first) sb_str(&out, "\n");
                render(&out, &ag[i], a);
            }
            if (nb > b0) {
                if (!first) sb_str(&out, "\n");
                sb_str(&out, "bridge.log:");
                for (int i = b0; i < nb; i++) {
                    sb_str(&out, "\n");
                    render(&out, &bg[i], a);
                }
            }
        }
        sbuf tt = {0};
        if (t) cut_link(&tt, t, 60, a);
        *title = sb_take(&tt);
        *text = sb_take(&out);
        if (!*title || !*text) fail = 1;
    } else free(out.p);
    for (int k = 0; k < nfound; k++)
        if (found[k].fix == FIX_ADMIN) free((char *)found[k].name);
    if (ok_run)
        for (int i = 0; i < nal; i++) free(ok_run[i]);
    free(ok_run);
    problems_free(found, nfound);
    problems_free(bp, nbp);
    free(kept);
    free(ag);
    free(bg);
    free(al);
    free(bl);
    free(ac);
    free(bc);
    if (fail) {
        free(*title);
        free(*text);
        *title = *text = NULL;
        return -1;
    }
    return 0;
}

int report_last_problem(const char *app, size_t an, const char *bridge, size_t bn, int64_t now, char **title, char **text) {
    return last_problem(app, an, bridge, bn, now, NULL, title, text);
}

/* ------------------------------------------------------------------ the link's masking */

static int ieq(char a, char b) { return (a | (a >= 'A' && a <= 'Z' ? 0x20 : 0)) == (b | (b >= 'A' && b <= 'Z' ? 0x20 : 0)); }

static int at_ci(const char *s, size_t n, size_t i, const char *p, size_t pl) {
    if (i + pl > n) return 0;
    for (size_t k = 0; k < pl; k++)
        if (!ieq(s[i + k], p[k])) return 0;
    return 1;
}

static int alnum_c(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int path_end(char c) { return c == '"' || c == '\'' || c == '(' || c == ')' || c == '\n' || c == 0; }

#if defined(_WIN32)
static int same_ci(const char *s, size_t n, const char *p, size_t pl) {
    wchar_t sw[REPORT_PATH_MAX], pw[REPORT_PATH_MAX];
    long sn = wr_utf8_to_16(s, n, (uint16_t *)sw, REPORT_PATH_MAX);
    long pn = wr_utf8_to_16(p, pl, (uint16_t *)pw, REPORT_PATH_MAX);
    return sn > 0 && pn > 0 && CompareStringOrdinal(sw, (int)sn, pw, (int)pn, TRUE) == CSTR_EQUAL;
}
#endif

static size_t short_home_len(const char *s, size_t n, const char *home, size_t sl) {
    if (at_ci(s, n, 0, home, sl)) return sl;
#if defined(_WIN32)
    if (n < 2 || !ieq(s[0], home[0]) || s[1] != home[1]) return 0;
    /* A Windows case match can use different numbers of UTF-8 bytes. */
    size_t end = 0;
    for (size_t k = 0; k < sl;) {
        if (end == n) return 0;
        while (++k < sl && ((unsigned char)home[k] & 0xC0) == 0x80) {}
        while (++end < n && ((unsigned char)s[end] & 0xC0) == 0x80) {}
    }
    if (same_ci(s, end, home, sl)) return end;
#endif
    return 0;
}

static char *mask_account(const report_mask_names *m, const char *in, size_t n, const char *const *keep, int nkeep, size_t *len) {
    const char *names[3];
    int nn = 0;
    /* Replace the full name first, in case a bare account name is part of it. */
#if defined(_WIN32)
    if (m->full[0] && !same_ci(m->full, strlen(m->full), m->user, strlen(m->user))) names[nn++] = m->full;
#else
    if (m->full[0] && strcasecmp(m->full, m->user)) names[nn++] = m->full;
#endif
    if (m->user[0]) names[nn++] = m->user;
    if (m->folder_name[0] && strcasecmp(m->folder_name, m->user)) names[nn++] = m->folder_name;
    core_mask_ctx mc;
    memset(&mc, 0, sizeof mc);
    mc.size = sizeof mc;
    mc.home = m->home[0] ? m->home : NULL;
    mc.names = names;
    mc.n_names = nn;
    mc.allowed = keep;
    mc.n_allowed = nkeep;
    size_t cap = n * 2 + 64;   /* "<user>" may be longer than the name it stands for */
    for (int tries = 0; tries < 3; tries++) {
        char *b = malloc(cap);
        if (!b) return NULL;
        size_t got = 0;
        int32_t r = core_mask(&mc, in, n, b, cap, &got);
        if (r == CORE_OK) {
            *len = got;
            return b;
        }
        free(b);
        if (r != CORE_E_SPACE) return NULL;   /* no memory, or refused */
        cap = got + 1;
    }
    return NULL;
}

char *report_mask_with(void *ctx, const char *in, size_t n, const char *const *keep, int nkeep, size_t *len) {
    const report_mask_names *m = ctx;
    size_t sl = strlen(m->short_home), last = 0;
    sbuf b = {0};
    /* An 8.3 alias is a second profile path. Its last component can occur in other paths. */
    for (size_t i = 0; sl && i < n; i++) {
        if (i && alnum_c(in[i - 1])) continue;
        size_t match = short_home_len(in + i, n - i, m->short_home, sl);
        if (!match) continue;
        size_t end = i + match;
        if (end < n && in[end] != '\\' && in[end] != '/' && !path_end(in[end]) && !isspace((unsigned char)in[end])
            && in[end] != '<' && in[end] != '>') continue;
        sb_add(&b, in + last, i - last);
        sb_str(&b, "~");
        last = end;
        i = end - 1;
    }
    if (!last) return mask_account(m, in, n, keep, nkeep, len);
    sb_add(&b, in + last, n - last);
    char *s = sb_take(&b);
    if (!s) return NULL;
    char *r = mask_account(m, s, b.n, keep, nkeep, len);
    free(s);
    return r;
}

char *report_mask_location(const char *s, const char *dir, char sysdrive, const char *const *names, int nnames) {
    size_t n = strlen(s), dl = dir ? strlen(dir) : 0;
    sbuf b = {0};
    char sys = (char)(sysdrive >= 'a' && sysdrive <= 'z' ? sysdrive - 'a' + 'A' : sysdrive);
    for (size_t i = 0; i < n;) {
        if (dl && at_ci(s, n, i, dir, dl)) {
            sb_str(&b, "<stems folder>");
            i += dl;
            continue;
        }
        char c = s[i], u = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        int drive = u >= 'A' && u <= 'Z' && i + 2 < n && s[i + 1] == ':' && (s[i + 2] == '\\' || s[i + 2] == '/')
            && (i == 0 || !alnum_c(s[i - 1])) && u != sys;
        int unc = c == '\\' && i + 2 < n && s[i + 1] == '\\'
            && (alnum_c(s[i + 2]) || s[i + 2] == '?' || s[i + 2] == '.' || (unsigned char)s[i + 2] >= 0x80);
        if (drive || unc) {
            size_t j = i;
            while (j < n && !path_end(s[j])) j++;
            sb_str(&b, drive ? "<drive>:\\<path>" : "\\\\<drive>\\<path>");
            i = j;
            continue;
        }
        int named = 0;
        if (c == '"')
            for (int k = 0; k < nnames && !named; k++) {
                size_t l = names[k] ? strlen(names[k]) : 0;
                if (l && i + l + 1 < n && !strncmp(s + i + 1, names[k], l) && s[i + 1 + l] == '"') {
                    sb_str(&b, "\"<drive>\"");
                    i += l + 2;
                    named = 1;
                }
            }
        if (named) continue;
        sb_add(&b, s + i, 1);
        i++;
    }
    return sb_take(&b);
}

/* ------------------------------------------------------------------ the facts */

void report_fact_add(report_facts *f, int link, const char *fmt, ...) {
    if (f->err) return;
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    int k = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    char *line = k >= 0 ? malloc((size_t)k + 1) : NULL;
    if (line) vsnprintf(line, (size_t)k + 1, fmt, b);
    va_end(b);
    if (!line) {
        f->err = ENOMEM;
        return;
    }
    if (f->n == f->cap) {
        int c = f->cap ? f->cap * 2 : 32;
        report_fact *q = realloc(f->v, sizeof *q * (size_t)c);
        if (!q) {
            free(line);
            f->err = ENOMEM;
            return;
        }
        f->v = q;
        f->cap = c;
    }
    f->v[f->n++] = (report_fact){line, link};
}

void report_facts_free(report_facts *f) {
    for (int i = 0; i < f->n; i++) free(f->v[i].line);
    free(f->v);
    memset(f, 0, sizeof *f);
}

const char *report_light_colour(const char *look) { return !strcmp(look, "0") ? "green" : !strcmp(look, "1") ? "yellow" : "red"; }

void report_ago(int64_t t, char *out, size_t n) {
    if (t < 0) snprintf(out, n, "never");
    else if (t < 3600) snprintf(out, n, "%lld min ago", (long long)(t / 60));
    else if (t < 48 * 3600) snprintf(out, n, "%lld h ago", (long long)(t / 3600));
    else snprintf(out, n, "%lld days ago", (long long)(t / 86400));
}

void report_gb(int64_t bytes, char *out, size_t n) {
    if (bytes < 0) snprintf(out, n, "unknown");
    else snprintf(out, n, "%.1f GB", (double)bytes / 1e9);
}

/* ------------------------------------------------------------------ the report */

void report_result_free(report_result *r) {
    free(r->title);
    free(r->details);
    r->title = r->details = NULL;
}

/* the last max bytes of dir\name (a regular file, no link followed) as UTF-8; 0 with *out
 * (malloc'd), ENOENT none, another errno */
static int tail_of(const char *dir, const char *name, size_t max, char **out, size_t *len) {
    *out = NULL;
    *len = 0;
    if (!dir || !dir[0]) return ENOENT;
    plat_dir d;
    plat_file f;
    int64_t size = 0;
    int e = plat_dir_open(dir, &d);
    if (e) return e;
    e = plat_file_open_at(d, name, INT64_MAX, &size, &f);
    plat_dir_close(d);
    if (e) return e;
    int64_t from = size > (int64_t)max ? size - (int64_t)max : 0;
    size_t want = (size_t)(size - from), got = 0;
    unsigned char *b = malloc(want ? want : 1);
    if (!b) e = ENOMEM;
    while (!e && got < want) {
        size_t k = 0;
        e = plat_pread(f, b + got, want - got, from + (int64_t)got, &k);
        if (!e && !k) break;   /* shorter now than it was */
        got += k;
    }
    plat_file_close(f);
    if (!e) {
        *out = report_text_utf8(b, got, len);
        if (!*out) e = ENOMEM;
    }
    free(b);
    return e;
}

static void say(const report_env *e, const char *line) {
    if (e->log) e->log(e->log_ctx, line);
}

/* name's text, masked, into the zip (an out-of-memory mask: z->err) */
static int add_masked(adder *a, const char *name, const char *text, size_t n) {
    size_t ml = 0;
    char *m = a->e->mask(a->e->ctx, text, n, a->keep, a->nkeep, &ml);
    char path[512];
    if (!m) return a->z->err = ENOMEM;
    int k = snprintf(path, sizeof path, "%s/%s", a->top, name);
    int r = k >= 0 && k < (int)sizeof path ? report_zip_add(a->z, path, m, ml, a->when) : ENAMETOOLONG;
    free(m);
    return r;
}

/* a WER folder's name as a zip name: printable ASCII but '/', '\\' and ':', at most 120 bytes */
static void wer_name(const char *path, char *out, size_t n) {
    const char *end = strrchr(path, SEP[0]), *start = end;
    while (start && start > path && start[-1] != SEP[0]) start--;
    size_t k = 0;
    for (const char *p = start; p && p < end && k + 1 < n && k < 120; p++) out[k++] = *p > 0x20 && *p < 0x7F && *p != '/' && *p != '\\' && *p != ':' ? *p : '_';
    out[k] = 0;
    if (!k) snprintf(out, n, "report");
}

static int write_zip(const char *dir, const char *name, const report_zip *z, report_result *out, int follow) {
    if (!dir || !dir[0]) return ENOENT;
    /* The caller needs the full path to open the saved report. */
    int k = snprintf(out->path, sizeof out->path, "%s" SEP "%s", dir, name);
    if (k < 0 || (size_t)k >= sizeof out->path) return ENAMETOOLONG;
    if (!follow) plat_mkdirs(dir);
    plat_dir d;
    int r = follow ? plat_dir_open_follow(dir, &d) : plat_dir_open(dir, &d);
    if (r) return r;
    r = win_write_at(d, name, z->b, z->n);
    plat_dir_close(d);
    return r;
}

int report_build(const report_env *e, report_result *out) {
    memset(out, 0, sizeof *out);
    if (!e->mask || !e->facts) return EINVAL;
    struct tm when;
    plat_localtime(e->now, &when);
    char top[96], zipname[112];
    snprintf(top, sizeof top, "RB Stems Plus report %04d-%02d-%02d %02d%02d%02d", when.tm_year + 1900, when.tm_mon + 1, when.tm_mday, when.tm_hour,
             when.tm_min, when.tm_sec);
    snprintf(zipname, sizeof zipname, "%s.zip", top);
    int err = 0;
    /* summary.txt: every fact, one a line */
    sbuf sum = {0};
    char head[96];
    snprintf(head, sizeof head, "RB Stems Plus report, %04d-%02d-%02d %02d:%02d:%02d\n", when.tm_year + 1900, when.tm_mon + 1, when.tm_mday,
             when.tm_hour, when.tm_min, when.tm_sec);
    sb_str(&sum, head);
    for (int i = 0; i < e->facts->n; i++) {
        sb_str(&sum, e->facts->v[i].line);
        sb_str(&sum, "\n");
    }
    char *summary = sb_take(&sum);
    /* the checksums the report prints itself, and payload.json's (public): never masked */
    char payload_dir[REPORT_PATH_MAX];
    char *manifest = NULL, **keep = NULL;
    size_t mlen = 0;
    int nkeep = 0;
    if (e->support && snprintf(payload_dir, sizeof payload_dir, "%s" SEP "payload", e->support) < (int)sizeof payload_dir)
        tail_of(payload_dir, "payload.json", REPORT_TAIL_MAX, &manifest, &mlen);
    else payload_dir[0] = 0;
    if (!summary || report_known_checksums(summary, strlen(summary), &keep, &nkeep) || (manifest && report_known_checksums(manifest, mlen, &keep, &nkeep)))
        err = ENOMEM;
    report_zip z;
    report_zip_init(&z);
    adder a = {e, &z, top, &when, (const char *const *)keep, nkeep};
    if (!err) add_masked(&a, "summary.txt", summary, strlen(summary));
    /* the logs (their last 2 MB) and the small state files, masked */
    struct {
        const char *dir, *name;
    } files[] = {{e->logs, "app.log"},       {e->logs, "watcher.log"}, {e->logs, "bridge.log"},  {e->support, "installed"}, {e->support, "dismissed"},
                 {e->support, "request"},    {e->support, "busy"},     {e->support, "config.ini"}, {payload_dir, "payload.json"}};
    for (size_t i = 0; i < sizeof files / sizeof *files && !err && !z.err; i++) {
        char *t = NULL;
        size_t tl = 0;
        if (tail_of(files[i].dir, files[i].name, REPORT_TAIL_MAX, &t, &tl) == 0) add_masked(&a, files[i].name, t, tl);
        free(t);
    }
    /* rekordbox's and our programs' errors of the last 30 minutes, from the Application log */
    if (e->events && !err && !z.err) {
        char *t = NULL;
        size_t tl = 0;
        int r = e->events(e->ctx, &t, &tl);
        if (r) {
            char line[96];
            free(t);
            snprintf(line, sizeof line, "the Application event log couldn't be read (error %d)\n", r);
            add_masked(&a, "event-log-errors.txt", line, strlen(line));
        } else add_masked(&a, "event-log-errors.txt", t ? t : "", t ? tl : 0);
        free(t);
    }
    /* crash and hang reports (Report.wer only) of the last 7 days: the newest 5 */
    report_wer wer[REPORT_WER_MAX];
    int found = 0, nwer = 0, included = 0;
    if (e->nwer > 0 && e->wer_roots) nwer = report_wer_find(e->wer_roots, e->nwer, e->now - (int64_t)REPORT_WER_DAYS * 86400, wer, REPORT_WER_MAX, &found);
    for (int i = 0; i < nwer && !err && !z.err; i++) {
        char *t = NULL, nm[160], zn[200];
        size_t tl = 0;
        if (report_wer_text(wer[i].path, REPORT_TAIL_MAX, &t, &tl)) continue;
        wer_name(wer[i].path, nm, sizeof nm);
        /* Different WER roots can contain the same folder name. */
        snprintf(zn, sizeof zn, "crash-reports/%d-%s.wer.txt", i, nm);
        if (!add_masked(&a, zn, t, tl)) included++;
        free(t);
    }
    char line[160];
    snprintf(line, sizeof line, "report: %d crash or hang report%s from the last 7 days, %d included", found, found == 1 ? "" : "s", included);
    say(e, line);
    /* the issue's title and details: the last problem, then the short lines, masked; neither names
     * a drive or a path on one (the zip keeps them) */
    char *app = NULL, *br = NULL, *ptitle = NULL, *ptext = NULL;
    size_t al = 0, bl = 0;
    tail_of(e->logs, "app.log", REPORT_PROBLEM_TAIL, &app, &al);
    tail_of(e->logs, "bridge.log", REPORT_PROBLEM_TAIL, &br, &bl);
    if (!err && last_problem(app, al, br, bl, report_local_s(e->now), &a, &ptitle, &ptext)) err = ENOMEM;
    free(app);
    free(br);
    char **links = NULL;
    int nlinks = 0;
    if (!err && e->facts->n && !(links = calloc((size_t)e->facts->n, sizeof *links))) err = ENOMEM;
    for (int i = 0; i < e->facts->n && !err; i++) {
        if (!e->facts->v[i].link) continue;
        links[nlinks] = link_mask(&a, e->facts->v[i].line);
        if (!links[nlinks++]) err = ENOMEM;
    }
    if (!err && !(out->details = report_issue_details(ptext, (const char *const *)links, nlinks))) err = ENOMEM;
    if (!err) {
        sbuf tb = {0};
        sb_str(&tb, "Problem report");
        if (ptitle[0]) {
            sb_str(&tb, ": ");
            sb_str(&tb, ptitle);
        }
        if (!(out->title = sb_take(&tb))) err = ENOMEM;
    }
    report_strings_free(links, nlinks);
    free(ptitle);
    free(ptext);
    if (!err && report_zip_end(&z)) err = z.err;
    if (!err && z.err) err = z.err;
    /* on the Desktop; if Windows (or the user) refuses that, next to the logs */
    if (!err) {
        int r = write_zip(e->desktop, zipname, &z, out, 1);
        if (r) {
            if (e->desktop && e->desktop[0]) {
                snprintf(line, sizeof line, "report: couldn't save it on the Desktop (error %d)", r);
                say(e, line);
            }
            r = write_zip(e->fallback, zipname, &z, out, 0);
            if (r) {
                snprintf(line, sizeof line, "report: couldn't save it next to the logs either (error %d)", r);
                say(e, line);
            }
        }
        if (r) err = r;
    } else {
        snprintf(line, sizeof line, "report: the zip couldn't be made (error %d)", err);
        say(e, line);
    }
    report_zip_free(&z);
    report_strings_free(keep, nkeep);
    free(manifest);
    free(summary);
    if (err) report_result_free(out);
    return err;
}
