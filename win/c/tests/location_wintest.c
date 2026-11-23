/* The location's native code on real Windows (plan N3; the NL rulings' wintest rows): the drive's
 * name and its hint, the probe of a chosen folder, the measure, and the location choice's steps on
 * a real volume through its volume-GUID path (the contract with the bridge). Built by run.sh with
 * plat_win.c, x64 and arm64; tests/win/suites/location.suite runs it in the VM on VHDX volumes, one
 * mode per call, and checks what it prints:
 *
 *   winc-location-wintest.exe volname PATH            plat_volume_name: "volname RC LABEL|LETTER", and
 *                                                     GetVolumePathNameW's raw answer ("raw ...")
 *   winc-location-wintest.exe drive SUPPORT PATH [1]  native_location_drive_name (1: remember):
 *                                                     "drive [NAME]"
 *   winc-location-wintest.exe probe PATH ID           native_location_probe: "probe N" (PROBE_*)
 *   winc-location-wintest.exe measure SUPPORT CACHE   native_location_slow_fact with the release gate
 *                                                     and wait, no rekordbox and Stems Cache saying
 *                                                     location=1: "measure RC KIND PROBE FLOOR|DRIVE|IN_USE"
 *   winc-location-wintest.exe measure-live SUPPORT CACHE
 *                                                     the same through the DLL's own
 *                                                     nl_rekordbox_running and nl_process_start
 *                                                     (review N3R5: a stand-in rekordbox.exe):
 *                                                     "measure ..." and "bridge RUNNING HAS|STATE|
 *                                                     REASON|DRIVE" (DRIVE "-" when not set)
 *   winc-location-wintest.exe start PID               nl_process_start: "start SECONDS" (-1 if
 *                                                     unavailable, including an exited process
 *                                                     whose handle is still open)
 *   winc-location-wintest.exe choice-probe FOLDER [--internal-ok]:
 *       LOCATION_PROBE: "choice RC CODE|REAL|MOUNT|DRIVE", "facts LINE" (original volume verdict).
 *       Append --internal-ok to choice-probe, probe, measure, measure-live, clear, remove or create
 *       to reach the path checks on internal volumes with a supported filesystem (N3R17; create
 *       checks the volume itself since 205ffda3).
 *   winc-location-wintest.exe create SUPPORT FOLDER   guid_of's fixture path (bypasses the probe),
 *                                                     then LOCATION_CREATE and LOCATION_WRITE:
 *                                                     "create RC|DIR|ID", "write RC"
 *   winc-location-wintest.exe discover SUPPORT FOLDER guid_of's fixture path, then LOCATION_DISCOVER:
 *                                                     "discover CODE|FOUND|ID"
 *   winc-location-wintest.exe support-error SUPPORT junction|file: native_support_write's errno
 *   winc-location-wintest.exe clear SUPPORT          REMOVE_LOCATION (contents only): "clear RC"
 *   winc-location-wintest.exe remove SUPPORT          REMOVE_LOCATION (whole) of config.ini's folder:
 *                                                     "remove RC"
 *   winc-location-wintest.exe remove-held SUPPORT PATH
 *                                                     "remove-held RC RENAME_BLOCKED"
 *   winc-location-wintest.exe remove-child-held SUPPORT PATH
 *                                                     "remove-child-held RC STEMS_REMAIN"
 *   winc-location-wintest.exe this-pc SUPPORT         LOCATION_WRITE "" (Use This PC): "this-pc RC"
 *
 * Exit 0 when the mode ran, 2 on a usage error. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../native_choice_win.h"
#include "../native_location.h"
#include "../native_steps.h"
#include "../volumes.h"
#include "plat.h"
#include "plat_win.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

int vol_test_internal_ok;

/* ---------------------------------------------------------------- core_native.c's ops, without rekordbox */

static int says = 1;
static int t_live_bridge(void *ctx, int *present, int *location) {
    (void)ctx;
    *present = 1;
    *location = says;
    return 0;
}
static int t_running(void *ctx) {
    (void)ctx;
    return 0;
}
static int64_t t_start(void *ctx, int32_t pid) {
    (void)ctx;
    (void)pid;
    return -1;
}
static int t_volume_name(void *ctx, const char *dir, char *label, size_t ln, char *letter, size_t lt) {
    (void)ctx;
    return plat_volume_name(dir, label, ln, letter, lt);
}
static int t_remote(void *ctx, const char *dir) {
    (void)ctx;
    return plat_win_drive_remote(dir);
}
static const native_location_ops lops = {NULL, t_live_bridge, t_running, t_start, t_volume_name, t_remote, NULL, NULL, native_pc_cache_probe};
/* the DLL's own rekordbox ops (native_choice_win.c): the process list and the creation time */
static const native_location_ops lops_live = {NULL, t_live_bridge, nl_rekordbox_running, nl_process_start, t_volume_name, t_remote, NULL, NULL,
                                             native_pc_cache_probe};

