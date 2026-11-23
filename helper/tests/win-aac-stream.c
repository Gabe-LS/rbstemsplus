/* Host ABI shims for the byte-stream section extracted from aac_mf.c. */
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "plat_helper.h"

typedef int32_t HRESULT, LONG;
typedef uint32_t DWORD, ULONG;
typedef int HANDLE;
typedef union { int64_t QuadPart; } LARGE_INTEGER;
typedef union { uint64_t QuadPart; } ULARGE_INTEGER;
typedef struct { DWORD type; ULARGE_INTEGER cbSize; DWORD grfMode; } STATSTG;
typedef const int *REFIID;
static const int rb_IID_IUnknown = 1, rb_IID_ISequentialStream = 2, rb_IID_IStream = 3;
#define IsEqualGUID(a, b) (*(a) == *(b))
#define STDMETHODCALLTYPE
#define FAILED(h) ((h) < 0)
enum { S_OK, S_FALSE, E_POINTER = -1, E_NOINTERFACE = -2, E_NOTIMPL = -3,
       STG_E_ACCESSDENIED = -4, STG_E_INVALIDFUNCTION = -5, STG_E_READFAULT = -6,
       STREAM_SEEK_SET = 0, STREAM_SEEK_CUR, STREAM_SEEK_END,
       STGTY_STREAM = 2, STGM_READ = 0, STATFLAG_NONAME = 1,
       FALSE = 0, DUPLICATE_SAME_ACCESS = 2 };

typedef struct IStream IStream;
typedef struct {
    HRESULT (*QueryInterface)(IStream *, REFIID, void **);
    ULONG (*AddRef)(IStream *);
    ULONG (*Release)(IStream *);
    HRESULT (*Read)(IStream *, void *, ULONG, ULONG *);
    HRESULT (*Write)(IStream *, const void *, ULONG, ULONG *);
    HRESULT (*Seek)(IStream *, LARGE_INTEGER, DWORD, ULARGE_INTEGER *);
    HRESULT (*SetSize)(IStream *, ULARGE_INTEGER);
    HRESULT (*CopyTo)(IStream *, IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *);
    HRESULT (*Commit)(IStream *, DWORD);
    HRESULT (*Revert)(IStream *);
    HRESULT (*LockRegion)(IStream *, ULARGE_INTEGER, ULARGE_INTEGER, DWORD);
    HRESULT (*UnlockRegion)(IStream *, ULARGE_INTEGER, ULARGE_INTEGER, DWORD);
    HRESULT (*Stat)(IStream *, STATSTG *, DWORD);
    HRESULT (*Clone)(IStream *, IStream **);
} IStreamVtbl;
struct IStream { IStreamVtbl *lpVtbl; };
#define IStream_Release(s) ((s)->lpVtbl->Release(s))
static LONG InterlockedIncrement(volatile LONG *v) { return ++*v; }
static LONG InterlockedDecrement(volatile LONG *v) { return --*v; }

typedef struct { IStream *stm; } IMFByteStream;
static IMFByteStream wrapped;
static int fail_wrap, fail_dup, reads, closes;
static int watched = -1;

static HRESULT wrap(IStream *s, IMFByteStream **out) {
    if (fail_wrap) return E_NOTIMPL;
    s->lpVtbl->AddRef(s);
    wrapped.stm = s;
    *out = &wrapped;
    return S_OK;
}
static const struct { HRESULT (*bs_on_stream)(IStream *, IMFByteStream **); } mf = {wrap};

/* plat_file uses h on Windows and fd on the host. */
#define h fd
static HANDLE GetCurrentProcess(void) { return 123; }
static int DuplicateHandle(HANDLE source, HANDLE f, HANDLE dest, HANDLE *out,
                           DWORD access, int inherit, DWORD options) {
    if (fail_dup || source != GetCurrentProcess() || dest != source ||
        access || inherit || options != DUPLICATE_SAME_ACCESS) return 0;
    *out = dup(f);
    return *out >= 0;
}
int plat_file_stat(plat_file f, plat_stat *st) {
    struct stat s;
    if (fstat(f.fd, &s)) return errno;
    st->size = s.st_size;
    return 0;
}
void plat_file_close(plat_file f) { close(f.fd); closes++; }
static void readable_watch_stream(plat_file f) { watched = f.fd; }
static int readable_watch_pread(plat_file f, void *buf, size_t n, int64_t off, size_t *got) {
    if (f.fd != watched) return EINVAL;
    reads++;
    /* Exercise repeated platform reads with a limit small enough for the fixture. */
    ssize_t r = pread(f.fd, buf, n > 2 ? 2 : n, off);
    if (r < 0) return errno;
    *got = (size_t)r;
    return 0;
}

