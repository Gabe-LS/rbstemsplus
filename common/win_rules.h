/* plat_win.c's rules that need no Windows call (bridge.md §1, T3b-T3d), as pure C: the NTSTATUS
 * and Win32 error maps (I83, r4:I47), the admin-only ACL rule (r4:I48), strict UTF-8 <-> UTF-16,
 * the NT path of an absolute UTF-8 path, one-component names, the mount-prefix match and the
 * FILETIME clock. plat_win.c #includes this file; common/tests/win_rules.c runs every rule on the
 * Mac, one fixture per mapping. Everything is static: nothing here is part of plat.h.
 *
 * Numbers are Microsoft's public ones (ntstatus.h, winerror.h, winnt.h), written out so the file
 * compiles without windows.h. */
#ifndef RBSP_WIN_RULES_H
#define RBSP_WIN_RULES_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- errors */

/* NTSTATUS values the map names */
#define WR_STATUS_SUCCESS 0x00000000u
#define WR_STATUS_STOPPED_ON_SYMLINK 0x8000002Du
#define WR_STATUS_NO_SUCH_DEVICE 0xC000000Eu
#define WR_STATUS_NO_SUCH_FILE 0xC000000Fu
#define WR_STATUS_NO_MEMORY 0xC0000017u
#define WR_STATUS_ACCESS_DENIED 0xC0000022u
#define WR_STATUS_OBJECT_NAME_INVALID 0xC0000033u
#define WR_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define WR_STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define WR_STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define WR_STATUS_CRC_ERROR 0xC000003Fu
#define WR_STATUS_SHARING_VIOLATION 0xC0000043u
#define WR_STATUS_FILE_LOCK_CONFLICT 0xC0000054u
#define WR_STATUS_DELETE_PENDING 0xC0000056u
#define WR_STATUS_PRIVILEGE_NOT_HELD 0xC0000061u
#define WR_STATUS_DISK_FULL 0xC000007Fu
#define WR_STATUS_FILE_INVALID 0xC0000098u
#define WR_STATUS_INSUFFICIENT_RESOURCES 0xC000009Au
#define WR_STATUS_DEVICE_NOT_CONNECTED 0xC000009Du
#define WR_STATUS_MEDIA_WRITE_PROTECTED 0xC00000A2u
#define WR_STATUS_DEVICE_NOT_READY 0xC00000A3u
#define WR_STATUS_FILE_IS_A_DIRECTORY 0xC00000BAu
#define WR_STATUS_NOT_SAME_DEVICE 0xC00000D4u
#define WR_STATUS_NAME_TOO_LONG 0xC0000106u
#define WR_STATUS_DIRECTORY_NOT_EMPTY 0xC0000101u
#define WR_STATUS_NOT_A_DIRECTORY 0xC0000103u
#define WR_STATUS_TOO_MANY_OPENED_FILES 0xC000011Fu
#define WR_STATUS_CANNOT_DELETE 0xC0000121u
#define WR_STATUS_COMMITMENT_LIMIT 0xC000012Du
#define WR_STATUS_IO_DEVICE_ERROR 0xC0000185u
#define WR_STATUS_UNTRUSTED_MOUNT_POINT 0xC00004BCu

/* what the failed call was: rename and delete map a few statuses differently (bridge.md §1) */
enum { WR_OP_OPEN, WR_OP_RENAME_DELETE };

/* The errno of an NTSTATUS (bridge.md §1, I83, r4:I47). NT_SUCCESS (success or informational) is
 * 0. A warning is a failure here (EIO unless listed): the call didn't do what was asked, and the
 * callers that expect one (STATUS_NO_MORE_FILES, STATUS_BUFFER_OVERFLOW) test for it before mapping.
 * On every operation a sharing violation, a lock conflict and a
 * pending delete are EBUSY. For rename and delete only, ACCESS_DENIED and CANNOT_DELETE are EBUSY
 * too: without POSIX semantics (exFAT, FAT) another process's transient open returns them; on an
 * open or a create ACCESS_DENIED stays EACCES (an ACL's refusal). UNTRUSTED_MOUNT_POINT is ELOOP:
 * under Redirection Guard (an sshd session's processes, pc-round 2026-11-16) Windows won't follow
 * a junction a non-admin made, a refusal at a reparse point like STOPPED_ON_SYMLINK. Anything not
 * listed is EIO. */
