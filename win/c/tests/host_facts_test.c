/* host_facts.c (the WPF shell's cheap facts) on a scratch tree and a fake compat_os: each row from
 * its reader, with nothing there, and with a current rekordbox, our bridge live, the choices, the
 * model, the helper, Stems Cache's traces and config.ini's cases; then host_facts_write's rows on
 * a fake sheet (the core's core_fact_* are this file's here: the core isn't linked). */
#include "check.h"

#include "../host_facts.h"
#include "../models.h"        /* MODELS_OURS_BUILTIN */
#include "core_win.h" /* core_win_cheap_facts, the FACT_* rows */

#include "../inventory.h"
/* This test shares the host-side sheet ABI. Expose the Windows policy constants
 * after the sheet types have been declared. */
#ifndef CORE_WIN_POLICY
#define CORE_WIN_POLICY
#define HF_UNDEF_WIN_POLICY
#endif
typedef struct arena arena;   /* only pure.h's enum values are used here */
#include "pure.h"
#ifdef HF_UNDEF_WIN_POLICY
#undef CORE_WIN_POLICY
#undef HF_UNDEF_WIN_POLICY
#endif

#define SAME_WIN_ROW(x) _Static_assert((int)WIN_ROW_##x == (int)INV_ROW_##x, #x " row")
#define SAME_WIN_WHY(x) _Static_assert((int)WIN_WHY_##x == (int)WHY_##x, #x " reason")
SAME_WIN_ROW(ON);
SAME_WIN_ROW(READY);
SAME_WIN_ROW(UNSUPPORTED);
SAME_WIN_ROW(REPAIR);
SAME_WIN_ROW(UNVERIFIED);
SAME_WIN_WHY(NONE);
SAME_WIN_WHY(RB_UNLISTED);
SAME_WIN_WHY(BUILD_UNLISTED);
SAME_WIN_WHY(ORT_UNLISTED);
SAME_WIN_WHY(ORT_NOT_ORIGINAL);
SAME_WIN_WHY(R1_MISSING);
SAME_WIN_WHY(R1_FOREIGN);
SAME_WIN_WHY(R1_ORT_MISSING);
SAME_WIN_WHY(DELISTED_BRIDGE);
SAME_WIN_WHY(NOT_VERIFIED);
SAME_WIN_WHY(NO_ANSWER);
SAME_WIN_WHY(NOT_RECORDED);
#undef SAME_WIN_ROW
#undef SAME_WIN_WHY

#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ a fake sheet */

enum { HFS_FLAG = 1, HFS_I64, HFS_STR };
static struct {
    int kind;
    int64_t v;
    char s[1200];
} hf_sheet[CORE_FACT_COUNT];
static int hf_writes;
static void hf_sheet_clear(void) {
    memset(hf_sheet, 0, sizeof hf_sheet);
    hf_writes = 0;
}
void core_fact_flag(core_facts *out, core_fact f, int32_t v) {
    (void)out;
    hf_writes++;
    hf_sheet[f].kind = HFS_FLAG;
    hf_sheet[f].v = v;
}
void core_fact_i64(core_facts *out, core_fact f, int64_t v) {
    (void)out;
    hf_writes++;
    hf_sheet[f].kind = HFS_I64;
    hf_sheet[f].v = v;
}
void core_fact_str(core_facts *out, core_fact f, const char *v, size_t len) {
    (void)out;
    hf_writes++;
    if (len == CORE_NTS) len = strlen(v);
    hf_sheet[f].kind = HFS_STR;
    snprintf(hf_sheet[f].s, sizeof hf_sheet[f].s, "%.*s", (int)len, v);
}
static int hf_flag(core_fact f, int v) { return hf_sheet[f].kind == HFS_FLAG && hf_sheet[f].v == v; }
static int hf_i64(core_fact f, int64_t v) { return hf_sheet[f].kind == HFS_I64 && hf_sheet[f].v == v; }
static int hf_s(core_fact f, const char *v) { return hf_sheet[f].kind == HFS_STR && !strcmp(hf_sheet[f].s, v); }
static int hf_unset(core_fact f) { return hf_sheet[f].kind == 0; }

struct core { int unused; };
struct core_facts { uint32_t parts; };
uint32_t core_facts_parts(const core_facts *out) { return out->parts; }

static void hf_cheap_facts_oom(void) {
    core c = {0};
    core_facts out = {.parts = CORE_FACTS_REFRESH | CORE_FACTS_PREPARE};
    hf_sheet_clear();
    host_facts_test_env_alloc_fails = 1;
    int r = core_win_cheap_facts(&c, &out);
    host_facts_test_env_alloc_fails = 0;
    CHECK(r == 0 && hf_flag(FACT_CACHE_TRACES, 1), "NW86: out of memory, Stems Cache's traces assumed");
    CHECK(hf_flag(FACT_APPLE_SILICON, 1), "NW86: out of memory, Prepare still knows this machine runs the helper");
}

