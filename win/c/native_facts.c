/* The core DLL's slow facts beside the sha256s (native_facts.h). C11 over common/plat.h. */
#include "native_facts.h"

#include "update.h"
#include "volumes.h"

#include <stdlib.h>
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

int native_listed(const char *v, const char (*list)[PAYLOAD_NAME_MAX], int n) {
    if (!v || !v[0]) return 0;
    for (int i = 0; i < n; i++) {
        size_t k = strlen(list[i]);
        if (k && !strncmp(v, list[i], k) && (v[k] == 0 || v[k] == '.')) return 1;
    }
    return 0;
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
    out->rekordbox_supported = m && native_listed(rb_version, m->win_rekordbox, m->n_win_rekordbox);
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
