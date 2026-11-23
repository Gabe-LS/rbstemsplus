/* notify.h on Windows: one thread with a message-only window, started at the first notification. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include "notify.h"

#include <stdlib.h>
#include <string.h>

enum { WM_NT_TRAY = WM_APP + 1, WM_NT_SHOW = WM_APP + 2, NT_ID = 1 };

typedef struct {
    wchar_t title[64], body[256];
} nt_notice;

static PVOID volatile nt_hwnd;
static INIT_ONCE nt_once = INIT_ONCE_STATIC_INIT;
static HICON nt_icon;   /* the exe's own icon, else the system's application icon */

static void nt_remove(HWND h) {
    NOTIFYICONDATAW d;
    memset(&d, 0, sizeof d);
    d.cbSize = sizeof d;
    d.hWnd = h;
    d.uID = NT_ID;
    Shell_NotifyIconW(NIM_DELETE, &d);
}

static LRESULT CALLBACK nt_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NT_SHOW) {
        nt_notice *n = (nt_notice *)l;
        NOTIFYICONDATAW d;
        memset(&d, 0, sizeof d);
        d.cbSize = sizeof d;
        d.hWnd = h;
        d.uID = NT_ID;
        d.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
        d.uCallbackMessage = WM_NT_TRAY;
        d.hIcon = nt_icon;
        wcsncpy(d.szTip, n->title, sizeof d.szTip / sizeof d.szTip[0] - 1);
        wcsncpy(d.szInfoTitle, n->title, sizeof d.szInfoTitle / sizeof d.szInfoTitle[0] - 1);
        wcsncpy(d.szInfo, n->body, sizeof d.szInfo / sizeof d.szInfo[0] - 1);
        d.dwInfoFlags = NIIF_INFO;
        nt_remove(h);   /* one at a time: the newest says what is so now */
        if (Shell_NotifyIconW(NIM_ADD, &d)) {
            d.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &d);
        }
        free(n);
        return 0;
    }
    if (msg == WM_NT_TRAY) {
        switch (LOWORD(l)) {
        case NIN_BALLOONUSERCLICK:
        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONHIDE: nt_remove(h); break;
        default: break;
        }
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static DWORD WINAPI nt_thread(LPVOID p) {
    HANDLE ready = p;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = nt_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"RBSP.Core.Notify";
    HWND h = NULL;
    if (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    if (h) {
        wchar_t exe[MAX_PATH];
        DWORD k = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (k > 0 && k < MAX_PATH) ExtractIconExW(exe, 0, NULL, &nt_icon, 1);
        if (!nt_icon) nt_icon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    }
    InterlockedExchangePointer(&nt_hwnd, h);
    SetEvent(ready);
    CloseHandle(ready);
    if (!h) return 0;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

static BOOL WINAPI nt_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ready) return FALSE;
    HANDLE self = GetCurrentProcess(), worker_ready;
    /* The worker can still signal after the caller's wait times out. */
    if (!DuplicateHandle(self, ready, self, &worker_ready, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        CloseHandle(ready);
        return FALSE;
    }
    HANDLE t = CreateThread(NULL, 0, nt_thread, worker_ready, 0, NULL);
    if (!t) {
        CloseHandle(worker_ready);
        CloseHandle(ready);
        return FALSE;
    }
    DWORD wait = WaitForSingleObject(ready, 10000);
    CloseHandle(ready);
    CloseHandle(t);   /* it runs as long as the process */
    return wait == WAIT_TIMEOUT || InterlockedCompareExchangePointer(&nt_hwnd, NULL, NULL) != NULL;
}

/* UTF-8 into out (cap units with its NUL), cut at cap - 1 units (a surrogate pair kept whole) */
static void nt_wide(const char *s, wchar_t *out, int cap) {
    out[0] = 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (!w) return;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) == n) {
        int k = n - 1 < cap - 1 ? n - 1 : cap - 1;
        if (k > 0 && k < n - 1 && w[k - 1] >= 0xD800 && w[k - 1] <= 0xDBFF) k--;
        memcpy(out, w, (size_t)k * sizeof *w);
        out[k] = 0;
    }
    free(w);
}

int notify_show(const char *title, const char *body) {
    InitOnceExecuteOnce(&nt_once, nt_init, NULL, NULL);
    HWND h = InterlockedCompareExchangePointer(&nt_hwnd, NULL, NULL);
    if (!h) return -1;
    nt_notice *n = calloc(1, sizeof *n);
    if (!n) return -1;
    nt_wide(title ? title : "", n->title, (int)(sizeof n->title / sizeof n->title[0]));
    nt_wide(body ? body : "", n->body, (int)(sizeof n->body / sizeof n->body[0]));
    if (!PostMessageW(h, WM_NT_SHOW, 0, (LPARAM)n)) {
        free(n);
        return -1;
    }
    return 0;
}
