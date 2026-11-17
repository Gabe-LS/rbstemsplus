/* lifecycle.h on Windows: the one-copy mutex and window, the .NET minimum, the token, another
 * process's start time, the Run value and StartupApproved, the Apps & features entry, the
 * shortcut and the AppUserModelID. Imports KERNEL32, ADVAPI32 (registry, token), USER32 (the
 * window, foreground rights), SHELL32 (known folders, AppUserModelID), ole32 (the shortcut's COM)
 * and ntdll (the process list): all on the core DLL's allowlist (app.md §7, I81). Everything
 * writes to HKCU and the user's own folders: no call needs elevation. Unverified until the VM
 * run (W13a): every call here. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#  define COBJMACROS
#endif
#include <windows.h>
#include <winternl.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>

#include "lifecycle.h"
#include "win_rules.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */

static int wide(const char *s, wchar_t *out, size_t cap) {
    return wr_utf8_to_16(s, strlen(s), (uint16_t *)out, cap) < 0 ? -1 : 0;
}

static int to8(const wchar_t *w, char *out, size_t cap) {
    return wr_utf16_to_8((const uint16_t *)w, wcslen(w), out, cap) < 0 ? -1 : 0;
}

static int reg_errno(LONG r) {
    return r == ERROR_SUCCESS ? 0 : r == ERROR_FILE_NOT_FOUND ? ENOENT : wr_errno_win32((uint32_t)r, WR_OP_OPEN);
}

static const wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t approved_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
static const wchar_t uninstall_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RBStemsPlus";   /* LIFE_ENTRY_KEY */

/* ------------------------------------------------------------------ one copy */

struct life_one {
    HANDLE mutex;
    DWORD thread;            /* the thread that took (owns) the mutex and makes the window */
    HWND wnd;
    UINT msg;
    ATOM cls_atom;
    wchar_t cls[160];
    void (*on_signal)(void *ctx);
    void *ctx;
    volatile LONG open;      /* the window acknowledges signals (cleared before DestroyWindow) */
};

/* the window class and the signal's name, from the mutex's: "Local\x" -> "x.window", "x.signal" */
static int names(const char *name, wchar_t *cls, size_t cap, UINT *msg) {
    const char *b = strchr(name, '\\');
    char c[128], s[128];
    b = b ? b + 1 : name;
    if (!*b || strlen(b) + 8 >= sizeof c) return -1;
    memcpy(c, b, strlen(b) + 1);
    memcpy(s, b, strlen(b) + 1);
    strcat(c, ".window");
    strcat(s, ".signal");
    wchar_t ws[128];
    if (wide(c, cls, cap) || wide(s, ws, 128)) return -1;
    *msg = RegisterWindowMessageW(ws);
    return *msg ? 0 : -1;
}

typedef struct {
    HANDLE mutex;
    const wchar_t *name, *cls;
    UINT msg;
    HWND found;
} one_os;

static int os_create(void *ctx) {
    one_os *o = ctx;
    o->mutex = CreateMutexW(NULL, TRUE, o->name);
    if (!o->mutex) return LIFE_MUTEX_FAILED;
    return GetLastError() == ERROR_ALREADY_EXISTS ? LIFE_MUTEX_EXISTS : LIFE_MUTEX_OWNED;
}

static int os_wait(void *ctx, uint32_t ms) {
    one_os *o = ctx;
    DWORD w = WaitForSingleObject(o->mutex, ms);
    return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED ? LIFE_MUTEX_OWNED : w == WAIT_TIMEOUT ? LIFE_MUTEX_TIMEOUT : LIFE_MUTEX_FAILED;
}

static uint32_t os_find(void *ctx) {
    one_os *o = ctx;
    DWORD pid = 0;
    o->found = FindWindowExW(HWND_MESSAGE, NULL, o->cls, NULL);
    if (o->found) GetWindowThreadProcessId(o->found, &pid);
    return pid != GetCurrentProcessId() ? pid : 0;
}

static int os_hand_off(void *ctx, uint32_t pid) {
    one_os *o = ctx;
    AllowSetForegroundWindow(pid);   /* fails when this start holds no rights itself: it still signals */
    /* handed only when the first copy's open window answers 'RBSP' within 2 s (Q3): a hung copy,
     * or one whose window is going, leaves this start waiting for the mutex */
    DWORD_PTR answer = 0;
    LRESULT sent = SendMessageTimeoutW(o->found, o->msg, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, LIFE_ONE_SEND_MS, &answer);
    return life_one_acked(sent != 0, (uintptr_t)answer) ? 0 : -1;
}

