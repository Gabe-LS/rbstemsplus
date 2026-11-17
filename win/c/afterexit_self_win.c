/* afterexit_self_win.c: the Windows half of the after-exit copy's own-folder check (afterexit.h's
 * ax_win_dir_ok; review UN1, UQ5), on KERNEL32 alone, so the core DLL (afterexit_win.c's
 * w_self_ok, after ax_is_temp_name on the folder's name) and the Windows test exe
 * (tests/afterexit_wintest.c, in the VM) run the same code. */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "afterexit.h"

#include <wchar.h>

/* a long path's form, its trailing separators dropped: 0, or -1 */
static int long_form(const wchar_t *p, wchar_t *out, DWORD cap) {
    DWORD n = GetLongPathNameW(p, out, cap);
    if (n == 0 || n >= cap) return -1;
    while (n > 0 && out[n - 1] == L'\\') out[--n] = 0;
    return n ? 0 : -1;
}

int ax_win_dir_ok(const wchar_t *self_dir) {
    static wchar_t tmp[AX_PATH_MAX], lt[AX_PATH_MAX], lp[AX_PATH_MAX], par[AX_PATH_MAX];
    if (!self_dir || !self_dir[0]) return 0;
    /* its own parent, cut from the same string (review U53) */
    const wchar_t *cut = wcsrchr(self_dir, L'\\');
    if (!cut || cut == self_dir || !cut[1] || (size_t)(cut - self_dir) + 2 > AX_PATH_MAX) return 0;
    wmemcpy(par, self_dir, (size_t)(cut - self_dir) + 1);
    par[cut - self_dir + 1] = 0;
    DWORD tn = GetTempPathW(AX_PATH_MAX, tmp);
    if (tn == 0 || tn >= AX_PATH_MAX || long_form(tmp, lt, AX_PATH_MAX) || long_form(par, lp, AX_PATH_MAX)) return 0;
    if (CompareStringOrdinal(lt, -1, lp, -1, TRUE) != CSTR_EQUAL) return 0;
    DWORD at = GetFileAttributesW(self_dir);
    return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) && !(at & FILE_ATTRIBUTE_REPARSE_POINT);
}
