/* Pure scope and status rules over the Windows inventory and its assessments. */
#include "inventory.h"
#include "native_facts.h"

#include <stdio.h>
#include <string.h>

int inv_original_bound(const payload_manifest *m, const char *exe_product_version, const compat_file *original) {
    if (!m || !m->has_windows || !original || !original->readable || original->pin != COMPAT_PIN_OK || original->marker)
        return 0;
    const char *bound = payload_win_ort_for(m, exe_product_version);
    return bound && *bound && m->win_ort_code_sha256[0]
        && !strcmp(original->sha256, bound) && !strcmp(original->code_sha256, m->win_ort_code_sha256);
}

static int is_bridge(int live) {
    return live == COMPAT_LIVE_OURS || live == COMPAT_LIVE_OURS_UNLISTED;
}

static int target_row(int verb, int complete, const inv_install *r, const inv_assess_row *a) {
    if (r->trust == INV_STALE) return 0;
    int bridge = is_bridge(a->live);
    /* D3: an incomplete inventory authorizes only putting Pioneer's file back where our bridge
     * and its receipt are; the worker keeps the shared root then */
    if (verb == INV_VERB_UNINSTALL) return complete ? bridge || a->r1 > NF_R1_NOTHING : bridge && a->receipt_ok;
    if (!payload_is_sha256(r->exe.sha256)) return 0;
    if (verb == INV_VERB_RESTORE)
        return a->r1 > NF_R1_NOTHING || (bridge && (!a->listed || !a->original_bound || !a->receipt_ok));
    if (r->trust != INV_VERIFIED || !a->listed || !a->original_bound || (bridge && !a->receipt_ok)) return 0;
    if (verb == INV_VERB_REINSTALL) return 1;
    return a->live == COMPAT_LIVE_PIONEER || a->live == COMPAT_LIVE_OURS_UNLISTED
        || (a->live == COMPAT_LIVE_MISSING && a->saved_complete);
}

int inv_targets(int verb, const inv_list *l, const inv_assess_row *a, const payload_manifest *m, admin_target *out, int max) {
    int swap = verb == INV_VERB_INSTALL || verb == INV_VERB_REINSTALL;
    if (swap && !l->complete) return -1;
    /* For swap scopes the bridge allowlist used by the assessment contains
     * exactly the selected payload's hash. A receipt can also bind an older bridge. */
    if (swap && (!m || !m->has_windows || !payload_is_sha256(m->win_bridge.sha256))) return 0;
    int n = 0;
    for (int i = l->n - 1; i >= 0 && n < max; i--) {
        const inv_install *r = &l->it[i];
        if (!target_row(verb, l->complete, r, &a[i]) || (n && payload_version_cmp(out[n - 1].version, r->version) == 0)) continue;
        strcpy(out[n].version, r->version);
        snprintf(out[n++].exe16, sizeof out->exe16, "%.16s", payload_is_sha256(r->exe.sha256) ? r->exe.sha256 : "");
    }
    return n;
}

static int repair_why(int r1) {
    switch (r1) {
    case NF_R1_LIVE_MISSING: return WHY_R1_MISSING;
    case NF_R1_FOREIGN: return WHY_R1_FOREIGN;
    case NF_R1_ORT_MISSING: return WHY_R1_ORT_MISSING;
    default: return WHY_NONE;
    }
}

static void row_status(const inv_install *r, const inv_assess_row *a, const payload_manifest *m, inv_row_view *v) {
    int bridge = is_bridge(a->live);
    int why = repair_why(a->r1);
    if (r->trust == INV_DISK_ONLY || (bridge && (!a->listed || !a->original_bound))) {
        v->state = INV_ROW_REPAIR;
        v->why = WHY_DELISTED_BRIDGE;
    } else if (bridge && !a->receipt_ok) {
        v->state = INV_ROW_REPAIR;
        v->why = WHY_NOT_RECORDED;
    } else if (r->trust != INV_VERIFIED) {
        v->state = INV_ROW_UNVERIFIED;
        v->why = r->trust == INV_UNVERIFIED ? WHY_NOT_VERIFIED : WHY_NO_ANSWER;
    } else if (why != WHY_NONE) {
        v->state = INV_ROW_REPAIR;
        v->why = why;
    } else if (a->r1 < 0 || a->live == COMPAT_LIVE_UNREADABLE) {
        v->state = INV_ROW_UNVERIFIED;
        v->why = WHY_NO_ANSWER;
    } else if (a->live == COMPAT_LIVE_OURS) {
        v->state = INV_ROW_ON;
    } else if (a->listed && a->original_bound) {
        v->state = INV_ROW_READY;
    } else {
        v->state = INV_ROW_UNSUPPORTED;
        if (!a->listed) v->why = WHY_RB_UNLISTED;
        else if (a->live != COMPAT_LIVE_PIONEER) v->why = WHY_ORT_NOT_ORIGINAL;
        else if (m && m->win_ort_code_sha256[0] && !strcmp(a->original_code, m->win_ort_code_sha256))
            v->why = WHY_BUILD_UNLISTED;
        else v->why = WHY_ORT_UNLISTED;
    }
}

