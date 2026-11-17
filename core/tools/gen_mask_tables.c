/* Prints core/mask_tables.h: the Unicode tables mask.c needs to match 1.3.0's masking (Report.swift
 * redact, NSRegularExpression on ICU, NSString's search), read from this Mac's own ICU and
 * CoreFoundation, the code 1.3.0 runs on. core/build.sh tables OUT builds it and writes
 * OUT/mask_tables.h; make core-test checks the committed header against it.
 *
 *   word   ICU's \w, which its \b tests: Alphabetic, Mark, Decimal_Number, Connector_Punctuation,
 *          U+200C, U+200D (read from the regex engine itself, not from the properties)
 *   ext    Grapheme_Extend: \b looks through these and Cf (rematch.cpp isWordBoundary); the name
 *          rule's character count (Swift's name.count) never starts a character at one
 *   cf     General_Category Cf
 *   fold   full case folding (u_strFoldCase, U_FOLD_CASE_DEFAULT): what a case-insensitive
 *          literal compares, on both sides (URX_STRING_I)
 *   canon  the code points NSString's search takes for one ASCII character (a canonical
 *          decomposition to it alone): the home folder's search
 *   page   for each 256 code points of the BMP, whether all or none are \w, none is
 *          Grapheme_Extend or Cf, none folds: the lookups' fast paths
 *
 * It also checks what mask.c writes by hand, and fails if ICU disagrees: \s, the case-insensitive
 * classes over ASCII ([A-Za-z] also holds U+017F and U+212A), and that no code point but those
 * three classes' folds to plain ASCII letters.
 *
 * Build (macOS only; ICU's C headers are in the SDK): clang gen_mask_tables.c -licucore
 * -framework CoreFoundation. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicode/uchar.h>
#include <unicode/uregex.h>
#include <unicode/ustring.h>

#define MAXCP 0x110000

static void die(const char *what) { fprintf(stderr, "gen_mask_tables: %s\n", what); exit(1); }

static int to_u16(uint32_t c, UChar *u) {
    if (c < 0x10000) { u[0] = (UChar)c; return 1; }
    u[0] = (UChar)(0xD800 + ((c - 0x10000) >> 10));
    u[1] = (UChar)(0xDC00 + ((c - 0x10000) & 0x3FF));
    return 2;
}

static int surrogate(uint32_t c) { return c >= 0xD800 && c <= 0xDFFF; }

/* the code points a pattern matches alone (each tried as the whole text) */
static void regex_set(const char *pattern, uint32_t flags, unsigned char *in) {
    UErrorCode e = U_ZERO_ERROR;
    UParseError pe;
    URegularExpression *re = uregex_openC(pattern, flags, &pe, &e);
    if (U_FAILURE(e)) die("uregex_openC");
    for (uint32_t c = 0; c < MAXCP; c++) {
        in[c] = 0;
        if (surrogate(c)) continue;
        UChar u[2];
        int n = to_u16(c, u);
        uregex_setText(re, u, n, &e);
        in[c] = (unsigned char)uregex_matches(re, 0, &e);
        if (U_FAILURE(e)) die("uregex_matches");
    }
    uregex_close(re);
}

static size_t total_bytes;

/* an inversion list: the code points where membership flips, starting outside */
static void inversion(const char *name, const char *what, const unsigned char *in) {
    uint32_t *v = malloc(sizeof *v * (MAXCP + 1));
    int n = 0, cur = 0;
    for (uint32_t c = 0; c < MAXCP; c++)
        if (!!in[c] != cur) { v[n++] = c; cur = !cur; }
    if (cur) v[n++] = MAXCP;
    printf("/* %s: %d ranges */\nstatic const uint32_t %s[%d] = {", what, n / 2, name, n);
    for (int i = 0; i < n; i++) printf("%s0x%X,", i % 12 ? " " : "\n    ", v[i]);
    printf("\n};\n\n");
    total_bytes += (size_t)n * 4;
    free(v);
}

