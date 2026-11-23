/* The core DLL's slow facts beside the sha256s (native_facts.h). C11 over common/plat.h. */
#include "native_facts.h"
#include "receipt.h"
#include "pe_code.h"

#include "update.h"
#include "volumes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int native_fact_read(const compat_os *os, int kind, const char *key, native_fact *out) {
    memset(out, 0, sizeof *out);
    if (!key || !key[0]) return -1;
    switch (kind) {
    case NF_SLOW_DRIVE: {
        plat_stat st;
        plat_volume v;
        if (plat_stat_path(key, 1, &st) || plat_volume_info(key, &v)) return -1;   /* a location not there: as the Mac's freeBytes */
        out->has_free = 1;
        out->free_bytes = v.avail > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)v.avail;
        return 0;
    }
    case NF_SLOW_SIZE_WALK: {
        int64_t n = vol_entries_size(key);
        if (n < 0) return -1;
        out->has_size = 1;
        out->size_bytes = n;
        return 0;
    }
    case NF_SLOW_ALPHATHETA: {
        compat_file f;
        if (compat_file_facts(os, key, COMPAT_PIN, &f)) return -1;
        if (f.present && f.exists && f.pin == COMPAT_PIN_ERROR) return -1;
        out->has_signature = 1;
        out->signature_ok = f.present && f.exists && f.pin == COMPAT_PIN_OK;
        return 0;
    }
    default: return -1;
    }
}

int native_r1(const compat_os *os, const char *folder, const char *root, const payload_manifest *saved, const payload_manifest *installed) {
    if (saved && !saved->has_windows) saved = NULL;
    if (installed && !installed->has_windows) installed = NULL;
    payload_sha bridges[2];
    int nb = 0;
    if (saved) memcpy(bridges[nb++], saved->win_bridge.sha256, sizeof bridges[0]);
    if (installed) memcpy(bridges[nb++], installed->win_bridge.sha256, sizeof bridges[0]);
    compat_r1 r;
    compat_r1_facts(os, folder, root, bridges, nb, saved ? saved : installed, &r);
    return native_r1_state(&r, nb > 0);
}

int native_listed_exact(const char *v, const char (*list)[PAYLOAD_NAME_MAX], int n) {
    if (!v || !v[0]) return 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(v, list[i])) return 1;
    return 0;
}

/* the list: the saved payload's, else the record's (NULL: none) */
static const payload_manifest *list_of(const payload_manifest *saved, const payload_manifest *installed) {
    if (saved && saved->has_windows) return saved;
    if (installed && installed->has_windows) return installed;
    return NULL;
}

void native_blocked_plus(const char *model, const payload_manifest *m, char *engine, size_t n, int *supported) {
    engine[0] = 0;
    *supported = 0;
    char p[1100];
    const char *sep = model ? strrchr(model, '\\') : NULL;
    const char *sep2 = model ? strrchr(model, '/') : NULL;
    if (sep2 > sep) sep = sep2;
    if (!sep || snprintf(p, sizeof p, "%.*s%cdemucs3_ver.txt", (int)(sep - model), model, *sep) >= (int)sizeof p) return;
    char *b = NULL;
    size_t bn = 0;
    if (plat_read_file(p, 4096, &b, &bn)) return;
    size_t a = 0;
    while (a < bn && (b[a] == ' ' || b[a] == '\t' || b[a] == '\r' || b[a] == '\n')) a++;
    while (bn > a && (b[bn - 1] == ' ' || b[bn - 1] == '\t' || b[bn - 1] == '\r' || b[bn - 1] == '\n')) bn--;
    snprintf(engine, n, "%.*s", (int)(bn - a), b + a);
    free(b);
    *supported = m && m->has_windows && native_listed_exact(engine, m->win_stems_engine, m->n_win_stems_engine);
}