static inline int wr_errno_nt(uint32_t s, int op) {
    if (s == WR_STATUS_STOPPED_ON_SYMLINK) return ELOOP;   /* a warning-severity status that fails */
    if (!(s >> 31)) return 0;                               /* NT_SUCCESS */
    switch (s) {
    case WR_STATUS_OBJECT_NAME_NOT_FOUND:
    case WR_STATUS_OBJECT_PATH_NOT_FOUND:
    case WR_STATUS_NO_SUCH_FILE: return ENOENT;
    case WR_STATUS_OBJECT_NAME_INVALID: return EINVAL;
    case WR_STATUS_NAME_TOO_LONG: return ENAMETOOLONG;
    case WR_STATUS_MEDIA_WRITE_PROTECTED: return EROFS;
    case WR_STATUS_DEVICE_NOT_READY:
    case WR_STATUS_DEVICE_NOT_CONNECTED:
    case WR_STATUS_NO_SUCH_DEVICE:
    case WR_STATUS_FILE_INVALID: return ENXIO;              /* the drive went, or was dismounted */
    case WR_STATUS_CRC_ERROR:
    case WR_STATUS_IO_DEVICE_ERROR: return EIO;
    case WR_STATUS_DISK_FULL: return ENOSPC;
    case WR_STATUS_OBJECT_NAME_COLLISION: return EEXIST;
    case WR_STATUS_NOT_A_DIRECTORY: return ENOTDIR;
    case WR_STATUS_UNTRUSTED_MOUNT_POINT: return ELOOP;    /* Redirection Guard refused a junction */
    case WR_STATUS_FILE_IS_A_DIRECTORY: return EISDIR;
    case WR_STATUS_DIRECTORY_NOT_EMPTY: return ENOTEMPTY;
    case WR_STATUS_INSUFFICIENT_RESOURCES:
    case WR_STATUS_NO_MEMORY:
    case WR_STATUS_COMMITMENT_LIMIT: return ENOMEM;         /* a miss, never lose() */
    case WR_STATUS_TOO_MANY_OPENED_FILES: return EMFILE;    /* a miss, never lose() */
    case WR_STATUS_NOT_SAME_DEVICE: return EXDEV;
    case WR_STATUS_PRIVILEGE_NOT_HELD: return EPERM;
    case WR_STATUS_SHARING_VIOLATION:
    case WR_STATUS_FILE_LOCK_CONFLICT:
    case WR_STATUS_DELETE_PENDING: return EBUSY;
    case WR_STATUS_ACCESS_DENIED:
    case WR_STATUS_CANNOT_DELETE: return op == WR_OP_RENAME_DELETE ? EBUSY : EACCES;
    default: return EIO;
    }
}

/* Win32 error codes the map names (calls with no NTSTATUS: the volume and module calls) */
#define WR_ERROR_FILE_NOT_FOUND 2u
#define WR_ERROR_PATH_NOT_FOUND 3u
#define WR_ERROR_TOO_MANY_OPEN_FILES 4u
#define WR_ERROR_ACCESS_DENIED 5u
#define WR_ERROR_NOT_ENOUGH_MEMORY 8u
#define WR_ERROR_OUTOFMEMORY 14u
#define WR_ERROR_NOT_SAME_DEVICE 17u
#define WR_ERROR_WRITE_PROTECT 19u
#define WR_ERROR_NOT_READY 21u
#define WR_ERROR_CRC 23u
#define WR_ERROR_SHARING_VIOLATION 32u
#define WR_ERROR_LOCK_VIOLATION 33u
#define WR_ERROR_HANDLE_DISK_FULL 39u
#define WR_ERROR_DEV_NOT_EXIST 55u
#define WR_ERROR_FILE_EXISTS 80u
#define WR_ERROR_DISK_FULL 112u
#define WR_ERROR_INVALID_NAME 123u
#define WR_ERROR_MOD_NOT_FOUND 126u
#define WR_ERROR_ALREADY_EXISTS 183u
#define WR_ERROR_FILENAME_EXCED_RANGE 206u
#define WR_ERROR_DIRECTORY 267u
#define WR_ERROR_DELETE_PENDING 303u
#define WR_ERROR_PRIVILEGE_NOT_HELD 1314u
#define WR_ERROR_UNRECOGNIZED_VOLUME 1005u
#define WR_ERROR_UNTRUSTED_MOUNT_POINT 448u
#define WR_ERROR_CANT_RESOLVE_FILENAME 1921u

/* the errno of a Win32 error code (GetLastError), with the same rules as wr_errno_nt */
static inline int wr_errno_win32(uint32_t e, int op) {
    switch (e) {
    case 0: return 0;
    case WR_ERROR_FILE_NOT_FOUND:
    case WR_ERROR_PATH_NOT_FOUND:
    case WR_ERROR_MOD_NOT_FOUND: return ENOENT;
    case WR_ERROR_INVALID_NAME: return EINVAL;
    case WR_ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case WR_ERROR_WRITE_PROTECT: return EROFS;
    case WR_ERROR_NOT_READY:
    case WR_ERROR_DEV_NOT_EXIST:
    case WR_ERROR_UNRECOGNIZED_VOLUME: return ENXIO;
    case WR_ERROR_CRC: return EIO;
    case WR_ERROR_DISK_FULL:
    case WR_ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case WR_ERROR_FILE_EXISTS:
    case WR_ERROR_ALREADY_EXISTS: return EEXIST;
    case WR_ERROR_DIRECTORY: return ENOTDIR;
    case WR_ERROR_NOT_ENOUGH_MEMORY:
    case WR_ERROR_OUTOFMEMORY: return ENOMEM;
    case WR_ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case WR_ERROR_NOT_SAME_DEVICE: return EXDEV;
    case WR_ERROR_PRIVILEGE_NOT_HELD: return EPERM;
    case WR_ERROR_UNTRUSTED_MOUNT_POINT:
    case WR_ERROR_CANT_RESOLVE_FILENAME: return ELOOP;
    case WR_ERROR_SHARING_VIOLATION:
    case WR_ERROR_LOCK_VIOLATION:
    case WR_ERROR_DELETE_PENDING: return EBUSY;
    case WR_ERROR_ACCESS_DENIED: return op == WR_OP_RENAME_DELETE ? EBUSY : EACCES;
    default: return EIO;
    }
}

