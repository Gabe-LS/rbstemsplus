/* config.ini's reader (cfg_ini.h), moved from the bridge's parse_config, utf8_ok, uuid_ok, yes_no and
 * c_locale, unchanged but for the names. */
#include "cfg_ini.h"
#include "plat.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(_WIN32)
#include <locale.h>
#else
#include <xlocale.h>
#endif

COMMON_API int cfg_yes_no(const char *v) {
    if (!strcmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcasecmp(v, "false")) return 0;
    if (!strcmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcasecmp(v, "true")) return 1;
    return -1;
}

COMMON_API int cfg_utf8_ok(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i], cp, min;
        size_t k;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { k = 1; cp = c & 0x1F; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { k = 2; cp = c & 0x0F; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { k = 3; cp = c & 0x07; min = 0x10000; }
        else return 0;
        if (n - i <= k) return 0;
        for (size_t j = 1; j <= k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (s[i + j] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += k + 1;
    }
    return 1;
}

COMMON_API int cfg_uuid_ok(const char *s) {
    if (strlen(s) != 36) return 0;
    for (int i = 0; i < 36; i++)
        if (i == 8 || i == 13 || i == 18 || i == 23 ? s[i] != '-' : !isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

static int cfg_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* the C locale, for numbers: the app's reader always takes '.' as the decimal mark, whatever the
 * locale of the process that reads (rekordbox's) */
#if defined(_WIN32)
static _locale_t cfg_loc;
static void cfg_locale_init(void) { cfg_loc = _create_locale(LC_ALL, "C"); }
#define CFG_STRTOD(s, e) _strtod_l(s, e, cfg_loc)
#else
static locale_t cfg_loc;
static void cfg_locale_init(void) { cfg_loc = newlocale(LC_ALL_MASK, "C", NULL); }
#define CFG_STRTOD(s, e) strtod_l(s, e, cfg_loc)
#endif
static void cfg_locale(void) {
    static plat_once_t o = PLAT_ONCE_INIT;
    plat_once(&o, cfg_locale_init);
}

COMMON_API int cfg_parse(const char *b, size_t n, cfg_config *c, cfg_log_fn log) {
    if (memchr(b, 0, n) || !cfg_utf8_ok((const unsigned char *)b, n)) return -1;
    int ours = 1, long_section = 0, loc_anywhere = 0, loc_lines = 0, bad = 0, have_dir = 0, have_id = 0;
    char val[CFG_LINE_MAX + 1];
    size_t i = n >= 3 && !memcmp(b, "\xEF\xBB\xBF", 3) ? 3 : 0;
    while (i < n) {
        const char *s = b + i, *nl = memchr(s, '\n', n - i), *end = nl ? nl : b + n, *p = s;
        size_t len = (size_t)(end - s);
        i += len + 1;
        int over = len > CFG_LINE_MAX;
        while (p < end && cfg_blank(*p)) p++;
        if (p == end || *p == '#') continue;
        if (over && log) log("config: ignored a line over %d bytes: %.*s", CFG_LINE_MAX, (int)(end - p < 40 ? end - p : 40), p);
        if (*p == '[') {                                    /* [section] */
            const char *a = p + 1, *q = over ? NULL : memchr(p, ']', (size_t)(end - p));
            long_section |= over;
            if (q) {
                while (a < q && (*a == ' ' || *a == '\t')) a++;
                while (q > a && (q[-1] == ' ' || q[-1] == '\t')) q--;
            }
            ours = q && q - a == 5 && !strncasecmp(a, "cache", 5);
            continue;
        }
        const char *eq = memchr(p, '=', (size_t)(end - p)), *ke = eq;
        if (!eq) continue;
        while (ke > p && cfg_blank(ke[-1])) ke--;
        char key[16];
        size_t kl = (size_t)(ke - p);
        if (kl >= sizeof key) continue;                     /* none of ours is that long */
        for (size_t j = 0; j < kl; j++) key[j] = (char)tolower((unsigned char)p[j]);
        key[kl] = 0;
        int loc = !strcmp(key, "cache_dir") || !strcmp(key, "cache_id");
        loc_anywhere |= loc;
        if (!ours) continue;
        loc_lines += loc;
        if (over) { bad |= loc; continue; }
        const char *v = eq + 1, *ve, *q;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        if (v < end && (*v == '"' || *v == '\'') && (q = memchr(v + 1, *v, (size_t)(end - v - 1)))) { v++; ve = q; }
        else for (ve = v; ve < end && !cfg_blank(*ve) && *ve != '#'; ve++) {}
        memcpy(val, v, (size_t)(ve - v));
        val[ve - v] = 0;
        if (!strcmp(key, "cache_dir")) {
            if (!paths_location_ok_os(val) || (have_dir && strcmp(c->dir, val))) bad = 1;
            else { memcpy(c->dir, val, strlen(val) + 1); have_dir = 1; }
        } else if (!strcmp(key, "cache_id")) {
            if (!cfg_uuid_ok(val) || (have_id && strcasecmp(c->id, val))) bad = 1;
            else { memcpy(c->id, val, 37); have_id = 1; }
        } else if (!strcmp(key, "enabled") || !strcmp(key, "rekordbox_model") || !strcmp(key, "near_hits")) {
            int yn = cfg_yes_no(val), *dst = key[0] == 'e' ? &c->enabled : key[0] == 'n' ? &c->near_hits : &c->rekordbox_model;
            if (yn >= 0) *dst = yn;
            else if (log) log("config: ignored %s=%.40s (allowed 1 or 0)", key, val);
        } else if (!strcmp(key, "max_gb") || !strcmp(key, "max_days")) {
            int gb = key[4] == 'g';
            double lo = gb ? 0.1 : 1, hi = gb ? 100000 : 36500;
            char *e;
            cfg_locale();
            double x = CFG_STRTOD(val, &e);                 /* '.' whatever rekordbox's locale */
            if (!*val || isspace((unsigned char)*val) || *e || !(x >= lo && x <= hi)) {
                if (log) log("config: ignored %s=%.40s (allowed %g to %g)", key, val, lo, hi);
            } else if (gb) c->max_bytes = (long long)(x * (1LL << 30));
            else c->max_days = x;
        }
    }
    int unknown = long_section && loc_anywhere;
    c->location = !loc_lines && !unknown ? 0 : bad || !have_dir || !have_id || unknown ? -1 : 1;
    return 0;
}
