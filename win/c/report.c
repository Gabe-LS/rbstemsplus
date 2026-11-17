/* report.h's pure parts and the WER walk over common/plat.h. */
#include "report.h"

#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define SEP '\\'
#else
#  define SEP '/'
#endif

static char low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int prefix_ci(const char *s, const char *p) {
    for (; *p; s++, p++)
        if (low(*s) != low(*p)) return 0;
    return 1;
}

/* the names WER's folders start with (after "AppCrash_" or "AppHang_", cut short by WER) */
static const char *const names[] = {"rekordbox", "RB Stems Plus", "rbsp-"};

/* ------------------------------------------------------------------ the event log */

static size_t find(const char *s, size_t n, size_t from, const char *w) {
    size_t l = strlen(w);
    for (size_t i = from; i + l <= n; i++)
        if (!memcmp(s + i, w, l)) return i;
    return n;
}


int report_event_query(uint32_t minutes, char *out, size_t n) {
    int r = snprintf(out, n, "*[System[(Level=1 or Level=2) and TimeCreated[timediff(@SystemTime) <= %llu]]]",
                     (unsigned long long)minutes * 60000ull);
    return r < 0 || (size_t)r >= n ? -1 : 0;
}

/* n bytes at s start with p, case ignored */
static int prefix_ci_n(const char *s, size_t n, const char *p) {
    size_t l = strlen(p);
    if (l > n) return 0;
    for (size_t i = 0; i < l; i++)
        if (low(s[i]) != low(p[i])) return 0;
    return 1;
}

static int alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* whether s[i, b) starts with one of our images' names: rekordbox.exe, RB Stems Plus.exe, or
 * rbsp-<letters, digits, '-'>.exe/.dll, not inside a longer word */
static int image_at(const char *s, size_t i, size_t b) {
    if (i > 0 && alnum(s[i - 1])) return 0;
    if (prefix_ci_n(s + i, b - i, "rekordbox.exe") || prefix_ci_n(s + i, b - i, "RB Stems Plus.exe")) return 1;
    if (!prefix_ci_n(s + i, b - i, "rbsp-")) return 0;
    size_t j = i + 5;
    while (j < b && (alnum(s[j]) || s[j] == '-')) j++;
    return j > i + 5 && (prefix_ci_n(s + j, b - j, ".exe") || prefix_ci_n(s + j, b - j, ".dll"));
}

static int images_in(const char *s, size_t a, size_t b) {
    for (size_t i = a; i < b; i++)
        if (image_at(s, i, b)) return 1;
    return 0;
}

int report_event_wanted(const char *text, size_t n) {
    /* EventData: Application Error 1000, WER 1001, Application Hang 1002 (the image in Data or
     * P1) and .NET Runtime 1026 ("Application: RB Stems Plus.exe") */
    for (size_t i = find(text, n, 0, "<EventData"); i < n; i = find(text, n, i, "<EventData")) {
        size_t e = find(text, n, i, "</EventData>");
        if (images_in(text, i, e)) return 1;
        i = e;
    }
    /* the provider's own name */
    for (size_t i = find(text, n, 0, "<Provider Name="); i < n; i = find(text, n, i + 1, "<Provider Name=")) {
        size_t q = i + 15, e;
        if (q >= n || (text[q] != '\'' && text[q] != '"')) continue;
        for (e = q + 1; e < n && text[e] != text[q]; e++) {}
        if (images_in(text, q + 1, e)) return 1;
    }
    return 0;
}

/* removes [a, b) from s (n bytes); the new length */
static size_t cut(char *s, size_t n, size_t a, size_t b) {
    memmove(s + a, s + b, n - b);
    return n - (b - a);
}

size_t report_event_clean(char *xml, size_t n) {
    /* every " UserID='...'" (or "...") attribute: the account's SID */
    for (size_t i = find(xml, n, 0, " UserID="); i < n; i = find(xml, n, i, " UserID=")) {
        size_t q = i + 8;
        if (q >= n || (xml[q] != '\'' && xml[q] != '"')) { i = q; continue; }
        size_t e = q + 1;
        while (e < n && xml[e] != xml[q]) e++;
        if (e >= n) { i = q + 1; continue; }               /* an open quote: look further */
        n = cut(xml, n, i, e + 1);
    }
    /* the Computer element's text: the PC's name */
    for (size_t i = find(xml, n, 0, "<Computer>"); i < n; i = find(xml, n, i, "<Computer>")) {
        size_t a = i + 10, e = find(xml, n, a, "</Computer>");
        if (e >= n) break;
        n = cut(xml, n, a, e);
        i = a;
    }
    xml[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ WER */

int report_wer_wanted(const char *folder) {
    const char *rest = prefix_ci(folder, "AppCrash_") ? folder + 9 : prefix_ci(folder, "AppHang_") ? folder + 8 : NULL;
    if (!rest) return 0;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (prefix_ci(rest, names[i])) return 1;
    return 0;
}

static size_t put(char *o, size_t k, uint32_t c) {
    if (c < 0x80) o[k++] = (char)c;
    else if (c < 0x800) { o[k++] = (char)(0xC0 | (c >> 6)); o[k++] = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
        o[k++] = (char)(0xE0 | (c >> 12)); o[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); o[k++] = (char)(0x80 | (c & 0x3F));
    } else {
        o[k++] = (char)(0xF0 | (c >> 18)); o[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
        o[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); o[k++] = (char)(0x80 | (c & 0x3F));
    }
    return k;
}

char *report_text_utf8(const unsigned char *b, size_t n, size_t *len) {
    int utf16 = 0;
    size_t off = 0;
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) utf16 = 1, off = 2;
    else if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) off = 3;
    else if (n >= 2) {
        size_t m = n < 64 ? n & ~(size_t)1 : 64;
        utf16 = 1;
        for (size_t i = 1; i < m && utf16; i += 2) utf16 = b[i] == 0;
    }
    char *o = malloc((n - off) * 3 + 1);   /* a unit or a byte becomes at most 3 bytes; a pair 4 for 2 units */
    size_t k = 0;
    if (!o) return NULL;
    if (utf16) {
        size_t units = (n - off) / 2;
        const unsigned char *p = b + off;
        for (size_t i = 0; i < units; i++) {
            uint32_t c = (uint32_t)p[2 * i] | (uint32_t)p[2 * i + 1] << 8;
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units) {
                uint32_t d = (uint32_t)p[2 * i + 2] | (uint32_t)p[2 * i + 3] << 8;
                if (d >= 0xDC00 && d <= 0xDFFF) {
                    k = put(o, k, 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00));
                    i++;
                    continue;
                }
            }
            if (c == 0) continue;
            k = put(o, k, c >= 0xD800 && c <= 0xDFFF ? 0xFFFD : c);
        }
    } else {
        for (size_t i = off; i < n;) {
            uint32_t c = b[i];
            int more = c < 0x80 ? 0 : c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : -1;
            int ok = more >= 0 && n - i > (size_t)more;
            uint32_t v = more > 0 ? c & (0x3Fu >> more) : c;
            for (int j = 1; ok && j <= more; j++) {
                ok = (b[i + j] & 0xC0) == 0x80;
                v = v << 6 | (b[i + j] & 0x3Fu);
            }
            if (ok && ((more == 2 && v < 0x800) || (more == 3 && (v < 0x10000 || v > 0x10FFFF)) || (v >= 0xD800 && v <= 0xDFFF)))
                ok = 0;                                     /* overlong, too big, or a surrogate */
            if (!ok) {
                k = put(o, k, 0xFFFD);
                i++;
                continue;
            }
            if (v) k = put(o, k, v);
            i += (size_t)more + 1;
        }
    }
    o[k] = 0;
    *len = k;
    return o;
}

