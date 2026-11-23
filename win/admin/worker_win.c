/* admin_os on Windows (worker.h): held handles, the root's protected DACL, the lock, the mapped
 * check, the load check, the worker's own removal, the pipe's lines. Static imports: KERNEL32,
 * ADVAPI32 and ntdll only (app.md §5.5's KnownDLLs rule); user32 (a KnownDLL) is loaded from
 * System32 at run time for rekordbox's windows, and only then. NT declarations mingw-w64's
 * winternl.h lacks are written from Microsoft's documentation (ntifs.h, wdm.h), as plat_win.c's.
 * Unverified until the VM run (W13a): every call here. */
#include "plat.h"
#include "plat_win.h"
#include "win_rules.h"

#include <winternl.h>
#include <tlhelp32.h>

#include "worker_win.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ NT bits */

#define WW_FileAttributeTagInformation 35
#define WW_FileRenameInformation 10
#define WW_FileRenameInformationEx 65
#define WW_FileDispositionInformation 13
#define WW_FileDispositionInformationEx 64
#define WW_CLASS(c) ((FILE_INFORMATION_CLASS)(c))
#define WW_RENAME_POSIX 0x2u
#define WW_DISPOSE_DELETE 0x1u
#define WW_DISPOSE_POSIX 0x2u
#define WW_DISPOSE_IGNORE_READONLY 0x10u
#define WW_STATUS_INVALID_INFO_CLASS 0xC0000003u
#define WW_STATUS_INVALID_PARAMETER 0xC000000Du
#define WW_STATUS_NOT_SUPPORTED 0xC00000BBu
#define WW_STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define WW_STATUS_BUFFER_OVERFLOW 0x80000005u
#define WW_SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
/* the held folders' rights and sharing (app.md §5.5, J8): READ_CONTROL for the ACL check on the
 * handle, never FILE_SHARE_DELETE */
#define WW_HOLD_ACCESS (READ_CONTROL | FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE)
#define WW_HOLD_SHARE (FILE_SHARE_READ | FILE_SHARE_WRITE)

NTSTATUS NTAPI NtQuerySecurityObject(HANDLE Handle, SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR SecurityDescriptor,
                                     ULONG Length, PULONG LengthNeeded);

typedef struct { ULONG FileAttributes, ReparseTag; } ww_tag_info;
typedef struct { ULONG Flags; HANDLE RootDirectory; ULONG FileNameLength; WCHAR FileName[1]; } ww_rename_info;
_Static_assert(offsetof(ww_rename_info, FileName) == 20 && sizeof(ww_rename_info) == 24, "FILE_RENAME_INFORMATION's x64 layout");

enum { WW_NAME_CAP = 512 };

static int unsupported(NTSTATUS s) {
    uint32_t u = (uint32_t)s;
    return u == WW_STATUS_INVALID_INFO_CLASS || u == WW_STATUS_INVALID_PARAMETER || u == WW_STATUS_NOT_SUPPORTED;
}

/* `name` relative to root, or the absolute path when root is NULL */
static int nt_open_sd(HANDLE root, const char *name, ACCESS_MASK access, ULONG share, ULONG disp, ULONG opts, int op, PSECURITY_DESCRIPTOR sd,
                      HANDLE *out) {
    uint16_t small[WW_NAME_CAP], *w = small;
    long n;
    *out = NULL;
    if (root) n = wr_name16(name, w, WW_NAME_CAP);
    else {
        if (!(w = malloc(sizeof(uint16_t) * (WR_NT_MAX + 1)))) return ENOMEM;
        n = wr_nt_path(name, w, WR_NT_MAX + 1);
    }
    int e = 0;
    if (n < 0) e = EINVAL;
    else if (n > 32767) e = ENAMETOOLONG;
    else {
        UNICODE_STRING us = {(USHORT)(n * 2), (USHORT)(n * 2), (PWSTR)w};
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, root, sd);   /* sd: a new file's own descriptor */
        IO_STATUS_BLOCK io;
        HANDLE h = NULL;
        NTSTATUS s = NtCreateFile(&h, access | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, share, disp,
                                  opts | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT, NULL, 0);
        if ((e = wr_errno_nt((uint32_t)s, op)) == 0) *out = h;
    }
    if (w != small) free(w);
    return e;
}

