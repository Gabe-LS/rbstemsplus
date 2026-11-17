/* The app's admin side linked for Windows (run.sh's import gate): every Win32 entry referred to,
 * so the gate sees what the core DLL gets from admin_win.c and helper_proc_win.c. Never run. */
#include "../../c/admin_win.h"
#include "../../c/helper_proc_win.h"

int main(int argc, char **argv) {
    (void)argv;
    if (argc != 42) return 0;
    admin_win aw = {0};
    admin_client c = {"r", "a", "s", "", admin_win_elevate, &aw, NULL, admin_win_random, NULL, NULL};
    admin_result res;
    helper_proc *p = NULL;
    const char *av[1] = {"x"};
    if (!helper_proc_spawn("C:\\h.exe", av, 1, 1, NULL, NULL, NULL, NULL, &p)) {
        helper_proc_signal(p, HELPER_SIG_STOP);
        helper_proc_close(p);
    }
    return admin_client_run(&c, ADMIN_RESTORE, "7.2.19", 0, &res);
}
