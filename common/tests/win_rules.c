/* common/win_rules.h on the Mac (bridge.md §4 "On the Mac"): plat_win.c's rules that need no
 * Windows call, one fixture per mapping of the NTSTATUS map (r4:I47), the ACL rule's fixtures (a
 * NULL DACL, an empty one, r4:I48), strict UTF-8/UTF-16, NT paths, names, the mount match and
 * the Program Files rule with a fake reader (C:, D:, unreadable or refused values).
 * Built and run by common/tests/win.sh.
 *
 *   win-rules        exits 1 when a check fails */
#include "../win_rules.h"
#include "check.h"


static void t_nt_map(void) {
    static const struct { uint32_t s; int open, rename; } m[] = {
        {WR_STATUS_SUCCESS, 0, 0},
        {0x00000103u, 0, 0},                                /* STATUS_PENDING: not an error */
        {0x80000005u, EIO, EIO},                            /* STATUS_BUFFER_OVERFLOW: a warning fails */
        {0x80000016u, EIO, EIO},                            /* STATUS_NO_MORE_EAS */
        {WR_STATUS_OBJECT_NAME_NOT_FOUND, ENOENT, ENOENT},
        {WR_STATUS_OBJECT_PATH_NOT_FOUND, ENOENT, ENOENT},
        {WR_STATUS_NO_SUCH_FILE, ENOENT, ENOENT},
        {WR_STATUS_OBJECT_NAME_INVALID, EINVAL, EINVAL},
        {WR_STATUS_NAME_TOO_LONG, ENAMETOOLONG, ENAMETOOLONG},
        {WR_STATUS_MEDIA_WRITE_PROTECTED, EROFS, EROFS},
        {WR_STATUS_DEVICE_NOT_READY, ENXIO, ENXIO},
        {WR_STATUS_DEVICE_NOT_CONNECTED, ENXIO, ENXIO},
        {WR_STATUS_NO_SUCH_DEVICE, ENXIO, ENXIO},
        {WR_STATUS_FILE_INVALID, ENXIO, ENXIO},
        {WR_STATUS_CRC_ERROR, EIO, EIO},
        {WR_STATUS_IO_DEVICE_ERROR, EIO, EIO},
        {WR_STATUS_DISK_FULL, ENOSPC, ENOSPC},
        {WR_STATUS_OBJECT_NAME_COLLISION, EEXIST, EEXIST},
        {WR_STATUS_NOT_A_DIRECTORY, ENOTDIR, ENOTDIR},
        {WR_STATUS_FILE_IS_A_DIRECTORY, EISDIR, EISDIR},
        {WR_STATUS_DIRECTORY_NOT_EMPTY, ENOTEMPTY, ENOTEMPTY},
        {WR_STATUS_STOPPED_ON_SYMLINK, ELOOP, ELOOP},
        {0xC00004BCu, ELOOP, ELOOP},                        /* STATUS_UNTRUSTED_MOUNT_POINT (Redirection Guard) */
        {WR_STATUS_INSUFFICIENT_RESOURCES, ENOMEM, ENOMEM},
        {WR_STATUS_NO_MEMORY, ENOMEM, ENOMEM},
        {WR_STATUS_COMMITMENT_LIMIT, ENOMEM, ENOMEM},
        {WR_STATUS_TOO_MANY_OPENED_FILES, EMFILE, EMFILE},
        {WR_STATUS_NOT_SAME_DEVICE, EXDEV, EXDEV},
        {WR_STATUS_PRIVILEGE_NOT_HELD, EPERM, EPERM},
        {WR_STATUS_SHARING_VIOLATION, EBUSY, EBUSY},
        {WR_STATUS_FILE_LOCK_CONFLICT, EBUSY, EBUSY},
        {WR_STATUS_DELETE_PENDING, EBUSY, EBUSY},
        {WR_STATUS_ACCESS_DENIED, EACCES, EBUSY},           /* an ACL's refusal; a transient open on exFAT */
        {WR_STATUS_CANNOT_DELETE, EACCES, EBUSY},
        {0xC0000001u, EIO, EIO},                            /* STATUS_UNSUCCESSFUL: anything else */
        {0xC000000Du, EIO, EIO},                            /* STATUS_INVALID_PARAMETER */
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        CHECK(wr_errno_nt(m[i].s, WR_OP_OPEN) == m[i].open, "0x%08x open: %d", m[i].s, wr_errno_nt(m[i].s, WR_OP_OPEN));
        CHECK(wr_errno_nt(m[i].s, WR_OP_RENAME_DELETE) == m[i].rename, "0x%08x rename: %d", m[i].s,
              wr_errno_nt(m[i].s, WR_OP_RENAME_DELETE));
    }
}

