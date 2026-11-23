/* Path rules (path_rules.h), moved from the bridge's location_path_ok and from the helper's
 * checks of a track's path (prepare.c, readable.c, manifest.c). */
#include "path_rules.h"

#include <string.h>

COMMON_API int paths_location_ok(const char *p) {
    size_t n = strlen(p), tail = strlen("/" PATHS_CACHE_FOLDER);
    if (p[0] != '/' || n > PATHS_LOCATION_MAX || n <= tail || strcmp(p + n - tail, "/" PATHS_CACHE_FOLDER)) return 0;
    for (const char *c = p; *c; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7F || *c == '"') return 0;
    for (const char *c = p; c;) {                           /* c: at a '/' */
        const char *e = strchr(c + 1, '/');
        size_t l = e ? (size_t)(e - c - 1) : strlen(c + 1);
        if (l == 0 || (l == 1 && c[1] == '.') || (l == 2 && c[1] == '.' && c[2] == '.')) return 0;
        c = e;
    }
    return 1;
}

static int pr_sep(char c) { return c == '/' || c == '\\'; }
static int pr_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

COMMON_API int paths_local_file_win(const char *p) {
    if (pr_letter(p[0]) && p[1] == ':' && pr_sep(p[2])) return 1;   /* X:/ or X:\ */
    return pr_sep(p[0]) && pr_sep(p[1]);                     /* //server/share or \\server\share */
}

COMMON_API int paths_same_win(const char *a, const char *b) {
    for (size_t i = 0;; i++) {
        char x = a[i], y = b[i];
        if (i == 0 && pr_letter(x) && pr_letter(y) && a[1] == ':' && b[1] == ':') {   /* the drive letter's case */
            x = (char)(x | 0x20);
            y = (char)(y | 0x20);
        }
        if (pr_sep(x) && pr_sep(y)) continue;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

#if defined(_WIN32) || defined(PATHS_WIN_FORM) || defined(PATHS_WIN_TRACKS)
static size_t pr_unc_root(const char *p, size_t at) {       /* at: the server's start; up to the share's end */
    size_t i = at;
    while (p[i] && !pr_sep(p[i])) i++;                       /* the server */
    if (!p[i]) return i;
    i++;
    while (p[i] && !pr_sep(p[i])) i++;                       /* the share */
    return i;
}

COMMON_API size_t paths_other_drive_win(const char *p, char sys) {
    size_t o = 0;
    if (pr_sep(p[0]) && pr_sep(p[1]) && (p[2] == '?' || p[2] == '.') && pr_sep(p[3])) {   /* \\?\ or \\.\ */
        o = 4;
        if ((p[4] | 0x20) == 'u' && (p[5] | 0x20) == 'n' && (p[6] | 0x20) == 'c' && pr_sep(p[7])) return pr_unc_root(p, 8);
        size_t g = paths_root_win(p);
        if (g == 49) return g;                               /* \\?\Volume{GUID}\ */
    }
    if (pr_letter(p[o]) && p[o + 1] == ':') {
        if (sys && (p[o] | 0x20) == (sys | 0x20)) return 0;
        return pr_sep(p[o + 2]) ? o + 3 : !p[o + 2] ? o + 2 : 0;
    }
    if (!o && pr_sep(p[0]) && pr_sep(p[1])) return pr_unc_root(p, 2);
    return 0;
}
#endif

#if !defined(_WIN32) && !defined(PATHS_WIN_TRACKS)
COMMON_API size_t paths_other_drive(const char *p) {
    if (strncmp(p, "/Volumes/", 9)) return 0;
    const char *e = strchr(p + 9, '/');
    return e ? (size_t)(e - p) : strlen(p);
}
#endif

/* ---------------------------------------------------------------- Windows' form (T3c) */

static int pr_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

COMMON_API size_t paths_root_win(const char *p) {
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && p[2] == '\\') return 3;
    if (strncmp(p, "\\\\?\\Volume{", 11)) return 0;
    for (int i = 0; i < 36; i++) {                          /* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
        char c = p[11 + i];
        if (i == 8 || i == 13 || i == 18 || i == 23 ? c != '-' : !pr_hex(c)) return 0;
    }
    return p[47] == '}' && p[48] == '\\' ? 49 : 0;
}

COMMON_API int paths_location_ok_win(const char *p) {
    size_t n = strlen(p), r = paths_root_win(p), tail = strlen(PATHS_CACHE_FOLDER);
    if (!r || n > PATHS_LOCATION_MAX || n < r + tail || strcmp(p + n - tail, PATHS_CACHE_FOLDER) || p[n - tail - 1] != '\\') return 0;
    for (const char *c = p + r;;) {                         /* c: a component's start */
        const char *e = c;
        for (; *e && *e != '\\'; e++) {
            unsigned char b = (unsigned char)*e;
            if (b < 0x20 || b == 0x7F || strchr("<>:\"/|?*", b)) return 0;
        }
        size_t l = (size_t)(e - c);
        if (l == 0 || (l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.') || c[l - 1] == '.' || c[l - 1] == ' ') return 0;
        if (!*e) return 1;
        c = e + 1;
    }
}