static int nt_open(HANDLE root, const char *name, ACCESS_MASK access, ULONG share, ULONG disp, ULONG opts, int op, HANDLE *out) {
    return nt_open_sd(root, name, access, share, disp, opts, op, NULL, out);
}

static int is_reparse(HANDLE h) {
    ww_tag_info t;
    IO_STATUS_BLOCK io;
    if (NtQueryInformationFile(h, &io, &t, sizeof t, WW_CLASS(WW_FileAttributeTagInformation))) return -1;
    return (t.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

static int to_wide(const char *s, wchar_t *out, size_t cap) { return wr_utf8_to_16(s, strlen(s), (uint16_t *)out, cap) >= 0; }

/* ------------------------------------------------------------------ held folders, files, the root */

static int ww_hold_dir(void *ctx, plat_dir parent, const char *name, plat_dir *out) {
    (void)ctx;
    HANDLE h;
    out->h = NULL;
    int e = nt_open(plat_dir_ok(parent) ? parent.h : NULL, name, WW_HOLD_ACCESS, WW_HOLD_SHARE, FILE_OPEN, FILE_DIRECTORY_FILE, WR_OP_OPEN, &h);
    if (e) return e;
    int rp = is_reparse(h);
    if (rp || !plat_win_handle_admin_only(h, 0)) {
        NtClose(h);
        return rp > 0 ? ELOOP : rp < 0 ? EIO : EACCES;
    }
    out->h = h;
    return 0;
}

static int ww_file_admin_only(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    HANDLE h;
    int e = nt_open(d.h, name, READ_CONTROL | FILE_READ_ATTRIBUTES, WW_SHARE_ALL, FILE_OPEN, FILE_NON_DIRECTORY_FILE, WR_OP_OPEN, &h);
    if (e) return e;
    int rp = is_reparse(h);
    int ok = !rp && plat_win_handle_admin_only(h, 0);
    NtClose(h);
    return ok ? 0 : rp > 0 ? ELOOP : EACCES;
}

/* the root's descriptor (app.md §5.5): owner Administrators, a protected DACL with exactly
 * SYSTEM and Administrators full and Users read and execute, each inherited by files and folders */
static const unsigned char sid_admins[] = {1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 0x02, 0, 0};
static const unsigned char sid_system[] = {1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0};
static const unsigned char sid_users[] = {1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x21, 0x02, 0, 0};

static int sid_kind(PSID s) {
    if (!s || !IsValidSid(s)) return ADMIN_SID_OTHER;
    DWORD n = GetLengthSid(s);
    if (n == sizeof sid_admins && !memcmp(s, sid_admins, n)) return ADMIN_SID_ADMINS;
    if (n == sizeof sid_system && !memcmp(s, sid_system, n)) return ADMIN_SID_SYSTEM;
    if (n == sizeof sid_users && !memcmp(s, sid_users, n)) return ADMIN_SID_USERS;
    return ADMIN_SID_OTHER;
}

static int security_of(HANDLE h, PSECURITY_DESCRIPTOR *out) {
    ULONG len = 512, need = 0;
    *out = NULL;
    for (int i = 0; i < 4; i++) {
        PSECURITY_DESCRIPTOR sd = malloc(len);
        if (!sd) return ENOMEM;
        NTSTATUS s = NtQuerySecurityObject(h, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, sd, len, &need);
        if (s == 0) {
            *out = sd;
            return 0;
        }
        free(sd);
        if ((uint32_t)s != WW_STATUS_BUFFER_TOO_SMALL && (uint32_t)s != WW_STATUS_BUFFER_OVERFLOW) return EIO;
        len = need > len ? need : len * 2;
    }
    return EIO;
}

static int descriptor_exact(HANDLE h, int lock) {
    PSECURITY_DESCRIPTOR sd;
    if (security_of(h, &sd)) return 0;
    PSID owner = NULL;
    BOOL defaulted, present = FALSE;
    PACL dacl = NULL;
    SECURITY_DESCRIPTOR_CONTROL ctl = 0;
    DWORD rev;
    int ok = 0;
    admin_ace a[8];
    size_t n = 0;
    if (GetSecurityDescriptorOwner(sd, &owner, &defaulted) && GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted)
        && GetSecurityDescriptorControl(sd, &ctl, &rev)) {
        ok = 1;
        if (present && dacl) {
            ACL_SIZE_INFORMATION si;
            if (!GetAclInformation(dacl, &si, sizeof si, AclSizeInformation) || si.AceCount > 8) ok = 0;
            for (DWORD i = 0; ok && i < si.AceCount; i++) {
                ACE_HEADER *hd;
                if (!GetAce(dacl, i, (LPVOID *)&hd)) {
                    ok = 0;
                    break;
                }
                a[n].type = hd->AceType;
                a[n].flags = hd->AceFlags;
                a[n].mask = 0;
                a[n].sid = ADMIN_SID_OTHER;
                if (hd->AceType == ACCESS_ALLOWED_ACE_TYPE || hd->AceType == ACCESS_DENIED_ACE_TYPE) {
                    ACCESS_ALLOWED_ACE *x = (ACCESS_ALLOWED_ACE *)hd;
                    a[n].mask = x->Mask;
                    a[n].sid = sid_kind((PSID)&x->SidStart);
                }
                n++;
            }
        }
        if (ok) ok = (lock ? admin_lock_acl_exact : admin_root_acl_exact)(sid_kind(owner), (ctl & SE_DACL_PROTECTED) != 0, present && dacl, a, n);
    }
    free(sd);
    return ok;
}

/* the descriptor a new root is made with */
static int root_sd(SECURITY_DESCRIPTOR *sd, PACL acl, DWORD acl_size) {
    if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) || !InitializeAcl(acl, acl_size, ACL_REVISION)) return -1;
    if (!AddAccessAllowedAceEx(acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, ADMIN_ROOT_FULL, (PSID)sid_system)
        || !AddAccessAllowedAceEx(acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, ADMIN_ROOT_FULL, (PSID)sid_admins)
        || !AddAccessAllowedAceEx(acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, ADMIN_ROOT_READ, (PSID)sid_users))
        return -1;
    if (!SetSecurityDescriptorOwner(sd, (PSID)sid_admins, FALSE) || !SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE)
        || !SetSecurityDescriptorControl(sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return -1;
    return 0;
}

