/* update_win.c's import gate (run.sh): update_win.c linked with the pure rules it calls, the
 * payload code they need and common/plat_win.c (its tree's deletion), as the core DLL links them,
 * so the import table must stay within the core DLL's allowlist (KERNEL32, ADVAPI32, USER32, ntdll,
 * SHELL32, ole32 and the UCRT; winhttp.dll is loaded at run time). Never run. */
#include "../update.h"

int main(void) {
    const update_net *n = update_net_windows();
    const update_os *o = update_os_windows();
    return n && o && n->get && o->spawn ? 0 : 1;
}
