/* Texts (mac-core §3.9): static constant tables, one column per OS, and the pure formatter. */
#include "internal.h"

static const char *const mac_texts[CORE_TEXT_COUNT > 0 ? CORE_TEXT_COUNT : 1] = {
#define TEXT(id, os, mac, win) CORE_KEEP_##os([id] = mac,)
#include "texts.def"
};

#ifdef CORE_WIN_POLICY
static const char *const win_texts[CORE_TEXT_COUNT > 0 ? CORE_TEXT_COUNT : 1] = {
#define TEXT(id, os, mac, win) CORE_KEEP_##os([id] = win,)
#include "texts.def"
};
#endif

const char *const core_slot_names[CORE_SLOT_COUNT > 0 ? CORE_SLOT_COUNT : 1] = {
#define SLOT(name, os) CORE_KEEP_##os(#name,)
#include "texts.def"
};

const char *core_text_template(core_os os, int32_t id) {
    if (id < 0 || id >= CORE_TEXT_COUNT) return NULL;
    if (os == CORE_OS_MAC) return mac_texts[id];
#ifdef CORE_WIN_POLICY
    if (os == CORE_OS_WIN) return win_texts[id];
#endif
    return NULL;
}

/* the slot named by p[0..n), or -1 */
static int slot_named(const char *p, size_t n) {
    for (int i = 0; i < CORE_SLOT_COUNT; i++)
        if (strlen(core_slot_names[i]) == n && !memcmp(core_slot_names[i], p, n)) return i;
    return -1;
}

int32_t core_text(core_os os, core_text_id id, const core_slot *slots, int32_t nslots, char *buf, size_t cap, size_t *len) {
    int32_t r = CORE_E_ARG;
    if (len) *len = 0;
    if (cap && buf) buf[0] = 0;
    const char *t = core_text_template(os, id);
    if (!t || nslots < 0 || nslots > CORE_SLOT_COUNT || (nslots && !slots) || (cap && !buf)) return CORE_E_ARG;
    if (!*t) return CORE_E_NOTYET;   /* a column not written yet (W-core writes the Windows one) */
    uint64_t given = 0, used = 0;
    for (int32_t i = 0; i < nslots; i++) {
        if (slots[i].reserved || slots[i].id < 0 || slots[i].id >= CORE_SLOT_COUNT || (given >> slots[i].id & 1)) return CORE_E_ARG;
        if (!slots[i].value && slots[i].len && slots[i].len != CORE_NTS) return CORE_E_ARG;
        given |= 1ull << slots[i].id;
    }
    size_t out = 0;
    for (const char *p = t; *p;) {
        const char *piece;
        size_t n;
        if (*p == '{') {
            const char *e = strchr(p, '}');
            int sid = e ? slot_named(p + 1, (size_t)(e - p - 1)) : -1;
            if (sid < 0 || !(given >> sid & 1)) goto fail;
            used |= 1ull << sid;
            const core_slot *sl = NULL;
            for (int32_t i = 0; i < nslots; i++)
                if (slots[i].id == sid) sl = &slots[i];
            piece = sl->value ? sl->value : "";
            n = sl->len == CORE_NTS ? strlen(piece) : sl->value ? sl->len : 0;
            p = e + 1;
        } else {
            const char *e = strchr(p, '{');
            n = e ? (size_t)(e - p) : strlen(p);
            piece = p;
            p += n;
        }
        if (cap && out < cap - 1) {   /* copy what fits; slot values are never read as templates */
            size_t room = cap - 1 - out;
            memcpy(buf + out, piece, n < room ? n : room);
        }
        if (n > SIZE_MAX - out - 1) goto fail;
        out += n;
    }
    if (used != given) goto fail;
    if (cap) buf[out < cap - 1 ? out : cap - 1] = 0;
    if (len) *len = out;
    return out < cap ? CORE_OK : CORE_E_SPACE;
fail:   /* an error after pieces were copied: the buffer holds "", as the header promises */
    if (cap) buf[0] = 0;
    if (len) *len = 0;
    return r;
}

/* every text, in every column this build has: each "{" opens a known slot that a "}" closes */
static int check_one(const char *t) {
    if (!t) return 0;
    for (const char *p = t; *p; p++) {
        if (*p == '}') return -1;
        if (*p != '{') continue;
        const char *e = strchr(p, '}');
        if (!e || slot_named(p + 1, (size_t)(e - p - 1)) < 0) return -1;
        p = e;
    }
    return utf8_valid(t, strlen(t)) ? 0 : -1;
}

