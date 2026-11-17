/* The app's own platform calls (W6a) on real Windows: the VM's checks (W13a), x64 and arm64.
 * Built by run.sh with plat_win.c, run unelevated in the VM by the main session:
 *
 *   winc-app-wintest.exe [--elevated] [--external X:\] [--share Z:\]
 *      --elevated     the run is elevated ("Run as administrator"): the token check expects ELEVATED
 *      --external X:\ an external drive (a USB stick, tests/vm-usb.swift's) to check the rule on
 *      --share Z:\    a mapped share (`net use Z: \\localhost\<share>`): refused as a network drive
 *
 * It writes only under HKCU's own values (the Run value, StartupApproved, the Apps & features
 * entry: each is saved first and put back after) and in %TEMP%. Each step prints what it saw,
 * for the run's log; the volume lines are the real-PC round's facts (J28). The second-copy cases
 * start this exe again as `--second NAME WAIT_MS`. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#  define COBJMACROS
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <dbt.h>

#include "../lifecycle.h"
#include "../report.h"
#include "../shell.h"
#include "../volumes.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stdout, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stdout, __VA_ARGS__);                                   \
            fputc('\n', stdout);                                            \
        }                                                                   \
    } while (0)

static void to8(const wchar_t *w, char *out, int cap) {
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) <= 0) out[0] = 0;
}

static void wide(const char *s, wchar_t *out, int cap) {
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) <= 0) out[0] = 0;
}

/* ------------------------------------------------------------------ the minimum, the token, start times */

static void test_os(int elevated) {
    uint32_t major, minor, build;
    CHECK(life_os_version(&major, &minor, &build) == 0 && major == 10 && build >= LIFE_MIN_BUILD, "RtlGetVersion: %u.%u.%u", major,
          minor, build);
    printf("os: %u.%u.%u, .NET release %lld\n", major, minor, build, (long long)life_net_release());
    CHECK(life_os_check(1) == LIFE_OS_OK && life_os_check(0) == LIFE_OS_OK, "this VM meets the minimum");
    int t = life_token_check();
    printf("token: %d\n", t);
    CHECK(t == (elevated ? LIFE_TOKEN_ELEVATED : LIFE_TOKEN_OK), "the token verdict %d", t);

    int64_t mine = life_process_start(GetCurrentProcessId());
    CHECK(mine == plat_process_start() && mine > 1700000000, "this process: %lld vs %lld", (long long)mine, (long long)plat_process_start());
    CHECK(life_process_start(0xFFFFFFF0u) == -1, "no such process");
    int64_t sys = life_process_start(4);
    printf("System's start: %lld\n", (long long)sys);
    CHECK(sys != -2, "System (pid 4) answers through the process list");
    /* an exited child: its pid stays openable while we hold its handle */
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf(cmd, MAX_PATH + 32, L"\"%ls\" --exit", exe);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CHECK(life_process_start(pi.dwProcessId) == -1, "an exited process, its handle still open: gone");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else CHECK(0, "CreateProcess %lu", GetLastError());
}

/* ------------------------------------------------------------------ one copy */

static int signalled;
static void on_signal(void *ctx) { (void)ctx; signalled++; }

static DWORD second(const char *name, uint32_t wait) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 160], wn[128];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wide(name, wn, 128);
    _snwprintf(cmd, MAX_PATH + 160, L"\"%ls\" --second %ls %u", exe, wn, wait);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0xFFFF;
    DWORD code = 0xFFFF;
    /* pump our window's messages while the second start runs */
    for (;;) {
        DWORD w = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 15000, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0 || w == WAIT_TIMEOUT) break;
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    MSG m;
    for (int i = 0; i < 20; i++) {                         /* the posted signal, if it is still queued */
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        Sleep(10);
    }
    return code;
}