#include "aac-stream.inc"
#undef h

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "AAC stream: %s failed\n", #c); return 1; } } while (0)

int main(int argc, char **argv) {
    const int64_t tail = (INT64_C(1) << 32) + 17;
    CHECK(argc == 2);
    plat_file original = {.fd = open(argv[1], O_CREAT | O_EXCL | O_RDWR, 0600)};
    CHECK(original.fd >= 0);
    CHECK(write(original.fd, "head", 4) == 4 && pwrite(original.fd, "tail", 4, tail) == 4);
    CHECK(lseek(original.fd, 2, SEEK_SET) == 2);
    CHECK(unlink(argv[1]) == 0);
    int replacement = open(argv[1], O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(replacement >= 0 && write(replacement, "swap", 4) == 4 && close(replacement) == 0);

    char why[100] = "";
    IMFByteStream *bs = open_stream(original, why, sizeof why);
    if (!bs) {
        fprintf(stderr, "AAC stream: opening sparse MP4 over 4 GiB failed: %s\n", why);
        return 1;
    }
    CHECK(!reads && !closes && watched >= 0 && watched != original.fd);
    IStream *s = bs->stm;
    const REFIID ids[] = {&rb_IID_IUnknown, &rb_IID_ISequentialStream, &rb_IID_IStream};
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) {
        void *iface = NULL;
        CHECK(s->lpVtbl->QueryInterface(s, ids[i], &iface) == S_OK && iface == s);
        CHECK(IStream_Release(s) == 1 && !closes);
    }
    STATSTG st;
    CHECK(s->lpVtbl->Stat(s, &st, STATFLAG_NONAME) == S_OK && st.type == STGTY_STREAM &&
          st.cbSize.QuadPart == (uint64_t)tail + 4);
    char buf[8];
    ULONG got = 0;
    CHECK(s->lpVtbl->Read(s, buf, 4, &got) == S_OK && got == 4 && !memcmp(buf, "head", 4));
    ULARGE_INTEGER pos;
    CHECK(s->lpVtbl->Seek(s, (LARGE_INTEGER){tail}, STREAM_SEEK_SET, &pos) == S_OK && pos.QuadPart == (uint64_t)tail);
    CHECK(s->lpVtbl->Read(s, buf, 4, NULL) == S_OK && !memcmp(buf, "tail", 4));
    CHECK(s->lpVtbl->Seek(s, (LARGE_INTEGER){-2}, STREAM_SEEK_CUR, &pos) == S_OK && pos.QuadPart == (uint64_t)tail + 2);
    CHECK(s->lpVtbl->Seek(s, (LARGE_INTEGER){-4}, STREAM_SEEK_END, NULL) == S_OK);
    CHECK(FAILED(s->lpVtbl->Seek(s, (LARGE_INTEGER){-tail - 1}, STREAM_SEEK_CUR, NULL)));
    CHECK(s->lpVtbl->Read(s, buf, sizeof buf, &got) == S_FALSE && got == 4 && !memcmp(buf, "tail", 4));
    CHECK(s->lpVtbl->Seek(s, (LARGE_INTEGER){1}, STREAM_SEEK_END, NULL) == S_OK);
    CHECK(s->lpVtbl->Read(s, buf, sizeof buf, &got) == S_FALSE && got == 0 && reads == 6);
    CHECK(s->lpVtbl->Seek(s, (LARGE_INTEGER){-1}, STREAM_SEEK_SET, &pos) == S_OK && pos.QuadPart == UINT64_MAX);
    CHECK(FAILED(s->lpVtbl->Seek(s, (LARGE_INTEGER){1}, STREAM_SEEK_CUR, NULL)));
    CHECK(IStream_Release(s) == 0 && closes == 1);
    CHECK(fcntl(watched, F_GETFD) == -1 && errno == EBADF);
    CHECK(lseek(original.fd, 0, SEEK_CUR) == 2);

    fail_wrap = 1;
    CHECK(!open_stream(original, why, sizeof why) && closes == 2 && reads == 6);
    CHECK(fcntl(watched, F_GETFD) == -1 && errno == EBADF);
    fail_dup = 1;
    CHECK(!open_stream(original, why, sizeof why) && closes == 2 && reads == 6);
    CHECK(lseek(original.fd, 0, SEEK_CUR) == 2 && close(original.fd) == 0);
    puts("AAC stream: sparse MP4 over 4 GiB, seeks, EOF, path replacement and handle lifetime passed");
    return 0;
}
