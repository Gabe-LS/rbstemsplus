/* A DLL for plat-win-test's plat_lib_open_checked cases: it only has to load and export one
 * function. Built by common/tests/win.sh next to plat-win-test.exe; copy both to the VM. */
__declspec(dllexport) int checked_dll_ok(void);
__declspec(dllexport) int checked_dll_ok(void) { return 42; }