/* every row host_facts_write writes, from a filled host_facts; and what it leaves unset */
static void hf_write_rows(void) {
    host_facts f;
    memset(&f, 0, sizeof f);
    f.has_admin = 1;
    f.admin = 1;
    f.has_own_bundle = 1;
    f.own_bundle = 0;
    f.chosen_model = 1;
    f.chosen_cache = 0;
    f.installed = 1;
    snprintf(f.version, sizeof f.version, "7.2.19.0342");
    snprintf(f.engine, sizeof f.engine, "1.0.0.4");
    f.engine_supported = 1;
    f.plus_present = 0;
    f.cache_present = 1;
    f.cache_record_ok = 1;   /* FACT_CACHE_RECORD_OK: CORE_WIN_POLICY's, the DLL's build only */
    f.cache_traces = 1;
    f.has_bridge_models = 1;
    snprintf(f.bridge_models, sizeof f.bridge_models, "aaaa\nbbbb");
    f.cache_answered = 1;
    f.blocked.rekordbox_supported = 1;
    f.blocked.root_ort_exists = 1;
    f.blocked.has_root_ort = 1;
    f.blocked.has_lib = 1;
    f.blocked.lib_exists = 1;
    f.blocked.lib_signed = 0;
    f.blocked.ort_listed = 1;
    snprintf(f.blocked.ort_version, sizeof f.blocked.ort_version, "1.18.0");
    f.has_model = 1;
    f.model_size = 308572524;
    f.model_mtime_ns = 1700000000LL * 1000000000LL;
    f.helper_there = 1;
    char text[] = "max_gb=1.5\n";
    f.config_text = text;
    f.config_n = strlen(text);
    f.config_enabled = 0;
    f.config_rb_model = 1;
    f.config_max_gb = 1.5;
    f.config_max_days = 30;
    f.config_location = HF_CONFIG_CHOSEN;
    snprintf(f.config_dir, sizeof f.config_dir, "E:\\RB Stems Plus Cache");
    hf_sheet_clear();
    int w_full = host_facts_write(&f, NULL);
    CHECK(hf_flag(FACT_USER_IS_ADMIN, 1) && hf_flag(FACT_OWN_BUNDLE, 0), "the account and the app's place");
    CHECK(hf_flag(FACT_CHOSEN_MODEL, 1) && hf_flag(FACT_CHOSEN_CACHE, 0), "the choices");
    CHECK(hf_flag(FACT_REKORDBOX_INSTALLED, 1) && hf_s(FACT_REKORDBOX_VERSION, "7.2.19.0342"), "rekordbox");
    CHECK(hf_s(FACT_ENGINE, "1.0.0.4") && hf_flag(FACT_ENGINE_SUPPORTED, 1) && hf_flag(FACT_STEMS_PLUS_PRESENT, 0), "the engine, Demucs v4");
    CHECK(hf_flag(FACT_STEMS_CACHE_PRESENT, 1) && hf_flag(FACT_CACHE_TRACES, 1), "Stems Cache in, its traces");
    CHECK(hf_s(FACT_BRIDGE_MODELS, "aaaa\nbbbb"), "the bridge's saved models");
    CHECK(hf_flag(FACT_REKORDBOX_SUPPORTED, 1) && hf_flag(FACT_ROOT_ORT_EXISTS, 1) && hf_flag(FACT_LIB_EXISTS, 1) && hf_flag(FACT_LIB_SIGNED, 0)
              && hf_s(FACT_ORT_VERSION, "1.18.0"),   /* FACT_ORT_LISTED: CORE_WIN_POLICY's, the DLL's build only */
          "BLOCKED cache's rows");
    CHECK(hf_i64(FACT_MODEL_SIZE, 308572524) && hf_i64(FACT_MODEL_MTIME, 1700000000LL * 1000000000LL), "the model's size and time");
    CHECK(hf_flag(FACT_HELPER_THERE, 1), "the helper");
    CHECK(hf_s(FACT_CONFIG_TEXT, "max_gb=1.5\n") && hf_unset(FACT_CONFIG_UNREADABLE) && hf_flag(FACT_CONFIG_ENABLED, 0) && hf_flag(FACT_CONFIG_RB_MODEL, 1)
              && hf_s(FACT_CONFIG_MAX_GB, "1.5") && hf_s(FACT_CONFIG_MAX_DAYS, "30") && hf_i64(FACT_CONFIG_LOCATION, HF_CONFIG_CHOSEN)
              && hf_s(FACT_CONFIG_DIR, "E:\\RB Stems Plus Cache") && hf_s(FACT_CHOSEN_LOCATION, "E:\\RB Stems Plus Cache"),
          "config.ini's rows (%s %s)", hf_sheet[FACT_CONFIG_MAX_GB].s, hf_sheet[FACT_CONFIG_MAX_DAYS].s);
    CHECK(hf_unset(FACT_CACHE_IS_LINK) && hf_unset(FACT_PROTOTYPE_EXISTS) && hf_unset(FACT_ORT_PREFIX),
          "the rows Windows has no reader for stay unset");
    CHECK(hf_flag(FACT_APPLE_SILICON, 1), "this machine runs the helper: always, on Windows");
    CHECK(hf_unset(FACT_BRIDGE_KEEPS) && hf_unset(FACT_STEMS_QUALITY) && hf_unset(FACT_OUR_MODELS), "CORE_FACTS_PREPARE's rows not read: unset");
    CHECK(hf_writes == 29, "29 rows written (FACT_ORT_LISTED is the DLL's only) (%d)", hf_writes);
    CHECK(w_full == 0, "answered: the sheet says checked (no warm on activation)");
    /* CORE_FACTS_PREPARE's rows, read */
    f.has_prepare = 1;
    f.bridge_keeps = 1;
    f.stems_quality = 0;
    hf_sheet_clear();
    host_facts_write(&f, NULL);
    CHECK(hf_flag(FACT_BRIDGE_KEEPS, 1) && hf_i64(FACT_STEMS_QUALITY, 0) && hf_s(FACT_OUR_MODELS, MODELS_OURS_BUILTIN) && hf_writes == 32,
          "prepare's rows: the bridge keeps, the quality, our built-in model alone (%d)", hf_writes);
    f.bridge_keeps = 0;
    f.stems_quality = -1;
    snprintf(f.saved_model, sizeof f.saved_model, "%s", "1111111111111111111111111111111111111111111111111111111111111111");
    hf_sheet_clear();
    host_facts_write(&f, NULL);
    CHECK(hf_flag(FACT_BRIDGE_KEEPS, 0) && hf_i64(FACT_STEMS_QUALITY, -1)
              && hf_s(FACT_OUR_MODELS, MODELS_OURS_BUILTIN "\n1111111111111111111111111111111111111111111111111111111111111111"),
          "prepare's rows: no kept=1, quality unknown, the saved payload's model after ours (%s)", hf_sheet[FACT_OUR_MODELS].s);
    f.blocked.rekordbox_supported = 0;   /* BLOCKED stopped before the root's copy: prepare's ortThere written */
    f.has_ort_there = 1;
    f.ort_there = 1;
    hf_sheet_clear();
    host_facts_write(&f, NULL);
    CHECK(hf_flag(FACT_REKORDBOX_SUPPORTED, 0) && hf_flag(FACT_ROOT_ORT_EXISTS, 1), "rekordbox not listed: prepare's ortThere written");
    f.has_prepare = 0;
    hf_sheet_clear();
    host_facts_write(&f, NULL);
    CHECK(hf_unset(FACT_ROOT_ORT_EXISTS), "without CORE_FACTS_PREPARE: not written (HF7's refresh rows)");
    f.blocked.rekordbox_supported = 1;
    f.has_ort_there = f.ort_there = 0;
    f.has_prepare = 0;
    f.saved_model[0] = 0;
    /* the answers that aren't there: those rows unset; an unreadable config.ini says why */
    f.has_admin = f.has_own_bundle = f.has_model = f.has_bridge_models = 0;
    f.blocked.has_lib = 0;
    f.config_text = NULL;
    snprintf(f.config_unreadable, sizeof f.config_unreadable, "a NUL byte");
    f.config_location = HF_CONFIG_WRONG;
    f.cache_traces = 0;
    hf_sheet_clear();
    int w_none = host_facts_write(&f, NULL);
    CHECK(hf_unset(FACT_USER_IS_ADMIN) && hf_unset(FACT_OWN_BUNDLE) && hf_unset(FACT_MODEL_SIZE) && hf_unset(FACT_MODEL_MTIME)
              && hf_unset(FACT_BRIDGE_MODELS), "no answer: unset");
    CHECK(hf_flag(FACT_ROOT_ORT_EXISTS, 1) && hf_unset(FACT_LIB_EXISTS) && hf_unset(FACT_ORT_VERSION), "no lib: its rows unset");
    CHECK(hf_flag(FACT_CACHE_TRACES, 0), "no traces: written as 0, not left unset");
    CHECK(hf_unset(FACT_CONFIG_TEXT) && hf_s(FACT_CONFIG_UNREADABLE, "a NUL byte") && hf_unset(FACT_CONFIG_DIR) && hf_s(FACT_CHOSEN_LOCATION, "")
              && hf_i64(FACT_CONFIG_LOCATION, HF_CONFIG_WRONG),
          "unreadable config.ini");
    CHECK(w_none == 0, "answered, no lib: checked all the same");
    f.blocked.rekordbox_supported = 0;
    hf_sheet_clear();
    int w_unsup = host_facts_write(&f, NULL);
    CHECK(hf_flag(FACT_REKORDBOX_SUPPORTED, 0) && hf_unset(FACT_ROOT_ORT_EXISTS), "not supported: nothing past it");
    CHECK(w_unsup == 0, "not supported: nothing to check");
    f.cache_answered = 0;
    hf_sheet_clear();
    int w_unsup_unans = host_facts_write(&f, NULL);
    CHECK(hf_flag(FACT_REKORDBOX_SUPPORTED, 0) && hf_unset(FACT_ROOT_ORT_EXISTS), "not supported, no answer past it: rekordbox unsupported written (HF7)");
    CHECK(w_unsup_unans == 0, "not supported, no answer past it: not unchecked");
    /* HF7: a supported rekordbox whose ORT had no answer: supported and the root's copy written,
     * Pioneer's ORT unset (FACT_LIB_UNCHECKED, the DLL's row, says why) */
    f.blocked.rekordbox_supported = 1;
    f.blocked.has_root_ort = 1;
    f.blocked.root_ort_exists = 0;
    f.blocked.has_lib = 0;
    hf_sheet_clear();
    int w_unchecked = host_facts_write(&f, NULL);
    CHECK(hf_flag(FACT_REKORDBOX_SUPPORTED, 1) && hf_flag(FACT_ROOT_ORT_EXISTS, 0) && hf_unset(FACT_LIB_EXISTS) && hf_unset(FACT_LIB_SIGNED),
          "supported, no answer for its ORT: supported and the root's copy written, the ORT's rows unset");
    CHECK(w_unchecked == 1, "supported, no answer for its ORT: the sheet says unchecked (the shell warms on activation)");
    f.blocked.has_root_ort = 0;
    hf_sheet_clear();
    int w_unchecked_noroot = host_facts_write(&f, NULL);
    CHECK(hf_flag(FACT_REKORDBOX_SUPPORTED, 1) && hf_unset(FACT_ROOT_ORT_EXISTS), "the root's copy not looked at: unset");
    CHECK(w_unchecked_noroot == 1, "the root's copy not looked at: unchecked too");
}

typedef struct {
    compat_uninstall entries[2];
    int n_entries;
    const char *signed_paths[4]; /* AlphaTheta's, by exact path */
    int pins;                  /* pin calls */
    int pin_error;             /* the pin has no answer (COMPAT_PIN_ERROR) */
    const compat_os *self;     /* reenter: the first pin call reads the file again, pinned, then answers ERROR (HR3) */
    int reenter;
    int inner_pin;             /* what the re-entered read returned */
    int incomplete;            /* the enumeration wasn't whole (uninstall_entries' *incomplete) */
    compat_proc processes[2];
    int n_processes;
} hf_fake;

static int hf_entries(void *ctx, compat_uninstall *out, int max, int *incomplete) {
    hf_fake *f = ctx;
    *incomplete = f->incomplete;
    int n = f->n_entries < max ? f->n_entries : max;
    memcpy(out, f->entries, (size_t)n * sizeof *out);
    return n;
}
static int hf_pin(void *ctx, const char *path, plat_file pf) {
    hf_fake *f = ctx;
    (void)pf;
    f->pins++;
    if (f->reenter) {
        f->reenter = 0;
        compat_file in;
        f->inner_pin = compat_file_facts(f->self, path, COMPAT_PIN, &in) ? -2 : in.pin;
        return COMPAT_PIN_ERROR;
    }
    if (f->pin_error) return COMPAT_PIN_ERROR;
    for (size_t i = 0; i < sizeof f->signed_paths / sizeof f->signed_paths[0]; i++)
        if (f->signed_paths[i] && !strcmp(f->signed_paths[i], path)) return COMPAT_PIN_OK;
    return COMPAT_PIN_NO;
}
static int hf_procs(void *ctx, compat_proc *out, int max) {
    hf_fake *f = ctx;
    int n = f->n_processes < max ? f->n_processes : max;
    memcpy(out, f->processes, (size_t)n * sizeof *out);
    return n;
}
static uint16_t hf_native(void *ctx) {
    (void)ctx;
    return COMPAT_MACHINE_ARM64;
}

/* pending_deletes' fake: the entries in hf_pend (hf_pending_parse's: a tag, a path, a NUL), or no
 * answer */
static struct {
    char list[8192];
    size_t n;
    int fail, calls;
} hf_pend;
static void hf_pend_add(char tag, const char *s) {
    size_t l = strlen(s) + 1;
    if (hf_pend.n + 1 + l > sizeof hf_pend.list) return;
    hf_pend.list[hf_pend.n++] = tag;
    memcpy(hf_pend.list + hf_pend.n, s, l);
    hf_pend.n += l;
}
/* k deletes ('D'), in this order */
static void hf_pend_set(int k, ...) {
    va_list a;
    va_start(a, k);
    hf_pend.n = 0;
    for (int i = 0; i < k; i++) hf_pend_add('D', va_arg(a, const char *));
    va_end(a);
}
/* a registry value's units from n bytes of ASCII (its NULs included) */
static size_t hf_u16(const char *s, size_t n, uint16_t *out) {
    for (size_t i = 0; i < n; i++) out[i] = (unsigned char)s[i];
    return n;
}
/* hf_pending_parse over one value given as n bytes of ASCII: *buf's entries equal want (wn bytes) */
static int hf_parse_is(const char *s, size_t n, const char *want, size_t wn) {
    uint16_t w[256];
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int ok = n <= 256 && !hf_pending_parse(w, hf_u16(s, n, w), &buf, &len, &cap) && len == wn && (!wn || !memcmp(buf, want, wn));
    free(buf);
    return ok;
}
static int hf_pending(void *ctx, char **out, size_t *n) {
    (void)ctx;
    hf_pend.calls++;
    *out = NULL;
    *n = 0;
    if (hf_pend.fail) return -1;
    if (!(*out = malloc(hf_pend.n + 1))) return -1;
    memcpy(*out, hf_pend.list, hf_pend.n);
    *n = hf_pend.n;
    return 0;
}

