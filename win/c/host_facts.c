/* The Windows shell's cheap facts (host_facts.h): the reads, and the DLL's export that writes them
 * into the core's sheet (core_win.h's core_win_cheap_facts). Each row is the Windows reader the
 * DLL's steps already use for the same fact, so the view and the flows agree:
 *  - the account: native_user_is_admin (FACT_USER_IS_ADMIN, as BLOCKED's);
 *  - the app in its own place: update.c's update_in_place (FACT_OWN_BUNDLE, UPDATE_CHECK's);
 *  - the choices: support\installed by the core's state_chosen (STATE_LINE's, the watcher's);
 *  - rekordbox: compat.c's current version folder under <Program Files>\rekordbox and its
 *    version, the uninstall entry's else the folder's (BLOCKED's current_folder);
 *  - the lists: the verified saved payload, else the verified installed-release record (BLOCKED's
 *    read_lists): native_blocked_plus (FACT_ENGINE*) and native_blocked_cache
 *    (FACT_REKORDBOX_SUPPORTED, FACT_ROOT_ORT_EXISTS, FACT_LIB_*, FACT_ORT_VERSION, FACT_ORT_LISTED);
 *  - Stems Cache in: R1's live onnxruntime.dll is our bridge, against both lists' bridges
 *    (STATE_LINE's cache_present, the watcher's);
 *  - the bridge's saved models (bridgeCacheModels): the live bridge's cache line, from the same
 *    memoised scan as R1's read;
 *  - Stems Cache's traces (cacheTraces): Stems Cache in, or anything at <Program Files>\RB Stems
 *    Plus (Pioneer's saved files, ort\'s copy, an empty folder), not following a link; a stat
 *    that fails for any reason but "nothing there" counts as traces (fail closed: Uninstall All
 *    then removes Stems Cache first, or stops);
 *  - Demucs v4 in: rekordbox's model file has Demucs v4's size, ours or the list's (the watcher's
 *    rule); its size and time (FACT_MODEL_SIZE, FACT_MODEL_MTIME, the sha256 memo's key);
 *  - the helper there: paths.helper a regular file;
 *  - config.ini: readConfigState over common/cfg_ini.c's grammar (the bridge's), with
 *    readConfigBytes' reasons for an unreadable file. */
#include "host_facts.h"

#ifndef RBSP_CORE_PLAT_SHIM_H
#  define RBSP_CORE_PLAT_SHIM_H   /* common/plat.h (through compat.h) has the shim's names */
#endif
#include "internal.h"   /* the core's state_chosen (and, in the DLL, fmt_config_number) */

#include "cfg_ini.h"
#include "native_location.h"   /* native_location_line_says: bridgeSaysLocation's rule */
#include "update.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define SEP "\\"
#else
#  define SEP "/"      /* the Mac tests: a scratch tree */
#endif

enum { HF_SMALL_MAX = 64 * 1024 };
/* the Mac's ourModelSize: Demucs v4's hdemucs.onnx (watch.h's WATCH_OUR_MODEL_SIZE) */
enum { HF_OUR_MODEL_SIZE = 308572524 };

static int join(char *out, size_t n, const char *a, const char *b) {
    int k = snprintf(out, n, "%s" SEP "%s", a, b);
    return k > 0 && (size_t)k < n;
}

/* a file's size the way Config.swift's maxGB came: max_bytes is (long long)(gb * 2^30); the
 * shortest decimal that gives the same bytes (the value as typed, for configNumber) */
static double gb_of(long long bytes) {
    double x = (double)bytes / (double)(1LL << 30);
    for (int digits = 1; digits <= 17; digits++) {
        char b[40];
        snprintf(b, sizeof b, "%.*g", digits, x);
        double y = strtod(b, NULL);
        if ((long long)(y * (double)(1LL << 30)) == bytes) return y;
    }
    return x;
}

