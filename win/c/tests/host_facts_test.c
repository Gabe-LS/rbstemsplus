/* host_facts.c (the WPF shell's cheap facts) on a scratch tree and a fake compat_os: each row from
 * its reader, with nothing there, and with a current rekordbox, our bridge live, the choices, the
 * model, the helper, Stems Cache's traces and config.ini's cases; then host_facts_write's rows on
 * a fake sheet (the core's core_fact_* are this file's here: the core isn't linked). */
#include "check.h"

#include "../host_facts.h"
#include "core.h"   /* core_fact, the FACT_* rows */

#include <fcntl.h>
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
    CHECK(hf_unset(FACT_CACHE_IS_LINK) && hf_unset(FACT_PROTOTYPE_EXISTS) && hf_unset(FACT_APPLE_SILICON)
              && hf_unset(FACT_ORT_PREFIX),
          "the rows Windows has no reader for stay unset");
    CHECK(hf_writes == 28, "28 rows written (FACT_ORT_LISTED is the DLL's only) (%d)", hf_writes);
    CHECK(w_full == 0, "answered: the sheet says checked (no warm on activation)");
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
    const char *signed_path;   /* AlphaTheta's, by exact path */
    int pins;                  /* pin calls */
    int pin_error;             /* the pin has no answer (COMPAT_PIN_ERROR) */
    const compat_os *self;     /* reenter: the first pin call reads the file again, pinned, then answers ERROR (HR3) */
    int reenter;
    int inner_pin;             /* what the re-entered read returned */
    int incomplete;            /* the enumeration wasn't whole (uninstall_entries' *incomplete) */
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
    return f->signed_path && !strcmp(f->signed_path, path) ? COMPAT_PIN_OK : COMPAT_PIN_NO;
}
static int hf_procs(void *ctx, compat_proc *out, int max) {
    (void)ctx;
    (void)out;
    (void)max;
    return 0;
}
static uint16_t hf_native(void *ctx) {
    (void)ctx;
    return COMPAT_MACHINE_ARM64;
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
    const char *signer = fk->signed_path;
    fk->signed_path = NULL;   /* another signer: couldn't verify rekordbox (COMPAT_RB_UNVERIFIED) */
    compat_memo_clear();
    CHECK(!hf_lb(os, programs, &pr, &lo) && !pr && !lo, "live bridge: rekordbox couldn't be verified: not present");
    fk->signed_path = signer;
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

void test_host_facts(const char *scratch, const char *pe) {
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
    fk.signed_path = exe;
    snprintf(live, sizeof live, "%s/onnxruntime.dll", v19);
    hf_text(live, "MZ... rbstems bridge: 1.5.0 ... rbstems-cache: rebuild=1 location=1;");
    compat_memo_clear();
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.installed && !strcmp(f.version, "7.2.19.0342"), "the current version folder: installed, the entry's version (%s)", f.version);
    CHECK(f.cache_present, "our bridge is the live onnxruntime.dll: Stems Cache in");
    hf_live_bridge(&os, &fk, programs, scratch, live);
    CHECK(f.cache_traces, "our bridge live, no root folder: traces");
    mkdir(root, 0755);
    host_facts_free(&f);
    host_facts_read(&e, &f);
    CHECK(f.cache_present && f.cache_traces, "our bridge live, the root folder there: traces");
    CHECK(f.has_bridge_models && !f.bridge_models[0], "an entry-only bridge (no pioneer=): FACT_BRIDGE_MODELS \"\"");

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
        fk.signed_path = NULL;   /* rekordbox.exe has another signer: unverified */
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
        fk.signed_path = exe;

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
    hf_write_rows();
}