static int ww_root_open(void *ctx, const char *path, int create, plat_dir *out, int *made) {
    (void)ctx;
    HANDLE h;
    out->h = NULL;
    *made = 0;
    int e = nt_open(NULL, path, WW_HOLD_ACCESS, WW_HOLD_SHARE, FILE_OPEN, FILE_DIRECTORY_FILE, WR_OP_OPEN, &h);
    int created = 0;
    if (e == ENOENT && create) {
        wchar_t w[ADMIN_PATH_MAX + 8] = L"\\\\?\\";
        SECURITY_DESCRIPTOR sd;
        union { ACL acl; unsigned char b[256]; } acl;
        if (!to_wide(path, w + 4, ADMIN_PATH_MAX) || root_sd(&sd, &acl.acl, sizeof acl)) return EINVAL;
        SECURITY_ATTRIBUTES sa = {sizeof sa, &sd, FALSE};
        /* made by this call only when CreateDirectoryW made it: one another run made meanwhile is
         * opened and stays that run's (re-review blocker A) */
        if (CreateDirectoryW(w, &sa)) created = 1;
        else if (GetLastError() != ERROR_ALREADY_EXISTS) return wr_errno_win32((uint32_t)GetLastError(), WR_OP_OPEN);
        e = nt_open(NULL, path, WW_HOLD_ACCESS, WW_HOLD_SHARE, FILE_OPEN, FILE_DIRECTORY_FILE, WR_OP_OPEN, &h);
    }
    if (e) return e;
    int rp = is_reparse(h);
    if (rp || !descriptor_exact(h, 0)) {
        NtClose(h);
        return rp > 0 ? ELOOP : EACCES;
    }
    out->h = h;
    *made = created;
    return 0;
}