static void hf_put(const char *path, const void *s, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    if (n) fwrite(s, 1, n, f);
    fclose(f);
}
static void hf_text(const char *path, const char *s) { hf_put(path, s, strlen(s)); }

/* the UI thread's misses, by path */
typedef struct {
    int n;
    char last[1300];
} hf_misses;
static void hf_miss(void *ctx, const char *path) {
    hf_misses *m = ctx;
    m->n++;
    snprintf(m->last, sizeof m->last, "%s", path);
}

static void hf_copy(const char *from, const char *to) {
    FILE *f = fopen(from, "rb");
    if (!f) abort();
    char *b = malloc(1 << 20);
    size_t n = fread(b, 1, 1 << 20, f);
    fclose(f);
    hf_put(to, b, n);
    free(b);
}

/* The source is the tiny VERSIONINFO test fixture built by run.sh. */
static void hf_copy_newer_fixture(const char *from, const char *to) {
    FILE *f = fopen(from, "rb");
    if (!f) abort();
    unsigned char *b = malloc(1 << 20);
    if (!b) abort();
    size_t n = fread(b, 1, 1 << 20, f);
    fclose(f);
    const char old[] = "7\0.\0" "2\0.\0" "1\0" "9\0.\0" "0\0" "3\0" "4\0" "2\0";
    const char newer[] = "7\0.\0" "2\0.\0" "2\0" "0\0.\0" "0\0" "3\0" "5\0" "4\0";
    int changed = 0;
    for (size_t i = 0; i + sizeof old - 1 <= n; i++) {
        if (memcmp(b + i, old, sizeof old - 1)) continue;
        memcpy(b + i, newer, sizeof newer - 1);
        changed = 1;
    }
    if (!changed) abort();
    hf_put(to, b, n);
    free(b);
}

/* config.ini with text, read again: the facts */
static void hf_config(host_facts_env *e, const char *support, const void *text, size_t n, host_facts *f) {
    char p[1200];
    snprintf(p, sizeof p, "%s/config.ini", support);
    if (text) hf_put(p, text, n);
    else remove(p);
    host_facts_free(f);
    CHECK(!host_facts_read(e, f), "read");
}

/* host_facts_live_bridge (CORE_SLOW_LOCATION's stemsCachePresent and bridgeSaysLocation): the
 * current rekordbox's live DLL as host_facts_read found it, from compat.c's memo; every answer and
 * the fail-closed ones. live holds "... rbstems bridge: ... rbstems-cache: rebuild=1 location=1;"
 * on entry and again on return. */
static int hf_lb(const compat_os *os, const char *programs, int *present, int *location) {
    *present = *location = 7;
    return host_facts_live_bridge(os, programs, present, location);
}
static void hf_live_bridge(const compat_os *os, hf_fake *fk, const char *programs, const char *scratch, const char *live) {
    int pr, lo;
    int64_t s0 = compat_test_scanned;
    CHECK(!hf_lb(os, programs, &pr, &lo) && pr == 1 && lo == 1, "live bridge: ours, location=1: present, says (%d %d)", pr, lo);
    CHECK(compat_test_scanned == s0, "live bridge: host_facts_read's scan, memoised: not read again");
    static const struct { const char *text; int present, location; const char *what; } rows[] = {
        {"MZ rbstems bridge: 1.5.0 rbstems-cache: rebuild=1 pioneer=abc;", 1, 0, "a line without location=1"},
        {"MZ rbstems bridge: 1.5.0 rbstems-cache: rebuild=1 pioneer=abc location=1;", 1, 0, "location=1 after pioneer="},
        {"MZ rbstems bridge: 1.5.0 rbstems-cache: rebuild=1 location=1", 1, 0, "no ';' after the line"},
        {"MZ rbstems bridge: 1.5.0 no cache line", 1, 0, "no cache line (1.0's bridge)"},
        {"MZ rbstems-cache: rebuild=1 location=1;", 0, 0, "the line without the bridge's text: not ours"},
        {"MZ Pioneer's own onnxruntime", 0, 0, "Pioneer's DLL"},
    };
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
        hf_text(live, rows[i].text);
        compat_memo_clear();
        CHECK(!hf_lb(os, programs, &pr, &lo) && pr == rows[i].present && lo == rows[i].location, "live bridge: %s (%d %d)", rows[i].what, pr, lo);
    }
    /* nothing there, or something that isn't a file: answered, Stems Cache not in */
    remove(live);
    compat_memo_clear();
    CHECK(!hf_lb(os, programs, &pr, &lo) && !pr && !lo, "live bridge: no live DLL: not present (%d %d)", pr, lo);
    mkdir(live, 0755);
    CHECK(!hf_lb(os, programs, &pr, &lo) && !pr && !lo, "live bridge: a folder in its place: not present");
    rmdir(live);
    /* fail closed: there but unreadable, no rekordbox answer, no Program Files */
    hf_text(live, "MZ... rbstems bridge: 1.5.0 ... rbstems-cache: rebuild=1 location=1;");   /* as on entry */
    chmod(live, 0);
    compat_memo_clear();
    CHECK(hf_lb(os, programs, &pr, &lo) == -1 && !pr && !lo, "live bridge: the DLL unreadable: no answer, both 0");
    chmod(live, 0644);
    compat_memo_clear();
    fk->pin_error = 1;
    CHECK(hf_lb(os, programs, &pr, &lo) == -1 && !pr && !lo, "live bridge: rekordbox.exe's pin has no answer (COMPAT_RB_NO_ANSWER): no answer");
    fk->pin_error = 0;
    compat_test_entries_alloc_fails = 1;   /* review NW3: out of memory in the rekordbox walk is no answer */
    CHECK(hf_lb(os, programs, &pr, &lo) == -1 && !pr && !lo, "live bridge: the walk out of memory: no answer, both 0 (%d %d)", pr, lo);
    compat_test_entries_alloc_fails = 0;
    CHECK(hf_lb(os, NULL, &pr, &lo) == -1 && !pr && !lo, "live bridge: no Program Files: no answer");
    /* no current rekordbox: answered, Stems Cache not in (FACT_STEMS_CACHE_PRESENT's verdict) */
    const char *signer = fk->signed_paths[0];
    fk->signed_paths[0] = NULL;   /* another signer: couldn't verify rekordbox (COMPAT_RB_UNVERIFIED) */
    compat_memo_clear();
    CHECK(!hf_lb(os, programs, &pr, &lo) && !pr && !lo, "live bridge: rekordbox couldn't be verified: not present");
    fk->signed_paths[0] = signer;
    char none[1200];
    snprintf(none, sizeof none, "%s/no-rekordbox", scratch);
    mkdir(none, 0755);
    int n = fk->n_entries;
    fk->n_entries = 0;
    compat_memo_clear();
    CHECK(!hf_lb(os, none, &pr, &lo) && !pr && !lo, "live bridge: no rekordbox installed: not present");
    fk->n_entries = n;
    rmdir(none);
    compat_memo_clear();
    CHECK(!hf_lb(os, programs, &pr, &lo) && pr == 1 && lo == 1, "live bridge: back again");
}

static void hf_live_bridge_multiple(const compat_os *os, hf_fake *fk, const char *programs, const char *pe, const char *older) {
    char folder[1100], exe[1200], live[1200], fixture[1200];
    snprintf(folder, sizeof folder, "%s/rekordbox 7.2.20", programs);
    snprintf(exe, sizeof exe, "%s/rekordbox.exe", folder);
    snprintf(live, sizeof live, "%s/onnxruntime.dll", folder);
    snprintf(fixture, sizeof fixture, "%s/rekordbox-x64.exe", pe);
    mkdir(folder, 0755);
    hf_copy_newer_fixture(fixture, exe);
    compat_uninstall *e = &fk->entries[fk->n_entries++];
    snprintf(e->key, sizeof e->key, "Pioneer rekordbox 7.2.20");
    snprintf(e->display_version, sizeof e->display_version, "7.2.20.0354");
    snprintf(e->install_location, sizeof e->install_location, "%s", folder);
    fk->signed_paths[1] = exe;
    hf_text(live, "rbstems bridge: 1.5.0 rbstems-cache: location=1;");
    hf_text(older, "rbstems bridge: 1.5.0 rbstems-cache: kept=1;");
    compat_memo_clear();
    int present, location;
    CHECK(!hf_lb(os, programs, &present, &location) && present && !location,
          "the older bridge also needs location=1");
    hf_text(older, "MZ... rbstems bridge: 1.5.0 ... rbstems-cache: rebuild=1 location=1;");
    compat_memo_clear();
    CHECK(!hf_lb(os, programs, &present, &location) && present && location, "all live bridges support location");
    fk->signed_paths[1] = NULL;
    memset(&fk->entries[--fk->n_entries], 0, sizeof fk->entries[0]);
    unlink(live);
    unlink(exe);
    rmdir(folder);
    compat_memo_clear();
}

/* CORE_FACTS_PREPARE's reads (e->prepare) with rekordbox 7.2.19 current and live the DLL: kept=1
 * among the cache line's tokens before pioneer=, stemsQuality() from rekordbox3.settings, no
 * verified saved payload (FACT_OUR_MODELS the built-in one alone) */
