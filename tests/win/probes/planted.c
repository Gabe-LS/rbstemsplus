/* Phase 0 spike: a planted DLL (built as ucrtbase.dll, bcrypt.dll, ...). If the loader ever maps
 * it, DllMain appends its own path and the process's to C:\rbtest\planted.log. No CRT, kernel32
 * only, so it can stand in for ucrtbase.dll itself. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)r;
    if (why != DLL_PROCESS_ATTACH) return TRUE;
    wchar_t me[MAX_PATH], exe[MAX_PATH], line[2 * MAX_PATH + 32];
    GetModuleFileNameW(h, me, MAX_PATH);
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    lstrcpyW(line, L"PLANTED ");
    lstrcatW(line, me);
    lstrcatW(line, L" loaded into ");
    lstrcatW(line, exe);
    lstrcatW(line, L"\r\n");
    HANDLE f = CreateFileW(L"C:\\rbtest\\planted.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(f, line, lstrlenW(line) * sizeof(wchar_t), &w, NULL);
        CloseHandle(f);
    }
    return TRUE;
}
