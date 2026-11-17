/* models.c against today's Swift: fixtures.swift's modelcase records. Models.swift ran each case
 * in a folder of its own; the same folders are made here and models.c runs the same operation.
 * The outcome (Swift's message read as HostSteps.swift's modelOutcomeCode reads it), what
 * modelLeftBehind finds, savedOriginals (its order) and verifiedOriginals, and both folders
 * afterwards must agree. One difference by design: a link where rekordbox's model is is never
 * followed here (Swift hashes what it points to), so restore says "can't be read", what's left
 * behind is "unreadable", and nothing is changed. */
#include "check.h"

#include "../models.h"

#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static void sha_hex(const void *d, size_t n, char out[65]) {
    unsigned char h[32];
    CC_SHA256(d, (CC_LONG)n, h);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0; }
/* hex ("=" empty) into a malloc'd NUL-terminated buffer; *n its length */
static char *unhex_s(const char *s, size_t *n) {
    size_t k = strcmp(s, "=") ? strlen(s) / 2 : 0;
    char *b = malloc(k + 1);
    if (!b) abort();
    for (size_t i = 0; i < k; i++) b[i] = (char)(hexv(s[2 * i]) << 4 | hexv(s[2 * i + 1]));
    b[k] = 0;
    if (n) *n = k;
    return b;
}
static void put_n(const char *path, const char *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    fwrite(b, 1, n, f);
    fclose(f);
}
static void set_mtime(const char *path, long t) {
    struct timeval tv[2] = {{t, 0}, {t, 0}};
    utimes(path, tv);
}
static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* Swift's mListing: "-" when not a folder, else name=hex;name@ (a link);name/, sorted */
static void listing(const char *d, char *out, size_t cap) {
    struct stat st;
    out[0] = 0;
    if (lstat(d, &st) || !S_ISDIR(st.st_mode)) {
        snprintf(out, cap, "-");
        return;
    }
    DIR *dir = opendir(d);
    char *names[64];
    int n = 0;
    for (struct dirent *x; dir && n < 64 && (x = readdir(dir));)
        if (strcmp(x->d_name, ".") && strcmp(x->d_name, "..")) names[n++] = strdup(x->d_name);
    if (dir) closedir(dir);
    qsort(names, (size_t)n, sizeof names[0], name_cmp);
    for (int i = 0; i < n; i++) {
        char p[1400];
        size_t w = strlen(out);
        snprintf(p, sizeof p, "%s/%s", d, names[i]);
        if (!lstat(p, &st) && S_ISREG(st.st_mode)) {
            FILE *f = fopen(p, "rb");
            unsigned char b[256];
            size_t k = f ? fread(b, 1, sizeof b, f) : 0;
            if (f) fclose(f);
            w += (size_t)snprintf(out + w, cap - w, "%s%s=", i ? ";" : "", names[i]);
            if (!k) w += (size_t)snprintf(out + w, cap - w, "=");
            for (size_t j = 0; j < k && w + 3 < cap; j++) w += (size_t)snprintf(out + w, cap - w, "%02x", b[j]);
        } else if (!lstat(p, &st))
            snprintf(out + w, cap - w, "%s%s%s", i ? ";" : "", names[i], S_ISLNK(st.st_mode) ? "@" : "/");
        free(names[i]);
    }
}

/* rb's listing "|" originals' */
static void both_listings(const char *rb, const char *orig, char *out, size_t cap) {
    char lo[1500];
    listing(rb, out, cap);
    listing(orig, lo, sizeof lo);
    size_t w = strlen(out);
    snprintf(out + w, cap - w, "|%s", lo);
}

