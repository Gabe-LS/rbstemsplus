/* NW6, on NTFS. The suite creates the synthetic payload and applies WOF compression. */
#include "check.h"
#include "../sha_memo.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

int checks, failures;

static HANDLE open_file(const char *path, DWORD access) {
    return CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

/* WOF can hide its reparse tag from ordinary file attribute queries. */
static int wof_compressed(const char *path) {
    HANDLE h = open_file(path, GENERIC_READ);
    if (h == INVALID_HANDLE_VALUE) return 0;
    struct {
        WOF_EXTERNAL_INFO external;
        FILE_PROVIDER_EXTERNAL_INFO_V1 provider;
    } info;
    DWORD bytes;
    int wof = DeviceIoControl(h, FSCTL_GET_EXTERNAL_BACKING, NULL, 0,
                              &info, sizeof info, &bytes, NULL) &&
              bytes >= sizeof info.external && info.external.Provider == WOF_PROVIDER_FILE;
    CloseHandle(h);
    return wof;
}

static int stamp(const char *path, sha_memo_stamp *out) {
    plat_file f;
    if (plat_file_open_path(path, &f)) return 0;
    int ok = !sha_memo_win_stamp(NULL, f, out);
    plat_file_close(f);
    return ok;
}

static int rewrite(const char *path, const char *text, int suspend) {
    HANDLE h = open_file(path, GENERIC_WRITE);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME saved;
    const FILETIME frozen = { MAXDWORD, MAXDWORD };
    DWORD bytes, size = (DWORD)strlen(text);
    int ok = GetFileTime(h, NULL, NULL, &saved);
    if (ok && suspend) ok = SetFileTime(h, NULL, NULL, &frozen);
    if (ok) ok = WriteFile(h, text, size, &bytes, NULL) && bytes == size;
    if (ok && !suspend) ok = SetFileTime(h, NULL, NULL, &saved);
    return CloseHandle(h) && ok;
}

static int only_change_time(const sha_memo_stamp *a, const sha_memo_stamp *b) {
    return a->size == b->size && a->mtime == b->mtime &&
           !memcmp(a->id, b->id, sizeof a->id);
}

static void timestamps(sha_memo *memo, const char *path) {
    char original[65], got[65], fresh[65];
    sha_memo_stamp before, restored, suspended;
    int ok = stamp(path, &before) && !sha_memo_file(memo, path, original);
    CHECK(ok, "ordinary NTFS fixture read and memoised");
    if (!ok) return;
    long count = memo->hashed;
    Sleep(50);
    ok = rewrite(path, "after", 0) && stamp(path, &restored);
    CHECK(ok && only_change_time(&before, &restored) && before.ctime != restored.ctime,
          "same size, identity and restored LastWriteTime, different ChangeTime");
    if (!ok) return;
    ok = !sha_file(path, fresh) && strcmp(original, fresh) &&
         !sha_memo_file(memo, path, got) && !strcmp(got, fresh) && memo->hashed == count + 1;
    CHECK(ok, "restored LastWriteTime: ChangeTime caused a miss and returned the new digest");
    if (!ok) return;

    Sleep(50);
    ok = rewrite(path, "again", 1) && stamp(path, &suspended);
    CHECK(ok && only_change_time(&restored, &suspended),
          "same size, identity and suspended LastWriteTime");
    if (!ok) return;
    int miss = restored.ctime != suspended.ctime;
    count = memo->hashed;
    char latest[65];
    CHECK(!sha_file(path, latest) && strcmp(fresh, latest) &&
          !sha_memo_file(memo, path, got) && !strcmp(got, miss ? latest : fresh) &&
          memo->hashed == count + miss, "suspended LastWriteTime: result follows the observed stamp");
    printf("NW6 suspended LastWriteTime: %s (ChangeTime %s)\n",
           miss ? "miss" : "hit", miss ? "changed" : "unchanged");
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: memo-stamp-wintest WOF-FILE EXPECTED-SHA256 STAMP-FILE\n");
        return 2;
    }
    sha_memo memo;
    char got[65];
    sha_memo_init(&memo);
    memo.stat = sha_memo_win_stamp;
    CHECK(wof_compressed(argv[1]), "compact produced a WOF-compressed file");
    CHECK(!sha_memo_file(&memo, argv[1], got) && !strcmp(got, argv[2]) && memo.hashed == 1,
          "WOF file hashed to its pre-compression digest");
    CHECK(!sha_memo_file(&memo, argv[1], got) && !strcmp(got, argv[2]) && memo.hashed == 1,
          "WOF file hit with the same stamp");
    timestamps(&memo, argv[3]);
    sha_memo_free(&memo);
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
