/* compat_os on Windows (compat.h): the uninstall entries, the AlphaTheta pin, rekordbox's
 * processes and the native machine. KERNEL32 and ADVAPI32 are imported; wintrust.dll and
 * crypt32.dll aren't KnownDLLs, so they are loaded at the first pin by their full System32 path
 * with LOAD_LIBRARY_SEARCH_SYSTEM32 (app.md §5.5, §7's allowlist). Unverified until the VM run
 * (W13a): every call here. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <tlhelp32.h>

#include "compat.h"

#include <string.h>

/* ------------------------------------------------------------------ UTF-8 and UTF-16 */

static int to_utf8(const wchar_t *w, char *out, size_t cap) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out, (int)cap, NULL, NULL);
    if (n <= 0) {
        if (cap) out[0] = 0;
        return 0;
    }
    return 1;
}

static int to_wide(const char *s, wchar_t *out, int cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap) > 0;
}

/* ------------------------------------------------------------------ the uninstall entries */

static void reg_string(HKEY k, const wchar_t *name, char *out, size_t cap) {
    wchar_t buf[COMPAT_PATH_MAX];
    DWORD size = sizeof buf;
    out[0] = 0;
    /* REG_SZ, or REG_EXPAND_SZ as written (not expanded: a path under Program Files has no variable) */
    if (RegGetValueW(k, NULL, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, buf, &size) == ERROR_SUCCESS)
        to_utf8(buf, out, cap);
}

static int prefix_ci(const wchar_t *s, const wchar_t *p) {
    size_t n = wcslen(p);
    return wcslen(s) >= n && CompareStringOrdinal(s, (int)n, p, (int)n, TRUE) == CSTR_EQUAL;
}

/* review RC4: *incomplete when a view's root is there but can't be opened, a subkey can't be
 * enumerated or opened (one deleted between the two is gone, not missed: the change notice moves
 * the generation anyway), or max is reached */
static int win_uninstall_entries(void *ctx, compat_uninstall *out, int max, int *incomplete) {
    (void)ctx;
    static const REGSAM views[2] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    int n = 0;
    *incomplete = 0;
    for (int v = 0; v < 2 && n < max; v++) {
        HKEY root;
        LONG ro = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | views[v], &root);
        if (ro) {
            if (ro != ERROR_FILE_NOT_FOUND) *incomplete = 1;
            continue;
        }
        for (DWORD i = 0; n < max; i++) {
            wchar_t name[256];
            DWORD len = 256;
            LONG r = RegEnumKeyExW(root, i, name, &len, NULL, NULL, NULL, NULL);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS) {
                *incomplete = 1;
                continue;
            }
            HKEY k;
            LONG ko = RegOpenKeyExW(root, name, 0, KEY_READ | views[v], &k);
            if (ko) {
                if (ko != ERROR_FILE_NOT_FOUND) *incomplete = 1;
                continue;
            }
            wchar_t dn[256];
            DWORD dsize = sizeof dn;
            if (RegGetValueW(k, NULL, L"DisplayName", RRF_RT_REG_SZ, NULL, dn, &dsize) != ERROR_SUCCESS) dn[0] = 0;
            if (prefix_ci(name, L"Pioneer rekordbox") || prefix_ci(name, L"rekordbox") || prefix_ci(dn, L"rekordbox")) {
                compat_uninstall *e = &out[n];
                memset(e, 0, sizeof *e);
                e->view = v == 0 ? COMPAT_VIEW_64 : COMPAT_VIEW_32;
                to_utf8(name, e->key, sizeof e->key);
                to_utf8(dn, e->display_name, sizeof e->display_name);
                reg_string(k, L"DisplayVersion", e->display_version, sizeof e->display_version);
                reg_string(k, L"UninstallString", e->uninstall_string, sizeof e->uninstall_string);
                reg_string(k, L"DisplayIcon", e->display_icon, sizeof e->display_icon);
                reg_string(k, L"InstallLocation", e->install_location, sizeof e->install_location);
                n++;
            }
            RegCloseKey(k);
        }
        RegCloseKey(root);
    }
    if (n >= max) *incomplete = 1;   /* more may follow */
    return n;
}

/* Both Uninstall views watched (review HF6): one event per view, armed with
 * RegNotifyChangeKeyValue (the subtree, names, values and security descriptors, not tied to the
 * arming thread; review RC4: a subkey that can't be opened is skipped, so its ACL counts too); a
 * signalled event moves the count and is armed again. 0 when the watch couldn't be set up or
 * re-armed: no cache then. A spurious signal (a key closed, a thread gone) only costs one walk. */