/* modelOutcomeCode (HostSteps.swift) of a failure's message */
static int swift_code(const char *msg) {
    static const struct { const char *prefix; int code; } whys[] = {
        {"The Demucs v4 model couldn't be copied: ", MODELS_NOT_COPIED_WHY},
        {"The Demucs v4 model couldn't be put in place: ", MODELS_NOT_PLACED_WHY},
        {"rekordbox's own stems model couldn't be put back: ", MODELS_NOT_BACK_WHY},
    };
    for (size_t i = 0; i < sizeof whys / sizeof whys[0]; i++)
        if (!strncmp(msg, whys[i].prefix, strlen(whys[i].prefix))) return whys[i].code;
    static const struct { const char *msg; int code; } all[] = {
        {"rekordbox's stems model isn't one RB Stems Plus knows, so it was left as it is.", MODELS_UNKNOWN},
        {"rekordbox's stems model can't be read.", MODELS_UNREADABLE},
        {"rekordbox's stems model isn't a file.", MODELS_NOT_A_FILE},
        {"There isn't enough free space to save a copy of rekordbox's own stems model.", MODELS_NO_SPACE_SAVE},
        {"A copy of rekordbox's own stems model couldn't be saved.", MODELS_NOT_SAVED},
        {"There isn't enough free space to copy the Demucs v4 model.", MODELS_NO_SPACE_COPY},
        {"The Demucs v4 model didn't copy correctly.", MODELS_COPY_BAD},
        {"rekordbox's stems model changed meanwhile.", MODELS_CHANGED},
        {"There's no saved copy of rekordbox's own stems model on this Mac.", MODELS_NO_SAVED_COPY},
        {"There isn't enough free space to put rekordbox's own stems model back.", MODELS_NO_SPACE_BACK},
        {"rekordbox's own stems model couldn't be put back.", MODELS_NOT_BACK},
        {"rekordbox's stems model didn't verify after it was put back.", MODELS_NOT_VERIFIED},
        {"The saved copy of rekordbox's own stems model is damaged.", MODELS_SAVED_DAMAGED},
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        if (!strcmp(msg, all[i].msg)) return all[i].code;
    return MODELS_NO_CODE;
}

/* leftBehindCode (HostSteps.swift) */
static int swift_left(const char *s) {
    return !strcmp(s, "rekordbox has no stems model.") ? 0
         : !strcmp(s, "rekordbox's stems model can't be read.") ? 1
         : !strcmp(s, "rekordbox still has the Demucs v4 model.") ? 2
                                                                   : 3;
}

int test_model_fixtures(const char *fixtures, const char *scratch) {
    FILE *f = fopen(fixtures, "r");
    if (!f) return 0;
    static critical_work fixture_work;
    critical_init(&fixture_work);
    static char rec[1 << 16];
    int cases = 0, differ = 0;
    while (fgets(rec, sizeof rec, f)) {
        if (strncmp(rec, "modelcase ", 10)) continue;
        rec[strcspn(rec, "\n")] = 0;
        char *fv[7] = {0};
        int nf = 0;
        for (char *save = NULL, *t = strtok_r(rec, " ", &save); t && nf < 7; t = strtok_r(NULL, " ", &save)) fv[nf++] = t;
        if (nf != 6) {
            CHECK(0, "a modelcase with %d fields", nf);
            continue;
        }
        cases++;
        char *spec = unhex_s(fv[1], NULL), *msg = unhex_s(fv[3], NULL), *after = unhex_s(fv[4], NULL), *extra = unhex_s(fv[5], NULL);
        int swift_ok = fv[2][0] == '1', is_link = 0, orig_link = 0;
        char live_sha[65] = "";
        char root[1100], rb[1200], orig[1200], p[1400], op[16] = "", src_sha[65] = "";
        snprintf(root, sizeof root, "%s/mc%d", scratch, cases);
        mkdir(root, 0755);
        snprintf(rb, sizeof rb, "%s/rb", root);
        snprintf(orig, sizeof orig, "%s/originals", root);
        models m = {.dir = rb, .originals = orig, .support = root, .critical = &fixture_work};   /* its own: the process's is closed by models_test */
        if (!strstr(spec, "\nnodir")) mkdir(rb, 0755);
        if (!strstr(spec, "\nnoorig")) mkdir(orig, 0755);
        for (char *save = NULL, *l = strtok_r(spec, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
            char a[80], b[200], c[1200], q[1300];
            long t = 0;
            size_t n;
            if (sscanf(l, "ours %64s", a) == 1) models_add_ours(&m, a);
            else if (sscanf(l, "olink %199s %1199s", b, c) == 2) {   /* <sha>.onnx a link (review MT10) */
                char *d = unhex_s(c, &n);
                snprintf(q, sizeof q, "%s/elsewhere-original.onnx", root);
                put_n(q, d, n);
                snprintf(p, sizeof p, "%s/%s", orig, b);
                if (symlink(q, p)) abort();
                orig_link = 1;
                free(d);
            } else if (sscanf(l, "odir %199s", b) == 1) {   /* <sha>.onnx a folder (MT10) */
                snprintf(p, sizeof p, "%s/%s", orig, b);
                mkdir(p, 0755);
            }
            else if (sscanf(l, "src %1199s %64s", c, src_sha) == 2) {
                char *d = unhex_s(c, &n);
                snprintf(p, sizeof p, "%s/src.onnx", root);
                put_n(p, d, n);
                free(d);
            } else if ((l[0] == 'd' || l[0] == 'o') && sscanf(l + 2, "%199s %1199s %ld", b, c, &t) == 3) {
                char *d = unhex_s(c, &n);
                snprintf(p, sizeof p, "%s/%s", l[0] == 'd' ? rb : orig, b);
                put_n(p, d, n);
                set_mtime(p, t);
                if (l[0] == 'd' && !strcmp(b, "hdemucs.onnx")) sha_hex(d, n, live_sha);   /* rekordbox's model, by its bytes */
                free(d);
            } else if (sscanf(l, "link %199s %1199s", b, c) == 2) {
                char *d = unhex_s(c, &n);
                snprintf(q, sizeof q, "%s/elsewhere.onnx", root);
                put_n(q, d, n);
                snprintf(p, sizeof p, "%s/%s", rb, b);
                if (symlink(q, p)) abort();
                is_link = 1;
                free(d);
            } else
                sscanf(l, "op %15s", op);
        }
        char why[128], got[3000], mine[600] = "", before[3000] = "";
        int r = 0, expect = 0, compare_after = 1, unchanged_only = 0;
        both_listings(rb, orig, before, sizeof before);
        /* a saved <sha>.onnx that is a link (MT10's set): Windows counts a linked or reparse copy as
         * unknown for good (review VU2: a real copy that can't be checked), while Models.swift's
         * scan skips a link; so these rows differ permanently, by design (review VQ3):
         *  - restore: a candidate next to it still goes back (those rows compare with Swift); else
         *    NOT_BACK with nothing changed;
         *  - install over a model that isn't ours (decided from the case's own bytes): NOT_SAVED,
         *    nothing changed;
         *  - MODELS_INFO: verified */
        struct stat rbst;
        int orig_differs = orig_link && !is_link && !lstat(rb, &rbst);   /* no rekordbox folder: "nothing to put back" comes first */
        int live_ours = 0;
        for (int i = 0; i < m.nours && live_sha[0]; i++) live_ours |= !strcmp(m.ours[i], live_sha);
        if (!strcmp(op, "install") || !strcmp(op, "installbad")) {
            r = models_install_ours(&m, root, "src.onnx", src_sha, why);
            expect = swift_ok ? MODELS_OK : swift_code(msg);
            if (orig_differs && live_sha[0] && !live_ours) {
                differ++;
                expect = MODELS_NOT_SAVED;
                compare_after = 0;
                unchanged_only = 1;
            }
        } else if (!strcmp(op, "restore")) {
            r = models_restore(&m, why);
            expect = swift_ok ? MODELS_OK : swift_code(msg);
            if (is_link) {   /* never followed: unreadable, nothing changed */
                differ++;
                expect = MODELS_UNREADABLE;
                compare_after = 0;
            } else if (orig_differs && strncmp(msg, "Pioneer's stems model is back", 29)) {
                differ++;
                expect = MODELS_NOT_BACK;
                compare_after = 0;
                unchanged_only = 1;
            }
        } else if (!strcmp(op, "left")) {
            r = models_left_behind(&m);
            expect = swift_left(extra);
            if (is_link) differ++, expect = 1;
        } else if (!strcmp(op, "info")) {
            snprintf(mine, sizeof mine, "%d%d", models_any_saved(&m), models_any_verified(&m));
            if (orig_differs && extra[0] && extra[1] == '0') differ++, extra[1] = '1';   /* the link: verified (unknown, kept) */
            r = strcmp(mine, extra);
        } else {
            char shas[16][65];
            int n = models_saved_list(&m, shas, 16);
            for (int i = 0; i < n; i++) {
                size_t w = strlen(mine);
                snprintf(mine + w, sizeof mine - w, "%s%s", i ? "," : "", shas[i]);
            }
            r = strcmp(mine, extra);
        }
        CHECK(r == expect, "modelcase %d (%s): models.c %d, Swift %d (\"%s\" \"%s\"; mine \"%s\")", cases, op, r, expect, msg, extra, mine);
        if (compare_after) {
            both_listings(rb, orig, got, sizeof got);
            CHECK(!strcmp(got, after), "modelcase %d (%s): the folders afterwards\n  models.c %s\n  Swift    %s", cases, op, got, after);
        } else if (unchanged_only) {   /* nothing changed in either folder */
            both_listings(rb, orig, got, sizeof got);
            CHECK(!strcmp(got, before), "modelcase %d (%s): a linked saved copy: nothing changed\n  before %s\n  after  %s", cases, op, before, got);
        } else {   /* nothing changed: both folders, the link itself, where it points, its target's bytes (review MT9) */
            both_listings(rb, orig, got, sizeof got);
            char q[1300], dest[1300], body[64];
            struct stat ls;
            snprintf(p, sizeof p, "%s/hdemucs.onnx", rb);
            snprintf(q, sizeof q, "%s/elsewhere.onnx", root);
            ssize_t k = readlink(p, dest, sizeof dest - 1);
            dest[k > 0 ? k : 0] = 0;
            FILE *tf = fopen(q, "rb");
            size_t bn = tf ? fread(body, 1, sizeof body - 1, tf) : 0;
            if (tf) fclose(tf);
            body[bn] = 0;
            CHECK(!strcmp(got, before) && !lstat(p, &ls) && S_ISLNK(ls.st_mode) && !strcmp(dest, q) && !strcmp(body, "Pioneer's model A"),
                  "modelcase %d (%s): a linked model: nothing changed\n  before %s\n  after  %s", cases, op, before, got);
        }
        free(spec), free(msg), free(after), free(extra);
    }
    fclose(f);
    printf("models: %d cases against Models.swift (%d differ by design: a linked model is never followed, a linked saved copy is never decided on)\n",
           cases, differ);
    return cases;
}