static void test_one_copy(void) {
    char name[128];
    snprintf(name, sizeof name, "Local\\io.github.rbstemsplus.wintest-%lu", GetCurrentProcessId());
    life_one *o = NULL;
    CHECK(life_one_take(name, 0, &o) == LIFE_ONE_HELD && o, "the first copy");
    CHECK(life_one_window(o, on_signal, NULL) == 0, "its window");
    DWORD c = second(name, 5000);
    CHECK(c == LIFE_ONE_HANDED && signalled == 1, "a second start signals the first: %lu, %d", c, signalled);
    life_one_window_close(o);
    DWORD t = GetTickCount();
    c = second(name, 300);
    CHECK(c == LIFE_ONE_BUSY && GetTickCount() - t >= 250, "no window: busy after the wait: %lu", c);
    life_one_close(o);
    c = second(name, 300);
    CHECK(c == LIFE_ONE_HELD, "the first copy gone: the next one runs: %lu", c);
    CHECK(life_set_aumid("io.github.rbstemsplus.wintest") == 0, "this process's AppUserModelID");
    PWSTR id = NULL;
    if (SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&id)) && id) {
        CHECK(!wcscmp(id, L"io.github.rbstemsplus.wintest"), "read back");
        CoTaskMemFree(id);
    } else CHECK(0, "GetCurrentProcessExplicitAppUserModelID");
}

/* ------------------------------------------------------------------ the Run value, the entry, the shortcut */

static const wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t approved_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

typedef struct { int had; DWORD type, size; BYTE data[4096]; } saved;

static void save(const wchar_t *key, const wchar_t *name, saved *s) {
    s->size = sizeof s->data;
    s->had = RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_ANY | RRF_NOEXPAND, &s->type, s->data, &s->size) == ERROR_SUCCESS;
}

static void restore(const wchar_t *key, const wchar_t *name, const saved *s) {
    if (s->had) RegSetKeyValueW(HKEY_CURRENT_USER, key, name, s->type, s->data, s->size);
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, key, name);
}