int native_blocked_cache(const compat_os *os, const char *folder, const char *rb_version, const char *root, const payload_manifest *saved,
                         const payload_manifest *installed, const compat_r1 *r1, native_blocked *out) {
    memset(out, 0, sizeof *out);
    const payload_manifest *m = list_of(saved, installed);
    out->rekordbox_supported = payload_win_rekordbox_supported(m, rb_version);
    if (!out->rekordbox_supported || !folder || !root) return 0;
    char ort[1100];
    plat_stat st;
    if (snprintf(ort, sizeof ort, "%s%cort%connxruntime.dll", root, root[0] == '/' ? '/' : '\\', root[0] == '/' ? '/' : '\\') >= (int)sizeof ort) return -1;
    out->root_ort_exists = !plat_stat_path(ort, 0, &st) && st.kind == PLAT_FILE;
    out->has_root_ort = 1;
    compat_r1 r;
    if (r1) r = *r1;
    else {
        payload_sha bridges[2];
        int nb = 0;
        if (saved && saved->has_windows) memcpy(bridges[nb++], saved->win_bridge.sha256, sizeof bridges[0]);
        if (installed && installed->has_windows) memcpy(bridges[nb++], installed->win_bridge.sha256, sizeof bridges[0]);
        compat_r1_facts(os, folder, root, bridges, nb, m, &r);
    }
    char live[1100];
    const char *lib = NULL;
    switch (r.live) {
    case COMPAT_LIVE_MISSING: out->has_lib = 1; return 0;   /* no ORT */
    case COMPAT_LIVE_FOREIGN: out->has_lib = 1, out->lib_exists = 1; return 0;   /* not Pioneer's */
    case COMPAT_LIVE_PIONEER:
        if (snprintf(live, sizeof live, "%s%connxruntime.dll", folder, folder[0] == '/' ? '/' : '\\') >= (int)sizeof live) return -1;
        lib = live;
        break;
    case COMPAT_LIVE_OURS:
    case COMPAT_LIVE_OURS_UNLISTED:
        if (!r.ort_looked) return -1;   /* ort\'s copy couldn't be looked at */
        out->has_lib = 1;
        if (!r.ort_copy) return 0;      /* our bridge in, Pioneer's copy gone */
        lib = ort;
        break;
    default: return -1;                 /* unreadable: no answer */
    }
    out->has_lib = 1;
    out->lib_exists = 1;
    if (r.ort == COMPAT_ORT_NONE) return -1;   /* read, but the pin had no answer */
    out->lib_signed = r.ort != COMPAT_ORT_NOT_ORIGINAL;
    out->ort_listed = r.ort == COMPAT_ORT_OK;
    compat_file f;
    if (!compat_file_facts(os, lib, 0, &f) && f.exists) snprintf(out->ort_version, sizeof out->ort_version, "%s", f.pe.product_version);
    return 0;
}

int native_r1_state(const compat_r1 *r, int have_lists) {
    switch (r->live) {
    case COMPAT_LIVE_MISSING: return NF_R1_LIVE_MISSING;
    case COMPAT_LIVE_FOREIGN: return NF_R1_FOREIGN;
    case COMPAT_LIVE_OURS_UNLISTED:   /* unlisted against a verified list; without one, ours (watch.c) */
    case COMPAT_LIVE_OURS:
        if (r->live == COMPAT_LIVE_OURS_UNLISTED && have_lists) return NF_R1_FOREIGN;
        if (!r->ort_looked) return -1;
        return r->ort_copy ? NF_R1_NOTHING : NF_R1_ORT_MISSING;
    case COMPAT_LIVE_PIONEER: return NF_R1_NOTHING;
    default: return -1;   /* unreadable */
    }
}

int native_r1_read(const compat_os *os, const char *support, const char *own, const sig_pubkey *keys, size_t nkeys, const char *folder,
                   const char *root) {
    if (!support || !own) return -1;
    payload_manifest *saved = malloc(sizeof *saved), *inst = malloc(sizeof *inst);
    int r = -1;
    if (saved && inst) {
        update_env ue;
        memset(&ue, 0, sizeof ue);
        ue.support = support;
        ue.own = own;
        ue.keys = keys;
        ue.nkeys = nkeys;
        update_store st;
        update_store_init(&ue, &st);   /* support\payload under support's floor, the app's own minimum */
        int has_saved = payload_saved(&st.files, saved) == PAYLOAD_OK && saved->has_windows;
        int has_inst = update_installed(&ue, inst) == PAYLOAD_OK && inst->has_windows;
        r = native_r1(os, folder, root, has_saved ? saved : NULL, has_inst ? inst : NULL);
    }
    free(saved);
    free(inst);
    return r;
}

#if defined(_WIN32)
#define NF_SEP "\\"
#else
#define NF_SEP "/"
#endif