static void hf_prepare_reads(host_facts_env *e, const char *live, const char *scratch) {
    char settings[1200];
    snprintf(settings, sizeof settings, "%s/rekordbox3.settings", scratch);
    host_facts f;
    static const struct { const char *text; int keeps; const char *what; } rows[] = {
        {"MZ rbstems bridge: 1.5.0 rbstems-cache: rebuild=1 location=1 kept=1 pioneer=abc;", 1, "the release bridge's line: keeps"},
        {"MZ rbstems bridge: 1.5.0 rbstems-cache: kept=1;", 1, "kept=1 alone"},
        {"MZ rbstems bridge: 1.5.0 rbstems-cache: rebuild=1 location=1 pioneer=abc kept=1;", 0, "kept=1 after pioneer="},
        {"MZ rbstems bridge: 1.5.0 rbstems-cache: rebuild=1 kept=10 xkept=1 pioneer=abc;", 0, "kept=10, xkept=1: not the token"},
        {"MZ rbstems bridge: 1.5.0 rbstems-cache: entry-only;", 0, "an entry-only bridge"},
        {"MZ rbstems bridge: 1.5.0 rbstems-cache: kept=1", 0, "no ';' after the line"},
        {"MZ rbstems-cache: kept=1;", 0, "the line without the bridge's text: not ours"},
    };
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
        hf_text(live, rows[i].text);
        compat_memo_clear();
        e->prepare = 1;
        host_facts_read(e, &f);
        CHECK(f.has_prepare && !f.bridge_keeps, "prepare requires coverage even when the live line says kept=1");
        const char *line = strstr(rows[i].text, "rbstems-cache: ");
        const char *end = line ? strchr(line, ';') : NULL;
        int says = end && strstr(rows[i].text, "rbstems bridge: ")
            && hf_line_says_kept(line + sizeof "rbstems-cache: " - 1, (size_t)(end - line) - (sizeof "rbstems-cache: " - 1));
        CHECK(says == rows[i].keeps, "prepare cache line: %s", rows[i].what);
        host_facts_free(&f);
    }
    e->prepare = 0;
    host_facts_read(e, &f);
    CHECK(!f.has_prepare && !f.bridge_keeps && !f.stems_quality, "without CORE_FACTS_PREPARE: none of its rows read");
    host_facts_free(&f);

    /* stemsQuality(): no file, then its cases */
    e->prepare = 1;
    e->rb_settings = settings;
    host_facts_read(e, &f);
    CHECK(f.stems_quality == -1 && !f.saved_model[0], "no rekordbox3.settings: quality unknown; no saved payload: no second model");
    CHECK(!f.blocked.rekordbox_supported && f.has_ort_there && !f.ort_there, "rekordbox not listed: ortThere read all the same, no copy");
    host_facts_free(&f);
    {
        char ortd[1300], ortf[1400];
        snprintf(ortd, sizeof ortd, "%s/ort", e->root);
        snprintf(ortf, sizeof ortf, "%s/onnxruntime.dll", ortd);
        mkdir(ortd, 0755);
        hf_text(ortf, "MZ");
        host_facts_read(e, &f);
        CHECK(f.has_ort_there && f.ort_there, "rekordbox not listed, ort\\'s copy there: ortThere");
        host_facts_free(&f);
        remove(ortf);
        rmdir(ortd);
    }
    hf_text(settings, "<PROPERTIES><VALUE name=\"PartAnalysisQuality\" val=\"1\"/></PROPERTIES>");
    host_facts_read(e, &f);
    CHECK(f.stems_quality == 1, "Prioritize speed: 1 (%d)", f.stems_quality);
    host_facts_free(&f);
    e->rb_settings = NULL;
    host_facts_read(e, &f);
    CHECK(f.stems_quality == -1, "no settings path: unknown");
    host_facts_free(&f);
    remove(settings);
    e->prepare = 0;
}

static void hf_quality_cases(void) {
    static const struct { const char *text; int q; const char *what; } q[] = {
        {"<VALUE name=\"PartAnalysisQuality\" val=\"0\"/>", 0, "sound quality"},
        {"<VALUE name=\"PartAnalysisQuality\"    val=\"12\"/>", 12, "spaces before val"},
        {"<VALUE name=\"PartAnalysisQuality\" val=\"\"/>", -1, "an empty value"},
        {"<VALUE name=\"PartAnalysisQuality\" val=\"x1\"/>", -1, "not digits"},
        {"<VALUE val=\"1\" name=\"PartAnalysisQuality\"/><VALUE name=\"Other\" val=\"0\"/>", 1, "reversed attributes use their own value"},
        {"<VALUE name=\"PartAnalysisQuality\"/><VALUE name=\"Other\" val=\"1\"/>", -1, "a missing value stays unknown"},
        {"<VALUE name=\"PartAnalysisQuality\" val=\"0\"", -1, "an unterminated element stays unknown"},
        {"<VALUE othername=\"PartAnalysisQuality\" val=\"1\"/>", -1, "a lookalike attribute is ignored"},
        {"<VALUE name=\"partanalysisquality\" val=\"1\"/>", -1, "the name in another case"},
        {"<VALUE name=\"PartAnalysisQuality\" val=\"99999999999\"/>", -1, "past Int32: unknown"},
        {"", -1, "an empty file"},
    };
    for (size_t i = 0; i < sizeof q / sizeof *q; i++) {
        int got = hf_stems_quality(q[i].text, strlen(q[i].text));
        CHECK(got == q[i].q, "stemsQuality: %s (%d)", q[i].what, got);
    }
    char far[200];
    snprintf(far, sizeof far, "<VALUE name=\"PartAnalysisQuality\"%*sval=\"1\"/>", 65, "");
    CHECK(hf_stems_quality(far, strlen(far)) == 1, "stemsQuality: attributes more than 64 bytes apart");
}

/* keptSets() (host_facts_kept_sets): sets by name, their dates and manifests, the passed and
 * malformed ones */
static void hf_kept_date_fake(void *ctx, double t, char *out, size_t n) {
    (void)ctx;
    snprintf(out, n, "day %.0f", t);
}

#define KEPT_TEST_HEX_TAIL "123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define KEPT_TEST_HEX "0" KEPT_TEST_HEX_TAIL

static void hf_kept_sets(const char *scratch) {
    char kept[1100], p[1300];
    char *t;
    snprintf(kept, sizeof kept, "%s/kept", scratch);
    CHECK(!host_facts_kept_sets(kept, 1000, hf_kept_date_fake, NULL, &t) && t && !t[0], "no kept folder: no sets");
    free(t);
    CHECK(!host_facts_kept_sets(NULL, 1000, hf_kept_date_fake, NULL, &t) && t && !t[0], "no kept path: no sets");
    free(t);
    mkdir(kept, 0755);
    snprintf(p, sizeof p, "%s/b-set.kept", kept);
    hf_text(p, "rbstemsplus-kept 1\nuntil 2000\n");
    snprintf(p, sizeof p, "%s/b-set.manifest", kept);
    hf_text(p, "rbstemsplus-manifest 1\nfile C:/a.mp3\nfile C:/b.mp3\nfiles 2\nfile x\n");
    snprintf(p, sizeof p, "%s/A_1.kept", kept);
    hf_text(p, "rbstemsplus-kept 1\nuntil 1500\n" KEPT_TEST_HEX " " KEPT_TEST_HEX "\n");
    snprintf(p, sizeof p, "%s/old.kept", kept);
    hf_text(p, "rbstemsplus-kept 1\nuntil 1000\n");   /* its date is now: passed */
    snprintf(p, sizeof p, "%s/odd.kept", kept);
    hf_text(p, "something else\n");                     /* not the bridge's format: shown, no date */
    snprintf(p, sizeof p, "%s/odd2.kept", kept);
    hf_text(p, "rbstemsplus-kept 1\nuntil  2000\n");
    snprintf(p, sizeof p, "%s/odd3.kept", kept);
    hf_text(p, "rbstemsplus-kept 1\nuntil 2000x\n");
    snprintf(p, sizeof p, "%s/bad name.kept", kept);
    hf_text(p, "rbstemsplus-kept 1\nuntil 2000\n");    /* keptNameOK: no space */
    snprintf(p, sizeof p, "%s/.hidden.kept", kept);
    hf_text(p, "rbstemsplus-kept 1\nuntil 2000\n");
    snprintf(p, sizeof p, "%s/c.manifest", kept);       /* a manifest alone: no set */
    hf_text(p, "file x\n");
    snprintf(p, sizeof p, "%s/dir.kept", kept);         /* a folder: no set */
    mkdir(p, 0755);
    char target[1300];
    snprintf(target, sizeof target, "%s/b-set.kept", kept);
    snprintf(p, sizeof p, "%s/link.kept", kept);        /* a link: not a regular file (the item itself) */
    CHECK(!symlink(target, p), "a link in the kept folder");
    CHECK(!host_facts_kept_sets(kept, 1000, hf_kept_date_fake, NULL, &t) && t, "the kept sets");
    const char *want = "A_1\tday 1500\t0\nb-set\tday 2000\t3\nodd\t\t0\nodd2\t\t0\nodd3\t\t0";
    CHECK(t && !strcmp(t, want), "by name (strcmp), passed ones out, the odd ones without a date, \"file \" lines counted (%s)", t ? t : "");
    free(t);
    CHECK(!host_facts_kept_sets(kept, 2000, NULL, NULL, &t) && t && !strcmp(t, "odd\t\t0\nodd2\t\t0\nodd3\t\t0"),
          "now past every date: only the undated ones (%s)", t ? t : "");
    free(t);

    const struct { const char *body; const char *what; } malformed[] = {
        {"900\n" KEPT_TEST_HEX " " KEPT_TEST_HEX "\nG" KEPT_TEST_HEX_TAIL " " KEPT_TEST_HEX "\n", "expired date with a non-hex entry"},
        {"1500\n" KEPT_TEST_HEX ":" KEPT_TEST_HEX "\n", "entry with the wrong separator"},
        {"1500\n" KEPT_TEST_HEX " " KEPT_TEST_HEX, "entry without its final newline"},
        {"01500\n", "date with a leading zero"},
        {"1500.5\n", "fractional date"},
        {"1500", "date without its final newline"},
        {"1000000000000\n", "date longer than twelve digits"},
    };
    char body[400];
    snprintf(p, sizeof p, "%s/broken.kept", kept);
    for (size_t i = 0; i < sizeof malformed / sizeof *malformed; i++) {
        snprintf(body, sizeof body, "rbstemsplus-kept 1\nuntil %s", malformed[i].body);
        hf_text(p, body);
        CHECK(!host_facts_kept_sets(kept, 1000, hf_kept_date_fake, NULL, &t) && t && strstr(t, "\nbroken\t\t0\n"),
              "kept sets: blank date for %s", malformed[i].what);
        free(t);
    }

    /* A sparse manifest over the old cap. The split prefixes count; the prefix at
     * a block boundary inside the padding line does not. The last line has no newline. */
    const off_t block = 64 << 10, old_cap = 64 << 20;
    snprintf(p, sizeof p, "%s/large.kept", kept);
    hf_text(p, "rbstemsplus-kept 1\nuntil 2000\n");
    snprintf(p, sizeof p, "%s/large.manifest", kept);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int written = fd >= 0
        && pwrite(fd, "file first\n", 11, 0) == 11
        && pwrite(fd, "\nfile split\n", 12, block - 3) == 12
        && pwrite(fd, "\nfile split\n", 12, 2 * block - 5) == 12
        && pwrite(fd, "file inside a line\n", 19, 3 * block) == 19
        && pwrite(fd, "\nfile tail", 10, old_cap) == 10;
    if (fd >= 0 && close(fd)) written = 0;
    t = NULL;
    CHECK(written && !host_facts_kept_sets(kept, 1000, hf_kept_date_fake, NULL, &t)
          && t && strstr(t, "\nlarge\tday 2000\t4\n"),
          "kept sets: count a manifest over 64 MiB with split prefixes and no final newline (%s)", t ? t : "");
    free(t);
}