static int ww_mkdir_at(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    return plat_mkdir_at(d, name);   /* inherits d's DACL; the owner is the token's default (Administrators, worker_win_init) */
}

static int dispose(HANDLE h) {
    IO_STATUS_BLOCK io;
    ULONG fl = WW_DISPOSE_DELETE | WW_DISPOSE_POSIX | WW_DISPOSE_IGNORE_READONLY;
    NTSTATUS s = NtSetInformationFile(h, &io, &fl, sizeof fl, WW_CLASS(WW_FileDispositionInformationEx));
    if (s && unsupported(s)) {
        BOOLEAN del = TRUE;
        s = NtSetInformationFile(h, &io, &del, sizeof del, WW_CLASS(WW_FileDispositionInformation));
    }
    return wr_errno_nt((uint32_t)s, WR_OP_RENAME_DELETE);
}

static int ww_rmdir_at(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    HANDLE h;
    int e = nt_open(d.h, name, DELETE | FILE_READ_ATTRIBUTES, WW_SHARE_ALL, FILE_OPEN, 0, WR_OP_RENAME_DELETE, &h);
    if (e) return e;
    e = dispose(h);
    NtClose(h);
    return e;
}

static int ww_rename_noreplace(void *ctx, plat_dir d, const char *from, const char *to) {
    (void)ctx;
    HANDLE h;
    int e = nt_open(d.h, from, DELETE | FILE_READ_ATTRIBUTES, WW_SHARE_ALL, FILE_OPEN, FILE_NON_DIRECTORY_FILE, WR_OP_RENAME_DELETE, &h);
    if (e) return e;
    union { ww_rename_info ri; unsigned char b[sizeof(ww_rename_info) + WW_NAME_CAP * 2]; } u;
    long n = wr_name16(to, (uint16_t *)u.ri.FileName, WW_NAME_CAP);
    if (n < 0) {
        NtClose(h);
        return EINVAL;
    }
    u.ri.Flags = WW_RENAME_POSIX;   /* no REPLACE_IF_EXISTS: a name there is a collision */
    u.ri.RootDirectory = d.h;
    u.ri.FileNameLength = (ULONG)n * 2;
    /* never under the struct's size: a 1-character name is 22 bytes of a 24-byte struct (VM round 8) */
    ULONG len = (ULONG)wr_info_len(offsetof(ww_rename_info, FileName), sizeof(ww_rename_info), (size_t)n);
    IO_STATUS_BLOCK io;
    NTSTATUS s = NtSetInformationFile(h, &io, &u.ri, len, WW_CLASS(WW_FileRenameInformationEx));
    if (s && unsupported(s)) {
        u.ri.Flags = 0;   /* ReplaceIfExists = FALSE */
        s = NtSetInformationFile(h, &io, &u.ri, len, WW_CLASS(WW_FileRenameInformation));
    }
    NtClose(h);
    return wr_errno_nt((uint32_t)s, WR_OP_RENAME_DELETE);   /* a name there: OBJECT_NAME_COLLISION, EEXIST */
}

/* ------------------------------------------------------------------ the lock, rekordbox */

/* the lock's own descriptor (app.md §5.5: admin-only, so no other account can open it first and
 * hold it): owner Administrators, a protected DACL of SYSTEM and Administrators full only */
static int lock_sd(SECURITY_DESCRIPTOR *sd, PACL acl, DWORD acl_size) {
    if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) || !InitializeAcl(acl, acl_size, ACL_REVISION)) return -1;
    if (!AddAccessAllowedAceEx(acl, ACL_REVISION, 0, ADMIN_ROOT_FULL, (PSID)sid_system)
        || !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, ADMIN_ROOT_FULL, (PSID)sid_admins))
        return -1;
    if (!SetSecurityDescriptorOwner(sd, (PSID)sid_admins, FALSE) || !SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE)
        || !SetSecurityDescriptorControl(sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return -1;
    return 0;
}