static int nf_path(char *out, size_t cap, const char *folder, const char *leaf) {
    int n = snprintf(out, cap, "%s" NF_SEP "%s", folder, leaf);
    return n >= 0 && (size_t)n < cap;
}

static int nf_read_live(const compat_os *os, const char *path, const payload_manifest *m,
                        const payload_manifest *installed, compat_file *file) {
    if (compat_file_facts(os, path, 0, file)) return COMPAT_LIVE_UNREADABLE;
    if (!file->present) return COMPAT_LIVE_MISSING;
    if (!file->exists) return COMPAT_LIVE_FOREIGN;
    if (!file->readable) return COMPAT_LIVE_UNREADABLE;
    if (m && !strcmp(file->sha256, m->win_bridge.sha256)) return COMPAT_LIVE_OURS;
    if (file->marker || (installed && !strcmp(file->sha256, installed->win_bridge.sha256))) return COMPAT_LIVE_OURS_UNLISTED;
    if (compat_ort_file_facts(os, path, COMPAT_PIN, pe_code_sha256, file) || !file->readable) return COMPAT_LIVE_UNREADABLE;
    if (m && !strcmp(file->sha256, m->win_bridge.sha256)) return COMPAT_LIVE_OURS;
    if (file->marker || (installed && !strcmp(file->sha256, installed->win_bridge.sha256))) return COMPAT_LIVE_OURS_UNLISTED;
    if (file->pin == COMPAT_PIN_OK) return COMPAT_LIVE_PIONEER;
    return file->pin == COMPAT_PIN_NO ? COMPAT_LIVE_FOREIGN : COMPAT_LIVE_UNREADABLE;
}

static int nf_saved(const compat_os *os, const char *root, const char *version, compat_file *file) {
    char parent[COMPAT_PATH_MAX], folder[COMPAT_PATH_MAX], path[COMPAT_PATH_MAX], sum[COMPAT_PATH_MAX];
    if (!root || !nf_path(parent, sizeof parent, root, "pioneer") || !nf_path(folder, sizeof folder, parent, version)
        || !nf_path(path, sizeof path, folder, "onnxruntime.dll") || !nf_path(sum, sizeof sum, folder, "SHA256SUMS")
        || compat_ort_file_facts(os, path, COMPAT_PIN, pe_code_sha256, file) || !file->readable) return 0;
    char expected[96];
    int n = snprintf(expected, sizeof expected, "%s  onnxruntime.dll\n", file->sha256);
    char *read = NULL;
    size_t size;
    if (plat_read_file(sum, sizeof expected, &read, &size)) return 0;
    int complete = n > 0 && (size_t)n == size && !memcmp(expected, read, size);
    free(read);
    return complete;
}

static int nf_runtime_ok(const compat_file *runtime, const payload_manifest *m) {
    if (!m || !runtime->readable || runtime->marker || runtime->pin != COMPAT_PIN_OK
        || !m->win_ort_code_sha256[0] || strcmp(runtime->code_sha256, m->win_ort_code_sha256)) return 0;
    for (int i = 0; i < m->n_win_ort_sha256; i++) {
        if (!strcmp(runtime->sha256, m->win_ort_sha256[i])) return 1;
    }
    return 0;
}