static void test_records(void) {
    wchar_t name[64];
    wide(LIFE_RUN_NAME, name, 64);
    saved run, ok;
    save(run_key, name, &run);
    save(approved_key, name, &ok);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, approved_key, name);   /* a known start: no StartupApproved value */
    const char *stub = "C:\\Users\\Test\\AppData\\Local\\Programs\\RB Stems Plus\\RB Stems Plus.exe";
    life_run r;
    CHECK(life_run_write(stub) == 0, "the Run value written");
    CHECK(life_run_read(stub, &r) == 0 && r.present && r.ours && !r.off, "ours, on");
    CHECK(life_run_read("C:\\elsewhere\\RB Stems Plus.exe", &r) == 0 && r.present && !r.ours, "another install's");
    static const BYTE off[12] = {0x03, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}, on[12] = {0x02};
    RegSetKeyValueW(HKEY_CURRENT_USER, approved_key, name, REG_BINARY, off, sizeof off);
    CHECK(life_run_read(stub, &r) == 0 && r.ours && r.off, "turned off in Settings > Apps > Startup");
    RegSetKeyValueW(HKEY_CURRENT_USER, approved_key, name, REG_BINARY, on, sizeof on);
    CHECK(life_run_read(stub, &r) == 0 && !r.off, "on again");
    RegSetKeyValueW(HKEY_CURRENT_USER, approved_key, name, REG_BINARY, off, sizeof off);
    CHECK(life_run_write(stub) == 0 && life_run_read(stub, &r) == 0 && r.ours && r.off, "the repair keeps the user's off (Q8)");
    CHECK(life_run_remove() == 0 && life_run_read(stub, &r) == 0 && !r.present && !r.off, "removed, with StartupApproved's");
    CHECK(life_run_remove() == 0, "removed again: fine");
    restore(run_key, name, &run);
    restore(approved_key, name, &ok);

    HKEY k;
    static const wchar_t entry[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RBStemsPlus";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, entry, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegCloseKey(k);
        printf("entry: an install's entry is here, its write and remove are skipped\n");
    } else {
        life_entry e = {stub, "C:\\Users\\Test\\AppData\\Local\\Programs\\RB Stems Plus", "1.5.1", NULL, 6800000};
        CHECK(life_entry_write(&e) == 0, "the entry written");
        wchar_t v[512];
        DWORD size = sizeof v, kb = 0, ks = sizeof kb;
        CHECK(RegGetValueW(HKEY_CURRENT_USER, entry, L"UninstallString", RRF_RT_REG_SZ, NULL, v, &size) == ERROR_SUCCESS
                  && wcsstr(v, L"RB Stems Plus.exe\" --uninstall"),
              "UninstallString");
        CHECK(RegGetValueW(HKEY_CURRENT_USER, entry, L"EstimatedSize", RRF_RT_REG_DWORD, NULL, &kb, &ks) == ERROR_SUCCESS && kb == 6641,
              "EstimatedSize %lu", kb);
        size = sizeof v;
        CHECK(RegGetValueW(HKEY_CURRENT_USER, entry, L"Publisher", RRF_RT_REG_SZ, NULL, v, &size) == ERROR_FILE_NOT_FOUND, "no Publisher");
        CHECK(life_entry_remove() == 0 && RegOpenKeyExW(HKEY_CURRENT_USER, entry, 0, KEY_READ, &k) == ERROR_FILE_NOT_FOUND, "removed");
        CHECK(life_entry_remove() == 0, "removed again: fine");
    }
    CHECK(life_records_remove("io.github.rbstemsplus.wintest-none") == 0, "no notification records: fine");
    CHECK(life_records_remove("a\\b") == EINVAL, "a key path refused");

    char path[LIFE_PATH_MAX], lnk[LIFE_PATH_MAX], target[LIFE_PATH_MAX];
    CHECK(life_shortcut_path(path, sizeof path) == 0 && strstr(path, "\\Start Menu\\Programs\\RB Stems Plus.lnk"), "%s", path);
    wchar_t tmp[MAX_PATH], exe[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    to8(tmp, lnk, sizeof lnk);
    strcat(lnk, "rbsp-wintest.lnk");
    to8(exe, target, sizeof target);
    CHECK(life_shortcut_write(lnk, target, "io.github.rbstemsplus.wintest") == 0, "the shortcut written");
    wchar_t wl[LIFE_PATH_MAX];
    wide(lnk, wl, LIFE_PATH_MAX);
    CHECK(GetFileAttributesW(wl) != INVALID_FILE_ATTRIBUTES, "it is there");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IPropertyStore *ps = NULL;
    static const GUID ps_iid = {0x886d8eeb, 0x8cf2, 0x4446, {0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99}};
    static const PROPERTYKEY aumid = {{0x9f4c2855, 0x9f79, 0x4b39, {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}}, 5};
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(wl, NULL, GPS_DEFAULT, &ps_iid, (void **)&ps))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        CHECK(SUCCEEDED(IPropertyStore_GetValue(ps, &aumid, &pv)) && pv.vt == VT_LPWSTR && !wcscmp(pv.pwszVal, L"io.github.rbstemsplus.wintest"),
              "its AppUserModelID");
        PropVariantClear(&pv);
        IPropertyStore_Release(ps);
    } else CHECK(0, "the shortcut's property store");
    CoUninitialize();
    CHECK(life_shortcut_write(lnk, target, "io.github.rbstemsplus.wintest") == 0, "written again over it");
    CHECK(life_shortcut_remove(lnk) == 0 && GetFileAttributesW(wl) == INVALID_FILE_ATTRIBUTES, "removed");
    CHECK(life_shortcut_remove(lnk) == 0, "removed again: fine");
}

/* ------------------------------------------------------------------ volumes */

static int remote_drive(const char *p) {
    wchar_t root[4] = {(wchar_t)(unsigned char)p[0], L':', L'\\', 0};
    return GetDriveTypeW(root) == DRIVE_REMOTE;
}

