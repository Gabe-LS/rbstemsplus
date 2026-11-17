/* volumes.h on Windows: the volume list, each volume's facts, the device tree above its disk, the
 * final paths and WM_DEVICECHANGE's broadcasts. KERNEL32 only is imported; cfgmgr32.dll (not on
 * the core DLL's allowlist, app.md §7) is loaded at its first use by its full System32 path with
 * LOAD_LIBRARY_SEARCH_SYSTEM32, as compat_win.c loads wintrust. No call needs elevation: the
 * volume and the disk are opened with no access rights, which every IOCTL here allows
 * (FILE_ANY_ACCESS). Unverified until the VM run (W13a): every call here; the external rule on a
 * Thunderbolt/USB4 enclosure and a hot-plug SATA port in the real-PC round (J28). */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <dbt.h>

#include "volumes.h"
#include "win_rules.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ UTF-8 and UTF-16 */

static int to8(const wchar_t *w, char *out, size_t cap) {
    return wr_utf16_to_8((const uint16_t *)w, wcslen(w), out, cap) < 0 ? -1 : 0;
}

/* A UTF-8 path as the wide "\\?\" form CreateFileW takes at any length: wr_nt_path's checks (an
 * absolute drive or volume GUID path, no "." or ".." component), "\??\" turned into "\\?\".
 * malloc'd; NULL when invalid. */
static wchar_t *wide_long(const char *p) {
    wchar_t *w = malloc(sizeof(wchar_t) * (WR_NT_MAX + 1));
    if (!w) return NULL;
    if (wr_nt_path(p, (uint16_t *)w, WR_NT_MAX + 1) < 0) {
        free(w);
        return NULL;
    }
    w[1] = L'\\';                                          /* "\??\" -> "\\?\" */
    return w;
}

static int last_errno(void) { return wr_errno_win32((uint32_t)GetLastError(), WR_OP_OPEN); }

static int same_ascii_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* ------------------------------------------------------------------ cfgmgr32, loaded at first use */

typedef CONFIGRET(WINAPI *list_size_fn)(PULONG, LPGUID, DEVINSTID_W, ULONG);
typedef CONFIGRET(WINAPI *list_fn)(LPGUID, DEVINSTID_W, PZZWSTR, ULONG, ULONG);
typedef CONFIGRET(WINAPI *iface_prop_fn)(LPCWSTR, const DEVPROPKEY *, DEVPROPTYPE *, PBYTE, PULONG, ULONG);
typedef CONFIGRET(WINAPI *locate_fn)(PDEVINST, DEVINSTID_W, ULONG);
typedef CONFIGRET(WINAPI *parent_fn)(PDEVINST, DEVINST, ULONG);
typedef CONFIGRET(WINAPI *reg_prop_fn)(DEVINST, ULONG, PULONG, PVOID, PULONG, ULONG);

static struct {
    list_size_fn list_size;
    list_fn list;
    iface_prop_fn iface_prop;
    locate_fn locate;
    parent_fn parent;
    reg_prop_fn reg_prop;
} cm;
static INIT_ONCE cm_once = INIT_ONCE_STATIC_INIT;

#define PROC(m, name) ((void (*)(void))GetProcAddress((m), (name)))

static BOOL WINAPI cm_load(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    static const wchar_t file[] = L"\\cfgmgr32.dll";
    if (n == 0 || n + sizeof file / sizeof *file > MAX_PATH) return TRUE;
    memcpy(path + n, file, sizeof file);
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return TRUE;
    list_size_fn a = (list_size_fn)PROC(m, "CM_Get_Device_Interface_List_SizeW");
    list_fn b = (list_fn)PROC(m, "CM_Get_Device_Interface_ListW");
    iface_prop_fn c = (iface_prop_fn)PROC(m, "CM_Get_Device_Interface_PropertyW");
    locate_fn d = (locate_fn)PROC(m, "CM_Locate_DevNodeW");
    parent_fn e = (parent_fn)PROC(m, "CM_Get_Parent");
    reg_prop_fn f = (reg_prop_fn)PROC(m, "CM_Get_DevNode_Registry_PropertyW");
    if (a && b && c && d && e && f) cm.list_size = a, cm.list = b, cm.iface_prop = c, cm.locate = d, cm.parent = e, cm.reg_prop = f;
    return TRUE;
}

