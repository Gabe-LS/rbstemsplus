/* Phase 0 spike, G1 item 6 (app.md §5.5, release.md §3.4): the admin worker's import rule.
 * Built like the worker (-mwindows -municode, UCRT, -ffunction-sections --gc-sections, a
 * requireAdministrator manifest), it uses the CRT, makes SetDefaultDllDirectories its first call,
 * loads bcrypt, wintrust, crypt32 and version only through LoadLibraryExW(SEARCH_SYSTEM32), and
 * writes every loaded module's full path to the report file named on its command line. Run next
 * to planted ucrtbase.dll, bcrypt.dll, ... (planted.c): none of them may load. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, PWSTR cmd, int show) {
    (void)a; (void)b; (void)show;
    BOOL dd = SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    while (*cmd == L' ') cmd++;
    if (*cmd == L'"') { cmd++; wchar_t *q = wcschr(cmd, L'"'); if (q) *q = 0; }
    FILE *f = _wfopen(*cmd ? cmd : L"C:\\rbtest\\workerspike.txt", L"w");
    if (!f) return 2;
    fprintf(f, "SetDefaultDllDirectories -> %d\n", dd);
    static const wchar_t *const libs[] = {L"bcrypt.dll", L"wintrust.dll", L"crypt32.dll", L"version.dll"};
    for (size_t i = 0; i < sizeof libs / sizeof *libs; i++) {
        HMODULE m = LoadLibraryExW(libs[i], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        wchar_t p[MAX_PATH] = L"";
        if (m) GetModuleFileNameW(m, p, MAX_PATH);
        fwprintf(f, L"LoadLibraryExW(%ls, SEARCH_SYSTEM32) -> %ls\n", libs[i], m ? p : L"FAILED");
    }
    wchar_t *buf = malloc(64 * sizeof *buf); /* the CRT heap works */
    swprintf(buf, 64, L"crt ok %d", (int)wcslen(L"abc"));
    fwprintf(f, L"%ls\n", buf);
    free(buf);
    HMODULE mods[256]; DWORD need = 0;
    K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need);
    for (DWORD i = 0; i < need / sizeof *mods && i < 256; i++) {
        wchar_t p[MAX_PATH];
        if (GetModuleFileNameW(mods[i], p, MAX_PATH)) fwprintf(f, L"module %ls\n", p);
    }
    fclose(f);
    return 0;
}