static int ww_lock(void *ctx, plat_dir root, void **token, int *made) {
    (void)ctx;
    SECURITY_DESCRIPTOR sd;
    union { ACL acl; unsigned char b[128]; } acl;
    *made = 0;
    if (lock_sd(&sd, &acl.acl, sizeof acl)) return EINVAL;
    /* an exclusive handle: share 0; the kernel closes it when its owner dies (no stale lock) */
    const ACCESS_MASK access = FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE;
    for (int pass = 0; pass < 2; pass++) {
        HANDLE h;
        /* made only when this call's FILE_CREATE made it; an existing one is opened */
        int e = nt_open_sd(root.h, "lock", access, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, WR_OP_OPEN, &sd, &h);
        if (!e) *made = !pass;
        else if (e == EEXIST && !pass) e = nt_open_sd(root.h, "lock", access, 0, FILE_OPEN, FILE_NON_DIRECTORY_FILE, WR_OP_OPEN, &sd, &h);
        if (e) return e;   /* a sharing violation is EBUSY: another run holds it */
        if (is_reparse(h)) {
            NtClose(h);
            return ELOOP;
        }
        if (descriptor_exact(h, 1)) {
            *token = h;
            return 0;
        }
        /* an older or a changed lock (one others could open): deleted, and made again with ours */
        e = dispose(h);
        NtClose(h);
        if (e) return e;
    }
    return EACCES;
}

static void ww_unlock(void *ctx, void *token) {
    (void)ctx;
    if (token) NtClose(token);
}

static int ww_mapped(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    HANDLE h;
    /* a mapped image refuses an open for writing its data, whatever the share mode (bridge.md §2) */
    int e = nt_open(d.h, name, FILE_WRITE_DATA, WW_SHARE_ALL, FILE_OPEN, FILE_NON_DIRECTORY_FILE, WR_OP_OPEN, &h);
    if (!e) {
        NtClose(h);
        return 0;
    }
    return e == EBUSY ? 1 : e == ENOENT ? 0 : -1;
}

/* a file held against writes, renames and removal: read access, share READ only (an open for
 * writing, or for DELETE as a rename or removal takes, fails with a sharing violation while this
 * handle is open), no reparse point */
static int ww_hold_file(void *ctx, plat_dir d, const char *name, void **token) {
    (void)ctx;
    HANDLE h;
    *token = NULL;
    int e = nt_open(d.h, name, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, WR_OP_OPEN, &h);
    if (e) return e;
    int rp = is_reparse(h);
    if (rp) {
        NtClose(h);
        return rp > 0 ? ELOOP : EIO;
    }
    *token = h;
    return 0;
}

static void ww_release_file(void *ctx, void *token) {
    (void)ctx;
    if (token) NtClose(token);
}

typedef BOOL(WINAPI *enum_windows_fn)(WNDENUMPROC, LPARAM);
typedef DWORD(WINAPI *window_pid_fn)(HWND, LPDWORD);
typedef BOOL(WINAPI *window_visible_fn)(HWND);
typedef struct {
    DWORD pids[64];
    int n, windows;
    window_pid_fn pid_of;
    window_visible_fn visible;
} ww_win_count;

static BOOL CALLBACK count_window(HWND w, LPARAM p) {
    ww_win_count *c = (ww_win_count *)p;
    DWORD pid = 0;
    if (!c->visible(w)) return TRUE;
    c->pid_of(w, &pid);
    for (int i = 0; i < c->n; i++)
        if (c->pids[i] == pid) c->windows++;
    return TRUE;
}

/* EnumWindows sees only this session's desktop: a rekordbox.exe in another session, or one whose
 * session can't be read, counts as a window (open, never closing), as native_steps.c's
 * native_rb_windows counts them; windows are matched for this session's processes only */