static void test_volumes(const char *external, const char *share) {
    vol_facts *list = calloc(64, sizeof *list);
    int n = vol_list(list, 64), systems = 0;
    CHECK(n >= 1, "the volume list: %d", n);
    for (int i = 0; i < n; i++) {
        vol_facts f;
        char line[1024];
        int e = vol_facts_read(list[i].guid, 1, &f);
        vol_facts_line(e ? NULL : &f, line, sizeof line);
        printf("%s mount=%s label=%s %s\n", list[i].guid, list[i].mount, list[i].label, line);
        if (!e && f.system) {
            systems++;
            CHECK(!strcmp(f.fstype, "ntfs") && f.disks == 1 && f.nodes >= 2 && !vol_external(&f) && vol_refusal(&f) == VOL_REFUSE_INTERNAL,
                  "the system volume: %s", line);
        }
    }
    CHECK(systems == 1, "one system volume: %d", systems);
    free(list);

    wchar_t wt[MAX_PATH];
    char t[MAX_PATH * 3], g[2048], d[2048], back[2048];
    GetTempPathW(MAX_PATH, wt);
    to8(wt, t, sizeof t);
    if (strlen(t) > 3 && t[strlen(t) - 1] == '\\') t[strlen(t) - 1] = 0;
    CHECK(vol_guid_path(t, g, sizeof g) == 0 && vol_guid_root(g), "%%TEMP%% as a GUID path: %s", g);
    CHECK(vol_dos_path(t, d, sizeof d) == 0 && d[1] == ':' && d[2] == '\\', "and as a drive path: %s", d);
    CHECK(vol_display_path(g, back, sizeof back) == 0 && !_stricmp(back, d), "the GUID path shown: %s vs %s", back, d);
    vol_facts f;
    CHECK(vol_facts_of(t, &f) == 0 && f.system, "%%TEMP%% is on the system volume");
    CHECK(vol_guid_path("C:\\no\\such\\folder", g, sizeof g) == ENOENT, "a missing folder");
    if (external) {
        char line[1024];
        int e = vol_facts_of(external, &f);
        vol_facts_line(e ? NULL : &f, line, sizeof line);
        printf("external %s: %s\n", external, line);
        CHECK(!e && vol_external(&f) && (vol_refusal(&f) == VOL_OK || vol_refusal(&f) == VOL_REFUSE_FILE_SYSTEM), "the external drive: %s", line);
    }

    /* network drives (Q2): a UNC path by its form; a mapped share by its drive type and final path */
    CHECK(vol_facts_of("\\\\localhost\\no-such-share\\x", &f) == 0 && f.drive_type == VOL_DRIVE_REMOTE
              && vol_refusal(&f) == VOL_REFUSE_NETWORK,
          "a UNC path: the network refusal");
    if (share) {
        int e = vol_facts_of(share, &f);
        CHECK(!e && vol_refusal(&f) == VOL_REFUSE_NETWORK, "the mapped share %s: %d, refusal %d", share, e, vol_refusal(&f));
        CHECK(remote_drive(share), "%s is a network drive", share);
        e = vol_dos_path(share, d, sizeof d);
        CHECK(e == EXDEV || (e == 0 && remote_drive(d)), "its drive path is a network path: %d", e);
    }

    /* WM_DEVICECHANGE's broadcasts, as the window gets them */
    DEV_BROADCAST_VOLUME v;
    memset(&v, 0, sizeof v);
    v.dbcv_size = sizeof v;
    v.dbcv_devicetype = DBT_DEVTYP_VOLUME;
    v.dbcv_unitmask = 1u << 4;
    char ev[512];
    CHECK(vol_device_message(DBT_DEVICEARRIVAL, (intptr_t)&v, ev, sizeof ev) == 1 && !strcmp(ev, "{\"ev\":\"mount\",\"path\":\"E:\\\\\"}\n"),
          "%s", ev);
    CHECK(vol_device_message(DBT_DEVICEQUERYREMOVE, (intptr_t)&v, ev, sizeof ev) == 0, "a query");
    union { DEV_BROADCAST_DEVICEINTERFACE_W d; BYTE b[512]; } di;
    memset(&di, 0, sizeof di);
    static const GUID volume_class = {0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
    di.d.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    di.d.dbcc_classguid = volume_class;
    static const wchar_t iface[] = L"\\\\?\\STORAGE#Volume#x";
    memcpy(di.b + offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name), iface, sizeof iface);
    di.d.dbcc_size = (DWORD)(offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name) + sizeof iface);
    CHECK(vol_device_message(DBT_DEVICEREMOVECOMPLETE, (intptr_t)&di, ev, sizeof ev) == 1 && strstr(ev, "\"unmount\"")
              && strstr(ev, "STORAGE#Volume#x"),
          "%s", ev);
    di.d.dbcc_classguid.Data1 ^= 1;
    CHECK(vol_device_message(DBT_DEVICEREMOVECOMPLETE, (intptr_t)&di, ev, sizeof ev) == 0, "another class");
}