/* ---------------------------------------------------------------- the admin-only ACL rule */

/* Access rights that change a file or a folder's contents or its protection (bridge.md §1
 * owner_ok): FILE_WRITE_DATA (= FILE_ADD_FILE), FILE_APPEND_DATA (= FILE_ADD_SUBDIRECTORY),
 * FILE_DELETE_CHILD, DELETE, WRITE_DAC, WRITE_OWNER, GENERIC_WRITE, GENERIC_ALL */
#define WR_WRITE_RIGHTS (0x00000002u | 0x00000004u | 0x00000040u | 0x00010000u | 0x00040000u | 0x00080000u | 0x40000000u | 0x10000000u)
#define WR_ACE_ALLOWED 0u               /* ACCESS_ALLOWED_ACE_TYPE */
#define WR_ACE_DENIED 1u                /* ACCESS_DENIED_ACE_TYPE */
#define WR_ACE_INHERIT_ONLY 0x08u       /* INHERIT_ONLY_ACE: applies to children only */

/* One ACE of a DACL as the rule needs it: trusted when its SID is Administrators, SYSTEM or
 * TrustedInstaller (in a test build also the running account). */
typedef struct { unsigned type, flags; uint32_t mask; int trusted; } wr_ace;

/* Whether a descriptor lets only administrators change the object (bridge.md §1, r4:I48). Fails
 * closed: no DACL (present = 0) or a NULL DACL (everyone may do anything), an owner that isn't
 * trusted, an ACE of a type the rule doesn't know (object, callback, compound...). Every ALLOW
 * ACE that isn't inherit-only and grants any of WR_WRITE_RIGHTS must be a trusted SID's. DENY
 * ACEs only take rights away. An empty DACL grants nothing: it passes. */
static inline int wr_acl_ok(int owner_trusted, int dacl_present, int dacl_null, const wr_ace *a, size_t n) {
    if (!owner_trusted || !dacl_present || dacl_null) return 0;
    for (size_t i = 0; i < n; i++) {
        if (a[i].type == WR_ACE_DENIED) continue;
        if (a[i].type != WR_ACE_ALLOWED) return 0;
        if (a[i].flags & WR_ACE_INHERIT_ONLY) continue;
        if ((a[i].mask & WR_WRITE_RIGHTS) && !a[i].trusted) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- UTF-8 and UTF-16, strictly */

/* UTF-8 (shortest forms, no surrogates, at most U+10FFFF) to UTF-16 into out (cap units, NUL
 * included): the units written without the NUL, or -1 (invalid, or it doesn't fit). n: the bytes
 * of s to convert (s needn't be NUL-terminated); a NUL byte inside is invalid. */
static inline long wr_utf8_to_16(const char *s, size_t n, uint16_t *out, size_t cap) {
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    size_t k = 0;
    while (p < end) {
        uint32_t c = *p++;
        int more;
        if (c == 0) return -1;
        if (c < 0x80) more = 0;
        else if (c >= 0xC2 && c <= 0xDF) { more = 1; c &= 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { more = 2; c &= 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { more = 3; c &= 0x07; }
        else return -1;
        if (end - p < more) return -1;
        for (int i = 0; i < more; i++) {
            if ((p[i] & 0xC0) != 0x80) return -1;
            c = (c << 6) | (p[i] & 0x3Fu);
        }
        p += more;
        if ((more == 2 && c < 0x800) || (more == 3 && (c < 0x10000 || c > 0x10FFFF)) || (c >= 0xD800 && c <= 0xDFFF))
            return -1;                                      /* overlong, too big, or a surrogate */
        if (c >= 0x10000) {
            if (k + 2 >= cap) return -1;
            c -= 0x10000;
            out[k++] = (uint16_t)(0xD800 + (c >> 10));
            out[k++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            if (k + 1 >= cap) return -1;
            out[k++] = (uint16_t)c;
        }
    }
    if (k >= cap) return -1;
    out[k] = 0;
    return (long)k;
}

/* UTF-16 (n units; pairs only) to UTF-8 into out (cap bytes, NUL included): the bytes written
 * without the NUL, or -1 (an unpaired surrogate, a NUL unit, or it doesn't fit) */
static inline long wr_utf16_to_8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c == 0) return -1;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= n || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF) return -1;
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00u);
        } else if (c >= 0xDC00 && c <= 0xDFFF) return -1;
        unsigned char b[4];
        int m;
        if (c < 0x80) { b[0] = (unsigned char)c; m = 1; }
        else if (c < 0x800) { b[0] = (unsigned char)(0xC0 | (c >> 6)); b[1] = (unsigned char)(0x80 | (c & 0x3F)); m = 2; }
        else if (c < 0x10000) {
            b[0] = (unsigned char)(0xE0 | (c >> 12)); b[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
            b[2] = (unsigned char)(0x80 | (c & 0x3F)); m = 3;
        } else {
            b[0] = (unsigned char)(0xF0 | (c >> 18)); b[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
            b[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (c & 0x3F)); m = 4;
        }
        if (k + (size_t)m >= cap) return -1;
        memcpy(out + k, b, (size_t)m);
        k += (size_t)m;
    }
    if (k >= cap) return -1;
    out[k] = 0;
    return (long)k;
}

/* ---------------------------------------------------------------- paths and names */

#define WR_NT_MAX 32767                 /* UTF-16 units in an NT path (UNICODE_STRING) */

/* What kind of absolute path p is: 1 a drive path "X:\..." (or "X:/..."), 2 a volume GUID path
 * "\\?\Volume{GUID}\...", 3 "\\?\X:\...", 0 anything else (relative, "X:" alone, UNC, a device).
 * *rest: where the path after its root starts (after "X:\" or the volume's "\"). */
static inline int wr_path_kind(const char *p, const char **rest) {
    int sep2 = 0;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) {
        *rest = p + 3;
        return 1;
    }
    if (strncmp(p, "\\\\?\\", 4)) return 0;
    p += 4;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && p[2] == '\\') {
        *rest = p + 3;
        return 3;
    }
    /* Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\ */
    if (strncmp(p, "Volume{", 7)) return 0;
    for (int i = 0; i < 36; i++) {
        char c = p[7 + i];
        int dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
        sep2 |= dash;
    }
    if (!sep2 || p[43] != '}' || p[44] != '\\') return 0;
    *rest = p + 45;
    return 2;
}