static uint64_t os_now(void *ctx) {
    (void)ctx;
    return GetTickCount64();
}

int life_one_take(const char *name, uint32_t wait_ms, life_one **out) {
    *out = NULL;
    wchar_t wname[160], cls[160];
    UINT msg;
    if (wide(name, wname, 160) || names(name, cls, 160, &msg)) return LIFE_ONE_FAILED;
    one_os o = {NULL, wname, cls, msg, NULL};
    life_one_ops ops = {&o, os_create, os_wait, os_find, os_hand_off, os_now};
    int r = life_one_copy(&ops, wait_ms);
    if (r == LIFE_ONE_HELD) {
        life_one *one = calloc(1, sizeof *one);
        if (one) {
            one->mutex = o.mutex;
            one->thread = GetCurrentThreadId();
            one->msg = msg;
            memcpy(one->cls, cls, sizeof cls);
            *out = one;
            return r;
        }
        r = LIFE_ONE_FAILED;
    }
    if (o.mutex) {
        ReleaseMutex(o.mutex);                              /* owned only when the memory ran out */
        CloseHandle(o.mutex);
    }
    return r;
}

static LRESULT WINAPI one_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
    life_one *o = (life_one *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (o && m == o->msg) {
        if (!InterlockedCompareExchange(&o->open, 0, 0)) return 0;   /* going: no acknowledgement */
        if (o->on_signal) o->on_signal(o->ctx);                      /* posts its work: never blocks the sender */
        return (LRESULT)LIFE_ONE_ACK;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* the module (the core DLL, or a test exe) this code is in: the window class is registered for it */
static const char module_anchor = 0;
static HINSTANCE this_module(void) {
    HMODULE m = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&module_anchor, &m);
    return m;
}

int life_one_window(life_one *o, void (*on_signal)(void *ctx), void *ctx) {
    if (!o || o->wnd) return EINVAL;
    o->on_signal = on_signal;
    o->ctx = ctx;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = one_proc;
    wc.hInstance = this_module();
    wc.lpszClassName = o->cls;
    o->cls_atom = RegisterClassExW(&wc);
    if (!o->cls_atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return wr_errno_win32((uint32_t)GetLastError(), WR_OP_OPEN);
    o->wnd = CreateWindowExW(0, o->cls, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, o);
    if (!o->wnd) return wr_errno_win32((uint32_t)GetLastError(), WR_OP_OPEN);
    InterlockedExchange(&o->open, 1);
    return 0;
}

void life_one_window_close(life_one *o) {
    if (o && o->wnd && GetCurrentThreadId() == o->thread) {   /* DestroyWindow works on its own thread only */
        InterlockedExchange(&o->open, 0);   /* no acknowledgement from here on (Q3) */
        DestroyWindow(o->wnd);
        o->wnd = NULL;
    }
}

void life_one_close(life_one *o) {
    if (!o) return;
    /* off the owner thread: nothing at all (review S56). The window may still be there and points
     * to o, and the mutex's last handle would drop its name while the owner still holds it; both
     * stay until the owner thread or the process ends */
    if (GetCurrentThreadId() != o->thread) return;
    life_one_window_close(o);
    if (o->mutex) {
        ReleaseMutex(o->mutex);
        CloseHandle(o->mutex);
    }
    free(o);
}

/* ------------------------------------------------------------------ the minimum, the token */

int64_t life_net_release(void) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                      &k) != ERROR_SUCCESS)
        return -1;
    DWORD v = 0, size = sizeof v, type = 0;
    LONG r = RegQueryValueExW(k, L"Release", NULL, &type, (BYTE *)&v, &size);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_DWORD && size == sizeof v ? (int64_t)v : -1;
}

int life_os_check(int net) {
    uint32_t major, minor, build;
    if (life_os_version(&major, &minor, &build)) major = build = 0;
    return life_os_verdict(major, build, net, net ? life_net_release() : -1);
}