void native_inventory_assess(const compat_os *os, const inv_list *list, const char *root, const payload_manifest *m,
                             const payload_manifest *installed, inv_assess_row *out, native_inv_caps *caps) {
    compat_file runtime = {0};
    int looked_at_runtime = 0, runtime_ok = 0;
    for (int i = 0; i < list->n; i++) {
        const inv_install *r = &list->it[i];
        inv_assess_row *a = &out[i];
        native_inv_caps *c = &caps[i];
        *a = (inv_assess_row){.live = COMPAT_LIVE_MISSING};
        *c = (native_inv_caps){0};
        if (r->trust == INV_STALE) continue;
        char path[COMPAT_PATH_MAX];
        compat_file live = {0}, saved = {0};
        a->live = nf_path(path, sizeof path, r->folder, "onnxruntime.dll")
            ? nf_read_live(os, path, m, installed, &live) : COMPAT_LIVE_UNREADABLE;
        strcpy(a->live_sha, live.sha256);
        a->listed = payload_win_rekordbox_supported(m, r->exe.pe.product_version);
        int bridge = a->live == COMPAT_LIVE_OURS || a->live == COMPAT_LIVE_OURS_UNLISTED;
        c->bridge = bridge;
        c->trace = bridge || a->live == COMPAT_LIVE_UNREADABLE || (live.present && !live.exists);
        if (r->trust == INV_DISK_ONLY && !bridge) continue;

        if (a->live != COMPAT_LIVE_PIONEER && a->live != COMPAT_LIVE_UNREADABLE) {
            a->saved_complete = nf_saved(os, root, r->version, &saved);
            if (a->saved_complete) strcpy(a->saved_sha, saved.sha256);
        }
        const compat_file *original = a->live == COMPAT_LIVE_PIONEER ? &live : a->saved_complete ? &saved : NULL;
        if (original) {
            strcpy(a->original_sha, original->sha256);
            strcpy(a->original_code, original->code_sha256);
            a->original_bound = inv_original_bound(m, r->exe.pe.product_version, original);
            c->original_signed = original->pin == COMPAT_PIN_OK && !original->marker;
            snprintf(c->ort_version, sizeof c->ort_version, "%s", original->pe.product_version);
        }

        if (bridge && !looked_at_runtime) {
            looked_at_runtime = root && nf_path(path, sizeof path, root, "ort" NF_SEP "onnxruntime.dll")
                && !compat_ort_file_facts(os, path, COMPAT_PIN, pe_code_sha256, &runtime) ? 1 : -1;
            runtime_ok = looked_at_runtime > 0 && nf_runtime_ok(&runtime, m);
        }
        if (!bridge) {
            if (r->trust == INV_VERIFIED || r->trust == INV_UNVERIFIED) {
                if (a->live == COMPAT_LIVE_MISSING) a->r1 = NF_R1_LIVE_MISSING;
                if (a->live == COMPAT_LIVE_FOREIGN) a->r1 = NF_R1_FOREIGN;
                if (a->live == COMPAT_LIVE_UNREADABLE) a->r1 = -1;
            }
            continue;
        }

        receipt saved_receipt;
        a->receipt_ok = root && a->saved_complete && runtime.readable && !receipt_read(root, r->version, &saved_receipt)
            && receipt_matches(&saved_receipt, r, live.sha256, saved.sha256, runtime.code_sha256);
        if (looked_at_runtime < 0) a->r1 = -1;
        else if (!runtime.present) a->r1 = NF_R1_ORT_MISSING;
        else if (!runtime_ok) a->r1 = NF_R1_FOREIGN;
        if (a->r1 == NF_R1_NOTHING && (!a->receipt_ok || !a->listed || !a->original_bound)) a->r1 = NF_R1_FOREIGN;
        if (r->trust == INV_DISK_ONLY) a->r1 = NF_R1_NOTHING;  /* disk-only cleanup uses the uninstall sweep */
        c->live_line = live.has_cache_line;
        if (live.has_cache_line) {
            memcpy(c->cache_line, live.cache_line, live.cache_line_len + 1);
            c->cache_line_len = live.cache_line_len;
        }
    }
}

void native_inventory_running(const compat_os *os, const inv_list *list, inv_assess_row *out, char *versions, size_t cap) {
    versions[0] = 0;
    if (!os || !os->processes) return;
    compat_proc *procs = malloc(COMPAT_PROCS_MAX * sizeof *procs);
    if (!procs) return;
    int n = os->processes(os->ctx, procs, COMPAT_PROCS_MAX);
    int known = n >= 0 && n < COMPAT_PROCS_MAX;
    for (int p = 0; p < n && p < COMPAT_PROCS_MAX; p++) {
        compat_procs process;
        compat_procs_classify(&procs[p], 1, NULL, &process);
        if (!process.running) continue;
        int found = 0;
        if (procs[p].path[0]) {
            for (int i = 0; i < list->n; i++) {
                if (list->it[i].trust == INV_STALE) continue;
                compat_procs_classify(&procs[p], 1, list->it[i].folder, &process);
                if (!process.running_here) continue;
                out[i].running = 1;
                found = 1;
                break;
            }
        }
        known &= found;
    }
    free(procs);
    if (!known) return;
    size_t at = 0;
    for (int i = list->n - 1; i >= 0; i--) {
        if (!out[i].running) continue;
        int k = snprintf(versions + at, cap - at, "%s\n", list->it[i].version);
        if (k < 0 || (size_t)k >= cap - at) {
            versions[0] = 0;
            return;
        }
        at += (size_t)k;
    }
}
