/* spike.exe: loads spike.dll by its full path beside it and runs spike_run (for the VM: lane W) */
#include <stdio.h>
#include <windows.h>

int main(void) {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, p, MAX_PATH);
    while (n && p[n - 1] != L'\\') n--;
    wcscpy(p + n, L"spike.dll");
    HMODULE h = LoadLibraryExW(p, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) { printf("blocks spike: can't load spike.dll (%lu)\n", GetLastError()); return 2; }
    int (*run)(void) = (int (*)(void))(void *)GetProcAddress(h, "spike_run");
    int r = run ? run() : 99;
    printf("blocks spike: %s (%d)\n", r ? "FAIL" : "ok", r);
    return r != 0;
}