/* The NT path of an absolute UTF-8 path, as UTF-16 (out: cap units, NUL included): "\??\" then the
 * path's own root and the rest, '/' turned into '\', so a long path needs no MAX_PATH opt-in
 * (bridge.md §1). The units without the NUL, or -1: not an absolute path wr_path_kind knows,
 * invalid UTF-8, an empty, "." or ".." component (the NT layer doesn't fold them), or too long. */
static inline long wr_nt_path(const char *p, uint16_t *out, size_t cap) {
    const char *rest;
    int kind = wr_path_kind(p, &rest);
    if (!kind || cap < 8) return -1;
    size_t k = 0;
    static const char pre[] = "\\??\\";
    for (size_t i = 0; i < 4; i++) out[k++] = (uint16_t)pre[i];
    const char *root = kind == 1 ? p : p + 4;               /* "X:\" or "Volume{...}\" */
    if ((size_t)(rest - root) + 5 > cap) return -1;         /* "\??\", the root and the NUL */
    for (const char *c = root; c < rest; c++) out[k++] = (uint16_t)(*c == '/' ? '\\' : *c);
    /* the rest, component by component */
    for (const char *c = rest; *c;) {
        const char *e = c;
        while (*e && *e != '\\' && *e != '/') e++;
        size_t l = (size_t)(e - c);
        if (l == 0 || (l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.')) {
            if (!*e && l == 0) break;                       /* a trailing separator: fine */
            return -1;
        }
        long u = wr_utf8_to_16(c, l, out + k, cap - k);
        if (u < 0) return -1;
        k += (size_t)u;
        if (!*e) break;
        if (k + 1 >= cap) return -1;
        out[k++] = '\\';
        c = e + 1;
    }
    if (k >= cap || k > WR_NT_MAX) return -1;
    out[k] = 0;
    return (long)k;
}

/* A UNC server or share name for a track's path (wr_track_nt_path): 1 to 255 bytes, not "." or
 * ".." (and so never the device namespaces "\\.\" and "\\?\": "?" isn't allowed either), no
 * separator, no control character or DEL, none of :*?"<>| */
static inline int wr_unc_name_ok(const char *s, size_t l) {
    if (l == 0 || l > 255 || (l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.')) return 0;
    for (size_t i = 0; i < l; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return 0;
    }
    return 1;
}

/* the shares that are a server's IPC endpoints, never files: pipe, mailslot and IPC$ (any case) */
static inline int wr_unc_ipc_share(const char *s, size_t l) {
    static const char *const ipc[] = {"pipe", "mailslot", "ipc$"};
    for (size_t i = 0; i < 3; i++) {
        size_t k = strlen(ipc[i]), j = 0;
        if (k != l) continue;
        while (j < l && (s[j] >= 'A' && s[j] <= 'Z' ? (char)(s[j] | 0x20) : s[j]) == ipc[i][j]) j++;
        if (j == l) return 1;
    }
    return 0;
}

/* A track's NT path (helper.md T13, the UNC ruling: option B), for the helper's read-only track
 * input only: never a cache location, the bridge's or the core's paths, which keep wr_path_kind
 * and wr_nt_path and so refuse UNC. wr_nt_path's paths as wr_nt_path gives them, and a UNC path:
 * "\\server\share\..." with '\' or '/' (mixed too), or "\\?\UNC\server\share\..." (backslashes,
 * as Win32 takes it); the server and the share wr_unc_name_ok's, then at least one component, each
 * as wr_nt_path's rest (none empty, "." or ".."; a trailing separator allowed); the shares pipe,
 * mailslot and IPC$ refused (a server's IPC endpoints, wr_unc_ipc_share). Out:
 * "\??\UNC\server\share\..." with every separator '\' (cap units, NUL included); the units without
 * the NUL, or -1 (a device path, traversal, a bad name, invalid UTF-8, too long). */
static inline long wr_track_nt_path(const char *p, uint16_t *out, size_t cap) {
    long n = wr_nt_path(p, out, cap);
    if (n >= 0) return n;
    const char *s;
    if (!strncmp(p, "\\\\?\\", 4)) {
        if (!((p[4] | 0x20) == 'u' && (p[5] | 0x20) == 'n' && (p[6] | 0x20) == 'c' && p[7] == '\\')) return -1;
        s = p + 8;
    } else if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) s = p + 2;
    else return -1;
    const char *e = s;
    while (*e && *e != '\\' && *e != '/') e++;                /* the server */
    if (!*e || !wr_unc_name_ok(s, (size_t)(e - s))) return -1;
    const char *h = e + 1, *f = h;
    while (*f && *f != '\\' && *f != '/') f++;                /* the share */
    if (!*f || !f[1] || !wr_unc_name_ok(h, (size_t)(f - h)) || wr_unc_ipc_share(h, (size_t)(f - h))) return -1;
    size_t k = 0;
    static const char pre[] = "\\??\\UNC\\";
    if (cap < sizeof pre) return -1;
    for (size_t i = 0; i + 1 < sizeof pre; i++) out[k++] = (uint16_t)pre[i];
    long u = wr_utf8_to_16(s, (size_t)(e - s), out + k, cap - k);
    if (u < 0 || k + (size_t)u + 1 >= cap) return -1;
    k += (size_t)u;
    out[k++] = '\\';
    u = wr_utf8_to_16(h, (size_t)(f - h), out + k, cap - k);
    if (u < 0 || k + (size_t)u + 1 >= cap) return -1;
    k += (size_t)u;
    out[k++] = '\\';
    for (const char *c = f + 1; *c;) {                        /* the rest, as wr_nt_path's */
        const char *x = c;
        while (*x && *x != '\\' && *x != '/') x++;
        size_t l = (size_t)(x - c);
        if (l == 0 || (l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.')) {
            if (!*x && l == 0) break;
            return -1;
        }
        u = wr_utf8_to_16(c, l, out + k, cap - k);
        if (u < 0) return -1;
        k += (size_t)u;
        if (!*x) break;
        if (k + 1 >= cap) return -1;
        out[k++] = '\\';
        c = x + 1;
    }
    if (k >= cap || k > WR_NT_MAX) return -1;
    out[k] = 0;
    return (long)k;
}

/* whether s (n units) starts with "\\?\" (Win32's no-folding prefix) or "\??\" (an NT path) */
static inline int wr_prefixed16(const uint16_t *s, size_t n) {
    return n >= 4 && s[0] == '\\' && (s[1] == '\\' || s[1] == '?') && s[2] == '?' && s[3] == '\\';
}

/* whether s (n units, after a "\\?\" or "\??\" prefix) starts with a root wr_full_nt keeps: "X:\",
 * "UNC\server\share" (both non-empty) or "Volume{GUID}\" (BX7) */
static inline int wr_prefixed_root16(const uint16_t *s, size_t n) {
    if (n >= 3 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':' && s[2] == '\\') return 1;
    if (n >= 4 && (s[0] | 0x20) == 'u' && (s[1] | 0x20) == 'n' && (s[2] | 0x20) == 'c' && s[3] == '\\') {
        size_t i = 4, j;
        while (i < n && s[i] != '\\') i++;                  /* the server */
        for (j = i + 1; j < n && s[j] != '\\';) j++;        /* the share */
        return i > 4 && i < n && j > i + 1;
    }
    static const char vol[] = "Volume{";
    if (n < 45) return 0;
    for (size_t i = 0; i < 7; i++) if (s[i] != (uint16_t)vol[i]) return 0;
    for (size_t i = 0; i < 36; i++) {
        uint16_t c = s[7 + i];
        int dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return s[43] == '}' && s[44] == '\\';
}

/* The NT path a follow-link open uses (plat_win.c's wr_full_path: B14, BW11), as UTF-16 (out: cap
 * units, NUL included). s (n units, no NUL among them) is a "\\?\" or "\??\" path whose root is
 * "X:\", "UNC\server\share" (both non-empty) or "Volume{GUID}\" (wr_path_kind's GUID), kept as it
 * is ("\\?\" becomes "\??\"), or what GetFullPathNameW made of any other path: "X:\..." gets
 * "\??\" and "\\server\share..." becomes "\??\UNC\server\share...". The units without the NUL, or
 * -1: anything else (a device "\\.\...", a prefixed "X:" with no '\', a prefixed device such as
 * PhysicalDrive0, pipe\..., CONIN$ or GLOBALROOT\..., a Volume{GUID} with no '\', "\\server" without
 * a share, a path still relative), or too long (BX7). */
static inline long wr_full_nt(const uint16_t *s, size_t n, uint16_t *out, size_t cap) {
    const char *pre = "\\??\\";
    size_t skip = 0;
    if (wr_prefixed16(s, n)) {
        if (!wr_prefixed_root16(s + 4, n - 4)) return -1;
        skip = 4;
    } else if (n >= 3 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':' && s[2] == '\\') skip = 0;
    else if (n >= 2 && s[0] == '\\' && s[1] == '\\') {
        size_t i = 2, j;
        while (i < n && s[i] != '\\') i++;                  /* the server */
        for (j = i + 1; j < n && s[j] != '\\';) j++;        /* the share */
        if (i == 2 || i >= n || j == i + 1) return -1;
        if (i == 3 && (s[2] == '.' || s[2] == '?')) return -1;   /* "\\.\" or "\\?\": a device */
        pre = "\\??\\UNC";
        skip = 1;                                           /* "\??\UNC" then "\server\share..." */
    } else return -1;
    size_t pl = strlen(pre), k = pl + (n - skip);
    if (k >= cap || k > WR_NT_MAX) return -1;
    for (size_t i = 0; i < pl; i++) out[i] = (uint16_t)pre[i];
    memcpy(out + pl, s + skip, sizeof(uint16_t) * (n - skip));
    out[k] = 0;
    return (long)k;
}

/* one component's UTF-16 (a name relative to a folder handle): not empty, ".", "..", and no '\'
 * or '/'; -1 otherwise (or invalid, or too long) */
static inline long wr_name16(const char *name, uint16_t *out, size_t cap) {
    size_t l = strlen(name);
    if (l == 0 || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '\\') || strchr(name, '/')) return -1;
    return wr_utf8_to_16(name, l, out, cap);
}

/* The length passed to NtSetInformationFile for a FILE_RENAME_INFORMATION-shaped buffer (a head
 * of `head` bytes, then `units` UTF-16 units of name): head + units * 2, never less than the whole
 * struct (`whole`, its sizeof), which the call requires. On 64-bit the head is 20 and the struct
 * 24, so a 1-character name's 22 bytes were refused (STATUS_INFO_LENGTH_MISMATCH, EIO): every
 * rename to a 1-character name failed (VM round 8). */
static inline size_t wr_info_len(size_t head, size_t whole, size_t units) {
    size_t n = head + units * 2;
    return n < whole ? whole : n;
}

#define WR_LEVELS_MAX 8

/* The parent folders plat_lib_open_checked opens top-down (owner decided, review B1): sep[j] is
 * the index in p of the separator that ends parent j+1 (parent 1 is the file's own folder), for
 * at most `levels` (<= WR_LEVELS_MAX) parents below the volume's root, as plat_owner_check walks
 * them (the root itself is never one). The count found, or -1 when p isn't an absolute path
 * wr_path_kind knows, ends in a separator, or levels is out of range. Parent j+1 is p[0, sep[j]);
 * the top one is opened by its path, each one below it by the name p(sep[j+1], sep[j]) relative
 * to the one above, and the file by the name after sep[0] relative to parent 1. */
static inline int wr_parents(const char *p, int levels, size_t sep[WR_LEVELS_MAX]) {
    const char *rest;
    size_t l = strlen(p), lo;
    int k = 0;
    if (levels < 0 || levels > WR_LEVELS_MAX || !wr_path_kind(p, &rest)) return -1;
    if (l == 0 || p[l - 1] == '\\' || p[l - 1] == '/') return -1;
    lo = (size_t)(rest - p);
    for (size_t i = l; i-- > lo && k < levels;)
        if (p[i] == '\\' || p[i] == '/') sep[k++] = i;
    return k;
}

/* Whether mount (a folder path ending in '\', as Windows lists a volume's mount points: "D:\",
 * "C:\mnt\x\") holds path: path starts with it, ASCII letters' case ignored (drive letters and
 * the GUID's hex digits), or path is the mount without its last '\'. 0 or mount's length. */
static inline size_t wr_mount_holds(const char *mount, const char *path) {
    size_t l = strlen(mount);
    if (l == 0 || mount[l - 1] != '\\') return 0;
    for (size_t i = 0; i < l; i++) {
        char a = mount[i], b = path[i];
        if (b == '/') b = '\\';
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return i == l - 1 && b == 0 ? l : 0;    /* "D:" for "D:\" */
    }
    return l;
}

/* ---------------------------------------------------------------- Program Files
 * Windows' own Program Files folder for the native architecture, on whichever drive Windows is
 * installed (owner decided 2026-11-12): HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion's
 * ProgramFilesDir in the 64-bit view (an x64 process on ARM64 shares the native view, so the
 * native folder there too), REG_SZ only, never expanded. Only an administrator can change it;
 * never %ProgramFiles% or any per-user setting. The one rule (plat_win.c's plat_program_files)
 * for the elevated worker, the core DLL, the bridge and the watcher, so they can never disagree.
 * A value that can't be read, or that this rule refuses, fails closed. */
#define WR_PF_KEY "SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
#define WR_PF_VALUE "ProgramFilesDir"
#define WR_PF_ROOT "RB Stems Plus"                         /* our root, <Program Files>\RB Stems Plus */
#define WR_PF_REKORDBOX "rekordbox"                        /* rekordbox's, <Program Files>\rekordbox */
#define WR_PF_REAL_ORT WR_PF_ROOT "\\ort\\onnxruntime.dll" /* the bridge's real library */
#define WR_PF_MAX 200                                      /* UTF-16 units (and UTF-8 bytes) a value may take */

/* The value's reader: its UTF-16 into buf (cap units), *n the units read (a trailing NUL may be
 * among them); 0, or nonzero when it can't be read (missing, another type, too long). */
typedef int (*wr_pf_reader)(void *ctx, uint16_t *buf, size_t cap, size_t *n);

/* Program Files as read(ctx) gives it, in UTF-8 into out (cap bytes, NUL included), one trailing
 * '\' dropped: its length, or -1 with out "" (refused): unreadable; not "X:\" on a drive letter
 * and exactly one name (relative, UNC, "\\?\", a drive's root, a nested folder: the checks and
 * holds above our root, A7, NT1 and B1, assume X:\<name>); a name that is empty, or ends in a dot
 * or a space (".", ".." and the names Windows trims); a character Windows refuses in a name
 * (<>:"/|?*, controls), '%' (an unexpanded variable) or '~' (an 8.3 short name: compat.c
 * compares rekordbox's folders as text, so an alias of the real name must never form);
 * invalid UTF-16; longer than WR_PF_MAX. */
static inline long wr_program_files(wr_pf_reader read, void *ctx, char *out, size_t cap) {
    uint16_t w[WR_PF_MAX + 2];
    char v[WR_PF_MAX + 1];
    size_t n = 0;
    if (cap) out[0] = 0;
    if (!read || read(ctx, w, sizeof w / sizeof *w, &n) || n > sizeof w / sizeof *w) return -1;
    while (n && w[n - 1] == 0) n--;                         /* RegGetValueW counts the NUL */
    long l = wr_utf16_to_8(w, n, v, sizeof v);
    if (l > 3 && v[l - 1] == '\\') v[--l] = 0;
    if (l < 4 || !((v[0] >= 'A' && v[0] <= 'Z') || (v[0] >= 'a' && v[0] <= 'z')) || v[1] != ':' || v[2] != '\\') return -1;
    for (long i = 3;; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c == '\\') return -1;                           /* exactly one name after X:\ */
        if (c == 0) {
            if (v[i - 1] == '.' || v[i - 1] == ' ') return -1;
            break;
        }
        if (c < 0x20 || c == 0x7F || strchr("<>:\"/|?*%~", c)) return -1;
    }
    if ((size_t)l >= cap) return -1;
    memcpy(out, v, (size_t)l + 1);
    return l;
}

/* pf + "\" + sub (sub NULL or "": pf itself) into out (cap bytes, NUL included): its length, or -1
 * with out "" (pf empty, or it doesn't fit) */
static inline long wr_program_path(const char *pf, const char *sub, char *out, size_t cap) {
    size_t a = strlen(pf), b = sub ? strlen(sub) : 0, l = a + (b ? b + 1 : 0);
    if (!a || l >= cap) {
        if (cap) out[0] = 0;
        return -1;
    }
    memmove(out, pf, a);
    if (b) {
        out[a] = '\\';
        memcpy(out + a + 1, sub, b);
    }
    out[l] = 0;
    return (long)l;
}

/* ---------------------------------------------------------------- time */

#define WR_FILETIME_1970 116444736000000000LL   /* 100 ns ticks from 1601 to 1970 */

/* a FILETIME (100 ns ticks since 1601) as seconds since 1970, rounded down */
static inline int64_t wr_filetime_s(int64_t ft) {
    int64_t t = ft - WR_FILETIME_1970;
    return t >= 0 ? t / 10000000 : -((-t + 9999999) / 10000000);
}

/* a FILETIME as nanoseconds since 1970 */
static inline int64_t wr_filetime_ns(int64_t ft) { return (ft - WR_FILETIME_1970) * 100; }

/* plat_cond_wait_ns's timeout in milliseconds: rounded up, so a wait is never shorter than asked,
 * and below INFINITE (0xFFFFFFFF) */
static inline uint32_t wr_ms_of_ns(uint64_t ns) {
    uint64_t ms = ns / 1000000u + (ns % 1000000u != 0);
    return ms >= 0xFFFFFFFEu ? 0xFFFFFFFEu : (uint32_t)ms;
}

/* ---------------------------------------------------------------- a folder tree's deletion */

/* The parent/leaf split of an absolute path (plat_remove_tree): *cut is where the leaf starts, *l
 * the length with trailing separators dropped, *plen the parent's length: path[0, cut) less its
 * separator, unless the parent is the root ("C:\", "\\?\C:\", "\\?\Volume{...}\" keep theirs).
 * 0, or -1: not an absolute path wr_path_kind knows, or the root itself. */
static inline int wr_split_leaf(const char *path, size_t *cut, size_t *l, size_t *plen) {
    const char *rest;
    if (!wr_path_kind(path, &rest)) return -1;
    size_t root = (size_t)(rest - path), n = strlen(path), c;
    while (n > root && (path[n - 1] == '\\' || path[n - 1] == '/')) n--;
    for (c = n; c > root && path[c - 1] != '\\' && path[c - 1] != '/'; c--) {}
    if (c == n) return -1;
    *cut = c;
    *l = n;
    *plen = c > root ? c - 1 : c;
    return 0;
}

/* plat_remove_tree_at's walk (plat_win.h), over these operations; plat_win.c gives the NT ones
 * (every open relative to the parent's handle with FILE_OPEN_REPARSE_POINT, listing and deletion
 * by handle), common/tests/win_rules.c a fake. Names are UTF-16 as the folder listing gives them,
 * never converted. */
typedef struct {
    /* opens `name` (n units) in the open folder parent, never following a reparse point: 0 with
     * *h and *is_dir (1 only for a real folder: a reparse point, a junction included, is 0 and is
     * deleted as itself); ENOENT when it is gone; another errno */
    int (*open)(void *ctx, void *parent, const uint16_t *name, size_t n, void **h, int *is_dir);
    /* every name in the open folder h but "." and "..": add(lctx, name, n) for each; 0 or an errno */
    int (*list)(void *ctx, void *h, int (*add)(void *lctx, const uint16_t *name, size_t n), void *lctx);
    /* deletes the open entry (a folder: once empty): 0 or an errno */
    int (*dispose)(void *ctx, void *h);
    void (*close)(void *ctx, void *h);
    void *ctx;
} wr_tree_ops;

enum { WR_TREE_DEPTH = 64 };

typedef struct {
    uint16_t *buf;   /* each name: its length, then its units */
    size_t used, cap;
    int err;
} wr_names;

static inline int wr_names_add(void *lctx, const uint16_t *name, size_t n) {
    wr_names *l = (wr_names *)lctx;
    if (n == 0 || n > 0xFFFF) return l->err = EINVAL;
    if (l->used + 1 + n > l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 1024;
        while (cap < l->used + 1 + n) cap *= 2;
        uint16_t *b = (uint16_t *)realloc(l->buf, cap * sizeof *b);
        if (!b) return l->err = ENOMEM;
        l->buf = b;
        l->cap = cap;
    }
    l->buf[l->used++] = (uint16_t)n;
    memcpy(l->buf + l->used, name, n * sizeof *name);
    l->used += n;
    return 0;
}

/* Deletes `name` in the open folder parent and, for a real folder, everything under it, first:
 * the folder is listed whole, then each name opened relative to it (a name swapped for a link or
 * a junction meanwhile is opened as itself and deleted as itself, never what it points to). Every
 * entry is tried; the first error is returned (0 all went; ENOENT nothing was there). */
static inline int wr_remove_tree(const wr_tree_ops *ops, void *parent, const uint16_t *name, size_t n, int depth) {
    void *h = NULL;
    int is_dir = 0;
    int e = ops->open(ops->ctx, parent, name, n, &h, &is_dir);
    if (e) return e;
    int first = 0;
    if (is_dir) {
        if (depth >= WR_TREE_DEPTH) {
            ops->close(ops->ctx, h);
            return ENAMETOOLONG;
        }
        wr_names l = {NULL, 0, 0, 0};
        int le = ops->list(ops->ctx, h, wr_names_add, &l);
        if (!le) le = l.err;
        if (le) first = le;
        for (size_t at = 0; at < l.used;) {
            size_t k = l.buf[at];
            int ce = wr_remove_tree(ops, h, l.buf + at + 1, k, depth + 1);
            if (ce && ce != ENOENT && !first) first = ce;
            at += 1 + k;
        }
        free(l.buf);
    }
    int de = ops->dispose(ops->ctx, h);
    if (de && !first) first = de;
    ops->close(ops->ctx, h);
    return first;
}

#endif