#ifndef REG_NOTIFY_THREAD_AGNOSTIC
#  define REG_NOTIFY_THREAD_AGNOSTIC 0x10000000L
#endif
static struct {
    SRWLOCK lock;
    int armed, failed;
    HKEY key[2];
    HANDLE ev[2];
    uint64_t gen;
} un_watch = {SRWLOCK_INIT, 0, 0, {NULL, NULL}, {NULL, NULL}, 0};

static int un_arm(int v) {
    return RegNotifyChangeKeyValue(un_watch.key[v], TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_SECURITY | REG_NOTIFY_THREAD_AGNOSTIC,
                                   un_watch.ev[v], TRUE) == ERROR_SUCCESS;
}

static void un_drop(void) {
    for (int v = 0; v < 2; v++) {
        if (un_watch.key[v]) RegCloseKey(un_watch.key[v]);
        if (un_watch.ev[v]) CloseHandle(un_watch.ev[v]);
        un_watch.key[v] = NULL;
        un_watch.ev[v] = NULL;
    }
    un_watch.armed = 0;
    un_watch.failed = 1;
}

uint64_t compat_uninstall_generation(void) {
    static const REGSAM views[2] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    AcquireSRWLockExclusive(&un_watch.lock);
    if (!un_watch.armed && !un_watch.failed) {
        int ok = 1;
        for (int v = 0; v < 2 && ok; v++) {
            ok = !RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_NOTIFY | views[v],
                                &un_watch.key[v])
                 && (un_watch.ev[v] = CreateEventW(NULL, FALSE, FALSE, NULL)) != NULL && un_arm(v);
        }
        if (ok) {
            un_watch.armed = 1;
            un_watch.gen = 1;
        } else {
            un_drop();
        }
    }
    if (un_watch.armed) {
        for (int v = 0; v < 2 && un_watch.armed; v++) {
            if (WaitForSingleObject(un_watch.ev[v], 0) != WAIT_OBJECT_0) continue;
            un_watch.gen++;
            if (!un_arm(v)) un_drop();
        }
    }
    uint64_t g = un_watch.armed ? un_watch.gen : 0;
    ReleaseSRWLockExclusive(&un_watch.lock);
    return g;
}

/* ------------------------------------------------------------------ the AlphaTheta pin */

typedef LONG(WINAPI *verify_fn)(HWND, GUID *, LPVOID);
typedef CRYPT_PROVIDER_DATA *(WINAPI *provdata_fn)(HANDLE);
typedef CRYPT_PROVIDER_SGNR *(WINAPI *signer_fn)(CRYPT_PROVIDER_DATA *, DWORD, BOOL, DWORD);
typedef DWORD(WINAPI *name_fn)(PCCERT_CONTEXT, DWORD, DWORD, void *, LPWSTR, DWORD);

static struct {
    verify_fn verify;
    provdata_fn provdata;
    signer_fn signer;
    name_fn name;
} wt;
static INIT_ONCE wt_once = INIT_ONCE_STATIC_INIT;

#define PROC(m, name) ((void (*)(void))GetProcAddress((m), (name)))

static HMODULE system_dll(const wchar_t *file) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    size_t f = wcslen(file);
    if (n == 0 || n + 1 + f + 1 > MAX_PATH) return NULL;
    path[n] = L'\\';
    memcpy(path + n + 1, file, (f + 1) * sizeof *file);
    return LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

static BOOL WINAPI wt_load(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    HMODULE w = system_dll(L"wintrust.dll"), c = system_dll(L"crypt32.dll");
    if (!w || !c) return TRUE;
    verify_fn v = (verify_fn)PROC(w, "WinVerifyTrust");
    provdata_fn p = (provdata_fn)PROC(w, "WTHelperProvDataFromStateData");
    signer_fn s = (signer_fn)PROC(w, "WTHelperGetProvSignerFromChain");
    name_fn nm = (name_fn)PROC(c, "CertGetNameStringW");
    if (v && p && s && nm) wt.verify = v, wt.provdata = p, wt.signer = s, wt.name = nm;
    return TRUE;
}

/* the leaf's subject attribute oid as UTF-8 */
static void subject_attr(PCCERT_CONTEXT cert, const char *oid, char *out, size_t cap) {
    wchar_t buf[256];
    out[0] = 0;
    if (wt.name(cert, CERT_NAME_ATTR_TYPE, 0, (void *)(uintptr_t)oid, buf, 256) > 1) to_utf8(buf, out, cap);
}