static void config(const host_facts_env *e, host_facts *o) {
    cfg_config c;
    memset(&c, 0, sizeof c);
    c.enabled = 1;                       /* Config's defaults */
    c.rekordbox_model = 0;
    c.max_bytes = 20LL << 30;
    c.max_days = 60;
    c.location = 0;
    char p[1100];
    char *b = NULL;
    size_t n = 0;
    int r = !e->support || !join(p, sizeof p, e->support, "config.ini") ? ENAMETOOLONG : plat_read_file(p, CFG_MAX, &b, &n);
    if (r == ENOENT) {
        /* no file: the defaults, stems saved on this PC */
    } else if (r) {
        snprintf(o->config_unreadable, sizeof o->config_unreadable, "%s", r == PLAT_EFTYPE ? "over 64 KB" : strerror(r));
    } else if (memchr(b, 0, n)) {
        snprintf(o->config_unreadable, sizeof o->config_unreadable, "a NUL byte");
    } else if (!cfg_utf8_ok((const unsigned char *)b, n) || cfg_parse(b, n, &c, NULL)) {
        snprintf(o->config_unreadable, sizeof o->config_unreadable, "not UTF-8");
    } else {
        o->config_text = b;              /* readable: its bytes */
        o->config_n = n;
        b = NULL;
    }
    free(b);
    o->config_enabled = c.enabled != 0;
    o->config_rb_model = c.rekordbox_model != 0;
    o->config_max_gb = gb_of(c.max_bytes);
    o->config_max_days = c.max_days;
    if (o->config_unreadable[0] || c.location < 0) o->config_location = HF_CONFIG_WRONG;
    else if (c.location == 1) {
        o->config_location = HF_CONFIG_CHOSEN;
        snprintf(o->config_dir, sizeof o->config_dir, "%s", c.dir);
    } else o->config_location = HF_CONFIG_THIS;
}

static int read_facts(const host_facts_env *e, host_facts *o);

int host_facts_read(const host_facts_env *e, host_facts *o) {
    if (e->on_main) compat_main_reads(1, e->miss, e->miss_ctx);
    int r = read_facts(e, o);
    if (e->on_main) compat_main_reads(0, NULL, NULL);
    return r;
}