static void t_covered_capabilities(void) {
    native_inv_caps newest = {.live_line = 1, .cache_line = "kept=1 location=1"};
    native_inv_caps older = {.live_line = 1, .cache_line = "location=1"};
    newest.cache_line_len = strlen(newest.cache_line);
    older.cache_line_len = strlen(older.cache_line);
    const native_inv_caps *covered[] = {&newest, &older};
    int keeps, location;
    hf_covered_capabilities(covered, 2, &keeps, &location);
    CHECK(!keeps && location, "Prepare uses kept=1 only when every covered bridge keeps playlists");
    strcpy(older.cache_line, "kept=1");
    older.cache_line_len = strlen(older.cache_line);
    hf_covered_capabilities(covered, 2, &keeps, &location);
    CHECK(keeps && !location, "location=1 must also be supported in every covered version");
    hf_covered_capabilities(covered, 0, &keeps, &location);
    CHECK(!keeps && !location, "no covered bridge advertises neither capability");
}

void test_host_facts(const char *scratch, const char *pe) {
    hf_quality_cases();
    t_covered_capabilities();
    char support[1100], programs[1100], root[1100], models[1100], model[1200], helper[1200], p[1300];
    snprintf(support, sizeof support, "%s/support", scratch);
    snprintf(programs, sizeof programs, "%s/rekordbox", scratch);
    snprintf(root, sizeof root, "%s/RB Stems Plus", scratch);
    snprintf(models, sizeof models, "%s/models", scratch);
    snprintf(model, sizeof model, "%s/hdemucs.onnx", models);
    snprintf(helper, sizeof helper, "%s/rbsp-prepare.exe", scratch);
    mkdir(support, 0755);
    mkdir(programs, 0755);
    mkdir(models, 0755);   /* our root folder comes later: Stems Cache's traces */
    hf_fake fk;
    memset(&fk, 0, sizeof fk);
    compat_os os = {hf_entries, hf_pin, hf_procs, hf_native, &fk};
    host_facts_env e;
    memset(&e, 0, sizeof e);
    e.os = &os;
    e.programs_rekordbox = programs;
    e.root = root;
    e.model = model;
    e.support = support;
    e.helper = helper;
    e.own = "1.5.0";
    e.admin = -1;
    e.own_bundle = -1;
    compat_memo_clear();

    /* nothing there */
    host_facts f;
    CHECK(!host_facts_read(&e, &f), "read");
    CHECK(!f.has_admin && !f.has_own_bundle, "no answer for the account or the app's place: those rows unset");
    CHECK(!f.chosen_model && !f.chosen_cache, "no installed: nothing chosen");
    CHECK(!f.installed && !f.version[0] && !f.cache_present, "no version folder: rekordbox not installed, no version, Stems Cache not in");
    CHECK(f.cache_answered && !f.blocked.rekordbox_supported, "no verified list: rekordbox not supported (Windows has no built-in list)");
    CHECK(!f.engine[0] && !f.engine_supported && !f.plus_present && !f.has_model, "no model: no engine, Demucs v4 not in, no size");
    CHECK(!f.helper_there, "no helper");
    CHECK(!f.cache_traces, "no root folder, no bridge: no traces");
    CHECK(!f.config_text && !f.config_unreadable[0] && f.config_enabled && !f.config_rb_model && f.config_max_gb == 20 && f.config_max_days == 60
              && f.config_location == HF_CONFIG_THIS && !f.config_dir[0],
          "no config.ini: the defaults, stems saved on this PC");

    {
        char disk[1200], dll[1300];
        snprintf(disk, sizeof disk, "%s/rekordbox 7.2.9", programs);
        snprintf(dll, sizeof dll, "%s/onnxruntime.dll", disk);
        mkdir(disk, 0755);
        hf_text(dll, "MZ... rbstems bridge: 1.5.0;");
        host_facts_free(&f);
        CHECK(!host_facts_read(&e, &f) && !f.installed && f.cache_present && f.cache_traces,
              "a disk-only bridge counts as Cache and cleanup evidence");
        unlink(dll);
        rmdir(disk);
    }

    /* Stems Cache's traces: anything at the root (an empty folder, a file, a link), with or
     * without rekordbox; no root given at all: none */
    mkdir(root, 0755);
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.cache_traces && !f.installed && !f.cache_present, "an empty root folder, no rekordbox: traces");
    rmdir(root);
    hf_text(root, "x");
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.cache_traces, "a file at the root: traces");
    remove(root);
    snprintf(p, sizeof p, "%s/elsewhere", scratch);
    symlink(p, root);   /* dangling: the link itself counts */
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.cache_traces, "a link at the root, its target missing: traces");
    remove(root);
    e.root = NULL;
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(!f.cache_traces, "no root given: no traces");
    {   /* a file where the root's folder would be: nothing there (ENOTDIR); a folder that can't be
         * looked into: fail closed */
        char under[1300], locked[1200];
        snprintf(p, sizeof p, "%s/a-file", scratch);
        hf_text(p, "x");
        snprintf(under, sizeof under, "%s/RB Stems Plus", p);
        e.root = under;
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.cache_traces, "a file in the root's parent's place: nothing there, no traces");
        remove(p);
        snprintf(locked, sizeof locked, "%s/locked", scratch);
        mkdir(locked, 0755);
        snprintf(under, sizeof under, "%s/RB Stems Plus", locked);
        chmod(locked, 0);
        e.root = under;
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_traces, "the root can't be looked at (EACCES): traces, fail closed");
        chmod(locked, 0755);
        rmdir(locked);
    }
    e.root = root;
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(!f.cache_traces, "the root gone again: no traces");

    /* hf_pending_parse: one entry per pair, tagged, in queue order, the prefixes taken off */
    {
#define S(x) x, sizeof x - 1
        CHECK(hf_parse_is(S("\\??\\C:\\d\0\0\0"), S("DC:\\d\0")), "a delete, then the terminator");
        CHECK(hf_parse_is(S("*1\\??\\C:\\d\0\0\0"), S("DC:\\d\0")), "Windows 11's *1 before the NT prefix");
        CHECK(hf_parse_is(S("*12x\0\0\0"), S("D*12x\0")), "a '*' without the NT prefix after it: left as it is");
        CHECK(hf_parse_is(S("\\??\\C:\\a\0\\??\\C:\\b\0\0"), S("RC:\\b\0")), "a rename: its new name");
        CHECK(hf_parse_is(S("\\??\\C:\\a\0!\\??\\C:\\b\0\0"), S("RC:\\b\0")), "a rename's '!' (replace existing) taken off");
        CHECK(hf_parse_is(S("\\??\\C:\\a\0!*1\\??\\C:\\b\0\0"), S("RC:\\b\0")), "a rename's '!' then *1\\??\\ taken off");
        CHECK(hf_parse_is(S("\\??\\C:\\a\0\0\\??\\C:\\b\0\\??\\C:\\c\0\0"), S("DC:\\a\0RC:\\c\0")), "a delete then a rename, in order");
        CHECK(hf_parse_is(S("\0\0\\??\\C:\\a\0\0\0"), S("DC:\\a\0")), "a pair with both names empty is nothing");
        CHECK(hf_parse_is(S("\0"), "", 0) && hf_parse_is("", 0, "", 0), "an empty value: no entries");
#undef S
        /* a name that isn't UTF-16: a delete's left out, a rename's kept with U+FFFD */
        uint16_t w[16] = {'C', ':', 0xD800, 0, 0, 'C', ':', 'a', 0, 'C', ':', 0xDC00, 'b', 0, 0};
        char *buf = NULL;
        size_t len = 0, cap = 0;
        CHECK(!hf_pending_parse(w, 15, &buf, &len, &cap) && len == 8 && !memcmp(buf, "RC:\xEF\xBF\xBD" "b", 8),
              "an unpaired surrogate: the delete left out, the rename kept with U+FFFD (%zu)", len);
        /* a second value appends, its entries after the first's */
        uint16_t w2[4] = {'x', 0, 0, 0};
        CHECK(!hf_pending_parse(w2, 4, &buf, &len, &cap) && len == 11 && !memcmp(buf + 8, "Dx", 3), "a second value's entries after the first's");
        free(buf);
    }

    /* the uninstall worker's reboot branch (I20, J7): a root holding only boot-time deletes (its
     * renamed image, the emptied bin\<sha12>\, bin\ and the root) isn't a trace; anything else in
     * it, or no answer, still is (fail closed) */
    {
        char bin[1200], d12[1300], del[1400], up[1100], lock[1200];
        snprintf(bin, sizeof bin, "%s/bin", root);
        snprintf(d12, sizeof d12, "%s/f8b640536cb1", bin);
        snprintf(del, sizeof del, "%s/RB Stems Plus.exe.0123456789abcdef.del", d12);
        snprintf(lock, sizeof lock, "%s/lock", root);
        mkdir(root, 0755);
        mkdir(bin, 0755);
        mkdir(d12, 0755);
        hf_text(del, "MZ");
        e.pending_deletes = hf_pending;
        hf_pend_set(5, "/elsewhere/x", del, d12, bin, root);
        hf_pend.calls = 0;
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.cache_traces && f.root_pending && hf_pend.calls == 1, "only the reboot branch's deletes in the root: no traces, pending (%d %d)",
              f.cache_traces, f.root_pending);
        snprintf(up, sizeof up, "%s", root);
        for (char *q = up; *q; q++)
            if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 'a' + 'A');
        hf_pend_set(4, del, d12, bin, up);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.cache_traces && f.root_pending, "the root's name cased otherwise (Windows ignores case): no traces");
        /* a rename into the root, wherever it is in the queue (URP1): something arrives there */
        {
            char into[1200], other[1200];
            snprintf(into, sizeof into, "%s/incoming", root);
            hf_pend.n = 0;
            hf_pend_add('R', into);
            hf_pend_add('D', del);
            hf_pend_add('D', d12);
            hf_pend_add('D', bin);
            hf_pend_add('D', root);
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(f.cache_traces && !f.root_pending, "a rename into the root before the worker's deletes: traces");
            hf_pend_set(4, del, d12, bin, root);
            hf_pend_add('R', root);
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(f.cache_traces && !f.root_pending, "a rename to the root itself: traces");
            snprintf(other, sizeof other, "%s/x", up);
            hf_pend_set(4, del, d12, bin, root);
            hf_pend_add('R', other);
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(f.cache_traces && !f.root_pending, "a rename under the root cased otherwise: traces");
            snprintf(other, sizeof other, "%s2/x", root);
            hf_pend_set(4, del, d12, bin, root);
            hf_pend_add('R', other);
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(!f.cache_traces && f.root_pending, "a rename into a sibling sharing the root's prefix: still pending");
            /* the folders above spelled otherwise (URQ1-URQ3): a part named like the root, or a short
             * name made from it, is enough; another short name isn't */
            snprintf(other, sizeof other, "%s/RBSTEM~1/incoming", scratch);
            hf_pend_set(4, del, d12, bin, root);
            hf_pend_add('R', other);
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(f.cache_traces && !f.root_pending, "a rename through the root's short name: traces");
            snprintf(other, sizeof other, "%s/RB1F2A~1/x", scratch);
            hf_pend_set(4, del, d12, bin, root);
            hf_pend_add('R', other);
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(f.cache_traces && !f.root_pending, "a rename through the root's hash-form short name: traces");
            snprintf(other, sizeof other, "Volume{0000}/RB Stems Plus/incoming");
            hf_pend_set(4, del, d12, bin, root);
            hf_pend_add('R', other);
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(f.cache_traces && !f.root_pending, "a rename through a volume-GUID spelling of the root: traces");
            snprintf(other, sizeof other, "%s/COMMON~1/x", scratch);
            hf_pend_set(4, del, d12, bin, root);
            hf_pend_add('R', other);
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(!f.cache_traces && f.root_pending, "a rename through another short name: still pending");
            snprintf(other, sizeof other, "%s/PR\xC3\x96GS/RB Stems Plus/x", scratch);
            hf_pend_set(4, del, d12, bin, root);
            hf_pend_add('R', other);
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(f.cache_traces && !f.root_pending, "a rename through a non-ASCII parent cased otherwise: traces");
            /* only a short name Windows could make from the root's name counts (URR1): no extension,
             * at most 8 bytes, a 1-6 character prefix of "RBSTEMSPLUS" or "RB" and 4 hex digits,
             * then '~' and only digits */
            static const struct {
                const char *rel;
                int traces;
            } urr[] = {
                {"RBSTE~10/x", 1},
                {"rbstem~1/x", 1},
                {"rb1f2a~1/x", 1},
                {"Users/RBROWN~1.COR/AppData/Local/Temp/x.tmp", 0},
                {"Users/RBIANC~1/AppData/Local/Temp/y", 0},
                {"Windows/RBX~9.TMP", 0},
                {"other/RBbackup~1/incoming", 0},
                {"RB~1foo/x", 0},
                {"RB1F2A~10/x", 0},
                {"RB4B8~10/x", 1},
                {"rb4b~100/x", 1},
                {"RB4~1000/x", 1},
                {"RB4B8~1/x", 0},
                {"RB4G8~10/x", 0},
                {"RBSTEM~/x", 0},
                {"Program Files/RB Stems Plus Old/x", 0},
            };
            for (size_t i = 0; i < sizeof urr / sizeof urr[0]; i++) {
                snprintf(other, sizeof other, "%s/%s", scratch, urr[i].rel);
                hf_pend_set(4, del, d12, bin, root);
                hf_pend_add('R', other);
                host_facts_free(&f);
                host_facts_read(&e, &f);
                CHECK(urr[i].traces ? (f.cache_traces && !f.root_pending) : (!f.cache_traces && f.root_pending),
                      "a rename through %s: %s", urr[i].rel, urr[i].traces ? "traces" : "still pending");
            }
        }
        /* the order (URP2): a name's last delete must come before its folder's, or it stays */
        hf_pend_set(4, root, bin, d12, del);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_traces && !f.root_pending, "the deletes parent before child: traces");
        hf_pend_set(5, root, del, d12, bin, root);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.cache_traces && f.root_pending, "a later delete of the root after its children: still pending");
        hf_pend_set(4, del, bin, d12, root);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_traces && !f.root_pending, "bin\\ deleted before bin\\<sha12>\\: traces");
        {   /* the file's only delete in PendingFileRenameOperations2, after the root's in the first */
            static uint16_t w[4096];
            char v[2][4096];
            size_t vn[2], wn;
            vn[0] = (size_t)snprintf(v[0], sizeof v[0], "%s%c%c%s%c%c%s%c%c%c", d12, 0, 0, bin, 0, 0, root, 0, 0, 0);
            vn[1] = (size_t)snprintf(v[1], sizeof v[1], "%s%c%c%c", del, 0, 0, 0);
            char *buf = NULL;
            size_t len = 0, cap = 0;
            int ok = 1;
            for (int i = 0; i < 2; i++) {
                wn = hf_u16(v[i], vn[i], w);
                ok = ok && !hf_pending_parse(w, wn, &buf, &len, &cap);
            }
            CHECK(ok && len < sizeof hf_pend.list, "the two values parsed");
            memcpy(hf_pend.list, buf, len);
            hf_pend.n = len;
            free(buf);
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(f.cache_traces && !f.root_pending, "the file's only delete in the second value, after the root's: traces");
        }
        /* a real leftover: a file nobody scheduled */
        hf_pend_set(4, del, d12, bin, root);
        hf_text(lock, "");
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_traces && !f.root_pending, "a file in the root that isn't scheduled: traces");
        remove(lock);
        /* a scheduled name that is a link: traces, even scheduled */
        symlink(scratch, lock);
        hf_pend_set(5, del, d12, bin, root, lock);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_traces && !f.root_pending, "a link in the root, though scheduled: traces");
        remove(lock);
        /* the folders scheduled but the worker's file isn't */
        hf_pend_set(3, d12, bin, root);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_traces && !f.root_pending, "the worker's file not scheduled: traces");
        /* everything in it scheduled, but not the root itself: it would stay */
        hf_pend_set(3, del, d12, bin);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_traces && !f.root_pending, "the root itself not scheduled: traces");
        /* no answer from the registry: traces */
        hf_pend_set(4, del, d12, bin, root);
        hf_pend.fail = 1;
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_traces && !f.root_pending, "the deletes couldn't be read: traces, fail closed");
        hf_pend.fail = 0;
        /* a folder in it that can't be looked into: traces */
        chmod(d12, 0);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_traces && !f.root_pending, "a scheduled folder that can't be listed: traces, fail closed");
        chmod(d12, 0755);
        /* the POSIX branch's case: the file gone, the folders left scheduled */
        remove(del);
        hf_pend_set(3, d12, bin, root);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.cache_traces && f.root_pending, "only scheduled empty folders: no traces");
        /* no reader at all: the root counts as it is */
        e.pending_deletes = NULL;
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_traces && !f.root_pending, "no reader: the root folder is traces");
        rmdir(d12);
        rmdir(bin);
        rmdir(root);
        /* nothing at the root: the deletes aren't read */
        e.pending_deletes = hf_pending;
        hf_pend.calls = 0;
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.cache_traces && !f.root_pending && hf_pend.calls == 0, "no root: no traces, the deletes not read");
        e.pending_deletes = NULL;
    }

    /* the account and the app's place, as the DLL answers them */
    e.admin = 1;
    e.own_bundle = 0;
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.has_admin && f.admin && f.has_own_bundle && !f.own_bundle, "an administrator, not in its own place");
    e.admin = 0;
    e.own_bundle = 1;
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.has_admin && !f.admin && f.has_own_bundle && f.own_bundle, "a standard account, in its own place");

    /* the choices, the model and its engine, the helper */
    snprintf(p, sizeof p, "%s/installed", support);
    hf_text(p, "model=1\ncache=0\n");
    hf_text(model, "not Demucs v4");
    snprintf(p, sizeof p, "%s/demucs3_ver.txt", models);
    hf_text(p, "  1.0.0.4\r\n");
    hf_text(helper, "MZ");
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.chosen_model && !f.chosen_cache, "installed: Demucs v4 chosen, Stems Cache not");
    CHECK(f.has_model && f.model_size == 13 && f.model_mtime_ns > 0 && f.model_mtime_ns % 1000000000LL == 0 && !f.plus_present,
          "rekordbox's own model: its size and time, Demucs v4 not in (%lld)", (long long)f.model_size);
    CHECK(!strcmp(f.engine, "1.0.0.4") && !f.engine_supported, "the engine, trimmed; not listed without a list (%s)", f.engine);
    CHECK(f.helper_there, "the helper is there");
    {
        int fd = open(model, O_WRONLY | O_TRUNC);
        CHECK(fd >= 0 && !ftruncate(fd, 308572524), "Demucs v4's size (sparse)");
        if (fd >= 0) close(fd);
    }
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.plus_present && f.model_size == 308572524, "Demucs v4's size: Demucs v4 in");
    remove(model);
    snprintf(p, sizeof p, "%s/link.onnx", models);
    hf_text(p, "x");
    symlink(p, model);
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(!f.has_model && !f.plus_present, "a link in the model's place is not the model (the item itself)");
    remove(model);

    /* a current rekordbox, our bridge live (the bridge's text, no list: ours unlisted) */
    char v19[1200], exe[1300], live[1300], us[1300];
    snprintf(v19, sizeof v19, "%s/rekordbox 7.2.19", programs);
    mkdir(v19, 0755);
    snprintf(exe, sizeof exe, "%s/rekordbox.exe", v19);
    snprintf(p, sizeof p, "%s/rekordbox-x64.exe", pe);
    hf_copy(p, exe);
    snprintf(us, sizeof us, "%s/Uninstall rekordbox.exe", v19);
    compat_uninstall *u = &fk.entries[fk.n_entries++];
    memset(u, 0, sizeof *u);
    snprintf(u->key, sizeof u->key, "Pioneer rekordbox 7.2.19");
    snprintf(u->display_name, sizeof u->display_name, "rekordbox 7.2.19 64bit");
    snprintf(u->display_version, sizeof u->display_version, "7.2.19.0342");
    snprintf(u->uninstall_string, sizeof u->uninstall_string, "%s", us);
    fk.signed_paths[0] = exe;
    snprintf(live, sizeof live, "%s/onnxruntime.dll", v19);
    hf_text(live, "MZ... rbstems bridge: 1.5.0 ... rbstems-cache: rebuild=1 location=1;");
    compat_memo_clear();
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.installed && !strcmp(f.version, "7.2.19.0342"), "the current version folder: installed, the entry's version (%s)", f.version);
    CHECK(f.cache_present, "our bridge is the live onnxruntime.dll: Stems Cache in");
    hf_live_bridge(&os, &fk, programs, scratch, live);
    hf_live_bridge_multiple(&os, &fk, programs, pe, live);
    CHECK(f.cache_traces, "our bridge live, no root folder: traces");
    mkdir(root, 0755);
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.cache_present && f.cache_traces, "our bridge live, the root folder there: traces");
    CHECK(f.has_bridge_models && !f.bridge_models[0], "an entry-only bridge (no pioneer=): FACT_BRIDGE_MODELS \"\"");
    CHECK(!f.cache_record_ok, "our bridge live, no receipt: the install isn't recorded as finished");

    {
        compat_file lf;
        CHECK(!compat_file_facts(&os, live, 0, &lf) && lf.readable, "read the bridge fixture");
        char rec[1300], body[66];
        snprintf(rec, sizeof rec, "%s/installed-bridge", root);   /* a one-bridge build's record: never read (D13) */
        snprintf(body, sizeof body, "%s\n", lf.sha256);
        hf_text(rec, body);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_present && !f.cache_record_ok && !f.ncovered, "D13 ignores even an exact legacy bridge record");
        unlink(rec);
    }

    strcpy(fk.processes[0].name, "rekordbox.exe");
    strcpy(fk.processes[0].path, exe);
    fk.n_processes = 1;
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(!strcmp(f.open_versions, "7.2.19\n") && !strcmp(f.win_rows, "7.2.19\t3\t8\t1\t3\t0\t7.2.19.0342\n"),
          "open version and Running row use the verified folder's path");
    strcpy(fk.processes[1].name, "rekordbox.exe");
    fk.n_processes = 2;
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(!f.open_versions[0] && strstr(f.win_rows, "\t1\t3\t"), "an unknown process path only clears the open-version list");
    fk.n_processes = 0;

    host_facts_free(&f);
    hf_prepare_reads(&e, live, scratch);
    hf_text(live, "MZ... rbstems bridge: 1.5.0 ... rbstems-cache: rebuild=1 location=1;");
    compat_memo_clear();

    /* HF4: the bridge's saved models from its cache line (bridgeCacheModels), memoised with the
     * scan: pioneer='s sha256s, one a line, anything else dropped */
    {
        const char *a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const char *b = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        char text[600], want[200];
        snprintf(text, sizeof text, "MZ rbstems bridge: 1.5.0 ... rbstems-cache: rebuild=1 location=1 pioneer=%s,NOTASHA,abcdef,,%s,ABC%s;tail", a, b, a + 3);
        hf_text(live, text);
        snprintf(want, sizeof want, "%s\n%s", a, b);
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_present && f.has_bridge_models && !strcmp(f.bridge_models, want), "pioneer='s sha256s, one a line (%s)", f.bridge_models);
        int64_t s0 = compat_test_scanned;
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(compat_test_scanned == s0 && !strcmp(f.bridge_models, want), "memoised: no read again");
        snprintf(text, sizeof text, "MZ rbstems bridge: 1.5.0 ... rbstems-cache: rebuild=1 pioneer=%s", a);   /* no ';' */
        hf_text(live, text);
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_present && !f.has_bridge_models, "no ';' after the line: unset");
        hf_text(live, "MZ rbstems bridge: 1.5.0 ... no cache line");
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_present && !f.has_bridge_models, "no cache line (1.0's bridge): unset");
        {   /* HG2: a NUL in the line is one of its bytes, as bridgeCacheModels reads it (the Swift
             * oracle's answers: a, b, none) */
            static const struct { const char *pre, *mid; const char *want; } rows[] = {
                {"rebuild=1", " pioneer=A", "A"},   /* a NUL before pioneer=: still found */
                {"pioneer=A", ",B", "B"},           /* a NUL ends A's token: A isn't a sha256, B is */
                {"pioneer=A", "junk", ""},          /* a NUL then junk: no sha256 */
            };
            for (size_t r = 0; r < sizeof rows / sizeof *rows; r++) {
                char line[400], w[100];
                size_t m = 0;
                for (const char *t = rows[r].pre; *t; t++) {
                    if (*t == 'A') { memcpy(line + m, a, 64); m += 64; }
                    else line[m++] = *t;
                }
                line[m++] = 0;
                for (const char *t = rows[r].mid; *t; t++) {
                    if (*t == 'A' || *t == 'B') { memcpy(line + m, *t == 'A' ? a : b, 64); m += 64; }
                    else line[m++] = *t;
                }
                char file[600];
                int k = snprintf(file, sizeof file, "MZ rbstems bridge: 1.5.0 ... rbstems-cache: ");
                memcpy(file + k, line, m);
                file[k + m] = ';';
                snprintf(w, sizeof w, "%s", !strcmp(rows[r].want, "A") ? a : !strcmp(rows[r].want, "B") ? b : "");
                hf_put(live, file, (size_t)k + m + 1);
                compat_memo_clear();
                host_facts_free(&f);
                host_facts_read(&e, &f);
                CHECK(f.cache_present && f.has_bridge_models && !strcmp(f.bridge_models, w), "a NUL in the line, row %zu (%s)", r, f.bridge_models);
            }
        }
        {   /* the line cut by the scan's 1 MiB chunks, its ';' 4096 bytes on and one past */
            size_t big = (1u << 20) + 8192;
            char *buf = malloc(big);
            memset(buf, 'x', big);
            memcpy(buf, "MZ rbstems bridge: ", 19);
            size_t at = (1u << 20) - 7;   /* "rbstems-cache: " across the boundary */
            memcpy(buf + at, "rbstems-cache: pioneer=", 23);
            memcpy(buf + at + 23, a, 64);
            buf[at + 23 + 64] = ',';   /* then a long token that isn't a sha256: dropped */
            size_t semi = at + 15 + 4095;   /* the 4096th byte of the line is the ';' */
            buf[semi] = ';';
            hf_put(live, buf, big);
            compat_memo_clear();
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(f.has_bridge_models && !strncmp(f.bridge_models, a, 64) && strlen(f.bridge_models) == 64,
                  "the line across a chunk, ';' its 4096th byte: read (%zu)", strlen(f.bridge_models));
            buf[semi] = 'x';
            buf[semi + 1] = ';';   /* the 4097th: too long, no line */
            hf_put(live, buf, big);
            compat_memo_clear();
            host_facts_free(&f);
            host_facts_read(&e, &f);
            CHECK(f.cache_present && !f.has_bridge_models, "';' past 4096 bytes: unset");
            /* the line itself cut by the chunk: copied once across it */
            memset(buf, 'x', big);
            memcpy(buf, "MZ rbstems bridge: ", 19);
            at = (1u << 20) - 60;
            memcpy(buf + at, "rbstems-cache: pioneer=", 23);
            memcpy(buf + at + 23, a, 64);
            memcpy(buf + at + 23 + 64, ",", 1);
            memcpy(buf + at + 23 + 65, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef;", 65);
            hf_put(live, buf, big);
            compat_memo_clear();
            host_facts_free(&f);
            host_facts_read(&e, &f);
            snprintf(want, sizeof want, "%s\n%s", a, b);
            CHECK(f.has_bridge_models && !strcmp(f.bridge_models, want), "the line cut by a chunk: each byte once (%s)", f.bridge_models);
            /* HG4: the marker and the line's prefix both near a chunk boundary, found in the same
             * chunk by the one pass, whichever comes first (one of them cut by the boundary) */
            for (int order = 0; order < 2; order++) {
                memset(buf, 'r', big);   /* every byte a candidate */
                at = (1u << 20) - 10;
                const char *first = order ? "rbstems-cache: pioneer=" : "rbstems bridge: ";
                memcpy(buf + at, first, strlen(first));
                size_t next = at + strlen(first);
                if (order) {
                    memcpy(buf + next, a, 64);
                    memcpy(buf + next + 64, "; rbstems bridge: ", 18);
                } else {
                    memcpy(buf + next, "rbstems-cache: pioneer=", 23);
                    memcpy(buf + next + 23, a, 64);
                    buf[next + 23 + 64] = ';';
                }
                hf_put(live, buf, big);
                compat_memo_clear();
                host_facts_free(&f);
                host_facts_read(&e, &f);
                CHECK(f.has_bridge_models && !strcmp(f.bridge_models, a), "%s cut by a chunk, the other after it: both found (%s)",
                      order ? "the line's prefix" : "the marker", f.bridge_models);
            }
            free(buf);
        }
        e.model = model;   /* Demucs v4 in: refresh doesn't read the bridge's models */
        hf_text(live, text);
        hf_text(model, "x");
        {
            int fd = open(model, O_WRONLY | O_TRUNC);
            CHECK(fd >= 0 && !ftruncate(fd, 308572524), "Demucs v4's size (sparse)");
            if (fd >= 0) close(fd);
        }
        snprintf(text, sizeof text, "MZ rbstems bridge: 1.5.0 ... rbstems-cache: pioneer=%s;", a);
        hf_text(live, text);
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.cache_present && f.plus_present && !f.has_bridge_models, "Demucs v4 in: unset");
        remove(model);
        hf_text(live, "MZ... rbstems bridge: 1.5.0 ... rbstems-cache: rebuild=1 location=1;");
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
    }
    CHECK(f.cache_answered && !f.blocked.rekordbox_supported, "still no list: not supported, nothing past it");
    hf_text(live, "MZ Pioneer's onnxruntime");
    compat_memo_clear();
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.installed && !f.cache_present, "Pioneer's DLL live: Stems Cache not in");

    /* HF2: a worker's read (the warm) leaves rekordbox's files memoised, so the UI thread's read
     * after it scans and pins nothing and logs nothing, with the same answer */
    {
        hf_misses ms;
        memset(&ms, 0, sizeof ms);
        compat_memo_clear();
        int p0 = fk.pins;
        int64_t s0 = compat_test_scanned;
        host_facts_free(&f);
        host_facts_read(&e, &f);   /* the warm */
        int p1 = fk.pins;
        int64_t s1 = compat_test_scanned;
        CHECK(p1 > p0 && s1 > s0, "the warm reads and pins (%d pins, %lld bytes)", p1 - p0, (long long)(s1 - s0));
        e.on_main = 1;
        e.miss = hf_miss;
        e.miss_ctx = &ms;
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(fk.pins == p1 && compat_test_scanned == s1 && ms.n == 0, "after the warm, the UI thread pins and scans nothing (%d pins, %lld bytes, %d misses)",
              fk.pins - p1, (long long)(compat_test_scanned - s1), ms.n);
        CHECK(f.installed && !strcmp(f.version, "7.2.19.0342") && !f.cache_present, "the same answer");
        /* no warm: the UI thread reads, and says so once per file and stat */
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        int n1 = ms.n, p2 = fk.pins;
        int64_t s2 = compat_test_scanned;
        CHECK(n1 >= 2 && strstr(ms.last, "onnxruntime.dll") && f.installed && !f.cache_present,
              "not warm: read on the UI thread, each file said (%d, %s)", n1, ms.last);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(ms.n == n1 && fk.pins == p2 && compat_test_scanned == s2, "then memoised: said once (%d)", ms.n);
        /* a pin with no answer: the UI thread doesn't ask again for that file and stat; a worker does */
        compat_memo_clear();
        fk.pin_error = 1;
        host_facts_free(&f);
        host_facts_read(&e, &f);
        int p3 = fk.pins, n3 = ms.n;
        CHECK(p3 > p2 && !f.installed, "the pin has no answer: rekordbox not verified (%d pins)", p3 - p2);
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(fk.pins == p3 && ms.n == n3, "the UI thread remembers the pin's no-answer: no pin again (%d), nothing said (%d)", fk.pins - p3, ms.n - n3);
        e.on_main = 0;
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(fk.pins > p3, "a worker's read asks the pin again");
        fk.pin_error = 0;
        e.miss = NULL;
        e.miss_ctx = NULL;
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.installed, "the pin answers again: rekordbox verified");

        /* HR3: a read whose pin has no answer, while another read of the same file pinned it OK
         * (here the pin re-enters: deterministic), keeps and takes that answer; a later read
         * pins nothing */
        compat_memo_clear();
        fk.self = &os;
        fk.reenter = 1;
        compat_file cf;
        int p5 = fk.pins;
        CHECK(!compat_file_facts(&os, exe, COMPAT_PIN, &cf) && cf.pin == COMPAT_PIN_OK && fk.inner_pin == COMPAT_PIN_OK,
              "the outer read's late no-answer keeps the inner read's OK (outer %d, inner %d)", cf.pin, fk.inner_pin);
        CHECK(fk.pins == p5 + 2, "two pins (%d)", fk.pins - p5);
        CHECK(!compat_file_facts(&os, exe, COMPAT_PIN, &cf) && cf.pin == COMPAT_PIN_OK && fk.pins == p5 + 2,
              "a later read: OK from the memo, no pin (%d)", fk.pins - p5);
        CHECK(!compat_file_facts(&os, exe, 0, &cf) && !compat_file_facts(&os, exe, COMPAT_PIN, &cf) && cf.pin == COMPAT_PIN_OK
                  && fk.pins == p5 + 2,
              "an unpinned read in between doesn't drop the pin");
        fk.self = NULL;
        compat_memo_clear();

        /* HR1: no root folder, rekordbox not verified (the pin had no answer, or another signer):
         * the live DLL read unpinned decides Stems Cache's traces, fail closed */
        rmdir(root);
        hf_text(live, "MZ... rbstems bridge: 1.5.0 ... rbstems-cache: rebuild=1 location=1;");
        fk.pin_error = 1;
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.installed && f.cache_traces, "the pin has no answer, our bridge live, no root: traces");
        fk.pin_error = 0;
        fk.signed_paths[0] = NULL;   /* rekordbox.exe has another signer: unverified */
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.installed && f.cache_traces, "rekordbox unverified, our bridge live, no root: traces");
        hf_text(live, "MZ Pioneer's onnxruntime");
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.installed && !f.cache_traces, "rekordbox unverified, Pioneer's DLL, no root: no traces");
        remove(live);
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.installed && !f.cache_traces, "rekordbox unverified, no live DLL, no root: no traces");
        mkdir(live, 0755);   /* a folder in its place: not Pioneer's file */
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(!f.installed && f.cache_traces, "rekordbox unverified, a folder as the live DLL: traces");
        rmdir(live);
        hf_text(live, "MZ Pioneer's onnxruntime");
        fk.signed_paths[0] = exe;

        /* RE3: a verified listed 7.2.19 (Pioneer's DLL live, no root) and an unlisted 7.2.22
         * holding rekordbox.exe. A whole walk: 7.2.22 isn't rekordbox's, no traces. A partial
         * walk: no answer and no candidate, so 7.2.19's DLL doesn't decide: traces, fail closed */
        char v22[1200], exe22[1300];
        snprintf(v22, sizeof v22, "%s/rekordbox 7.2.22", programs);
        mkdir(v22, 0755);
        snprintf(exe22, sizeof exe22, "%s/rekordbox.exe", v22);
        hf_copy(exe, exe22);
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.installed && !strcmp(f.version, "7.2.19.0342") && !f.cache_traces,
              "RE3: a whole walk, an unlisted 7.2.22: 7.2.19 installed, no traces (%d %d)", f.installed, f.cache_traces);
        fk.incomplete = 1;
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
        CHECK(f.installed == 0 && f.cache_traces == 1,
              "RE3: a partial walk, an unlisted 7.2.22 holding rekordbox.exe: not installed, traces fail closed (%d %d)", f.installed,
              f.cache_traces);
        fk.incomplete = 0;
        remove(exe22);
        rmdir(v22);

        mkdir(root, 0755);
        compat_memo_clear();
        host_facts_free(&f);
        host_facts_read(&e, &f);
    }

    /* config.ini */
    const char *values = "enabled=0\nrekordbox_model=1\nmax_gb=0.1\nmax_days=30\n";
    hf_config(&e, support, values, strlen(values), &f);
    CHECK(f.config_text && f.config_n == strlen(values) && !memcmp(f.config_text, values, f.config_n) && !f.config_unreadable[0],
          "readable: its bytes");
    CHECK(!f.config_enabled && f.config_rb_model && f.config_max_gb == 0.1 && f.config_max_days == 30 && f.config_location == HF_CONFIG_THIS,
          "its values, max_gb as typed (%.17g)", f.config_max_gb);
    hf_config(&e, support, "max_gb=1.5\nmax_days=0\n", 22, &f);
    CHECK(f.config_max_gb == 1.5 && f.config_max_days == 60, "a value out of range: the default (%.17g, %g)", f.config_max_gb, f.config_max_days);
    const char *chosen = "cache_dir=\"/Volumes/Stems/RB Stems Plus Cache.noindex\"\ncache_id=0123abcd-0000-4000-8000-0123456789ab\n";
    hf_config(&e, support, chosen, strlen(chosen), &f);
    CHECK(f.config_location == HF_CONFIG_CHOSEN && !strcmp(f.config_dir, "/Volumes/Stems/RB Stems Plus Cache.noindex"),
          "a chosen location: its folder (%d %s)", f.config_location, f.config_dir);
    hf_config(&e, support, "cache_dir=\"/Volumes/Stems/RB Stems Plus Cache.noindex\"\n", 55, &f);
    CHECK(f.config_location == HF_CONFIG_WRONG && !f.config_dir[0], "a folder with no id: set wrongly");
    hf_config(&e, support, "enabled=0\0\n", 11, &f);
    CHECK(!f.config_text && !strcmp(f.config_unreadable, "a NUL byte") && f.config_location == HF_CONFIG_WRONG && f.config_enabled,
          "a NUL byte: unreadable, set wrongly, the defaults (%s)", f.config_unreadable);
    hf_config(&e, support, "max_gb=\xff\n", 8, &f);
    CHECK(!f.config_text && !strcmp(f.config_unreadable, "not UTF-8") && f.config_location == HF_CONFIG_WRONG, "not UTF-8: unreadable");
    {
        char *big = malloc(70000);
        memset(big, '#', 70000);
        hf_config(&e, support, big, 70000, &f);
        free(big);
        CHECK(!f.config_text && !strcmp(f.config_unreadable, "over 64 KB"), "over 64 KB: unreadable (%s)", f.config_unreadable);
    }
    hf_config(&e, support, NULL, 0, &f);
    CHECK(!f.config_text && !f.config_unreadable[0] && f.config_location == HF_CONFIG_THIS, "removed again: the defaults");
    host_facts_free(&f);
    compat_memo_clear();
    hf_kept_sets(scratch);
    hf_write_rows();
    hf_cheap_facts_oom();
}