/* GUID_DEVINTERFACE_DISK, GUID_DEVINTERFACE_VOLUME (winioctl.h) and DEVPKEY_Device_InstanceId
 * (devpkey.h), written out: no INITGUID, no uuid library */
static GUID disk_class = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
static const GUID volume_class = {0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
static const DEVPROPKEY instance_id = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};

/* ------------------------------------------------------------------ the device tree */

/* a node's registry property as UTF-8 (a multi-string's strings joined with ' '); "" when none */
static void node_text(DEVINST dn, ULONG prop, char *out, size_t cap) {
    wchar_t buf[2048];
    ULONG size = sizeof buf - 2 * sizeof(wchar_t), type = 0;
    out[0] = 0;
    memset(buf, 0, sizeof buf);
    if (cm.reg_prop(dn, prop, &type, buf, &size, 0) != CR_SUCCESS) return;
    if (type == REG_MULTI_SZ)   /* "a\0b\0\0" -> "a b": the buffer ends in two NULs (sized so above) */
        for (size_t i = 0; i + 1 < sizeof buf / sizeof *buf && !(buf[i] == 0 && buf[i + 1] == 0); i++)
            if (buf[i] == 0) buf[i] = L' ';
    if (to8(buf, out, cap)) out[0] = 0;
}

static void read_node(DEVINST dn, vol_node *v) {
    char t[4096];
    memset(v, 0, sizeof *v);
    node_text(dn, CM_DRP_ENUMERATOR_NAME, v->enumerator, sizeof v->enumerator);
    for (char *c = v->enumerator; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
    DWORD caps = 0;
    ULONG size = sizeof caps, type = 0;
    if (cm.reg_prop(dn, CM_DRP_CAPABILITIES, &type, &caps, &size, 0) == CR_SUCCESS) v->removable = (caps & CM_DEVCAP_REMOVABLE) != 0;
    static const ULONG texts[] = {CM_DRP_HARDWAREID, CM_DRP_COMPATIBLEIDS, CM_DRP_SERVICE, CM_DRP_DEVICEDESC, CM_DRP_FRIENDLYNAME};
    for (size_t i = 0; i < sizeof texts / sizeof *texts && !v->tunnel; i++) {
        node_text(dn, texts[i], t, sizeof t);
        v->tunnel = vol_tunnel_text(t);
    }
}

/* the disk with this number: its bus, hot-plug and device tree into f */
static void read_disk(DWORD number, vol_facts *f) {
    InitOnceExecuteOnce(&cm_once, cm_load, NULL, NULL);
    if (!cm.list) return;
    ULONG len = 0;
    if (cm.list_size(&len, &disk_class, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len == 0) return;
    wchar_t *list = calloc(len + 1, sizeof(wchar_t));
    if (!list) return;
    if (cm.list(&disk_class, NULL, list, len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        free(list);
        return;
    }
    for (wchar_t *iface = list; *iface; iface += wcslen(iface) + 1) {
        HANDLE h = CreateFileW(iface, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER num;
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &num, sizeof num, &got, NULL) || num.DeviceNumber != number
            || num.DeviceType != FILE_DEVICE_DISK) {
            CloseHandle(h);
            continue;
        }
        /* the bus */
        STORAGE_PROPERTY_QUERY q;
        memset(&q, 0, sizeof q);
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        union { STORAGE_DEVICE_DESCRIPTOR d; BYTE b[1024]; } desc;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &desc, sizeof desc, &got, NULL)
            && got >= offsetof(STORAGE_DEVICE_DESCRIPTOR, RawPropertiesLength))
            f->bus = (int)desc.d.BusType;
        /* hot-plug */
        STORAGE_HOTPLUG_INFO hp;
        memset(&hp, 0, sizeof hp);
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_HOTPLUG_INFO, NULL, 0, &hp, sizeof hp, &got, NULL)) f->hotplug = hp.DeviceHotplug != 0;
        CloseHandle(h);
        /* the device tree, from the disk up */
        wchar_t id[MAX_DEVICE_ID_LEN + 1];
        ULONG size = sizeof id - sizeof(wchar_t);
        DEVPROPTYPE type = 0;
        DEVINST dn;
        memset(id, 0, sizeof id);
        if (cm.iface_prop(iface, &instance_id, &type, (PBYTE)id, &size, 0) == CR_SUCCESS && type == DEVPROP_TYPE_STRING
            && cm.locate(&dn, id, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
            int n = 0;
            for (;;) {
                read_node(dn, &f->node[n++]);
                DEVINST up;
                if (n >= VOL_NODES_MAX || cm.parent(&up, dn, 0) != CR_SUCCESS) break;
                dn = up;
            }
            f->nodes = n;
        }
        break;
    }
    free(list);
}