static void t_win32_map(void) {
    static const struct { uint32_t e; int open, rename; } m[] = {
        {0, 0, 0},
        {WR_ERROR_FILE_NOT_FOUND, ENOENT, ENOENT},
        {WR_ERROR_PATH_NOT_FOUND, ENOENT, ENOENT},
        {WR_ERROR_MOD_NOT_FOUND, ENOENT, ENOENT},
        {WR_ERROR_INVALID_NAME, EINVAL, EINVAL},
        {WR_ERROR_FILENAME_EXCED_RANGE, ENAMETOOLONG, ENAMETOOLONG},
        {WR_ERROR_WRITE_PROTECT, EROFS, EROFS},
        {WR_ERROR_NOT_READY, ENXIO, ENXIO},
        {WR_ERROR_DEV_NOT_EXIST, ENXIO, ENXIO},
        {WR_ERROR_UNRECOGNIZED_VOLUME, ENXIO, ENXIO},
        {WR_ERROR_CRC, EIO, EIO},
        {WR_ERROR_DISK_FULL, ENOSPC, ENOSPC},
        {WR_ERROR_HANDLE_DISK_FULL, ENOSPC, ENOSPC},
        {WR_ERROR_FILE_EXISTS, EEXIST, EEXIST},
        {WR_ERROR_ALREADY_EXISTS, EEXIST, EEXIST},
        {WR_ERROR_DIRECTORY, ENOTDIR, ENOTDIR},
        {WR_ERROR_NOT_ENOUGH_MEMORY, ENOMEM, ENOMEM},
        {WR_ERROR_OUTOFMEMORY, ENOMEM, ENOMEM},
        {WR_ERROR_TOO_MANY_OPEN_FILES, EMFILE, EMFILE},
        {WR_ERROR_NOT_SAME_DEVICE, EXDEV, EXDEV},
        {WR_ERROR_PRIVILEGE_NOT_HELD, EPERM, EPERM},
        {448, ELOOP, ELOOP},                                /* ERROR_UNTRUSTED_MOUNT_POINT */
        {WR_ERROR_CANT_RESOLVE_FILENAME, ELOOP, ELOOP},
        {WR_ERROR_SHARING_VIOLATION, EBUSY, EBUSY},
        {WR_ERROR_LOCK_VIOLATION, EBUSY, EBUSY},
        {WR_ERROR_DELETE_PENDING, EBUSY, EBUSY},
        {WR_ERROR_ACCESS_DENIED, EACCES, EBUSY},
        {31, EIO, EIO},                                     /* ERROR_GEN_FAILURE: anything else */
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        CHECK(wr_errno_win32(m[i].e, WR_OP_OPEN) == m[i].open, "%u open", m[i].e);
        CHECK(wr_errno_win32(m[i].e, WR_OP_RENAME_DELETE) == m[i].rename, "%u rename", m[i].e);
    }
}

static void t_acl(void) {
    /* Program Files' own DACL (phase0-probes.md §2.4): TrustedInstaller, SYSTEM, Administrators
     * full; Users, ALL APPLICATION PACKAGES and ALL RESTRICTED APPLICATION PACKAGES read and
     * execute; CREATOR OWNER full, inherit only */
    wr_ace pf[] = {
        {WR_ACE_ALLOWED, 0, 0x001F01FFu, 1}, {WR_ACE_ALLOWED, 0x02, 0x10000000u, 1}, {WR_ACE_ALLOWED, 0, 0x001F01FFu, 1},
        {WR_ACE_ALLOWED, 0, 0x001F01FFu, 1}, {WR_ACE_ALLOWED, 0x0B, 0x10000000u, 0},
        {WR_ACE_ALLOWED, 0, 0x001200A9u, 0}, {WR_ACE_ALLOWED, 0, 0x001200A9u, 0}, {WR_ACE_ALLOWED, 0, 0x001200A9u, 0},
    };
    size_t n = sizeof pf / sizeof pf[0];
    CHECK(wr_acl_ok(1, 1, 0, pf, n), "Program Files' DACL");
    CHECK(!wr_acl_ok(0, 1, 0, pf, n), "an owner that isn't trusted");
    CHECK(!wr_acl_ok(1, 0, 0, pf, n), "no DACL");
    CHECK(!wr_acl_ok(1, 1, 1, NULL, 0), "a NULL DACL");
    CHECK(wr_acl_ok(1, 1, 0, NULL, 0), "an empty DACL grants nothing");
    static const uint32_t rights[] = {0x2, 0x4, 0x40, 0x10000, 0x40000, 0x80000, 0x40000000u, 0x10000000u};
    for (size_t i = 0; i < sizeof rights / sizeof rights[0]; i++) {
        wr_ace a[] = {{WR_ACE_ALLOWED, 0, rights[i], 0}};
        CHECK(!wr_acl_ok(1, 1, 0, a, 1), "a user's ALLOW of 0x%x", rights[i]);
        a[0].flags = WR_ACE_INHERIT_ONLY | 0x03;
        CHECK(wr_acl_ok(1, 1, 0, a, 1), "inherit-only 0x%x applies to children only", rights[i]);
        a[0].flags = 0;
        a[0].type = WR_ACE_DENIED;
        CHECK(wr_acl_ok(1, 1, 0, a, 1), "a DENY of 0x%x takes rights away", rights[i]);
    }
    wr_ace other[] = {{WR_ACE_ALLOWED, 0, 0x100 | 0x80 | 0x10 | 0x8 | 0x1 | 0x20 | 0x20000 | 0x100000, 0}};
    CHECK(wr_acl_ok(1, 1, 0, other, 1), "read, execute, attributes and EAs don't change the file");
    wr_ace object[] = {{5, 0, 0x1, 1}}, callback[] = {{9, 0, 0x1, 1}};
    CHECK(!wr_acl_ok(1, 1, 0, object, 1) && !wr_acl_ok(1, 1, 0, callback, 1), "an ACE type the rule doesn't know");
}