static int fold(uint32_t c, uint32_t out[3]) {
    UChar u[2], f[8];
    UErrorCode e = U_ZERO_ERROR;
    int32_t m = u_strFoldCase(f, 8, u, to_u16(c, u), U_FOLD_CASE_DEFAULT, &e);
    if (U_FAILURE(e)) die("u_strFoldCase");
    int k = 0;
    for (int32_t i = 0; i < m;) {
        UChar32 x;
        U16_NEXT(f, i, m, x);
        if (k == 3) die("a fold longer than 3 code points");
        out[k++] = (uint32_t)x;
    }
    return k;
}

int main(void) {
    static unsigned char word[MAXCP], ws[MAXCP], ext[MAXCP], cf[MAXCP], ci_alpha[MAXCP];
    regex_set("\\w", 0, word);
    regex_set("\\s", 0, ws);
    regex_set("[A-Za-z]", UREGEX_CASE_INSENSITIVE, ci_alpha);
    for (uint32_t c = 0; c < MAXCP; c++) {
        ext[c] = !surrogate(c) && u_hasBinaryProperty((UChar32)c, UCHAR_GRAPHEME_EXTEND);
        cf[c] = !surrogate(c) && u_charType((UChar32)c) == U_FORMAT_CHAR;
    }

    /* mask.c's hand-written classes */
    for (uint32_t c = 0; c < MAXCP; c++) {
        int w = (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A)
            || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
        if (ws[c] != w) die("ICU's \\s isn't mask.c's is_ws");
        int a = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == 0x17F || c == 0x212A;
        if (ci_alpha[c] != a) die("ICU's case-insensitive [A-Za-z] isn't mask.c's ci_alpha");
        if (c < 0x80) {
            int aw = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (word[c] != aw) die("ICU's \\w over ASCII isn't mask.c's");
            if (ext[c] || cf[c]) die("an ASCII Grapheme_Extend or Cf");
        }
    }

    char version[U_MAX_VERSION_STRING_LENGTH];
    UVersionInfo uv;
    u_getUnicodeVersion(uv);
    u_versionToString(uv, version);
    printf("/* Generated by core/tools/gen_mask_tables.c (core/build.sh tables) from this Mac's ICU and\n"
           " * CoreFoundation (Unicode %s), the code 1.3.0's masking runs on. Do not edit. */\n\n", version);
    printf("#define MT_UNICODE \"%s\"\n\n", version);
    inversion("mt_word", "ICU's \\w", word);
    inversion("mt_ext", "Grapheme_Extend", ext);
    inversion("mt_cf", "General_Category Cf", cf);

    /* folds: 1:1 ones as runs {first, count, stride, delta}; longer ones on their own */
    printf("/* full case folding, one code point to one: runs of code points step apart, each + delta */\n");
    printf("typedef struct { uint32_t lo; uint16_t n; uint16_t step; int32_t delta; } mt_fold_run;\n");
    printf("static const mt_fold_run mt_fold1[] = {");
    int runs = 0, multi = 0;
    uint32_t lo = 0, step = 0, cnt = 0;
    int32_t delta = 0;
    int open = 0;
#define FLUSH() do { if (open) { printf("%s{0x%X, %u, %u, %d},", runs % 4 ? " " : "\n    ", lo, cnt, step ? step : 1, delta); runs++; open = 0; } } while (0)
    for (uint32_t c = 0; c < MAXCP; c++) {
        if (surrogate(c)) continue;
        uint32_t f[3];
        int k = fold(c, f);
        if (k != 1) { multi++; continue; }
        if (f[0] == c) continue;
        int32_t d = (int32_t)f[0] - (int32_t)c;
        uint32_t last = lo + (cnt - 1) * (step ? step : 1);
        if (open && d == delta && cnt < 0xFFFF && ((cnt == 1 && c - lo <= 2) || (cnt > 1 && c - last == step))) {
            if (cnt == 1) step = c - lo;
            cnt++;
            continue;
        }
        FLUSH();
        lo = c; cnt = 1; step = 0; delta = d; open = 1;
    }
    FLUSH();
    printf("\n};\n\n");
    total_bytes += (size_t)runs * 12;

    printf("/* full case folding to two or three code points (all in the BMP) */\n");
    printf("typedef struct { uint32_t cp; uint16_t to[3]; } mt_fold_many;\n");
    printf("static const mt_fold_many mt_foldn[%d] = {", multi);
    int k2 = 0;
    for (uint32_t c = 0; c < MAXCP; c++) {
        if (surrogate(c)) continue;
        uint32_t f[3];
        int k = fold(c, f);
        if (k == 1) continue;
        for (int j = 0; j < k; j++) if (f[j] > 0xFFFF) die("a multi-code-point fold outside the BMP");
        printf("%s{0x%X, {0x%X, 0x%X, 0x%X}},", k2++ % 4 ? " " : "\n    ", c, f[0], f[1], k > 2 ? f[2] : 0);
    }
    printf("\n};\n\n");
    total_bytes += (size_t)multi * 12;

    /* mask.c's literal patterns are ASCII: only these fold wholly to ASCII letters */
    for (uint32_t c = 0x80; c < MAXCP; c++) {
        if (surrogate(c)) continue;
        uint32_t f[3];
        int k = fold(c, f), ascii = 1;
        for (int j = 0; j < k; j++) ascii &= f[j] < 0x80;
        if (ascii && !(c == 0x17F || c == 0x212A || c == 0xDF || c == 0x1E9E || (c >= 0xFB00 && c <= 0xFB06)))
            die("a new code point folds to ASCII: mask.c's comment and vectors need it");
    }

    /* a summary of each 256 code points of the BMP, for the lookups' fast paths */
    printf("/* each 256 code points of the BMP: 1 all \\w, 2 none \\w, 4 no Grapheme_Extend or Cf, 8 none folds */\n");
    printf("enum { MT_ALL_WORD = 1, MT_NO_WORD = 2, MT_NO_SKIP = 4, MT_NO_FOLD = 8 };\n");
    printf("static const uint8_t mt_page[256] = {");
    for (uint32_t pg = 0; pg < 256; pg++) {
        int all = 1, none = 1, noskip = 1, nofold = 1;
        for (uint32_t c = pg << 8; c < (pg + 1) << 8; c++) {
            if (surrogate(c)) { all = 0; continue; }   /* never seen in UTF-8; not \w */
            if (word[c]) none = 0; else all = 0;
            if (ext[c] || cf[c]) noskip = 0;
            uint32_t f[3];
            if (fold(c, f) != 1 || f[0] != c) nofold = 0;
        }
        printf("%s%d,", pg % 16 ? " " : "\n    ", (all ? 1 : 0) | (none ? 2 : 0) | (noskip ? 4 : 0) | (nofold ? 8 : 0));
    }
    printf("\n};\n\n");
    total_bytes += 256;

    /* the code points NSString's (canonical) search takes for one printable ASCII character */
    printf("/* code points canonically equivalent to one ASCII character: {code point, the character} */\n");
    printf("static const uint32_t mt_canon_ascii[][2] = {");
    int nc = 0;
    for (uint32_t c = 0x80; c < MAXCP; c++) {
        if (surrogate(c)) continue;
        UChar u[2];
        int n = to_u16(c, u);
        CFMutableStringRef s = CFStringCreateMutable(NULL, 0);
        CFStringAppendCharacters(s, u, n);
        CFStringNormalize(s, kCFStringNormalizationFormD);
        if (CFStringGetLength(s) == 1) {
            UniChar d = CFStringGetCharacterAtIndex(s, 0);
            if (d >= 0x20 && d < 0x7F) { printf("%s{0x%X, 0x%X},", nc % 6 ? " " : "\n    ", c, d); nc++; }
        }
        CFRelease(s);
    }
    printf("\n};\n\n");
    total_bytes += (size_t)nc * 8;
    printf("/* about %zu bytes of tables */\n", total_bytes);
    fprintf(stderr, "gen_mask_tables: Unicode %s, about %zu bytes of tables\n", version, total_bytes);
    return 0;
}