int inv_rows(const inv_list *l, const inv_assess_row *a, const payload_manifest *m, inv_row_view *out, int max) {
    int n = 0;
    for (int i = 0; i < l->n && n < max; i++) {
        const inv_install *r = &l->it[i];
        if (r->trust == INV_STALE || (r->trust == INV_DISK_ONLY && !is_bridge(a[i].live))) continue;
        inv_row_view *v = &out[n++];
        memset(v, 0, sizeof *v);
        strcpy(v->version, r->version);
        strcpy(v->name, r->name);
        strcpy(v->display_version, r->display_version);
        v->r1 = a[i].r1;
        v->running = a[i].running != 0;
        row_status(r, &a[i], m, v);
    }
    return n;
}

const char *inv_outdated_bridge(const inv_list *l, const inv_assess_row *a, const char *latest) {
    if (!l->complete || !payload_is_sha256(latest)) return "";
    for (int i = 0; i < l->n; i++) {
        const inv_assess_row *r = &a[i];
        if (l->it[i].trust == INV_VERIFIED && r->listed && r->original_bound && r->receipt_ok && r->r1 == NF_R1_NOTHING
            && is_bridge(r->live) && payload_is_sha256(r->live_sha) && strcmp(r->live_sha, latest)) return r->live_sha;
    }
    return "";
}

void inv_state_text(const inv_row_view *rows, int count, char *out, size_t cap) {
    static const char *const names[] = {"On", "Ready to install", "Unsupported", "Needs repair", "Couldn't verify"};
    if (!cap) return;
    snprintf(out, cap, "%s", count ? "" : "none");
    for (int i = 0; i < count; i++) {
        int n = snprintf(out, cap, "%s%s %s%s", i ? ", " : "", rows[i].version, names[rows[i].state],
                         rows[i].running ? " (Running)" : "");
        if (n < 0 || (size_t)n >= cap) return;
        out += n;
        cap -= (size_t)n;
    }
}

static void sha_text(plat_sha256_ctx *hash, const char *s) {
    plat_sha256_update(hash, s, strlen(s) + 1);
}

int inv_list_digest(const payload_manifest *m, unsigned char out[32]) {
    plat_sha256_ctx hash;
    if (plat_sha256_init(&hash)) return -1;
    if (m) {
        for (int i = 0; i < m->n_win_rekordbox; i++) sha_text(&hash, m->win_rekordbox[i]);
        sha_text(&hash, "");
        for (int i = 0; i < m->n_win_ort_bind; i++) {
            sha_text(&hash, m->win_ort_bind[i].rekordbox);
            sha_text(&hash, m->win_ort_bind[i].sha256);
        }
        sha_text(&hash, "");
        for (int i = 0; i < m->n_win_ort_sha256; i++) sha_text(&hash, m->win_ort_sha256[i]);
        sha_text(&hash, m->win_ort_code_sha256);
    }
    return plat_sha256_final(&hash, out);
}

static int has_refresh_key(const char *seen, const char *key) {
    for (const char *p = seen; (p = strstr(p, key)); p++)
        if (p == seen || p[-1] == '\n') return 1;
    return 0;
}

size_t inv_refresh_keys(const inv_row_view *rows, int count, const payload_manifest *m, const char *seen,
                        char out[INV_REFRESH_KEYS_MAX]) {
    out[0] = 0;
    int on = 0, unsupported = 0;
    for (int i = 0; i < count; i++) {
        on |= rows[i].state == INV_ROW_ON;
        unsupported |= rows[i].state == INV_ROW_UNSUPPORTED;
    }
    unsigned char digest[32];
    if (!m || !on || !unsupported || inv_list_digest(m, digest)) return 0;
    char list[65];
    for (int i = 0; i < 32; i++) snprintf(list + 2 * i, 3, "%02x", (unsigned)digest[i]);
    size_t n = 0;
    for (int i = 0; i < count; i++) {
        if (rows[i].state != INV_ROW_UNSUPPORTED) continue;
        char key[65 + COMPAT_VERSION_MAX + 1];
        int k = snprintf(key, sizeof key, "%s:%s\n", list, rows[i].version);
        if (k <= 0 || (size_t)k >= sizeof key || has_refresh_key(seen, key) || has_refresh_key(out, key)) continue;
        if ((size_t)k >= INV_REFRESH_KEYS_MAX - n) break;
        memcpy(out + n, key, (size_t)k + 1);
        n += (size_t)k;
    }
    return n;
}
