/* Phase 0 helper: writes a minidump (threads, stacks, modules, thread info) of a running process.
 *   dumpnow.exe PID FILE
 * Used to see where rekordbox's shutdown waits when its stems engine failed to start. dbghelp is
 * an OS component, loaded from System32. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t **argv) {
    if (argc != 3) { fprintf(stderr, "usage: dumpnow PID FILE\n"); return 2; }
    DWORD pid = (DWORD)wcstoul(argv[1], NULL, 10);
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) { printf("open failed %lu\n", GetLastError()); return 1; }
    HANDLE f = CreateFileW(argv[2], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("create failed %lu\n", GetLastError()); return 1; }
    BOOL ok = MiniDumpWriteDump(p, pid, f, MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory, NULL, NULL, NULL);
    printf("dump %s (%lu)\n", ok ? "written" : "FAILED", ok ? 0 : GetLastError());
    CloseHandle(f);
    return !ok;
}