static int read_facts(const host_facts_env *e, host_facts *o) {
    memset(o, 0, sizeof *o);
    o->has_admin = e->admin >= 0;
    o->admin = e->admin == 1;
    o->has_own_bundle = e->own_bundle >= 0;
    o->own_bundle = e->own_bundle == 1;

    /* the choices */
    char p[1100], *t = NULL;
    size_t tn = 0;
    if (e->support && join(p, sizeof p, e->support, "installed") && !plat_read_file(p, HF_SMALL_MAX, &t, &tn)) {
        int32_t model = 0, cache = 0;
        state_chosen(t, tn, &model, &cache);
        o->chosen_model = model != 0;
        o->chosen_cache = cache != 0;
    }
    free(t);

    /* the lists: payload\'s, else installed-release\'s */
    payload_manifest *saved = calloc(1, sizeof *saved), *inst = calloc(1, sizeof *inst);
    compat_rekordbox *rb = calloc(COMPAT_ENTRIES_MAX, sizeof *rb);
    if (!saved || !inst || !rb) {
        free(saved);
        free(inst);
        free(rb);
        return -1;
    }
    int hs = 0, hi = 0;
    if (e->support && e->own) {
        update_env ue;
        memset(&ue, 0, sizeof ue);
        ue.support = e->support;
        ue.own = e->own;
        ue.keys = e->keys;
        ue.nkeys = e->nkeys;
        update_store st;
        update_store_init(&ue, &st);
        hs = payload_saved(&st.files, saved) == PAYLOAD_OK && saved->has_windows;
        hi = update_installed(&ue, inst) == PAYLOAD_OK && inst->has_windows;
    }
    const payload_manifest *m = hs ? saved : hi ? inst : NULL;

    /* rekordbox's current version folder */
    char folder[COMPAT_PATH_MAX] = "";
    int cur = -1, state = 0, nrb = 0;
    if (e->programs_rekordbox) nrb = compat_find_rekordbox(e->os, e->programs_rekordbox, rb, COMPAT_ENTRIES_MAX, &cur, &state);
    if (cur >= 0) {
        o->installed = 1;
        snprintf(folder, sizeof folder, "%s", rb[cur].folder);
        snprintf(o->version, sizeof o->version, "%s", rb[cur].display_version[0] ? rb[cur].display_version : rb[cur].folder_version);
    }
    /* a candidate that couldn't be verified (its pin had no answer, or its versions or signer
     * disagree) isn't installed, so R1 isn't read; its live onnxruntime.dll read without a pin
     * (memoised) still says whether our bridge may be in: Stems Cache's traces fail closed (HR1) */
    int unverified_traces = 0;
    if (cur < 0 && (state == COMPAT_RB_NO_ANSWER || state == COMPAT_RB_UNVERIFIED)) {
        int had = 0;
        for (int i = 0; i < nrb; i++) {
            if (!rb[i].candidate) continue;
            had = 1;
            char lp[COMPAT_PATH_MAX + 32];
            compat_file lf;
            if (!join(lp, sizeof lp, rb[i].folder, "onnxruntime.dll") || compat_file_facts(e->os, lp, 0, &lf)) unverified_traces = 1;
            else if (lf.present)
                unverified_traces = !lf.exists || !lf.readable || lf.marker || (hs && !strcmp(lf.sha256, saved->win_bridge.sha256))
                                 || (hi && !strcmp(lf.sha256, inst->win_bridge.sha256));
            break;
        }
        /* review RE3: no answer with no candidate (a partial walk's unlisted newer folder, a root
         * that couldn't be listed): which folder decides isn't known, so the traces fail closed */
        if (!had && state == COMPAT_RB_NO_ANSWER) unverified_traces = 1;
    }
    free(rb);

    /* R1's facts once (the live DLL, ort\'s copy, Pioneer's ORT): Stems Cache in, and the cache's
     * facts by BLOCKED's rules over the same read */
    compat_r1 r1;
    int have_r1 = o->installed && e->root;
    if (have_r1) {
        payload_sha bridges[2];
        int nb = 0;
        if (hs) memcpy(bridges[nb++], saved->win_bridge.sha256, sizeof bridges[0]);
        if (hi) memcpy(bridges[nb++], inst->win_bridge.sha256, sizeof bridges[0]);
        compat_r1_facts(e->os, folder, e->root, bridges, nb, m, &r1);
        o->cache_present = r1.live == COMPAT_LIVE_OURS || r1.live == COMPAT_LIVE_OURS_UNLISTED;
    }
    native_blocked_plus(e->model, m, o->engine, sizeof o->engine, &o->engine_supported);
    o->cache_answered = native_blocked_cache(e->os, o->installed ? folder : NULL, o->version, e->root, hs ? saved : NULL, hi ? inst : NULL,
                                             have_r1 ? &r1 : NULL, &o->blocked) == 0;

    /* Stems Cache's traces: our root folder, whatever is in it (or anything in its place) */
    plat_stat rst;
    int rs = e->root ? plat_stat_path(e->root, 0, &rst) : ENOENT;
    o->cache_traces = o->cache_present || unverified_traces || (rs != ENOENT && rs != ENOTDIR);

    /* rekordbox's model file: the item itself, not a link's target */
    plat_stat ms;
    if (e->model && !plat_stat_path(e->model, 0, &ms) && ms.kind == PLAT_FILE) {
        o->has_model = 1;
        o->model_size = ms.size;
        o->model_mtime_ns = ms.mtime * 1000000000LL;
        o->plus_present = ms.size == HF_OUR_MODEL_SIZE || (m && ms.size == m->model.size);
    }

    /* the bridge's saved models (bridgeCacheModels), as refresh reads them: only with Stems Cache
     * in and rekordbox's own model; the live DLL's cache line, memoised by R1's read (HF4) */
    if (o->cache_present && !o->plus_present) {
        char lp[COMPAT_PATH_MAX + 32];
        compat_file lf;
        if (join(lp, sizeof lp, folder, "onnxruntime.dll") && !compat_file_facts(e->os, lp, 0, &lf) && lf.readable && lf.marker
            && lf.has_cache_line) {
            o->has_bridge_models = 1;
            /* the line's bytes, a NUL among them, as bridgeCacheModels reads them; a ',' or "pioneer="
             * next to a grapheme-extending code point isn't mirrored (Swift splits by Character), as
             * only our bridges write the line, in ASCII */
            const char *ln = lf.cache_line, *z = ln + lf.cache_line_len;
            size_t at = 0, n = 0;
            while (at + 8 <= lf.cache_line_len && memcmp(ln + at, "pioneer=", 8)) at++;
            for (const char *s = at + 8 <= lf.cache_line_len ? ln + at + 8 : z; s < z;) {
                const char *end = memchr(s, ',', (size_t)(z - s));
                size_t len = end ? (size_t)(end - s) : (size_t)(z - s);
                int sha = len == 64;
                for (size_t i = 0; sha && i < len; i++) sha = (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f');
                if (sha && n + 65 < sizeof o->bridge_models) {
                    if (n) o->bridge_models[n++] = '\n';
                    memcpy(o->bridge_models + n, s, 64);
                    n += 64;
                }
                s = end ? end + 1 : z;
            }
            o->bridge_models[n] = 0;
        }
    }
    free(saved);
    free(inst);

    plat_stat hs_st;
    o->helper_there = e->helper && !plat_stat_path(e->helper, 1, &hs_st) && hs_st.kind == PLAT_FILE;

    config(e, o);
    return 0;
}

int host_facts_live_bridge(const compat_os *os, const char *programs_rekordbox, int *present, int *location) {
    *present = *location = 0;
    if (!programs_rekordbox) return -1;
    compat_rekordbox *rb = calloc(COMPAT_ENTRIES_MAX, sizeof *rb);
    if (!rb) return -1;
    int cur = -1, state = 0;
    compat_find_rekordbox(os, programs_rekordbox, rb, COMPAT_ENTRIES_MAX, &cur, &state);
    char lp[COMPAT_PATH_MAX + 32];
    int r = cur < 0 ? (state == COMPAT_RB_NO_ANSWER ? -1 : 0) : join(lp, sizeof lp, rb[cur].folder, "onnxruntime.dll") ? 1 : -1;
    free(rb);
    if (r <= 0) return r;   /* no current rekordbox: Stems Cache isn't in; or no answer */
    compat_file *lf = malloc(sizeof *lf);
    if (!lf) return -1;
    r = compat_file_facts(os, lp, 0, lf) ? -1                        /* couldn't be looked at */
      : !lf->present || !lf->exists ? 0                               /* missing, or a link or folder in its place */
      : !lf->readable ? -1                                            /* there, but unreadable */
                      : 1;
    if (r == 1) {
        *present = lf->marker != 0;
        *location = lf->marker && lf->has_cache_line && native_location_line_says(lf->cache_line, lf->cache_line_len);
        r = 0;
    }
    free(lf);
    return r;
}

void host_facts_free(host_facts *f) {
    free(f->config_text);
    f->config_text = NULL;
    f->config_n = 0;
}

/* ---------------------------------------------------------------- the sheet */

static void hf_str(core_facts *out, core_fact f, const char *s) { core_fact_str(out, f, s, CORE_NTS); }

int host_facts_write(const host_facts *f, core_facts *out) {
    if (f->has_admin) core_fact_flag(out, FACT_USER_IS_ADMIN, f->admin);
    if (f->has_own_bundle) core_fact_flag(out, FACT_OWN_BUNDLE, f->own_bundle);
    core_fact_flag(out, FACT_CHOSEN_MODEL, f->chosen_model);
    core_fact_flag(out, FACT_CHOSEN_CACHE, f->chosen_cache);
    core_fact_flag(out, FACT_REKORDBOX_INSTALLED, f->installed);
    hf_str(out, FACT_REKORDBOX_VERSION, f->version);
    hf_str(out, FACT_ENGINE, f->engine);
    core_fact_flag(out, FACT_ENGINE_SUPPORTED, f->engine_supported);
    core_fact_flag(out, FACT_STEMS_PLUS_PRESENT, f->plus_present);
    core_fact_flag(out, FACT_STEMS_CACHE_PRESENT, f->cache_present);
    core_fact_flag(out, FACT_CACHE_TRACES, f->cache_traces);
    if (f->has_bridge_models) hf_str(out, FACT_BRIDGE_MODELS, f->bridge_models);
    {   /* BLOCKED "cache"'s rows: rekordbox supported always (decided before any file read), the
         * root's ORT copy once it was looked at, Pioneer's ORT only with an answer (HF7) */
        const native_blocked *b = &f->blocked;
        core_fact_flag(out, FACT_REKORDBOX_SUPPORTED, b->rekordbox_supported);
        if (b->rekordbox_supported && b->has_root_ort) core_fact_flag(out, FACT_ROOT_ORT_EXISTS, b->root_ort_exists);
#ifdef CORE_WIN_POLICY   /* a Windows row: the Mac tests' core has no such fact */
        if (b->rekordbox_supported && !f->cache_answered) core_fact_flag(out, FACT_LIB_UNCHECKED, 1);
#endif
        if (b->rekordbox_supported && f->cache_answered) {
            if (b->has_lib) {
                core_fact_flag(out, FACT_LIB_EXISTS, b->lib_exists);
                core_fact_flag(out, FACT_LIB_SIGNED, b->lib_signed);
#ifdef CORE_WIN_POLICY   /* a Windows row: the Mac tests' core has no such fact */
                core_fact_flag(out, FACT_ORT_LISTED, b->ort_listed);
#endif
                if (b->ort_version[0]) hf_str(out, FACT_ORT_VERSION, b->ort_version);
            }
        }
    }
    if (f->has_model) {
        core_fact_i64(out, FACT_MODEL_SIZE, f->model_size);
        core_fact_i64(out, FACT_MODEL_MTIME, f->model_mtime_ns);
    }
    core_fact_flag(out, FACT_HELPER_THERE, f->helper_there);
    if (f->config_text) core_fact_str(out, FACT_CONFIG_TEXT, f->config_text, f->config_n);
    if (f->config_unreadable[0]) hf_str(out, FACT_CONFIG_UNREADABLE, f->config_unreadable);
    core_fact_flag(out, FACT_CONFIG_ENABLED, f->config_enabled);
    core_fact_flag(out, FACT_CONFIG_RB_MODEL, f->config_rb_model);
    char num[32];
    fmt_config_number(f->config_max_gb, num);
    hf_str(out, FACT_CONFIG_MAX_GB, num);
    fmt_config_number(f->config_max_days, num);
    hf_str(out, FACT_CONFIG_MAX_DAYS, num);
    core_fact_i64(out, FACT_CONFIG_LOCATION, f->config_location);
    if (f->config_location == HF_CONFIG_CHOSEN) hf_str(out, FACT_CONFIG_DIR, f->config_dir);
    hf_str(out, FACT_CHOSEN_LOCATION, f->config_location == HF_CONFIG_CHOSEN ? f->config_dir : "");
    return f->blocked.rekordbox_supported && !f->cache_answered;
}

/* ---------------------------------------------------------------- the DLL's export */

#if defined(_WIN32) && defined(CORE_WIN_POLICY)
#include "core_win.h"
#include "native.h"
#include "core_native_admin.h"   /* native_user_is_admin, native_key_texts */
#include "win_rules.h"           /* wr_program_path */

enum { HF_KEYS_MAX = 4 };
static sig_pubkey hf_keys[HF_KEYS_MAX];
static size_t hf_nkeys;
static int hf_keys_read;
static plat_mutex hf_keys_lock = PLAT_MUTEX_INIT;   /* the warm (a worker) and main may both be first */

typedef struct {
    host_facts_env e;
    char root[1100], rbs[1100], apps[1100];
} hf_env_buf;

/* the start config's paths as host_facts reads them; valid while c is */
static void hf_env(core *c, hf_env_buf *b) {
    plat_mutex_lock(&hf_keys_lock);
    if (!hf_keys_read) {   /* once */
        hf_keys_read = 1;
        for (int i = 0; i < native_n_key_texts && hf_nkeys < HF_KEYS_MAX; i++)
            if (!sig_key_from_text(&native_key_texts[i], &hf_keys[hf_nkeys])) hf_nkeys++;
    }
    plat_mutex_unlock(&hf_keys_lock);
    const char *pf = native_core_programs(c), *app = native_core_path(c, "app");
    int have_pf = pf && wr_program_path(pf, WR_PF_ROOT, b->root, sizeof b->root) >= 0
               && wr_program_path(pf, WR_PF_REKORDBOX, b->rbs, sizeof b->rbs) >= 0;
    host_facts_env *e = &b->e;
    memset(e, 0, sizeof *e);
    e->os = compat_os_windows();
    e->programs_rekordbox = have_pf ? b->rbs : NULL;
    e->root = have_pf ? b->root : NULL;
    e->model = native_core_path(c, "model");
    e->support = native_core_path(c, "support");
    e->helper = native_core_path(c, "helper");
    e->own = native_core_version(c);
    e->keys = hf_keys;
    e->nkeys = hf_nkeys;
    e->admin = native_user_is_admin();
    e->own_bundle = -1;
    if (app && e->own && e->support && snprintf(b->apps, sizeof b->apps, "%s", app) < (int)sizeof b->apps) {
        char *cut = strrchr(b->apps, '\\');   /* the app's own folder is <apps>\<version> */
        if (cut && cut != b->apps) {
            *cut = 0;
            update_env ue;
            memset(&ue, 0, sizeof ue);
            ue.support = e->support;
            ue.apps = b->apps;
            ue.own = e->own;
            ue.os = update_os_windows();
            e->own_bundle = update_in_place(&ue) == 0;
        }
    }
}

/* a file the warm didn't leave memoised, read on the UI thread: said in the log (once per file
 * and stat: compat.c remembers the answer) */
static void hf_miss(void *ctx, const char *path) {
    char line[1200];
    snprintf(line, sizeof line, "facts: %s read on the UI thread (not warm)", path);
    native_core_log((core *)ctx, line);
}

CORE_API int32_t CORE_CALL core_win_cheap_facts(core *c, core_facts *out) {
    if (!c || !out || !(core_facts_parts(out) & CORE_FACTS_REFRESH)) return -1;
    /* fail closed until the read answers (host_facts_write writes it again): out of memory leaves
     * the other rows unset (core.h's "on failure"), but Stems Cache's traces assumed (HR2) */
    core_fact_flag(out, FACT_CACHE_TRACES, 1);
    hf_env_buf *b = malloc(sizeof *b);
    if (!b) {
        native_core_log(c, "facts: out of memory: the cheap facts are unset, Stems Cache's traces assumed");
        return 0;
    }
    hf_env(c, b);
    b->e.on_main = 1;
    b->e.miss = hf_miss;
    b->e.miss_ctx = c;
    host_facts f;
    int unchecked = 0;
    if (!host_facts_read(&b->e, &f)) unchecked = host_facts_write(&f, out);
    else native_core_log(c, "facts: out of memory reading the cheap facts: the rows read so far are unset, Stems Cache's traces assumed");
    host_facts_free(&f);
    free(b);
    return unchecked;
}

CORE_API void CORE_CALL core_win_warm_facts(core *c) {
    if (!c) return;
    hf_env_buf *b = malloc(sizeof *b);
    if (!b) return;
    hf_env(c, b);
    host_facts f;
    host_facts_read(&b->e, &f);   /* compat.c's memo keeps the files' facts; the rows go */
    host_facts_free(&f);
    free(b);
}
#endif