#ifdef CORE_WIN_POLICY
core_text_id nameless_text(core_text_id named) {
    switch (named) {
    case T_LOC_CHECKING_DRIVE: return T_WIN_LOC_CHECKING_NAMELESS;
    case T_LOC_AGAIN_SOON_DRIVE: return T_WIN_LOC_AGAIN_SOON_NAMELESS;
    case T_LOC_NOT_USING_DRIVE: return T_WIN_LOC_NOT_USING_NAMELESS;
    case T_LOC_BRIDGE_TOO_OLD: return T_WIN_LOC_BRIDGE_TOO_OLD_NAMELESS;
    case T_LOC_NOT_CONNECTED: return T_WIN_LOC_NOT_CONNECTED_NAMELESS;
    case T_LOC_FOLDER_MISSING: return T_WIN_LOC_FOLDER_MISSING_NAMELESS;
    case T_LOC_LINK: return T_WIN_LOC_LINK_NAMELESS;
    case T_LOC_NETWORK: return T_WIN_LOC_NETWORK_NAMELESS;
    case T_LOC_READ_ONLY: return T_WIN_LOC_READ_ONLY_NAMELESS;
    case T_LOC_NO_PERMISSION: return T_WIN_LOC_NO_PERMISSION_NAMELESS;
    case T_CACHE_PAUSED: return T_WIN_CACHE_PAUSED_NAMELESS;
    case T_CACHE_PAUSED_AT_INSTALL: return T_WIN_CACHE_PAUSED_AT_INSTALL_NAMELESS;
    case T_LEFT_DRIVE_NOT_CONNECTED: return T_WIN_LEFT_DRIVE_NOT_CONNECTED_NAMELESS;
    case T_REMOVAL_NOTE_NOT_CONNECTED: return T_WIN_REMOVAL_NOTE_NOT_CONNECTED_NAMELESS;
    case T_UNINSTALL_ALL_DRIVE_GONE: return T_WIN_UNINSTALL_ALL_DRIVE_GONE_NAMELESS;
    case T_OLD_DRIVE_STAYS: return T_WIN_OLD_DRIVE_STAYS_NAMELESS;
    case T_UNINSTALL_ALL_DRIVE: return T_WIN_UNINSTALL_ALL_DRIVE_NAMELESS;
    case T_REMOVAL_NOTE_CONNECTED: return T_WIN_REMOVAL_NOTE_CONNECTED_NAMELESS;
    case T_DELETING_DRIVE: return T_WIN_DELETING_DRIVE_NAMELESS;
    case T_DELETING_DRIVE_N: return T_WIN_DELETING_DRIVE_N_NAMELESS;
    case T_LEFT_DRIVE_PARTLY: return T_WIN_LEFT_DRIVE_PARTLY_NAMELESS;
    case T_LEFT_DRIVE_NOT_NAMED: return T_WIN_LEFT_DRIVE_NOT_NAMED_NAMELESS;
    case T_CLEAR_LOCATION_MSG: return T_WIN_CLEAR_LOCATION_MSG_NAMELESS;
    case T_CLEARING_DRIVE: return T_WIN_CLEARING_DRIVE_NAMELESS;
    case T_CLEARING_DRIVE_N: return T_WIN_CLEARING_DRIVE_N_NAMELESS;
    case T_NOT_CLEARED_DRIVE_MSG: return T_WIN_NOT_CLEARED_DRIVE_MSG_NAMELESS;
    case T_LOCATION_LABEL: return T_WIN_LOCATION_LABEL_NAMELESS;
    default: return named;
    }
}

int location_refuse_nameless(core_os os, const char *drive) { return os == CORE_OS_WIN && (!drive || !drive[0]); }

#endif
int core_text_check_all(void) {
    for (int i = 0; i < CORE_TEXT_COUNT; i++) {
        if (!mac_texts[i] || check_one(mac_texts[i])) return i + 1;
#ifdef CORE_WIN_POLICY
        if (!win_texts[i] || check_one(win_texts[i])) return i + 1;
#endif
    }
    return 0;
}

/* ---------------------------------------------------------------- the text builder (pure.h) */

void tb_init(tbuf *b, core_os os, char *p, size_t cap) {
    b->p = p;
    b->cap = p ? cap : 0;
    b->len = 0;
    b->err = 0;
    b->os = os;
    if (b->cap) p[0] = 0;
}

void tb_put(tbuf *b, const char *s, size_t n) {
    if (!n) return;
    if (b->len < b->cap) {
        size_t room = b->cap - 1 - b->len;
        memcpy(b->p + b->len, s, n < room ? n : room);
    }
    if (n > SIZE_MAX - b->len - 1) { b->err = CORE_E_SPACE; return; }
    b->len += n;
    if (b->len >= b->cap) {
        if (!b->err) b->err = CORE_E_SPACE;
        if (b->cap) b->p[b->cap - 1] = 0;
    } else b->p[b->len] = 0;
}

void tb_puts(tbuf *b, const char *s) { tb_put(b, s, strlen(s)); }

void tb_i64(tbuf *b, int64_t v) {
    char d[24], o[24];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do { d[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) d[n++] = '-';
    for (int i = 0; i < n; i++) o[i] = d[n - 1 - i];
    tb_put(b, o, (size_t)n);
}

size_t tb_len(const tbuf *t) { return t->err ? 0 : t->len < t->cap ? t->len : t->cap ? t->cap - 1 : 0; }

void tb_text(tbuf *b, core_text_id id, const core_slot *slots, int32_t nslots) {
    if (b->err) return;   /* composition stops at the first error: the result is dropped */
    size_t full = 0, room = b->len < b->cap ? b->cap - b->len : 0;
    int32_t r = core_text(b->os, id, slots, nslots, room ? b->p + b->len : NULL, room, &full);
    if (r == CORE_OK) { b->len += full; return; }
    if (r != CORE_E_SPACE) {   /* a bad slot, or a column not written yet: the result is dropped */
        b->err = r;
        if (room) b->p[b->len] = 0;
        return;
    }
    if (full > SIZE_MAX - b->len - 1) { b->err = CORE_E_SPACE; return; }
    b->len += full;
    if (!b->err) b->err = CORE_E_SPACE;
}