static int measure_write(void *ctx, const native_location *l) {
    *(native_location *)ctx = *l;
    return 0;
}
static void measure_log(void *ctx, const char *line) {
    (void)ctx;
    puts(line);
}

/* ---------------------------------------------------------------- the shared Windows choice ops */

static const char *support_dir = "";
static void c_facts_log(void *ctx, const char *line) {
    (void)ctx;
    printf("facts %s\n", line);
}
static int c_volume(void *ctx, const char *folder, int *refusal, char *root, size_t rn, char *real, size_t n) {
    return nc_volume(ctx, folder, refusal, root, rn, real, n, c_facts_log);
}
static void c_drive(void *ctx, const char *dir, int remember, char *out, size_t n) {
    (void)ctx;
    native_location_env le = {support_dir, "C:\\unused", &lops};
    native_location_drive_name(&le, dir, remember, out, n);
}
static const native_choice_ops cops = {NULL, c_volume, c_drive, nc_new_id, nc_not_indexed, NULL};

static int guid_paths(const char *folder, const vol_facts *f, char *real, size_t n, char *root, size_t rn) {
    if (snprintf(root, rn, "%s", f->guid) >= (int)rn) return ENAMETOOLONG;
    return vol_guid_path(folder, real, n);
}

static int is_not_indexed(const char *dir) {
    wchar_t w[NS_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dir, -1, w, NS_PATH_MAX)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) != 0;
}

static void e_log(void *ctx, const char *line) {
    (void)ctx;
    printf("log %s\n", line);
}
static int e_rm(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    return plat_remove_tree_at(d, name);
}
static int e_rmdir(void *ctx, plat_dir d, const char *name, plat_id expect) {
    (void)ctx;
    return plat_remove_empty_dir_at(d, name, expect);
}
static void env(native_steps_env *e, const char *support) {
    memset(e, 0, sizeof *e);
    e->support = support;
    e->log = e_log;
    e->remove_tree_at = e_rm;
    e->remove_empty_dir_at = e_rmdir;
}

/* Fixture bypass for CREATE and DISCOVER: VHDX sticks are internal and LOCATION_PROBE refuses
 * them. Resolve their real paths and volume roots directly in cache_dir's form. */
static int guid_of(const char *folder, char *real, size_t n, char *root, size_t rn) {
    vol_facts f;
    int e = vol_facts_of(folder, &f);
    return e ? e : guid_paths(folder, &f, real, n, root, rn);
}

static int config_dir(const char *support, char *dir, size_t dn, char *id, size_t in) {
    char p[1100], *b = NULL;
    size_t n = 0;
    snprintf(p, sizeof p, "%s\\config.ini", support);
    if (plat_read_file(p, 65536, &b, &n)) return -1;
    dir[0] = id[0] = 0;
    for (char *l = b; l && l < b + n;) {
        char *nl = memchr(l, '\n', (size_t)(b + n - l));
        size_t ll = nl ? (size_t)(nl - l) : (size_t)(b + n - l);
        if (ll > 13 && !strncmp(l, "cache_dir = \"", 13) && l[ll - 1] == '"') snprintf(dir, dn, "%.*s", (int)(ll - 14), l + 13);
        else if (ll > 11 && !strncmp(l, "cache_id = ", 11)) snprintf(id, in, "%.*s", (int)(ll - 11), l + 11);
        else if (ll > 11 && !strncmp(l, "cache_dir=\"", 11) && l[ll - 1] == '"') snprintf(dir, dn, "%.*s", (int)(ll - 12), l + 11);
        else if (ll > 9 && !strncmp(l, "cache_id=", 9)) snprintf(id, in, "%.*s", (int)(ll - 9), l + 9);
        l = nl ? nl + 1 : NULL;
    }
    printf("config %s|%s\n", dir, id);
    free(b);
    return dir[0] && id[0] ? 0 : -1;
}

/* A preexisting DELETE handle must prevent authorization. A removal handle, once
 * acquired, must in turn prevent another thread from opening the folder for rename. */