int life_token_check(void) {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return LIFE_TOKEN_UNKNOWN;
    TOKEN_ELEVATION_TYPE type = 0;
    TOKEN_ELEVATION el;
    DWORD got = 0;
    int etype = GetTokenInformation(t, TokenElevationType, &type, sizeof type, &got) ? (int)type : 0;
    int elevated = GetTokenInformation(t, TokenElevation, &el, sizeof el, &got) ? el.TokenIsElevated != 0 : -1;
    union { TOKEN_USER u; BYTE b[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } user;
    int service = 0;
    if (GetTokenInformation(t, TokenUser, &user, sizeof user, &got))
        service = IsWellKnownSid(user.u.User.Sid, WinLocalSystemSid) || IsWellKnownSid(user.u.User.Sid, WinLocalServiceSid)
                  || IsWellKnownSid(user.u.User.Sid, WinNetworkServiceSid);
    else
        etype = 0;                                          /* unknown: refused */
    CloseHandle(t);
    return life_token_verdict(etype, elevated, service);
}

/* ------------------------------------------------------------------ another process's start */

#define STATUS_INFO_LENGTH_MISMATCH_ ((LONG)0xC0000004)

/* from the system's process list, which needs no access to the process */
static int64_t listed_start(uint32_t pid) {
    ULONG size = 1 << 18, need = 0;
    for (int tries = 0; tries < 6; tries++) {
        BYTE *buf = malloc(size);
        if (!buf) return -2;
        NTSTATUS s = NtQuerySystemInformation(SystemProcessInformation, buf, size, &need);
        if (s == STATUS_INFO_LENGTH_MISMATCH_) {
            free(buf);
            size = need > size ? need + (1 << 16) : size * 2;
            continue;
        }
        if (s < 0) { free(buf); return -2; }
        int64_t r = -1;
        for (BYTE *p = buf;;) {
            const SYSTEM_PROCESS_INFORMATION *i = (const SYSTEM_PROCESS_INFORMATION *)p;
            if ((uint32_t)(ULONG_PTR)i->UniqueProcessId == pid) {
                r = wr_filetime_s(i->CreateTime.QuadPart);
                break;
            }
            if (!i->NextEntryOffset) break;
            p += i->NextEntryOffset;
        }
        free(buf);
        return r;
    }
    return -2;
}

int64_t life_process_start(uint32_t pid) {
    if (pid == 0) return -1;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!h) return GetLastError() == ERROR_INVALID_PARAMETER ? -1 : listed_start(pid);
    int64_t r = -2;
    FILETIME c, x, k, u;
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) r = -1;   /* exited; a handle somewhere keeps its pid */
    else if (GetProcessTimes(h, &c, &x, &k, &u)) r = wr_filetime_s((int64_t)(((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime));
    CloseHandle(h);
    return r;
}

/* ------------------------------------------------------------------ the Run value, StartupApproved */

int life_run_read(const char *stub, life_run *out) {
    memset(out, 0, sizeof *out);
    wchar_t name[64], value[LIFE_PATH_MAX + 32];
    if (wide(LIFE_RUN_NAME, name, 64)) return EINVAL;
    DWORD size = sizeof value - sizeof(wchar_t);
    memset(value, 0, sizeof value);
    LONG r = RegGetValueW(HKEY_CURRENT_USER, run_key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, value, &size);
    if (r == ERROR_MORE_DATA) out->present = 1;             /* too long to be ours */
    else if (r == ERROR_SUCCESS) {
        char v8[(LIFE_PATH_MAX + 32) * 3];
        out->present = 1;
        out->ours = !to8(value, v8, sizeof v8) && life_run_names(v8, stub);
    } else if (r != ERROR_FILE_NOT_FOUND) return reg_errno(r);
    unsigned char b[64];
    size = sizeof b;
    r = RegGetValueW(HKEY_CURRENT_USER, approved_key, name, RRF_RT_REG_BINARY, NULL, b, &size);
    if (r == ERROR_SUCCESS) out->off = out->present && life_approved_off(b, size);
    else if (r != ERROR_FILE_NOT_FOUND && r != ERROR_MORE_DATA) return reg_errno(r);
    return 0;
}

static int set_sz(HKEY k, const wchar_t *name, const char *v8) {
    size_t n = strlen(v8) + 1;
    wchar_t *w = malloc(sizeof(wchar_t) * n);
    if (!w) return ENOMEM;
    int e = wide(v8, w, n) ? EINVAL : reg_errno(RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)w, (DWORD)((wcslen(w) + 1) * sizeof(wchar_t))));
    free(w);
    return e;
}