/* how many disks the volume spans, and the first one's number */
static int volume_disks(HANDLE v, DWORD *first) {
    union { VOLUME_DISK_EXTENTS x; BYTE b[sizeof(VOLUME_DISK_EXTENTS) + 15 * sizeof(DISK_EXTENT)]; } e;
    DWORD got = 0;
    if (!DeviceIoControl(v, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &e, sizeof e, &got, NULL))
        return GetLastError() == ERROR_MORE_DATA ? 2 : -1;
    if (e.x.NumberOfDiskExtents == 0) return 0;
    *first = e.x.Extents[0].DiskNumber;
    int disks = 1;
    for (DWORD i = 1; i < e.x.NumberOfDiskExtents && i < 16; i++)
        if (e.x.Extents[i].DiskNumber != *first) disks = 2;
    return disks;
}

/* ------------------------------------------------------------------ a volume's facts */

/* the volume GUID path of the volume holding %SystemRoot% */
static int system_volume(char *out, size_t n) {
    wchar_t win[MAX_PATH + 1], root[MAX_PATH + 1], guid[64];
    UINT k = GetSystemWindowsDirectoryW(win, MAX_PATH + 1);
    if (k == 0 || k > MAX_PATH || !GetVolumePathNameW(win, root, MAX_PATH + 1) || !GetVolumeNameForVolumeMountPointW(root, guid, 64))
        return -1;
    return to8(guid, out, n);
}

/* the volume's mount points as UTF-8, NUL-separated, an empty one last: malloc'd, *len bytes */
static char *mount_list(const wchar_t *wguid, size_t *len) {
    DWORD need = 0;
    wchar_t *w = NULL;
    for (DWORD cap = 512;; cap = need) {
        free(w);
        w = malloc(sizeof(wchar_t) * cap);
        if (!w) return NULL;
        if (GetVolumePathNamesForVolumeNameW(wguid, w, cap, &need)) break;
        if (GetLastError() != ERROR_MORE_DATA || need <= cap) { free(w); return NULL; }
    }
    size_t cap8 = 4096, k = 0;
    char *out = malloc(cap8);
    if (!out) { free(w); return NULL; }
    for (const wchar_t *m = w; *m; m += wcslen(m) + 1) {
        char one[VOL_PATH_MAX];
        if (to8(m, one, sizeof one)) continue;
        size_t l = strlen(one) + 1;
        if (k + l + 1 > cap8) break;
        memcpy(out + k, one, l);
        k += l;
    }
    out[k++] = 0;
    *len = k;
    free(w);
    return out;
}

