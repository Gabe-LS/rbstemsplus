/* The start stub's pure rules (stub.h). No plat.h and no core: the stub stays a small exe. */
#include "stub.h"

#include <string.h>

/* 1 to 9 ASCII digits from s, *end after them */
static int digits(const char *s, const char **end) {
    int n = 0;
    while (s[n] >= '0' && s[n] <= '9') n++;
    if (n < 1 || n > 9) return 0;
    *end = s + n;
    return 1;
}

int stub_version_ok(const char *s) {
    const char *p = s;
    for (int i = 0; i < 3; i++) {
        if (!digits(p, &p)) return 0;
        if (i < 2 && *p++ != '.') return 0;
    }
    return !*p;
}

int stub_current(const char *data, size_t n, wchar_t out[STUB_VERSION_MAX]) {
    if (!data) return 0;
    size_t k = n;
    if (k && data[k - 1] == '\n') k--;
    if (k && data[k - 1] == '\r') k--;
    if (!k || k >= STUB_VERSION_MAX) return 0;
    char v[STUB_VERSION_MAX];
    memcpy(v, data, k);
    v[k] = 0;
    if (strlen(v) != k || !stub_version_ok(v)) return 0;
    for (size_t i = 0; i <= k; i++) out[i] = (wchar_t)(unsigned char)v[i];   /* ASCII */
    return 1;
}

const wchar_t *stub_args(const wchar_t *cmdline) {
    const wchar_t *p = cmdline;
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"') p++;
        if (*p) p++;
    }
    while (*p && *p != L' ' && *p != L'\t') p++;
    while (*p == L' ' || *p == L'\t') p++;
    return p;
}

int stub_mode(const wchar_t *args) {
    static const wchar_t w[] = L"--watcher", ax[] = L"--after-exit";
    size_t ak = wcslen(ax);
    if (!wcsncmp(args, ax, ak) && (args[ak] == L' ' || args[ak] == L'\t')) return STUB_AFTER_EXIT;
    size_t k = wcslen(w);
    if (wcsncmp(args, w, k)) return STUB_APP;
    for (const wchar_t *p = args + k; *p; p++)
        if (*p != L' ' && *p != L'\t') return STUB_APP;
    return STUB_WATCHER;
}

int stub_after_exit_dir(const wchar_t *name) {
    static const wchar_t pre[] = L"rbsp-uninstall-";
    size_t pn = sizeof pre / sizeof pre[0] - 1;
    if (!name || wcsncmp(name, pre, pn)) return 0;
    for (size_t i = 0; i < 16; i++) {
        wchar_t c = name[pn + i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return 0;
    }
    return name[pn + 16] == 0;
}

static int append(wchar_t *out, size_t cap, size_t *at, const wchar_t *s) {
    size_t k = wcslen(s);
    if (*at + k + 1 > cap) return -1;
    memcpy(out + *at, s, k * sizeof *s);
    *at += k;
    out[*at] = 0;
    return 0;
}

int stub_target(const wchar_t *dir, const wchar_t *version, int mode, const wchar_t *arch, wchar_t *out, size_t cap) {
    size_t at = 0;
    if (!cap) return -1;
    out[0] = 0;
    if (append(out, cap, &at, dir) || append(out, cap, &at, L"\\") || append(out, cap, &at, version) || append(out, cap, &at, L"\\")) return -1;
    if (mode == STUB_WATCHER)
        return append(out, cap, &at, L"rbsp-watch-") || append(out, cap, &at, arch) || append(out, cap, &at, L".exe") ? -1 : 0;
    return append(out, cap, &at, L"RB Stems Plus.exe");
}

int stub_cmdline(const wchar_t *target, const wchar_t *args, wchar_t *out, size_t cap) {
    size_t at = 0;
    if (!cap || wcschr(target, L'"')) return -1;
    out[0] = 0;
    if (append(out, cap, &at, L"\"") || append(out, cap, &at, target) || append(out, cap, &at, L"\"")) return -1;
    if (args && *args && (append(out, cap, &at, L" ") || append(out, cap, &at, args))) return -1;
    return 0;
}
