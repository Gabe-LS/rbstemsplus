/* compat_win.c's import gate (run.sh): compat_win.c linked alone into an exe, with no -l flags,
 * so its import table is exactly what compat_win.c pulls in. Never run. */
#include "../compat.h"

int compat_pin_subject(const char *organization, const char *serial_number) {
    return organization && serial_number;
}

int main(void) {
    const compat_os *os = compat_os_windows();
    return os && os->native_machine ? (int)os->native_machine(os->ctx) : 0;
}