int report_wer_root_paths(const char *const *bases, int nbases, char roots[][REPORT_PATH_MAX], int cap) {
    static const char *const kinds[] = {"ReportArchive", "ReportQueue"};
    int n = 0;
    for (int i = 0; i < nbases; i++) {
        if (!bases[i] || !bases[i][0]) continue;
        for (size_t k = 0; k < sizeof kinds / sizeof *kinds && n < cap; k++) {
            int r = snprintf(roots[n], REPORT_PATH_MAX, "%s%cMicrosoft%cWindows%cWER%c%s", bases[i], SEP, SEP, SEP, SEP, kinds[k]);
            if (r > 0 && r < REPORT_PATH_MAX) n++;
        }
    }
    return n;
}

typedef struct {
    plat_dir root;
    const char *root_path;
    int64_t since;
    report_wer *out;
    int max, n, found;
} wer_walk;

static int wer_each(void *ctx, const char *name, const plat_stat *st) {
    wer_walk *w = ctx;
    if (st->kind != PLAT_DIR || !report_wer_wanted(name)) return 0;
    plat_dir d;
    plat_stat f;
    if (plat_dir_open_at(w->root, name, &d)) return 0;
    int e = plat_stat_at(d, "Report.wer", &f);
    plat_dir_close(d);
    if (e || f.kind != PLAT_FILE || f.mtime < w->since) return 0;
    w->found++;
    report_wer one;
    int r = snprintf(one.path, sizeof one.path, "%s%c%s%cReport.wer", w->root_path, SEP, name, SEP);
    if (r < 0 || (size_t)r >= sizeof one.path) return 0;
    one.mtime = f.mtime;
    /* newest first, the newest max kept */
    int at = w->n;
    while (at > 0 && w->out[at - 1].mtime < one.mtime) at--;
    if (at >= w->max) return 0;
    int last = w->n < w->max ? w->n : w->max - 1;
    memmove(&w->out[at + 1], &w->out[at], sizeof one * (size_t)(last - at));
    w->out[at] = one;
    if (w->n < w->max) w->n++;
    return 0;
}

int report_wer_find(const char *const *roots, int nroots, int64_t since, report_wer *out, int max, int *found) {
    wer_walk w = {{0}, NULL, since, out, max, 0, 0};
    for (int i = 0; i < nroots && max > 0; i++) {
        if (!roots[i] || !roots[i][0] || plat_dir_open(roots[i], &w.root)) continue;
        w.root_path = roots[i];
        plat_dir_each(w.root, wer_each, &w);
        plat_dir_close(w.root);
    }
    if (found) *found = w.found;
    return w.n;
}

int report_wer_text(const char *path, size_t max, char **out, size_t *len) {
    static const char want[] = "Report.wer";
    const char *slash = strrchr(path, SEP);
    *out = NULL;
    *len = 0;
    if (!slash || strcmp(slash + 1, want) || (size_t)(slash - path) >= REPORT_PATH_MAX) return EINVAL;   /* never a dump */
    char dir[REPORT_PATH_MAX];
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = 0;
    plat_dir d;
    plat_file f;
    int64_t size = 0;
    int e = plat_dir_open(dir, &d);
    if (e) return e;
    e = plat_file_open_at(d, want, INT64_MAX, &size, &f);
    plat_dir_close(d);
    if (e) return e;
    size_t take = size < 0 ? 0 : (uint64_t)size < max ? (size_t)size : max;
    unsigned char *b = malloc(take ? take : 1);
    if (!b) e = ENOMEM;
    else if (take) e = plat_read_all(f, b, take);
    plat_file_close(f);
    if (!e) {
        *out = report_text_utf8(b, take, len);
        if (!*out) e = ENOMEM;
    }
    free(b);
    return e;
}
