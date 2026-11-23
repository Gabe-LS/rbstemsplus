/* sysdesk.exe (tests/win/uia, the by-hand plan's W1): runs a program in the console session from
 * session 0, as SYSTEM on winsta0\winlogon (the secure desktop: UAC's consent prompt, the
 * sign-out block screen, LogonUI) or as the signed-in user on winsta0\default (a sign-out as
 * Start > Sign out does it). PsExec's -s -x technique, our own (FOSS rule): this process's SYSTEM
 * token duplicated, its TokenSessionId set to the console session (SeTcbPrivilege), then
 * CreateProcessAsUserW; --as user takes the session's user token (WTSQueryUserToken) instead.
 * Must run as SYSTEM (prlctl exec, or a scheduled task as SYSTEM). Test only, never shipped.
 *
 *   sysdesk [--desktop winlogon|default] [--as system|user] [--session N] [--out FILE]
 *           [--timeout SECS] -- PROGRAM [ARGS...]
 *
 * Defaults: winlogon, system, the active console session (waited for up to 30 s while there is
 * none: a sign-out or a sign-in in progress), no output file, 600 s. With --out the program's
 * stdout and stderr go to FILE, after sysdesk's own lines, and FILE.done gets the exit code at
 * the end (as hidden.exe writes it). Exit: the program's code; 124 when it ran past the timeout
 * (ended then); 120 no console session; 121 a token or privilege step failed; 122 the program
 * didn't start; 2 usage. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static HANDLE out_h = INVALID_HANDLE_VALUE;

/* one line, UTF-8, to the output file (or stderr) */
static void say(const wchar_t *fmt, ...) {
    wchar_t w[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(w, 2046, fmt, ap);
    va_end(ap);
    if (n < 0) n = 2045;
    w[n++] = L'\n';
    w[n] = 0;
    char u[6144];
    int m = WideCharToMultiByte(CP_UTF8, 0, w, n, u, (int)sizeof u, NULL, NULL);
    if (m <= 0) return;
    DWORD wr;
    HANDLE h = out_h != INVALID_HANDLE_VALUE ? out_h : GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, u, (DWORD)m, &wr, NULL);
}

static int usage(void) {
    say(L"usage: sysdesk [--desktop winlogon|default] [--as system|user] [--session N] [--out FILE] [--timeout SECS] -- PROGRAM [ARGS...]");
    return 2;
}

/* one argument appended to a command line, quoted as CommandLineToArgvW reads it back */
static int append_arg(wchar_t *buf, size_t cap, size_t *len, const wchar_t *a) {
    size_t n = *len;
#define PUT(c) do { if (n + 1 >= cap) return 0; buf[n++] = (c); } while (0)
    if (n) PUT(L' ');
    int plain = *a != 0 && wcspbrk(a, L" \t\n\v\"") == NULL;
    if (plain) {
        for (; *a; a++) PUT(*a);
    } else {
        PUT(L'"');
        for (;;) {
            size_t bs = 0;
            while (*a == L'\\') { a++; bs++; }
            if (!*a) { for (size_t i = 0; i < 2 * bs; i++) PUT(L'\\'); break; }
            if (*a == L'"') { for (size_t i = 0; i < 2 * bs + 1; i++) PUT(L'\\'); PUT(L'"'); }
            else { for (size_t i = 0; i < bs; i++) PUT(L'\\'); PUT(*a); }
            a++;
        }
        PUT(L'"');
    }
    buf[n] = 0;
    *len = n;
    return 1;
#undef PUT
}