static int test_removal_hold(const char *support, const char *path, int child) {
    char dir[1100], id[64], name[256], err[128], identity[NL_IDENTITY_MAX];
    wchar_t src[1100], dst[1116];
    if (config_dir(support, dir, sizeof dir, id, sizeof id) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, src, 1100)) return 1;
    if (swprintf(dst, sizeof dst / sizeof dst[0], L"%ls-moved", src) < 0) return 1;
    plat_stat st;
    if (plat_stat_path(dir, 0, &st)) return 1;
    native_location_identity(st.id, identity);
    HANDLE competing = CreateFileW(src, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (competing == INVALID_HANDLE_VALUE) return 1;
    native_steps_env ne;
    env(&ne, support);
    int rc = native_remove_location(&ne, dir, id, 1, name, err, identity);
    CloseHandle(competing);

    if (child) {
        if (swprintf(dst, sizeof dst / sizeof dst[0], L"%ls\\stems", src) < 0) return 1;
        printf("remove-child-held %d %d\n", rc, GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES);
        return 0;
    }

    char parent[1100];
    snprintf(parent, sizeof parent, "%s", path);
    char *leaf = strrchr(parent, '\\');
    if (!leaf) return 1;
    *leaf++ = 0;
    plat_dir p, held;
    if (plat_dir_open(parent, &p)) return 1;
    int open_err = plat_dir_open_at_for_removal(p, leaf, &held);
    plat_dir_close(p);
    if (open_err) return 1;
    int renamed = MoveFileExW(src, dst, 0) != 0;
    DWORD rename_error = GetLastError();
    plat_dir_close(held);
    if (renamed) MoveFileExW(dst, src, 0);
    printf("remove-held %d %d\n", rc, !renamed && rename_error == ERROR_SHARING_VIOLATION);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) return 2;
    const char *m = argv[1];
    if (argc > 3 && !strcmp(argv[argc - 1], "--internal-ok")) {
        if (strcmp(m, "choice-probe") && strcmp(m, "probe") && strcmp(m, "measure") && strcmp(m, "measure-live")
            && strcmp(m, "clear") && strcmp(m, "remove") && strcmp(m, "create")) return 2;
        vol_test_internal_ok = 1;
        argc--;
    }
    if ((!strcmp(m, "remove-held") || !strcmp(m, "remove-child-held")) && argc == 4) {
        vol_test_internal_ok = 1;
        return test_removal_hold(argv[2], argv[3], !strcmp(m, "remove-child-held"));
    }
    if (!strcmp(m, "volname")) {
        char label[256] = "", letter[8] = "";
        int rc = plat_volume_name(argv[2], label, sizeof label, letter, sizeof letter);
        printf("volname %d %s|%s\n", rc, label, letter);
        wchar_t w[1100], r[1100];
        if (MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, w, 1100) && GetVolumePathNameW(w, r, 1100)) {
            char u[3300];
            WideCharToMultiByte(CP_UTF8, 0, r, -1, u, sizeof u, NULL, NULL);
            printf("raw %s\n", u);
            wchar_t name[MAX_PATH + 1], fs[32];
            DWORD flags = 0;
            if (GetVolumeInformationW(r, name, MAX_PATH + 1, NULL, NULL, &flags, fs, 32)) printf("gvi ok flags %08lx\n", flags);
            else printf("gvi error %lu\n", GetLastError());
            wchar_t g[64];
            if (GetVolumeNameForVolumeMountPointW(r, g, 64)) printf("gvn ok\n");
            else printf("gvn error %lu\n", GetLastError());
        } else printf("raw error %lu\n", GetLastError());
        return 0;
    }
    if (!strcmp(m, "drive") && argc >= 4) {
        support_dir = argv[2];
        char name[256];
        c_drive(NULL, argv[3], argc >= 5 && !strcmp(argv[4], "1"), name, sizeof name);
        printf("drive [%s]\n", name);
        return 0;
    }
    if (!strcmp(m, "probe") && argc >= 4) {
        printf("probe %d\n", native_location_probe(&lops, argv[2], argv[3]));
        return 0;
    }
    if ((!strcmp(m, "measure") || !strcmp(m, "measure-live")) && argc >= 4) {
        int with_processes = !strcmp(m, "measure-live");
        native_location_env le = {argv[2], argv[3], with_processes ? &lops_live : &lops};
        static native_location_runner runner = NATIVE_LOCATION_RUNNER_INIT;
        native_location l = {.probe = -1, .paused_floor = -1};
        int rc = native_location_slow_fact(&runner, &le, NULL, NL_WAIT_NS, measure_write, measure_log, &l);
        printf("measure %d %d %d %lld|%s|%s\n", rc, l.kind, l.probe, (long long)l.paused_floor, l.has_status_drive ? l.status_drive : "-",
               l.has_in_use ? l.in_use : "-");
        if (with_processes)
            printf("bridge %d %d|%s|%s|%s\n", l.rekordbox_running, l.has_bridge, l.bridge_state, l.bridge_reason,
                   l.has_bridge_drive ? l.bridge_drive : "-");
        return 0;
    }
    if (!strcmp(m, "start")) {
        printf("start %lld\n", (long long)nl_process_start(NULL, (int32_t)strtol(argv[2], NULL, 10)));
        return 0;
    }
    if (!strcmp(m, "free")) {   /* the measure's free bytes and total for a folder (review NL6) */
        const char *how = "";
        plat_volume v;
        uint64_t avail = plat_free_bytes(argv[2], 0, &how);
        int e = plat_volume_info(argv[2], &v);
        printf("free %llu %s|%d %llu\n", (unsigned long long)avail, how ? how : "", e, e ? 0ull : (unsigned long long)v.total);
        return 0;
    }
    if (!strcmp(m, "choice-probe")) {
        if (argc != 3) return 2;
        support_dir = getenv("TEMP") ? getenv("TEMP") : "C:\\Windows\\Temp";
        native_choice_probe p;
        int rc = native_choice_probe_folder(&cops, argv[2], &p);
        printf("choice %d %lld|%s|%s|%s\n", rc, (long long)p.code, p.real, p.mount, p.drive);
        return 0;
    }
    if (!strcmp(m, "create") && argc >= 4) {
        support_dir = argv[2];
        char real[1100] = "", root[80] = "", dir[NS_PATH_MAX], id[40], err[128], identity[NS_CACHE_IDENTITY_MAX];
        int e = guid_of(argv[3], real, sizeof real, root, sizeof root);
        printf("real %d %s|%s\n", e, real, root);
        if (e) return 0;
        native_steps_env ne;
        env(&ne, argv[2]);
        int rc = native_choice_create(&cops, &ne, real, dir, id, err, identity);
        printf("create %d %s|%s|%s|indexed %d\n", rc, dir, id, err, !rc && is_not_indexed(dir));
        if (rc) return 0;
        printf("write %d\n", native_choice_write(&cops, &ne, dir, id, root, 1, identity));
        char cd[1100], ci[64];
        config_dir(argv[2], cd, sizeof cd, ci, sizeof ci);
        return 0;
    }
    if (!strcmp(m, "discover") && argc >= 4) {
        char real[1100] = "", root[80] = "";
        int e = guid_of(argv[3], real, sizeof real, root, sizeof root);
        printf("real %d %s|%s\n", e, real, root);
        if (e) return 0;
        native_choice_found f;
        int rc = native_choice_discover(argv[2], real, root, &f);
        printf("discover %d %lld|%s|%s|%s\n", rc, (long long)f.code, f.found, f.id, f.current);
        return 0;
    }
    if (!strcmp(m, "support-error") && argc == 4) {
        int want = !strcmp(argv[3], "junction") ? ELOOP : !strcmp(argv[3], "file") ? ENOTDIR : 0;
        if (!want) return 2;
        int rc = native_support_write(argv[2], "probe", "test", 4);
        printf("support write %d (expected %d)\n", rc, want);
        return rc != want;
    }
    if (!strcmp(m, "remove") || !strcmp(m, "clear")) {
        int whole = !strcmp(m, "remove");
        char dir[1100], id[64], name[256], err[128];
        if (config_dir(argv[2], dir, sizeof dir, id, sizeof id)) {
            printf("%s no-config\n", m);
            return 0;
        }
        native_steps_env ne;
        env(&ne, argv[2]);
        plat_stat st;
        char identity[NL_IDENTITY_MAX] = "";
        if (!plat_stat_path(dir, 0, &st)) native_location_identity(st.id, identity);
        int rc = native_remove_location(&ne, dir, id, whole, name, err, identity);
        printf("%s %d %s|%s\n", m, rc, name, err);
        return 0;
    }
    if (!strcmp(m, "this-pc")) {
        native_steps_env ne;
        env(&ne, argv[2]);
        printf("this-pc %d\n", native_choice_write(&cops, &ne, "", "", "", 0, NULL));
        return 0;
    }
    return 2;
}