static int set_dword(HKEY k, const wchar_t *name, DWORD v) { return reg_errno(RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v)); }

int life_run_write(const char *stub) {
    char v[LIFE_PATH_MAX + 32];
    wchar_t name[64];
    if (life_run_value(stub, v, sizeof v) || wide(LIFE_RUN_NAME, name, 64)) return EINVAL;
    HKEY k;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return reg_errno(r);
    int e = set_sz(k, name, v);
    RegCloseKey(k);
    return e;
}

static int delete_value(const wchar_t *key, const wchar_t *name) {
    LONG r = RegDeleteKeyValueW(HKEY_CURRENT_USER, key, name);
    return r == ERROR_FILE_NOT_FOUND ? 0 : reg_errno(r);
}

int life_run_remove(void) {
    wchar_t name[64];
    if (wide(LIFE_RUN_NAME, name, 64)) return EINVAL;
    int a = delete_value(run_key, name), b = delete_value(approved_key, name);
    return a ? a : b;
}

/* ------------------------------------------------------------------ the Apps & features entry */

int life_entry_write(const life_entry *e) {
    char unin[LIFE_PATH_MAX + 32];
    if (!e->stub || !e->location || !e->version || life_uninstall_string(e->stub, unin, sizeof unin)) return EINVAL;
    HKEY k;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, uninstall_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return reg_errno(r);
    int x = set_sz(k, L"DisplayName", LIFE_DISPLAY_NAME);
    if (!x) x = set_sz(k, L"DisplayVersion", e->version);
    if (!x) x = set_sz(k, L"DisplayIcon", e->stub);
    if (!x) x = set_sz(k, L"InstallLocation", e->location);
    if (!x) x = set_sz(k, L"UninstallString", unin);
    if (!x && e->publisher && e->publisher[0]) x = set_sz(k, L"Publisher", e->publisher);
    else if (!x) {                                          /* none: an earlier one goes, so the entry is exact */
        LONG d = RegDeleteValueW(k, L"Publisher");
        x = d == ERROR_FILE_NOT_FOUND ? 0 : reg_errno(d);
    }
    if (!x) x = set_dword(k, L"NoModify", 1);
    if (!x) x = set_dword(k, L"NoRepair", 1);
    if (!x) x = set_dword(k, L"EstimatedSize", life_size_kb(e->size_bytes));
    RegCloseKey(k);
    return x;
}

static int delete_tree(const wchar_t *key) {
    LONG r = RegDeleteTreeW(HKEY_CURRENT_USER, key);
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) return reg_errno(r);
    r = RegDeleteKeyW(HKEY_CURRENT_USER, key);              /* RegDeleteTree leaves an empty key on some versions */
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND ? 0 : reg_errno(r);
}

int life_entry_remove(void) { return delete_tree(uninstall_key); }

int life_records_remove(const char *aumid) {
    static const char base[] = "Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings\\";
    char k8[512];
    wchar_t k[512];
    if (!aumid[0] || strchr(aumid, '\\') || strlen(base) + strlen(aumid) >= sizeof k8) return EINVAL;
    memcpy(k8, base, sizeof base - 1);
    memcpy(k8 + sizeof base - 1, aumid, strlen(aumid) + 1);
    return wide(k8, k, 512) ? EINVAL : delete_tree(k);
}

/* ------------------------------------------------------------------ the AppUserModelID, the shortcut */

int life_set_aumid(const char *aumid) {
    wchar_t w[128];
    return wide(aumid, w, 128) || FAILED(SetCurrentProcessExplicitAppUserModelID(w)) ? -1 : 0;
}

/* FOLDERID_Programs {A77F5D77-2E2B-44C3-A6A2-ABA601054A51}, CLSID_ShellLink, IID_IShellLinkW,
 * IID_IPersistFile, IID_IPropertyStore and PKEY_AppUserModel_ID, written out: no uuid library */