static int enable_privilege(const wchar_t *name) {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return 0;
    TOKEN_PRIVILEGES tp = {0};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    int ok = LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)
          && AdjustTokenPrivileges(t, FALSE, &tp, sizeof tp, NULL, NULL)
          && GetLastError() == ERROR_SUCCESS;   /* ERROR_NOT_ALL_ASSIGNED: we don't hold it */
    CloseHandle(t);
    return ok;
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *desktop = L"winlogon", *as = L"system", *out = NULL;
    DWORD session = 0xFFFFFFFF;
    int timeout = 600, i;
    for (i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--")) { i++; break; }
        if (i + 1 >= argc) return usage();
        if (!wcscmp(argv[i], L"--desktop")) desktop = argv[++i];
        else if (!wcscmp(argv[i], L"--as")) as = argv[++i];
        else if (!wcscmp(argv[i], L"--out")) out = argv[++i];
        else if (!wcscmp(argv[i], L"--session")) session = (DWORD)wcstoul(argv[++i], NULL, 10);
        else if (!wcscmp(argv[i], L"--timeout")) timeout = _wtoi(argv[++i]);
        else return usage();
    }
    if (i >= argc || timeout < 1 || timeout > 3600) return usage();
    if (wcscmp(desktop, L"winlogon") && wcscmp(desktop, L"default")) return usage();
    int as_user = !wcscmp(as, L"user");
    if (!as_user && wcscmp(as, L"system")) return usage();

    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    if (out) {
        out_h = CreateFileW(out, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out_h == INVALID_HANDLE_VALUE) { fwprintf(stderr, L"sysdesk: can't write %ls (%lu)\n", out, GetLastError()); return 2; }
    }

    int code = 121;
    HANDLE self = NULL, tok = NULL, utok = NULL, nul = INVALID_HANDLE_VALUE;
    void *env = NULL;
    wchar_t *cmd = NULL;
    PROCESS_INFORMATION pi = {0};

    if (session == 0xFFFFFFFF) {
        for (int t = 0; t < 60 && (session = WTSGetActiveConsoleSessionId()) == 0xFFFFFFFF; t++) Sleep(500);
        if (session == 0xFFFFFFFF) { say(L"sysdesk: no console session"); code = 120; goto done; }
    }
    /* SeTcb for TokenSessionId and WTSQueryUserToken, the other two for CreateProcessAsUserW */
    if (!enable_privilege(L"SeTcbPrivilege")) { say(L"sysdesk: SeTcbPrivilege isn't ours (%lu): run as SYSTEM", GetLastError()); goto done; }
    enable_privilege(L"SeAssignPrimaryTokenPrivilege");
    enable_privilege(L"SeIncreaseQuotaPrivilege");
    if (as_user) {
        if (!WTSQueryUserToken(session, &utok)) { say(L"sysdesk: no user token for session %lu (%lu)", session, GetLastError()); goto done; }
        if (!DuplicateTokenEx(utok, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &tok)) { say(L"sysdesk: DuplicateTokenEx (%lu)", GetLastError()); goto done; }
    } else {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &self)
            || !DuplicateTokenEx(self, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &tok)) {
            say(L"sysdesk: our token couldn't be duplicated (%lu)", GetLastError()); goto done;
        }
        if (!SetTokenInformation(tok, TokenSessionId, &session, sizeof session)) { say(L"sysdesk: TokenSessionId %lu (%lu)", session, GetLastError()); goto done; }
    }
    if (!CreateEnvironmentBlock(&env, tok, FALSE)) env = NULL;

    size_t cap = 32768, len = 0;
    cmd = calloc(cap, sizeof *cmd);
    if (!cmd) goto done;
    for (int k = i; k < argc; k++)
        if (!append_arg(cmd, cap, &len, argv[k])) { say(L"sysdesk: the command line is too long"); code = 2; goto done; }

    wchar_t desk[32];
    swprintf(desk, 32, L"winsta0\\%ls", desktop);
    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    si.lpDesktop = desk;
    if (out_h != INVALID_HANDLE_VALUE) {
        nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = out_h;
        si.hStdError = out_h;
    }
    say(L"sysdesk: session %lu, %ls, as %ls: %ls", session, desk, as, cmd);
    if (!CreateProcessAsUserW(tok, NULL, cmd, NULL, NULL, out_h != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                              env, NULL, &si, &pi)) {
        say(L"sysdesk: the program didn't start (%lu)", GetLastError()); code = 122; goto done;
    }
    if (WaitForSingleObject(pi.hProcess, (DWORD)timeout * 1000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 5000);
        say(L"sysdesk: still running after %d s: ended", timeout);
        code = 124;
    } else {
        DWORD c = 1;
        GetExitCodeProcess(pi.hProcess, &c);
        code = (int)c;
        say(L"sysdesk: exit %lu", c);
    }

done:
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (env) DestroyEnvironmentBlock(env);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (tok) CloseHandle(tok);
    if (utok) CloseHandle(utok);
    if (self) CloseHandle(self);
    free(cmd);
    if (out_h != INVALID_HANDLE_VALUE) {
        CloseHandle(out_h);
        out_h = INVALID_HANDLE_VALUE;
        wchar_t d[MAX_PATH + 8];
        swprintf(d, MAX_PATH + 8, L"%ls.done", out);
        FILE *f = _wfopen(d, L"w");
        if (f) { fprintf(f, "%d\n", code); fclose(f); }
    }
    return code;
}