static int win_pin(void *ctx, const char *path, plat_file f) {
    (void)ctx;
    InitOnceExecuteOnce(&wt_once, wt_load, NULL, NULL);
    if (!wt.verify) return COMPAT_PIN_ERROR;
    wchar_t wpath[COMPAT_PATH_MAX];
    if (!to_wide(path, wpath, COMPAT_PATH_MAX)) return COMPAT_PIN_ERROR;
    WINTRUST_FILE_INFO file;
    memset(&file, 0, sizeof file);
    file.cbStruct = sizeof file;
    file.pcwszFilePath = wpath;
    file.hFile = (HANDLE)f.h;   /* the handle compat.c read the file through (its share modes kept:
                                 * rekordbox's updater is never blocked, app.md §5.7) */
    WINTRUST_DATA data;
    memset(&data, 0, sizeof data);
    data.cbStruct = sizeof data;
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;              /* offline gigs: no revocation check */
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG st = wt.verify((HWND)INVALID_HANDLE_VALUE, &action, &data);
    DWORD why = GetLastError();   /* right after the call: what TRUST_E_NOSIGNATURE means */
    /* NO only for a real verdict (a valid signature by someone else, none at all, or a broken or
     * untrusted one); anything else (a file that couldn't be read, a missing provider...) is
     * ERROR: no answer, so no offer is made on it */
    int r = COMPAT_PIN_ERROR;
    if (st == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA *prov = wt.provdata(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR *sgnr = prov ? wt.signer(prov, 0, FALSE, 0) : NULL;
        if (sgnr && sgnr->csCertChain > 0 && sgnr->pasCertChain && sgnr->pasCertChain[0].pCert) {
            char org[128], serial[64];
            subject_attr(sgnr->pasCertChain[0].pCert, szOID_ORGANIZATION_NAME, org, sizeof org);
            subject_attr(sgnr->pasCertChain[0].pCert, szOID_DEVICE_SERIAL_NUMBER, serial, sizeof serial);
            r = compat_pin_subject(org, serial) ? COMPAT_PIN_OK : COMPAT_PIN_NO;
        }
    } else if (st == (LONG)TRUST_E_NOSIGNATURE) {
        if (why == (DWORD)TRUST_E_NOSIGNATURE || why == (DWORD)TRUST_E_SUBJECT_FORM_UNKNOWN || why == (DWORD)TRUST_E_PROVIDER_UNKNOWN)
            r = COMPAT_PIN_NO;   /* really unsigned (else the file couldn't be read) */
    } else if (st == (LONG)TRUST_E_BAD_DIGEST || st == (LONG)TRUST_E_EXPLICIT_DISTRUST || st == (LONG)CERT_E_UNTRUSTEDROOT
               || st == (LONG)CERT_E_CHAINING || st == (LONG)CERT_E_EXPIRED || st == (LONG)CERT_E_WRONG_USAGE
               || st == (LONG)TRUST_E_CERT_SIGNATURE || st == (LONG)CRYPT_E_SECURITY_SETTINGS) {
        r = COMPAT_PIN_NO;
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    wt.verify((HWND)INVALID_HANDLE_VALUE, &action, &data);
    return r;
}

/* ------------------------------------------------------------------ processes, machines */

static uint16_t process_machine(HANDLE h) {
    PROCESS_MACHINE_INFORMATION info;
    if (GetProcessInformation(h, ProcessMachineTypeInfo, &info, sizeof info)) return info.ProcessMachine;
    USHORT pm = 0, nm = 0;
    if (IsWow64Process2(h, &pm, &nm)) return pm != IMAGE_FILE_MACHINE_UNKNOWN ? pm : nm;
    return COMPAT_MACHINE_UNKNOWN;
}

static int win_processes(void *ctx, compat_proc *out, int max) {
    (void)ctx;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    int n = 0;
    for (BOOL more = Process32FirstW(snap, &pe); more && n < max; more = Process32NextW(snap, &pe)) {
        /* only rekordbox's own (rekordbox.exe, Upmgr rekordbox.exe, Uninstall rekordbox.exe...) */
        const wchar_t *e = pe.szExeFile;
        int ours = 0;
        for (const wchar_t *p = e; *p && !ours; p++) ours = prefix_ci(p, L"rekordbox");
        if (!ours) continue;
        compat_proc *c = &out[n++];
        memset(c, 0, sizeof *c);
        to_utf8(e, c->name, sizeof c->name);
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (h) {
            wchar_t path[COMPAT_PATH_MAX];
            DWORD len = COMPAT_PATH_MAX;
            if (QueryFullProcessImageNameW(h, 0, path, &len)) to_utf8(path, c->path, sizeof c->path);
            c->machine = process_machine(h);
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return n;
}

static uint16_t win_native_machine(void *ctx) {
    (void)ctx;
    USHORT pm = 0, nm = 0;
    return IsWow64Process2(GetCurrentProcess(), &pm, &nm) ? nm : COMPAT_MACHINE_UNKNOWN;
}

static const compat_os windows_os = {win_uninstall_entries, win_pin, win_processes, win_native_machine, NULL};

const compat_os *compat_os_windows(void) { return &windows_os; }