static int ww_rekordbox_windows(void *ctx) {
    (void)ctx;
    ww_win_count c;
    memset(&c, 0, sizeof c);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    DWORD here = 0;
    int have_here = ProcessIdToSessionId(GetCurrentProcessId(), &here), other = 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32FirstW(snap, &pe); more && c.n < 64; more = Process32NextW(snap, &pe))
        if (CompareStringOrdinal(pe.szExeFile, -1, L"rekordbox.exe", -1, TRUE) == CSTR_EQUAL) {
            DWORD sid = 0;
            if (have_here && ProcessIdToSessionId(pe.th32ProcessID, &sid) && sid == here) c.pids[c.n++] = pe.th32ProcessID;
            else other++;
        }
    CloseHandle(snap);
    if (other) return other;   /* open in another session: never still closing */
    if (!c.n) return 0;
    /* user32 is a KnownDLL; loaded only here, from System32 */
    HMODULE u = LoadLibraryExW(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!u) return -1;
    FARPROC a = GetProcAddress(u, "EnumWindows"), b = GetProcAddress(u, "GetWindowThreadProcessId"), v = GetProcAddress(u, "IsWindowVisible");
    int r = -1;
    if (a && b && v) {
        enum_windows_fn en;
        memcpy(&en, &a, sizeof en);
        memcpy(&c.pid_of, &b, sizeof c.pid_of);
        memcpy(&c.visible, &v, sizeof c.visible);
        if (en(count_window, (LPARAM)&c)) r = c.windows;
    }
    FreeLibrary(u);
    return r;
}

static int ww_load_check(void *ctx, const char *path, const char *symbol) {
    (void)ctx;
    wchar_t w[ADMIN_PATH_MAX + 64];
    if (!to_wide(path, w, sizeof w / sizeof w[0])) return -1;
    /* its own folder and System32 only, never LOAD_WITH_ALTERED_SEARCH_PATH (helper.md §1, I62) */
    HMODULE m = LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return -1;
    int r = GetProcAddress(m, symbol) ? 0 : -1;
    FreeLibrary(m);   /* before any rollback rename */
    return r;
}

/* ------------------------------------------------------------------ the worker's own file */

static int ww_self_path(void *ctx, char *out, size_t n) {
    (void)ctx;
    wchar_t w[ADMIN_PATH_MAX];
    DWORD k = GetModuleFileNameW(NULL, w, ADMIN_PATH_MAX);
    if (!k || k >= ADMIN_PATH_MAX) return -1;
    return wr_utf16_to_8((const uint16_t *)w, k, out, n) < 0 ? -1 : 0;
}