int vol_facts_read(const char *guid, int device, vol_facts *out) {
    memset(out, 0, sizeof *out);
    out->disks = out->bus = out->hotplug = -1;
    size_t g = vol_guid_root(guid);
    if (!g || guid[g] || g >= sizeof out->guid) return EINVAL;
    memcpy(out->guid, guid, g + 1);
    wchar_t wguid[VOL_GUID_MAX], label[MAX_PATH + 1], fs[32];
    if (wr_utf8_to_16(guid, g, (uint16_t *)wguid, VOL_GUID_MAX) < 0) return EINVAL;
    size_t len = 0;
    char *mounts = mount_list(wguid, &len);
    if (mounts) {
        if (vol_pick_mount(mounts, len, out->mount, sizeof out->mount)) out->mount[0] = 0;
        free(mounts);
    }
    out->drive_type = (int)GetDriveTypeW(wguid);
    DWORD flags = 0;
    if (!GetVolumeInformationW(wguid, label, MAX_PATH + 1, NULL, NULL, &flags, fs, 32)) return last_errno();
    if (to8(label, out->label, sizeof out->label)) out->label[0] = 0;
    if (to8(fs, out->fstype, sizeof out->fstype)) out->fstype[0] = 0;
    for (char *c = out->fstype; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    out->readonly = (flags & FILE_READ_ONLY_VOLUME) != 0;
    char sys[VOL_GUID_MAX];
    out->system = system_volume(sys, sizeof sys) == 0 && same_ascii_ci(sys, out->guid);
    if (!device) return 0;
    /* the volume's device: "\\?\Volume{...}" without its last '\' */
    wguid[g - 1] = 0;
    HANDLE v = CreateFileW(wguid, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (v == INVALID_HANDLE_VALUE) return 0;               /* the device's facts stay unknown */
    DWORD disk = 0;
    out->disks = volume_disks(v, &disk);
    CloseHandle(v);
    if (out->disks == 1) read_disk(disk, out);
    return 0;
}

int vol_list(vol_facts *out, int max) {
    wchar_t vol[VOL_GUID_MAX];
    HANDLE f = FindFirstVolumeW(vol, VOL_GUID_MAX);
    if (f == INVALID_HANDLE_VALUE) return -1;
    int n = 0;
    do {
        char g[VOL_GUID_MAX];
        if (n < max && !to8(vol, g, sizeof g) && vol_facts_read(g, 0, &out[n]) == 0) n++;   /* no media: not listed */
    } while (n < max && FindNextVolumeW(f, vol, VOL_GUID_MAX));
    FindVolumeClose(f);
    return n;
}

/* ------------------------------------------------------------------ final paths */

static int final_path(const char *path, DWORD how, char *out, size_t n) {
    wchar_t *w = wide_long(path);
    if (!w) return EINVAL;
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return last_errno();
    wchar_t *buf = malloc(sizeof(wchar_t) * (WR_NT_MAX + 1));
    DWORD k = buf ? GetFinalPathNameByHandleW(h, buf, WR_NT_MAX + 1, FILE_NAME_NORMALIZED | how) : 0;
    int e = !buf ? ENOMEM : k == 0 ? last_errno() : k > WR_NT_MAX ? ENAMETOOLONG : 0;
    CloseHandle(h);
    if (!e && to8(buf, out, n)) e = ENAMETOOLONG;
    free(buf);
    return e;
}

int vol_guid_path(const char *path, char *out, size_t n) {
    int e = final_path(path, VOLUME_NAME_GUID, out, n);
    return e ? e : vol_guid_root(out) ? 0 : EINVAL;
}

int vol_display_path(const char *guid_path, char *out, size_t n) {
    size_t g = vol_guid_root(guid_path);
    wchar_t wguid[VOL_GUID_MAX];
    if (!g || wr_utf8_to_16(guid_path, g, (uint16_t *)wguid, VOL_GUID_MAX) < 0) return EINVAL;
    size_t len = 0;
    char *mounts = mount_list(wguid, &len), mount[VOL_PATH_MAX];
    if (!mounts) return last_errno();
    int r = vol_pick_mount(mounts, len, mount, sizeof mount);
    free(mounts);
    if (r) return ENOENT;
    return vol_join_mount(guid_path, mount, out, n) ? ENAMETOOLONG : 0;
}

int vol_dos_path(const char *path, char *out, size_t n) {
    char *tmp = malloc(VOL_PATH_MAX * 4);
    if (!tmp) return ENOMEM;
    int e = final_path(path, VOLUME_NAME_DOS, tmp, VOL_PATH_MAX * 4);
    if (!e) e = vol_path_remote(tmp) ? EXDEV : vol_plain_dos(tmp, out, n) ? ENAMETOOLONG : 0;
    else if (!(e = final_path(path, VOLUME_NAME_GUID, tmp, VOL_PATH_MAX * 4)))   /* a volume mounted in a folder only */
        e = vol_display_path(tmp, out, n);
    free(tmp);
    return e;
}

/* whether a drive path's letter is a mapped network drive */
static int remote_letter(const char *p) {
    const char *rest;
    int k = wr_path_kind(p, &rest);
    if (k != 1 && k != 3) return 0;
    const char *d = k == 3 ? p + 4 : p;
    wchar_t root[4] = {(wchar_t)(unsigned char)d[0], L':', L'\\', 0};
    return GetDriveTypeW(root) == DRIVE_REMOTE;
}

int vol_facts_of(const char *path, vol_facts *out) {
    char g[VOL_PATH_MAX * 4];
    if (vol_path_remote(path) || remote_letter(path)) {
        vol_remote_facts(out);
        return 0;
    }
    int e = vol_guid_path(path, g, sizeof g);
    if (e) {
        /* no volume GUID: a share reached through a link or a folder (its final path is UNC) */
        if (!final_path(path, VOLUME_NAME_DOS, g, sizeof g) && (vol_path_remote(g) || remote_letter(g))) {
            vol_remote_facts(out);
            return 0;
        }
        return e;
    }
    g[vol_guid_root(g)] = 0;
    return vol_facts_read(g, 1, out);
}

/* ------------------------------------------------------------------ device events */

int vol_device_message(uintptr_t wparam, intptr_t lparam, char *out, size_t n) {
    const DEV_BROADCAST_HDR *h = (const DEV_BROADCAST_HDR *)lparam;
    if (n) out[0] = 0;
    if ((wparam != DBT_DEVICEARRIVAL && wparam != DBT_DEVICEREMOVECOMPLETE) || !h || h->dbch_size < sizeof *h) return 0;
    if (h->dbch_devicetype == DBT_DEVTYP_VOLUME && h->dbch_size >= sizeof(DEV_BROADCAST_VOLUME))
        return vol_device_events((uint32_t)wparam, VOL_DBT_VOLUME, ((const DEV_BROADCAST_VOLUME *)h)->dbcv_unitmask, NULL, out, n);
    if (h->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE && h->dbch_size > offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name)) {
        const DEV_BROADCAST_DEVICEINTERFACE_W *d = (const DEV_BROADCAST_DEVICEINTERFACE_W *)h;
        if (memcmp(&d->dbcc_classguid, &volume_class, sizeof volume_class)) return 0;
        size_t units = (h->dbch_size - offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name)) / sizeof(wchar_t), l = 0;
        while (l < units && d->dbcc_name[l]) l++;
        char name[VOL_PATH_MAX];
        if (wr_utf16_to_8((const uint16_t *)d->dbcc_name, l, name, sizeof name) < 0) return 0;
        return vol_device_events((uint32_t)wparam, VOL_DBT_INTERFACE, 0, name, out, n);
    }
    return 0;
}