static void t_utf(void) {
    uint16_t w[64];
    char b[64];
    CHECK(wr_utf8_to_16("J\xc3\xbcrgen", 7, w, 64) == 6 && w[1] == 0xFC && w[6] == 0, "Jürgen");
    CHECK(wr_utf8_to_16("\xe6\x9d\xb1\xe4\xba\xac", 6, w, 64) == 2 && w[0] == 0x6771 && w[1] == 0x4EAC, "CJK");
    CHECK(wr_utf8_to_16("\xf0\x9f\x8e\xb5", 4, w, 64) == 2 && w[0] == 0xD83C && w[1] == 0xDFB5, "a pair");
    static const char *bad[] = {"\xc0\xaf", "\xe0\x80\xaf", "\xf0\x80\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80",
                                "\xc3", "\xff", "a\xe6\x9d"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(wr_utf8_to_16(bad[i], strlen(bad[i]), w, 64) == -1, "invalid UTF-8 %zu", i);
    CHECK(wr_utf8_to_16("a\0b", 3, w, 64) == -1, "a NUL byte");
    CHECK(wr_utf8_to_16("abc", 3, w, 3) == -1 && wr_utf8_to_16("abc", 3, w, 4) == 3, "the NUL must fit");
    CHECK(wr_utf8_to_16("\xf0\x9f\x8e\xb5", 4, w, 2) == -1, "a pair that doesn't fit");
    uint16_t pair[] = {0xD83C, 0xDFB5}, lone[] = {0x61, 0xD83C}, low[] = {0xDFB5, 0x61};
    CHECK(wr_utf16_to_8(pair, 2, b, 64) == 4 && !memcmp(b, "\xf0\x9f\x8e\xb5", 5), "pair back");
    CHECK(wr_utf16_to_8(lone, 2, b, 64) == -1 && wr_utf16_to_8(low, 2, b, 64) == -1, "unpaired surrogates");
    uint16_t ju[] = {'J', 0xFC, 'r'};
    CHECK(wr_utf16_to_8(ju, 3, b, 64) == 4 && !strcmp(b, "J\xc3\xbcr"), "back to UTF-8");
    CHECK(wr_utf16_to_8(ju, 3, b, 4) == -1 && wr_utf16_to_8(ju, 3, b, 5) == 4, "the NUL must fit");
}

static int same16(const uint16_t *w, const uint_least16_t *want) {
    size_t i = 0;
    for (; want[i]; i++)
        if (w[i] != want[i]) return 0;
    return w[i] == 0;
}

static void t_paths(void) {
    uint16_t w[400];
    const char *rest;
    CHECK(wr_path_kind("C:\\Users\\a", &rest) == 1 && !strcmp(rest, "Users\\a"), "a drive path");
    CHECK(wr_path_kind("c:/x", &rest) == 1, "a drive path with '/'");
    CHECK(wr_path_kind("\\\\?\\D:\\x", &rest) == 3 && !strcmp(rest, "x"), "\\\\?\\D:\\");
    CHECK(wr_path_kind("\\\\?\\Volume{0a1b2c3d-0000-1111-2222-333344445555}\\x\\y", &rest) == 2 && !strcmp(rest, "x\\y"), "a GUID path");
    static const char *notabs[] = {"x\\y", "C:", "C:x", "\\\\server\\share\\x", "\\\\.\\C:\\x", "\\\\?\\Volume{0a1b}\\",
                                   "\\\\?\\Volume{0a1b2c3d-0000-1111-2222-33334444555g}\\", "/Users/x", ""};
    for (size_t i = 0; i < sizeof notabs / sizeof notabs[0]; i++) CHECK(wr_path_kind(notabs[i], &rest) == 0, "not absolute: %s", notabs[i]);
    CHECK(wr_nt_path("C:\\Program Files\\RB Stems Plus\\ort\\onnxruntime.dll", w, 400) > 0
              && same16(w, u"\\??\\C:\\Program Files\\RB Stems Plus\\ort\\onnxruntime.dll"), "the real ORT's NT path");
    CHECK(wr_nt_path("C:/Users/J\xc3\xbcrgen/x/hdemucs.onnx", w, 400) > 0 && same16(w, u"\\??\\C:\\Users\\J\u00fcrgen\\x\\hdemucs.onnx"),
          "'/' as rekordbox joins, and UTF-8");
    CHECK(wr_nt_path("\\\\?\\Volume{0a1b2c3d-0000-1111-2222-333344445555}\\RB Stems Plus Cache.noindex", w, 400) > 0
              && same16(w, u"\\??\\Volume{0a1b2c3d-0000-1111-2222-333344445555}\\RB Stems Plus Cache.noindex"), "a GUID path");
    CHECK(wr_nt_path("D:\\", w, 400) == 7 && same16(w, u"\\??\\D:\\"), "a root");
    CHECK(wr_nt_path("D:\\a\\", w, 400) > 0 && same16(w, u"\\??\\D:\\a\\"), "a trailing separator");
    static const char *bad[] = {"D:\\a\\\\b", "D:\\.\\b", "D:\\a\\..\\b", "D:\\a\\..", "relative", "D:\\\xc3"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(wr_nt_path(bad[i], w, 400) == -1, "refused: %s", bad[i]);
    CHECK(wr_nt_path("D:\\abcdef", w, 10) == -1 && wr_nt_path("D:\\abcdef", w, 14) == 13, "the NUL must fit");
    uint16_t small[10];
    CHECK(wr_nt_path("\\\\?\\Volume{0a1b2c3d-0000-1111-2222-333344445555}\\x", small, 10) == -1, "a GUID root that doesn't fit");
    CHECK(wr_nt_path("\\\\?\\Volume{0a1b2c3d-0000-1111-2222-333344445555}\\", w, 50) == 49
              && wr_nt_path("\\\\?\\Volume{0a1b2c3d-0000-1111-2222-333344445555}\\", w, 49) == -1, "a GUID root exactly fits");
    /* the follow-link opens' NT path (B14, BW11): "\\?\" and "\??\" paths as they are, else what
     * GetFullPathNameW made of the path (here its result for a relative, a dotted and a UNC path) */
    {
        static const struct { const char *in, *nt; } fl[] = {
            {"\\\\?\\C:\\a\\..\\b", "\\??\\C:\\a\\..\\b"},                /* "\\?\": no folding */
            {"\\??\\C:\\a\\.\\b", "\\??\\C:\\a\\.\\b"},
            {"\\\\?\\UNC\\nas\\music\\x.onnx", "\\??\\UNC\\nas\\music\\x.onnx"},
            {"\\\\?\\UNC\\srv\\share\\a", "\\??\\UNC\\srv\\share\\a"},           /* BX7: UNC\server\share kept */
            {"\\??\\UNC\\srv\\share", "\\??\\UNC\\srv\\share"},
            {"\\\\?\\Volume{0a1b2c3d-0000-1111-2222-333344445555}\\m", "\\??\\Volume{0a1b2c3d-0000-1111-2222-333344445555}\\m"},
            {"C:\\rbtest\\models\\hdemucs.onnx", "\\??\\C:\\rbtest\\models\\hdemucs.onnx"},   /* "models\hdemucs.onnx" in C:\rbtest */
            {"d:\\b", "\\??\\d:\\b"},                                     /* "D:\a\..\.\b" folded */
            {"C:\\", "\\??\\C:\\"},
            {"\\\\nas\\music\\x.onnx", "\\??\\UNC\\nas\\music\\x.onnx"},  /* "\\nas\music\a\..\x.onnx" folded too */
            {"\\\\nas\\music", "\\??\\UNC\\nas\\music"},
            {"\\\\nas\\music\\", "\\??\\UNC\\nas\\music\\"},
        };
        for (size_t i = 0; i < sizeof fl / sizeof fl[0]; i++) {
            uint16_t in[200], want[200];
            long n = wr_utf8_to_16(fl[i].in, strlen(fl[i].in), in, 200), m = wr_utf8_to_16(fl[i].nt, strlen(fl[i].nt), want, 200);
            long k = n > 0 ? wr_full_nt(in, (size_t)n, w, 400) : -2;
            CHECK(k == m && !memcmp(w, want, sizeof(uint16_t) * (size_t)(m + 1)), "full path %s: %ld", fl[i].in, k);
        }
        static const char *nofull[] = {"models\\hdemucs.onnx", "..\\x", ".\\x", "C:x", "C:", "\\x", "/x",
                                       "\\\\.\\C:\\x", "\\\\.\\pipe\\x", "\\\\?", "\\\\nas", "\\\\nas\\", "\\\\\\x", "\\\\", "C:/x",
                                       /* BX7: after the prefix, only X:\, UNC\server\share or Volume{GUID}\ */
                                       "\\\\?\\E:", "\\??\\E:", "\\\\?\\PhysicalDrive0", "\\\\?\\GLOBALROOT\\Device\\X\\y",
                                       "\\\\?\\pipe\\x", "\\\\?\\CONIN$", "\\\\?\\Volume{0a1b2c3d-0000-1111-2222-333344445555}",
                                       "\\\\?\\Volume{0a1b2c3d-0000-1111-2222-33334444555}\\x", "\\\\?\\", "\\??\\",
                                       "\\\\?\\UNC\\srv", "\\\\?\\UNC\\srv\\", "\\\\?\\UNC\\\\share", "\\\\?\\UNC"};
        for (size_t i = 0; i < sizeof nofull / sizeof nofull[0]; i++) {
            uint16_t in[200];
            long n = wr_utf8_to_16(nofull[i], strlen(nofull[i]), in, 200);
            CHECK(n > 0 && wr_full_nt(in, (size_t)n, w, 400) == -1, "not a full path: %s", nofull[i]);
        }
        uint16_t pfx[] = {'\\', '\\', '?', '\\'}, q[] = {'\\', '?', '?', '\\'}, dev[] = {'\\', '\\', '.', '\\'};
        CHECK(wr_prefixed16(pfx, 4) && wr_prefixed16(q, 4) && !wr_prefixed16(dev, 4) && !wr_prefixed16(q, 3), "the prefixes");
        uint16_t d[] = {'D', ':', '\\', 'a', 'b'};
        CHECK(wr_full_nt(d, 5, w, 9) == -1 && wr_full_nt(d, 5, w, 10) == 9 && same16(w, u"\\??\\D:\\ab"), "the NUL must fit");
        uint16_t u[] = {'\\', '\\', 's', '\\', 't'};
        CHECK(wr_full_nt(u, 5, w, 11) == -1 && wr_full_nt(u, 5, w, 12) == 11 && same16(w, u"\\??\\UNC\\s\\t"), "a UNC path's NUL must fit");
        /* WR_NT_MAX: a full path of exactly that many units, and one more */
        uint16_t *big = malloc(sizeof(uint16_t) * (WR_NT_MAX + 2)), *nt = malloc(sizeof(uint16_t) * (WR_NT_MAX + 2));
        if (!big || !nt) abort();
        big[0] = 'C', big[1] = ':', big[2] = '\\';
        for (size_t i = 3; i < WR_NT_MAX + 1; i++) big[i] = 'a';
        CHECK(wr_full_nt(big, WR_NT_MAX - 4, nt, WR_NT_MAX + 2) == WR_NT_MAX, "WR_NT_MAX units");
        CHECK(wr_full_nt(big, WR_NT_MAX - 3, nt, WR_NT_MAX + 2) == -1, "one over WR_NT_MAX");
        free(big);
        free(nt);
    }
    /* plat_lib_open_checked's parents (B1) */
    size_t sep[WR_LEVELS_MAX];
    const char *real = "C:\\Program Files\\RB Stems Plus\\ort\\onnxruntime.dll";
    CHECK(wr_parents(real, 2, sep) == 2 && sep[0] == 34 && sep[1] == 30, "two parents: %zu %zu", sep[0], sep[1]);
    CHECK(!strncmp(real, "C:\\Program Files\\RB Stems Plus", sep[1]) && sep[1] == strlen("C:\\Program Files\\RB Stems Plus"), "the top one");
    CHECK(wr_parents(real, 3, sep) == 3 && sep[2] == 16 && wr_parents(real, 4, sep) == 3, "never the root: %zu", sep[2]);
    CHECK(wr_parents(real, 0, sep) == 0, "no parents");
    CHECK(wr_parents("C:\\x.dll", 2, sep) == 0, "a file at the root has none to check");
    CHECK(wr_parents("C:/a/b/x.dll", 2, sep) == 2 && sep[0] == 6 && sep[1] == 4, "'/' separators");
    CHECK(wr_parents("\\\\?\\Volume{0a1b2c3d-0000-1111-2222-333344445555}\\a\\x.dll", 5, sep) == 1 && sep[0] == 50, "a GUID path");
    CHECK(wr_parents("C:\\a\\", 2, sep) == -1 && wr_parents("a\\x.dll", 2, sep) == -1 && wr_parents(real, -1, sep) == -1
              && wr_parents(real, WR_LEVELS_MAX + 1, sep) == -1, "refused");
    CHECK(wr_name16("abc.flac", w, 400) == 8, "a name");
    static const char *badname[] = {"", ".", "..", "a\\b", "a/b"};
    for (size_t i = 0; i < sizeof badname / sizeof badname[0]; i++) CHECK(wr_name16(badname[i], w, 400) == -1, "not a name: %s", badname[i]);
    CHECK(wr_mount_holds("D:\\", "d:\\Cache") == 3 && wr_mount_holds("D:\\", "D:") == 3 && wr_mount_holds("D:\\", "D:\\") == 3, "D:\\");
    CHECK(wr_mount_holds("C:\\mnt\\x\\", "C:\\mnt\\x\\y") == 9 && wr_mount_holds("C:\\mnt\\x\\", "C:\\mnt\\xy") == 0, "a mounted folder");
    CHECK(wr_mount_holds("C:\\mnt\\x\\", "C:/mnt/x/y") == 9, "'/' in the path");
    CHECK(wr_mount_holds("D:\\", "E:\\x") == 0 && wr_mount_holds("D:", "D:\\x") == 0 && wr_mount_holds("", "D:\\") == 0, "no match");

    /* plat_remove_tree's parent/leaf split */
    static const struct { const char *path, *parent, *leaf; } sp[] = {
        {"C:\\a", "C:\\", "a"},
        {"C:/a", "C:/", "a"},
        {"C:\\a\\b\\", "C:\\a", "b"},
        {"C:\\a\\b\\\\", "C:\\a", "b"},
        {"\\\\?\\C:\\a", "\\\\?\\C:\\", "a"},
        {"\\\\?\\Volume{0123abcd-0000-1111-2222-333344445555}\\app", "\\\\?\\Volume{0123abcd-0000-1111-2222-333344445555}\\", "app"},
        {"\\\\?\\Volume{0123abcd-0000-1111-2222-333344445555}\\x\\y\\app", "\\\\?\\Volume{0123abcd-0000-1111-2222-333344445555}\\x\\y", "app"},
    };
    for (size_t i = 0; i < sizeof sp / sizeof sp[0]; i++) {
        size_t cut = 0, l = 0, plen = 0;
        int r = wr_split_leaf(sp[i].path, &cut, &l, &plen);
        char par[200] = "", leaf[64] = "";
        if (!r) {
            memcpy(par, sp[i].path, plen);
            par[plen] = 0;
            memcpy(leaf, sp[i].path + cut, l - cut);
            leaf[l - cut] = 0;
        }
        CHECK(!r && !strcmp(par, sp[i].parent) && !strcmp(leaf, sp[i].leaf), "split %s: \"%s\" \"%s\"", sp[i].path, par, leaf);
        CHECK(wr_nt_path(par, w, 400) > 0, "the parent of %s is an NT path", sp[i].path);
    }
    static const char *const nosplit[] = {"C:\\", "C:\\\\", "rel\\a", "C:", "", "\\\\?\\Volume{0123abcd-0000-1111-2222-333344445555}\\"};
    for (size_t i = 0; i < sizeof nosplit / sizeof nosplit[0]; i++) {
        size_t cut, l, plen;
        CHECK(wr_split_leaf(nosplit[i], &cut, &l, &plen) == -1, "no split: %s", nosplit[i]);
    }
    /* a path over 32,768 bytes but under WR_NT_MAX units (CJK): split, and its parent an NT path */
    {
        size_t cap = 40000;
        char *lp = malloc(cap);
        uint16_t *nt = malloc(sizeof(uint16_t) * (WR_NT_MAX + 1));
        if (!lp || !nt) abort();
        size_t k = 0;
        lp[k++] = 'C', lp[k++] = ':';
        for (int c = 0; c < 113; c++) {
            lp[k++] = '\\';
            for (int j = 0; j < 100; j++) lp[k++] = '\xe6', lp[k++] = '\x9d', lp[k++] = '\xb1';
        }
        memcpy(lp + k, "\\leaf", 6);
        size_t cut, l, plen;
        int r = wr_split_leaf(lp, &cut, &l, &plen);
        CHECK(strlen(lp) > 32768 && !r && !strcmp(lp + cut, "leaf"), "a long CJK path split");
        lp[plen] = 0;
        long units = wr_nt_path(lp, nt, WR_NT_MAX + 1);
        CHECK(units > 11000 && units <= WR_NT_MAX, "its parent an NT path of %ld units", units);
        free(lp);
        free(nt);
    }
}

/* a raw UTF-16 value (an inner NUL, a lone surrogate) */
typedef struct { const uint16_t *u; size_t n; } raw_pf;
static int raw_pf_read(void *ctx, uint16_t *buf, size_t cap, size_t *n) {
    const raw_pf *r = ctx;
    if (r->n > cap) return -1;
    memcpy(buf, r->u, r->n * sizeof *buf);
    *n = r->n;
    return 0;
}

/* the fake ProgramFilesDir reader: the value as UTF-8 (NULL: unreadable), and its trailing NUL
 * counted as RegGetValueW counts it */
typedef struct { const char *v; int with_nul, calls; } fake_pf;
static int fake_pf_read(void *ctx, uint16_t *buf, size_t cap, size_t *n) {
    fake_pf *f = ctx;
    f->calls++;
    if (!f->v) return -1;
    long u = wr_utf8_to_16(f->v, strlen(f->v), buf, cap);
    if (u < 0) return -1;
    *n = (size_t)u + (f->with_nul ? 1 : 0);
    return 0;
}

static void t_program_files(void) {
    char out[WR_PF_MAX + 1], p[300];
    /* Windows on C:, on a second drive, with or without the NUL or a trailing '\' */
    static const struct { const char *v, *want; } ok[] = {
        {"C:\\Program Files", "C:\\Program Files"},
        {"D:\\Program Files", "D:\\Program Files"},
        {"D:\\Program Files\\", "D:\\Program Files"},
        {"F:\\Programme \xc3\xa9t\xc3\xa9", "F:\\Programme \xc3\xa9t\xc3\xa9"},
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        for (int nul = 0; nul < 2; nul++) {
            fake_pf f = {ok[i].v, nul, 0};
            long l = wr_program_files(fake_pf_read, &f, out, sizeof out);
            CHECK(l == (long)strlen(ok[i].want) && !strcmp(out, ok[i].want) && f.calls == 1, "%s (NUL %d): \"%s\"", ok[i].v, nul, out);
        }
    /* refused: nothing in out */
    static const char *const bad[] = {"", "Program Files", "C:", "C:\\", "D:\\\\", "C:Program Files", "C:/Program Files",
        "C:\\Program Files/x", "\\\\server\\share\\Program Files", "\\\\?\\D:\\Program Files", "\\\\.\\D:\\Program Files",
        "%SystemDrive%\\Program Files", "C:\\%ProgramFiles%", "D:\\Program Files\\..", "D:\\Program Files\\.", "D:\\.\\Program Files",
        "D:\\Program Files.", "D:\\Program Files ", "D:\\Program Files\\\\x", "D:\\Pro:gram", "D:\\Pro\tgram", "D:\\Pro\x7fgram",
        "D:\\Pro\"gram", "D:\\Pro<gram", "D:\\Pro|gram", "D:\\Pro?gram", "D:\\Pro*gram", "1:\\Program Files", ":\\Program Files",
        "C:\\PROGRA~1", "e:\\Windows Install\\Program Files", "C:\\a\\b"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fake_pf f = {bad[i], 1, 0};
        memcpy(out, "x", 2);
        CHECK(wr_program_files(fake_pf_read, &f, out, sizeof out) == -1 && !out[0], "refused: %s", bad[i]);
    }
    /* unreadable (missing, a REG_EXPAND_SZ, another type: the reader says so): refused */
    fake_pf none = {NULL, 0, 0};
    memcpy(out, "x", 2);
    CHECK(wr_program_files(fake_pf_read, &none, out, sizeof out) == -1 && !out[0] && none.calls == 1, "unreadable: refused");
    CHECK(wr_program_files(NULL, NULL, out, sizeof out) == -1 && !out[0], "no reader: refused");
    /* too long, the NUL's room */
    char longv[WR_PF_MAX + 8];
    memcpy(longv, "D:\\", 3);
    memset(longv + 3, 'a', sizeof longv - 4);
    longv[sizeof longv - 1] = 0;
    fake_pf lf = {longv, 1, 0};
    CHECK(wr_program_files(fake_pf_read, &lf, out, sizeof out) == -1 && !out[0], "too long: refused");
    longv[WR_PF_MAX] = 0;                                   /* WR_PF_MAX bytes: fits */
    lf.v = longv;
    CHECK(wr_program_files(fake_pf_read, &lf, out, sizeof out) == WR_PF_MAX, "WR_PF_MAX bytes: taken");
    longv[WR_PF_MAX] = 'a';
    longv[WR_PF_MAX + 1] = 0;
    CHECK(wr_program_files(fake_pf_read, &lf, out, sizeof out) == -1, "WR_PF_MAX + 1 bytes: refused");
    CHECK(wr_program_files(fake_pf_read, &(fake_pf){"D:\\Program Files", 1, 0}, out, 16) == -1 && !out[0]
              && wr_program_files(fake_pf_read, &(fake_pf){"D:\\Program Files", 1, 0}, out, 17) == 16, "the NUL must fit");
    /* a NUL inside, then more: the value isn't a string Windows would use */
    static const uint16_t inner[] = {'D', ':', '\\', 'P', 0, 'x', 0};
    static const uint16_t lone[] = {'D', ':', '\\', 0xD800, 'x', 0};
    raw_pf raw[] = {{inner, 7}, {lone, 6}};
    for (size_t i = 0; i < 2; i++) CHECK(wr_program_files(raw_pf_read, &raw[i], out, sizeof out) == -1 && !out[0], "raw value %zu: refused", i);
    /* the joins every component makes */
    CHECK(wr_program_path("D:\\Program Files", WR_PF_ROOT, p, sizeof p) > 0 && !strcmp(p, "D:\\Program Files\\RB Stems Plus"), "the root");
    CHECK(wr_program_path("D:\\Program Files", WR_PF_REKORDBOX, p, sizeof p) > 0 && !strcmp(p, "D:\\Program Files\\rekordbox"), "rekordbox's");
    CHECK(wr_program_path("D:\\Program Files", WR_PF_REAL_ORT, p, sizeof p) > 0
              && !strcmp(p, "D:\\Program Files\\RB Stems Plus\\ort\\onnxruntime.dll"), "the real ORT");
    CHECK(wr_program_path("D:\\Program Files", NULL, p, sizeof p) == 16 && !strcmp(p, "D:\\Program Files")
              && wr_program_path("D:\\Program Files", "", p, sizeof p) == 16, "the folder itself");
    memcpy(p, "x", 2);
    CHECK(wr_program_path("", WR_PF_ROOT, p, sizeof p) == -1 && !p[0], "no folder: refused");
    CHECK(wr_program_path("D:\\Program Files", "rekordbox", p, 26) == -1 && !p[0] && wr_program_path("D:\\Program Files", "rekordbox", p, 27) == 26,
          "the NUL must fit");
    size_t sep[WR_LEVELS_MAX];
    wr_program_path("D:\\Program Files", WR_PF_REAL_ORT, p, sizeof p);
    CHECK(wr_parents(p, 2, sep) == 2 && sep[1] == strlen("D:\\Program Files\\RB Stems Plus"), "the bridge's two checked folders on D:");
}

static void t_time(void) {
    CHECK(wr_filetime_s(WR_FILETIME_1970) == 0 && wr_filetime_s(WR_FILETIME_1970 + 10000000) == 1, "1970");
    CHECK(wr_filetime_s(WR_FILETIME_1970 - 1) == -1 && wr_filetime_s(WR_FILETIME_1970 + 9999999) == 0, "rounded down");
    CHECK(wr_filetime_s(133000000000000000LL) == 1655526400LL, "2022-06-18");
    CHECK(wr_filetime_ns(WR_FILETIME_1970 + 3) == 300, "ns");
    CHECK(wr_ms_of_ns(0) == 0 && wr_ms_of_ns(1) == 1 && wr_ms_of_ns(1000000) == 1 && wr_ms_of_ns(1000001) == 2, "ms up");
    CHECK(wr_ms_of_ns(UINT64_MAX) == 0xFFFFFFFEu, "never INFINITE");
}

/* ---------------------------------------------------------------- wr_remove_tree on a fake volume */

enum { N_DIR, N_FILE, N_LINK };   /* a link: a reparse point (a symlink, a junction), never followed */
typedef struct fnode {
    uint16_t name[32];
    size_t n;
    int kind, parent, deleted, busy, swap_to_link;   /* swap_to_link: becomes a link once its parent is listed */
} fnode;
static fnode fs[128];
static int nfs, opens, closes;

static int fs_add(int parent, const char *name, int kind) {
    fnode *f = &fs[nfs];
    memset(f, 0, sizeof *f);
    for (f->n = 0; name[f->n]; f->n++) f->name[f->n] = (uint16_t)(unsigned char)name[f->n];
    f->kind = kind;
    f->parent = parent;
    return nfs++;
}

static int fake_open(void *ctx, void *parent, const uint16_t *name, size_t n, void **h, int *is_dir) {
    (void)ctx;
    int p = (int)(intptr_t)parent - 1;
    for (int i = 0; i < nfs; i++) {
        if (fs[i].deleted || fs[i].parent != p || fs[i].n != n || memcmp(fs[i].name, name, n * 2)) continue;
        *h = (void *)(intptr_t)(i + 1);
        *is_dir = fs[i].kind == N_DIR;
        opens++;
        return 0;
    }
    return ENOENT;
}
static int fake_list(void *ctx, void *h, int (*add)(void *lctx, const uint16_t *name, size_t n), void *lctx) {
    (void)ctx;
    int d = (int)(intptr_t)h - 1;
    for (int i = 0; i < nfs; i++)
        if (!fs[i].deleted && fs[i].parent == d) {
            int r = add(lctx, fs[i].name, fs[i].n);
            if (r) return r;
        }
    /* the junction swap: a listed folder replaced by a link before it is opened */
    for (int i = 0; i < nfs; i++)
        if (!fs[i].deleted && fs[i].parent == d && fs[i].swap_to_link) fs[i].kind = N_LINK;
    return 0;
}
static int fake_dispose(void *ctx, void *h) {
    (void)ctx;
    int i = (int)(intptr_t)h - 1;
    if (fs[i].busy) return EBUSY;
    if (fs[i].kind == N_DIR)
        for (int k = 0; k < nfs; k++)
            if (!fs[k].deleted && fs[k].parent == i) return ENOTEMPTY;
    fs[i].deleted = 1;
    return 0;
}
static void fake_close(void *ctx, void *h) {
    (void)ctx, (void)h;
    closes++;
}
static const wr_tree_ops fake_ops = {fake_open, fake_list, fake_dispose, fake_close, NULL};

static int rm(int parent, const char *name) {
    uint16_t w[32];
    size_t n = 0;
    for (; name[n]; n++) w[n] = (uint16_t)(unsigned char)name[n];
    return wr_remove_tree(&fake_ops, (void *)(intptr_t)(parent + 1), w, n, 0);
}

static void t_tree(void) {
    /* root(0) / app / { a.dll, sub / { x, y }, link -> outside }; outside(-) / keep */
    nfs = 0;
    int root = fs_add(-2, "root", N_DIR);
    int app = fs_add(root, "app", N_DIR);
    fs_add(app, "a.dll", N_FILE);
    int sub = fs_add(app, "sub", N_DIR);
    fs_add(sub, "x", N_FILE);
    fs_add(sub, "y", N_FILE);
    int outside = fs_add(root, "outside", N_DIR);
    int keep = fs_add(outside, "keep", N_FILE);
    int link = fs_add(app, "link", N_LINK);   /* its target is `outside`: never walked */
    (void)link;
    opens = closes = 0;
    CHECK(rm(root, "app") == 0, "a tree removed");
    int left = 0;
    for (int i = 0; i < nfs; i++) left += !fs[i].deleted;
    CHECK(left == 3 && !fs[outside].deleted && !fs[keep].deleted && fs[app].deleted, "all of it, the link itself, nothing it points to (%d left)", left);
    CHECK(opens == closes && opens == 6, "every handle closed (%d opened, %d closed)", opens, closes);
    CHECK(rm(root, "app") == ENOENT, "nothing there: ENOENT");

    /* a folder swapped for a junction after its parent was listed: deleted as a link */
    CHECK(opens == closes, "every handle closed (%d opened, %d closed)", opens, closes);
    nfs = 0;
    root = fs_add(-2, "root", N_DIR);
    app = fs_add(root, "app", N_DIR);
    sub = fs_add(app, "sub", N_DIR);
    fs[sub].swap_to_link = 1;
    int inner = fs_add(sub, "inner", N_FILE);   /* what the junction's target would hold: not touched */
    CHECK(rm(root, "app") == 0, "the tree with a swapped folder removed");
    CHECK(fs[sub].deleted && !fs[inner].deleted, "the swapped-in junction deleted as itself, its target's file kept");

    /* a busy file: the rest goes, the error is EBUSY, its folders stay */
    CHECK(opens == closes, "every handle closed (%d opened, %d closed)", opens, closes);
    nfs = 0;
    root = fs_add(-2, "root", N_DIR);
    app = fs_add(root, "app", N_DIR);
    int exe = fs_add(app, "RB Stems Plus.exe", N_FILE);
    fs[exe].busy = 1;
    int other = fs_add(app, "other", N_FILE);
    CHECK(rm(root, "app") == EBUSY, "a running exe: EBUSY first");
    CHECK(fs[other].deleted && !fs[exe].deleted && !fs[app].deleted, "the rest deleted, the exe and its folder kept");
    fs[exe].busy = 0;
    CHECK(rm(root, "app") == 0 && fs[app].deleted, "once free, all of it");

    /* deeper than the limit: refused there, nothing above it deleted */
    CHECK(opens == closes, "every handle closed (%d opened, %d closed)", opens, closes);
    nfs = 0;
    root = fs_add(-2, "root", N_DIR);
    int at = root;
    for (int i = 0; i < WR_TREE_DEPTH + 2; i++) at = fs_add(at, "d", N_DIR);
    CHECK(rm(root, "d") == ENAMETOOLONG && !fs[1].deleted, "past the depth limit: ENAMETOOLONG");
    CHECK(nfs <= 128, "(the fake's room)");

    /* a lone file and a lone link */
    CHECK(opens == closes, "every handle closed (%d opened, %d closed)", opens, closes);
    nfs = 0;
    root = fs_add(-2, "root", N_DIR);
    int f = fs_add(root, "f", N_FILE), l = fs_add(root, "l", N_LINK);
    CHECK(rm(root, "f") == 0 && fs[f].deleted && rm(root, "l") == 0 && fs[l].deleted, "a file, a link: deleted as themselves");

    CHECK(opens == closes, "every handle closed (%d opened, %d closed)", opens, closes);
    /* names: an empty one is refused by the list's add */
    wr_names names = {NULL, 0, 0, 0};
    uint16_t one = 'a';
    CHECK(wr_names_add(&names, &one, 1) == 0 && wr_names_add(&names, &one, 0) == EINVAL && names.used == 2, "the name list");
    free(names.buf);
}

/* FILE_RENAME_INFORMATION's length (VM round 8): a 1-character name is passed as the whole
 * 24-byte struct, not the 22 bytes of its head and name; longer names as head + name. The mirror
 * has the 64-bit layout (ULONG, HANDLE, ULONG, WCHAR[1]), which plat_win.c and worker_win.c
 * static-assert on Windows */
typedef struct { uint32_t Flags; void *RootDirectory; uint32_t FileNameLength; uint16_t FileName[1]; } mirror_rename_info;
static void t_info_len(void) {
    size_t head = offsetof(mirror_rename_info, FileName), whole = sizeof(mirror_rename_info);
    CHECK(head == 20 && whole == 24, "the mirror has FILE_RENAME_INFORMATION's 64-bit layout (%zu, %zu)", head, whole);
    CHECK(wr_info_len(head, whole, 1) == 24, "a 1-character name: the whole struct (%zu)", wr_info_len(head, whole, 1));
    CHECK(wr_info_len(head, whole, 2) == 24 && wr_info_len(head, whole, 3) == 26 && wr_info_len(head, whole, 255) == 530,
          "2 characters fill the struct; longer names are head + name");
    CHECK(wr_info_len(head, whole, 0) == 24, "never under the struct's size");
}

int main(void) {
    t_info_len();
    t_nt_map();
    t_win32_map();
    t_acl();
    t_utf();
    t_paths();
    t_program_files();
    t_time();
    t_tree();
    printf("win_rules: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
