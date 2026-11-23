/* volumes.h's pure rules and the size walk over common/plat.h (plat_win.c on Windows, plat_mac.c
 * in the Mac tests). */
#include "volumes.h"

#include "plat.h"
#include "win_rules.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ text helpers */

static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }

/* whether s contains w (upper-case ASCII), case ignored */
static int has_ci(const char *s, const char *w) {
    size_t n = strlen(w);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && up(s[i]) == w[i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

static int prefix_ci(const char *s, const char *p) {
    for (; *p; s++, p++)
        if (up(*s) != up(*p)) return 0;
    return 1;
}

static int is_drive_root(const char *m) {
    return ((m[0] >= 'A' && m[0] <= 'Z') || (m[0] >= 'a' && m[0] <= 'z')) && m[1] == ':' && m[2] == '\\' && m[3] == 0;
}

static int copy(char *out, size_t n, const char *a, size_t la, const char *b) {
    size_t lb = strlen(b);
    if (la + lb + 1 > n) return -1;
    memcpy(out, a, la);
    memcpy(out + la, b, lb + 1);
    return 0;
}

/* ------------------------------------------------------------------ the external rule */

int vol_tunnel_text(const char *s) {
    return s && (has_ci(s, "THUNDERBOLT") || has_ci(s, "USB4") || has_ci(s, "CC_0C0340"));
}

/* a USB node: the USB bus driver's children ("USB"), the mass-storage disks ("USBSTOR") and the
 * UAS ones ("UASPSTOR") */
static int usb_enumerator(const char *e) { return prefix_ci(e, "USB") || prefix_ci(e, "UASPSTOR"); }

int vol_external(const vol_facts *f) {
    if (!f || f->system || f->drive_type == VOL_DRIVE_REMOTE || f->disks != 1) return 0;
    if (f->bus == VOL_BUS_USB || f->bus == VOL_BUS_1394 || f->bus == VOL_BUS_SD || f->bus == VOL_BUS_MMC) return 1;
    /* I89: the disk or an ancestor removable or hot-pluggable; J28: from that node up, a USB node
     * or a Thunderbolt/USB4 one. A hot-plug SATA port's disk has neither above it. */
    int n = f->nodes < 0 ? 0 : f->nodes > VOL_NODES_MAX ? VOL_NODES_MAX : f->nodes, start = -1;
    for (int i = 0; i < n && start < 0; i++)
        if (f->node[i].removable || (i == 0 && f->hotplug == 1)) start = i;
    if (start < 0) return 0;
    for (int i = start; i < n; i++)
        if (usb_enumerator(f->node[i].enumerator) || f->node[i].tunnel) return 1;
    return 0;
}

int vol_fs_allowed(const char *fstype) {
    return fstype && (!strcmp(fstype, "ntfs") || !strcmp(fstype, "exfat") || !strcmp(fstype, "fat32"));
}

int vol_refusal(const vol_facts *f) {
    if (!f) return VOL_REFUSE_UNREADABLE;
    if (f->drive_type == VOL_DRIVE_REMOTE) return VOL_REFUSE_NETWORK;
    if (f->drive_type == VOL_DRIVE_UNKNOWN || f->drive_type == VOL_DRIVE_NO_ROOT) return VOL_REFUSE_UNREADABLE;
    if (f->system || !vol_external(f)) return VOL_REFUSE_INTERNAL;
    if (!vol_fs_allowed(f->fstype)) return VOL_REFUSE_FILE_SYSTEM;
    if (f->readonly) return VOL_REFUSE_READ_ONLY;
    return VOL_OK;
}

int vol_location_refusal(const vol_facts *f) {
    int refusal = vol_refusal(f);
#ifdef VOL_TEST_INTERNAL_OK
    /* Supplied by the test executable and set before any location worker starts. */
    extern int vol_test_internal_ok;
    if (vol_test_internal_ok && refusal == VOL_REFUSE_INTERNAL && vol_fs_allowed(f->fstype))
        return f->readonly ? VOL_REFUSE_READ_ONLY : VOL_OK;
#endif
    return refusal;
}

void vol_facts_line(const vol_facts *f, char *out, size_t n) {
    static const char *const drives[] = {"unknown", "no-root", "removable", "fixed", "remote", "cdrom", "ramdisk"};
    if (!n) return;
    out[0] = 0;
    if (!f) {
        snprintf(out, n, "volume: unreadable");
        return;
    }
    const char *dt = f->drive_type >= 0 && f->drive_type <= 6 ? drives[f->drive_type] : "?";
    size_t k = 0;
    int r = snprintf(out, n, "volume: fs=%s drive=%s system=%d readonly=%d disks=%d bus=%d hotplug=%d nodes=", f->fstype, dt,
                     f->system, f->readonly, f->disks, f->bus, f->hotplug);
    if (r < 0 || (size_t)r >= n) return;
    k = (size_t)r;
    int nodes = f->nodes < 0 ? 0 : f->nodes > VOL_NODES_MAX ? VOL_NODES_MAX : f->nodes;
    for (int i = 0; i < nodes; i++) {
        const vol_node *v = &f->node[i];
        r = snprintf(out + k, n - k, "%s%s%s%s", i ? ">" : "", v->enumerator[0] ? v->enumerator : "?", v->removable ? "(r)" : "",
                     v->tunnel ? "(tb)" : "");
        if (r < 0 || (size_t)r >= n - k) return;
        k += (size_t)r;
    }
    snprintf(out + k, n - k, "%s external=%d refusal=%d", nodes ? "" : "-", vol_external(f), vol_refusal(f));
}

/* ------------------------------------------------------------------ GUID paths */

size_t vol_guid_root(const char *p) {
    const char *rest;
    return p && wr_path_kind(p, &rest) == 2 ? (size_t)(rest - p) : 0;
}

int vol_pick_mount(const char *mounts, size_t len, char *out, size_t n) {
    const char *first = NULL;
    for (size_t i = 0; i < len && mounts[i];) {
        const char *m = mounts + i;
        size_t l = 0;
        while (l < len - i && m[l]) l++;
        if (l == len - i) break;                            /* not terminated: ignore the rest */
        if (is_drive_root(m)) return copy(out, n, m, l, "");
        if (!first) first = m;
        i += l + 1;
    }
    return first ? copy(out, n, first, strlen(first), "") : -1;
}

int vol_join_mount(const char *guid_path, const char *mount, char *out, size_t n) {
    size_t g = vol_guid_root(guid_path), m = mount ? strlen(mount) : 0;
    if (!g || !m || mount[m - 1] != '\\') return -1;
    return copy(out, n, mount, m, guid_path + g);
}

const char *vol_uninstall_path(const char *dir, int (*display)(const char *, char *, size_t), char *out, size_t n) {
    if (vol_guid_root(dir) && !display(dir, out, n)) return out;
    return dir;
}

int vol_path_remote(const char *p) {
    if (!p) return 0;
    if (!strncmp(p, "\\\\?\\", 4)) return prefix_ci(p + 4, "UNC\\");
    return (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/') && p[2] && p[2] != '?' && p[2] != '.'
           && p[2] != '\\' && p[2] != '/';
}

void vol_remote_facts(vol_facts *out) {
    memset(out, 0, sizeof *out);
    out->drive_type = VOL_DRIVE_REMOTE;
    out->disks = out->bus = out->hotplug = -1;
}

int vol_plain_dos(const char *p, char *out, size_t n) {
    const char *rest;
    int k = wr_path_kind(p, &rest);
    if (k == 1) return copy(out, n, p, 0, p);
    if (k == 3) return copy(out, n, p + 4, 0, p + 4);
    return -1;
}

/* ------------------------------------------------------------------ device events */

/* s as a JSON string's contents into out at *k; -1 when it doesn't fit */
static int json_str(char *out, size_t n, size_t *k, const char *s) {
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        char esc[8];
        size_t l;
        if (*c == '"' || *c == '\\') { esc[0] = '\\'; esc[1] = (char)*c; l = 2; }
        else if (*c < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*c); l = 6; }
        else { esc[0] = (char)*c; l = 1; }
        if (*k + l >= n) return -1;
        memcpy(out + *k, esc, l);
        *k += l;
    }
    return 0;
}

static int event(char *out, size_t n, size_t *k, const char *ev, const char *path) {
    static const char a[] = "{\"ev\":\"", b[] = "\",\"path\":\"", c[] = "\"}\n";
    size_t e = strlen(ev);
    if (*k + sizeof a - 1 + e + sizeof b - 1 >= n) return -1;
    memcpy(out + *k, a, sizeof a - 1);
    *k += sizeof a - 1;
    memcpy(out + *k, ev, e);
    *k += e;
    memcpy(out + *k, b, sizeof b - 1);
    *k += sizeof b - 1;
    if (json_str(out, n, k, path) || *k + sizeof c - 1 >= n) return -1;
    memcpy(out + *k, c, sizeof c);
    *k += sizeof c - 1;
    return 0;
}

int vol_device_events(uint32_t ev, uint32_t devtype, uint32_t unitmask, const char *iface, char *out, size_t n) {
    const char *name = ev == VOL_DBT_ARRIVAL ? "mount" : ev == VOL_DBT_REMOVE_COMPLETE ? "unmount" : NULL;
    size_t k = 0;
    int count = 0;
    if (n) out[0] = 0;
    if (!name) return 0;
    if (devtype == VOL_DBT_VOLUME) {
        for (int i = 0; i < 26; i++) {
            if (!(unitmask & (1u << i))) continue;
            char root[4] = {(char)('A' + i), ':', '\\', 0};
            if (event(out, n, &k, name, root)) goto full;
            count++;
        }
        return count;
    }
    if (devtype == VOL_DBT_INTERFACE && iface && iface[0]) {
        if (event(out, n, &k, name, iface)) goto full;
        return 1;
    }
    return 0;
full:
    if (n) out[0] = 0;
    return -1;
}

/* ------------------------------------------------------------------ size on disk */

static int lower_hex(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

int vol_counts_in_entry_size(const char *name, const char *prefix) {
    const char *k = strncmp(name, "._", 2) ? name : name + 2;
    size_t p = strlen(prefix);
    if (strlen(k) != 69 || (strcmp(k + 64, ".flac") && strcmp(k + 64, ".desc"))) return 0;
    return lower_hex(k, 64) && p <= 64 && !strncmp(k, prefix, p);
}

typedef struct {
    plat_dir dir;
    const char *prefix;
    int64_t total;
} walk;

static int each_file(void *ctx, const char *name, const plat_stat *st) {
    walk *w = ctx;
    if (st->kind == PLAT_FILE && vol_counts_in_entry_size(name, w->prefix)) w->total += st->alloc;
    return 0;
}

static int walk_subdir(walk *w, const char *name, const char *prefix, plat_each_fn each) {
    walk in = {{0}, prefix, 0};
    int e = plat_dir_open_at(w->dir, name, &in.dir);
    if (e) {
        errno = e;
        return -1;
    }
    int r = plat_dir_each(in.dir, each, &in);
    e = errno;
    plat_dir_close(in.dir);
    if (r) {
        errno = e;
        return -1;
    }
    w->total += in.total;
    return 0;
}

static int each_prefix(void *ctx, const char *name, const plat_stat *st) {
    if (st->kind != PLAT_DIR || strlen(name) != 2 || !lower_hex(name, 2)) return 0;
    return walk_subdir(ctx, name, name, each_file);
}

static int each_model(void *ctx, const char *name, const plat_stat *st) {
    if (st->kind != PLAT_DIR || strlen(name) != 64 || !lower_hex(name, 64)) return 0;
    return walk_subdir(ctx, name, NULL, each_prefix);
}

int64_t vol_entries_size(const char *folder) {
    plat_stat st;
    int e = plat_stat_path(folder, 0, &st);
    if (e == ENOENT) return 0;
    if (e || st.kind != PLAT_DIR) return -1;
    walk w = {{0}, NULL, 0};
    if (plat_dir_open(folder, &w.dir)) return -1;
    int r = plat_dir_each(w.dir, each_model, &w);
    plat_dir_close(w.dir);
    return r ? -1 : w.total;
}