static int ww_delete_self(void *ctx, const char *self, const char *const folders[3], int *reboot) {
    worker_win *ww = ctx;
    wchar_t s[ADMIN_PATH_MAX], f[3][ADMIN_PATH_MAX], del[ADMIN_PATH_MAX + 32];
    *reboot = 0;
    if (!to_wide(self, s, ADMIN_PATH_MAX)) return EINVAL;
    for (int i = 0; i < 3; i++)
        if (!to_wide(folders[i], f[i], ADMIN_PATH_MAX)) return EINVAL;
    /* the POSIX branch: the name goes now, the image when the process ends */
    HANDLE h = CreateFileW(s, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        int e = dispose(h);
        CloseHandle(h);
        if (!e) {
            /* each folder now, or at the next restart; one that can be neither stays (said) */
            for (int i = 0; i < 3; i++)
                if (!RemoveDirectoryW(f[i])) *reboot |= MoveFileExW(f[i], NULL, MOVEFILE_DELAY_UNTIL_REBOOT) ? ADMIN_GONE_AT_RESTART : ADMIN_FOLDERS_STAY;
            return 0;
        }
    }
    /* the reboot branch (J7): renamed to a name no install uses, then it and the folders
     * scheduled, child before parent; PendingFileRenameOperations never edited by hand */
    char rnd[17];
    ww->os.random_hex(ww, rnd, 16);
    wchar_t wr[17];
    for (int i = 0; i < 17; i++) wr[i] = (wchar_t)rnd[i];
    if (_snwprintf(del, sizeof del / sizeof del[0], L"%ls.%ls.del", s, wr) < 0) return ENAMETOOLONG;
    if (!MoveFileExW(s, del, 0)) return wr_errno_win32((uint32_t)GetLastError(), WR_OP_RENAME_DELETE);
    if (!MoveFileExW(del, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) return wr_errno_win32((uint32_t)GetLastError(), WR_OP_RENAME_DELETE);
    *reboot = ADMIN_GONE_AT_RESTART;
    for (int i = 0; i < 3; i++)
        if (!MoveFileExW(f[i], NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) *reboot |= ADMIN_FOLDERS_STAY;
    return 0;
}

/* ------------------------------------------------------------------ the pipe, time, randomness */

static void ww_say(void *ctx, const char *line) {
    worker_win *ww = ctx;
    if (!ww->pipe) return;
    size_t n = strlen(line);
    char buf[ADMIN_LINE_MAX + 1];
    if (n > ADMIN_LINE_MAX - 1) n = ADMIN_LINE_MAX - 1;
    memcpy(buf, line, n);
    buf[n++] = '\n';
    DWORD put = 0;
    if (!WriteFile(ww->pipe, buf, (DWORD)n, &put, NULL)) {
        CloseHandle(ww->pipe);   /* the app is gone: the run goes on, silently (never fails the run) */
        ww->pipe = NULL;
    }
}

static void ww_sleep(void *ctx, uint32_t ms) {
    (void)ctx;
    Sleep(ms);
}

static uint64_t ww_now(void *ctx) {
    (void)ctx;
    return GetTickCount64();
}

BOOLEAN WINAPI SystemFunction036(PVOID buffer, ULONG length);   /* RtlGenRandom, ADVAPI32 */

static void ww_random_hex(void *ctx, char *out, size_t n) {
    (void)ctx;
    static const char h[] = "0123456789abcdef";
    unsigned char b[64];
    size_t k = (n + 1) / 2 > sizeof b ? sizeof b : (n + 1) / 2;
    if (!SystemFunction036(b, (ULONG)k)) {
        uint64_t t = plat_now_mono_ns() ^ ((uint64_t)GetCurrentProcessId() << 32);
        for (size_t i = 0; i < k; i++) b[i] = (unsigned char)(t >> (8 * (i & 7))) ^ (unsigned char)i;
    }
    for (size_t i = 0; i < n && i / 2 < k; i++) out[i] = h[(b[i / 2] >> (i & 1 ? 0 : 4)) & 15];
    out[n] = 0;
}

/* ------------------------------------------------------------------ the start */

/* the parent's pid (NtQueryInformationProcess's ProcessBasicInformation) */
typedef struct {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId, InheritedFromUniqueProcessId;
} ww_basic_info;

static DWORD parent_pid(void) {
    ww_basic_info bi;
    ULONG len = 0;
    if (NtQueryInformationProcess(GetCurrentProcess(), ProcessBasicInformation, &bi, sizeof bi, &len)) return 0;
    return (DWORD)bi.InheritedFromUniqueProcessId;
}

int worker_win_connect(worker_win *ww, const char *pipe) {
    wchar_t w[ADMIN_PIPE_MAX];
    ww->pipe = NULL;
    if (!admin_pipe_name_ok(pipe) || !to_wide(pipe, w, ADMIN_PIPE_MAX)) return -1;
    /* identification only: the app's server can't act as this account */
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ULONG server = 0;
    DWORD parent = parent_pid();
    if (!GetNamedPipeServerProcessId(h, &server) || !parent || server != parent) {
        CloseHandle(h);
        return -1;
    }
    ww->pipe = h;
    return 0;
}

int worker_win_elevated(void) {
    HANDLE tok;
    TOKEN_ELEVATION el = {0};
    DWORD len = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &el, sizeof el, &len);
    CloseHandle(tok);
    return ok && el.TokenIsElevated;
}

int worker_win_owner_admins(void) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &tok)) return -1;
    TOKEN_OWNER o = {(PSID)sid_admins};
    BOOL ok = SetTokenInformation(tok, TokenOwner, &o, sizeof o);
    CloseHandle(tok);
    return ok ? 0 : -1;
}

void worker_win_init(worker_win *ww) {
    ww->os = (admin_os){compat_os_windows(), ww,           ww_root_open,  ww_hold_dir,          ww_file_admin_only, ww_mkdir_at,
                        ww_rmdir_at,          ww_rename_noreplace, ww_lock, ww_unlock,         ww_mapped,          ww_rekordbox_windows,
                        ww_load_check,        ww_self_path, ww_delete_self, ww_say,             ww_sleep,           ww_now,
                        ww_random_hex,        ww_hold_file, ww_release_file};
}