/* ------------------------------------------------------------------ report, CriticalWork */

static critical_work work;
static DWORD WINAPI step(LPVOID p) {
    Sleep((DWORD)(uintptr_t)p);
    critical_end(&work);
    return 0;
}

static void test_report(void) {
    char desk[REPORT_PATH_MAX], roots[REPORT_WER_ROOTS][REPORT_PATH_MAX];
    CHECK(report_desktop(desk, sizeof desk) == 0 && desk[1] == ':', "the Desktop: %s", desk);
    int n = report_wer_roots(roots);
    CHECK(n == 4 && strstr(roots[0], "\\WER\\ReportArchive") && strstr(roots[1], "\\WER\\ReportQueue")
              && strstr(roots[2], "\\WER\\ReportArchive") && strstr(roots[3], "\\WER\\ReportQueue"),
          "WER's archives and queues, the account's and the machine's: %d", n);
    const char *r[REPORT_WER_ROOTS] = {roots[0], roots[1], roots[2], roots[3]};
    report_wer w[REPORT_WER_MAX];
    int found = 0, got = report_wer_find(r, n, plat_now_wall_s() - REPORT_WER_DAYS * 86400, w, REPORT_WER_MAX, &found);
    printf("WER: %d of %d reports\n", got, found);
    for (int i = 0; i < got; i++) {
        char *t = NULL;
        size_t l = 0;
        CHECK(report_wer_text(w[i].path, REPORT_TEXT_MAX, &t, &l) == 0 && t && strstr(t, "Version="), "%s", w[i].path);
        free(t);
    }
    char *log = NULL;
    size_t len = 0;
    CHECK(report_event_log(REPORT_EVENT_MINUTES, REPORT_EVENT_LINES, REPORT_TEXT_MAX, &log, &len) == 0 && log, "the event log");
    printf("event log: %zu bytes\n", len);
    CHECK(!log || (!strstr(log, "S-1-5-21") && !strstr(log, "<Computer>D")), "no SID, no PC name");
    free(log);

    critical_init(&work);
    critical_begin(&work);
    DWORD t = GetTickCount();
    CHECK(critical_wait_idle(&work, 100) == 0 && GetTickCount() - t >= 90, "CriticalWork times out");
    HANDLE th = CreateThread(NULL, 0, step, (LPVOID)(uintptr_t)50, 0, NULL);
    CHECK(critical_wait_idle(&work, 5000) == 1, "and returns when the step ends");
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--exit")) return 0;
    if (argc >= 4 && !strcmp(argv[1], "--second")) {
        life_one *o = NULL;
        int r = life_one_take(argv[2], (uint32_t)atoi(argv[3]), &o);
        life_one_close(o);
        return r;
    }
    int elevated = 0;
    const char *external = NULL, *share = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--elevated")) elevated = 1;
        else if (!strcmp(argv[i], "--external") && i + 1 < argc) external = argv[++i];
        else if (!strcmp(argv[i], "--share") && i + 1 < argc) share = argv[++i];
    }
    test_os(elevated);
    test_one_copy();
    test_records();
    test_volumes(external, share);
    test_report();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
