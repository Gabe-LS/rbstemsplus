/* A chosen location's checks (location.h), the bridge's check_location and gone_why over plat.h. */
#include "location.h"

#include "marker.h"
#include "path_rules.h"
#if defined(_WIN32)
#include "plat_win.h"
#endif

#include <errno.h>
#include <string.h>

COMMON_API const char *location_why(int e) {
    return e == ENOENT || e == ENOTDIR || e == ELOOP || e == EXDEV ? "folder"
         : e == EACCES || e == EPERM ? "no-permission"
         : e == EROFS ? "read-only" : "io-error";
}

/* the same volume: the same bytes, a Windows GUID path's hex digits in either case */
static int loc_same_mount(const char *a, const char *b, int win) {
    if (!win) return !strcmp(a, b);
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

COMMON_API const char *location_check(const char *dir, const char *want, int any, location_pin *pin) {
    int win = dir[0] != '/', system = 0, e;
    char sep = win ? '\\' : '/', p[PATHS_LOCATION_MAX + 1];
    size_t n = strlen(dir), root = win ? paths_root_win(dir) : 1;
    pin->mount[0] = 0;
    if (!root) return "not-connected";                      /* not absolute: on no volume */
    if (n > PATHS_LOCATION_MAX) return "folder";
    memcpy(p, dir, n + 1);
    /* the volume: the one holding the parent folder, so never dir itself */
    char *last = strrchr(p, sep);
    if (!last || (size_t)(last - p) + 1 < root) return "not-connected";
    if ((size_t)(last - p) + 1 == root) last[1] = 0;        /* the parent is the root ("/", "X:\", the GUID path) */
    else *last = 0;
    if ((e = plat_volume_holding(p, pin->mount, sizeof pin->mount, &system))) {
#if defined(_WIN32)
        if (e == ENOENT && plat_win_drive_remote(dir)) return "not-local";   /* the table lists no network drive */
#endif
        return e == ENOENT ? "not-connected" : "io-error";
    }
    if (system && !any) return "not-connected";
    plat_stat st;
    /* its root unreadable: gone ("a failure means not-connected", as location_gone answers for the
     * same state), unless it is a permission refusal on a volume still mounted (review TC4) */
    if ((e = plat_stat_path(pin->mount, 0, &st)))
        return e == ENOENT || e == ENOTDIR || (e != EACCES && e != EPERM && !plat_volume_mounted(pin->mount)) ? "not-connected"
                                                                                                           : location_why(e);
    uint64_t vol = st.id.vol;
    /* from there down, every component a real folder on that volume (no link followed) */
    size_t start = win ? root : (strcmp(pin->mount, "/") ? strlen(pin->mount) : 0) + 1;
    memcpy(p, dir, n + 1);
    if (start > n) return "folder";
    for (char *s = p + start;;) {
        char *c = strchr(s, sep);
        if (c) *c = 0;
        if ((e = plat_stat_path(p, 0, &st))) return e == ENOENT ? "folder" : location_why(e);
        if (st.kind != PLAT_DIR || (!any && st.id.vol != vol)) return "folder";
        if (!c) break;
        *c = sep;
        s = c + 1;
    }
    /* the volume as the folder sees it (no link on the way now): the same one, local, writable */
    plat_volume v;
    if ((e = plat_volume_info(dir, &v))) return location_why(e);
    if (!any && !loc_same_mount(v.mount, pin->mount, win)) return "folder";
    if (!v.local) return "not-local";
    if (v.readonly) return "read-only";
    plat_dir d;
    if ((e = plat_dir_open(dir, &d))) return location_why(e);
    /* the folder opened is still on that volume: an ancestor swapped after the walk can't move it
     * to another one (review TC1) */
    const char *why = plat_dir_stat(d, &st) ? "io-error" : !any && st.id.vol != vol ? "folder" : marker_check_at(d, want);
    if (why && !*why) why = "io-error";                     /* checked once: a shortage is a failure here */
    pin->id = st.id;
    if (!why && (e = plat_dir_writable(d))) why = e == EROFS ? "read-only" : "no-permission";
    plat_dir_close(d);
    return why;
}

COMMON_API const char *location_gone(const char *mount, int any, int e) {
    if (!any && !plat_volume_mounted(mount)) return "not-connected";
    return location_why(e);
}