static const GUID programs_folder = {0xa77f5d77, 0x2e2b, 0x44c3, {0xa6, 0xa2, 0xab, 0xa6, 0x01, 0x05, 0x4a, 0x51}};
static const GUID shell_link_clsid = {0x00021401, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID shell_link_iid = {0x000214f9, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID persist_file_iid = {0x0000010b, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID property_store_iid = {0x886d8eeb, 0x8cf2, 0x4446, {0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99}};
static const PROPERTYKEY aumid_key = {{0x9f4c2855, 0x9f79, 0x4b39, {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}}, 5};

int life_shortcut_path(char *out, size_t n) {
    PWSTR p = NULL;
    int r = -1;
    if (SHGetKnownFolderPath(&programs_folder, KF_FLAG_DONT_VERIFY, NULL, &p) == S_OK && p) {
        size_t k;
        if (!to8(p, out, n) && (k = strlen(out)) + 1 + sizeof LIFE_SHORTCUT_NAME <= n) {
            out[k] = '\\';
            memcpy(out + k + 1, LIFE_SHORTCUT_NAME, sizeof LIFE_SHORTCUT_NAME);
            r = 0;
        }
    }
    if (p) CoTaskMemFree(p);
    if (r && n) out[0] = 0;
    return r;
}

/* the shortcut saved at tmp: target, its folder as the working folder, aumid */
static int save_link(const wchar_t *tmp, const wchar_t *target, const wchar_t *aumid) {
    IShellLinkW *sl = NULL;
    IPropertyStore *ps = NULL;
    IPersistFile *pf = NULL;
    int e = EIO;
    if (FAILED(CoCreateInstance(&shell_link_clsid, NULL, CLSCTX_INPROC_SERVER, &shell_link_iid, (void **)&sl))) return EIO;
    wchar_t dir[LIFE_PATH_MAX];
    size_t l = wcslen(target);
    while (l > 0 && target[l - 1] != L'\\') l--;
    if (l == 0 || l >= LIFE_PATH_MAX) goto done;
    memcpy(dir, target, (l - 1) * sizeof(wchar_t));
    dir[l - 1] = 0;
    if (FAILED(IShellLinkW_SetPath(sl, target)) || FAILED(IShellLinkW_SetWorkingDirectory(sl, dir))) goto done;
    if (FAILED(IShellLinkW_QueryInterface(sl, &property_store_iid, (void **)&ps))) goto done;
    PROPVARIANT pv;
    memset(&pv, 0, sizeof pv);
    size_t al = (wcslen(aumid) + 1) * sizeof(wchar_t);
    pv.vt = VT_LPWSTR;
    pv.pwszVal = CoTaskMemAlloc(al);
    if (!pv.pwszVal) goto done;
    memcpy(pv.pwszVal, aumid, al);
    HRESULT hr = IPropertyStore_SetValue(ps, &aumid_key, &pv);
    PropVariantClear(&pv);
    if (FAILED(hr) || FAILED(IPropertyStore_Commit(ps))) goto done;
    if (FAILED(IShellLinkW_QueryInterface(sl, &persist_file_iid, (void **)&pf))) goto done;
    if (FAILED(IPersistFile_Save(pf, tmp, TRUE))) goto done;
    e = 0;
done:
    if (pf) IPersistFile_Release(pf);
    if (ps) IPropertyStore_Release(ps);
    IShellLinkW_Release(sl);
    return e;
}

int life_shortcut_write(const char *lnk, const char *target, const char *aumid) {
    wchar_t wl[LIFE_PATH_MAX], wt[LIFE_PATH_MAX], tmp[LIFE_PATH_MAX + 16], wa[128];
    if (wide(lnk, wl, LIFE_PATH_MAX) || wide(target, wt, LIFE_PATH_MAX) || wide(aumid, wa, 128)) return EINVAL;
    size_t l = wcslen(wl);
    memcpy(tmp, wl, l * sizeof(wchar_t));
    memcpy(tmp + l, L".rbsp-new", sizeof L".rbsp-new");
    HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(ci) && ci != RPC_E_CHANGED_MODE) return EIO;   /* CHANGED_MODE: COM is already on (MTA): use it */
    int e = save_link(tmp, wt, wa);
    if (SUCCEEDED(ci)) CoUninitialize();
    if (!e && !MoveFileExW(tmp, wl, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) e = wr_errno_win32((uint32_t)GetLastError(), WR_OP_RENAME_DELETE);
    if (e) DeleteFileW(tmp);
    return e;
}

int life_shortcut_remove(const char *lnk) {
    wchar_t w[LIFE_PATH_MAX];
    if (wide(lnk, w, LIFE_PATH_MAX)) return EINVAL;
    if (DeleteFileW(w)) return 0;
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 0 : wr_errno_win32(e, WR_OP_RENAME_DELETE);
}
