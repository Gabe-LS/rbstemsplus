/* A file's sha256, memoised by path and stamp (sha_memo.h). C11 over common/plat.h. */
#include "sha_memo.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* sha_memo's stamp on Windows: the handle's size and 128-bit file id (plat_file_stat; its kind is
 * ignored, since it calls any reparse point PLAT_OTHER and the path was opened through links
 * already, so a WOF-compressed or cloud file still hashes) and its 100 ns last-write and change
 * times (FileBasicInfo), finer than plat_stat's seconds. A folder: EINVAL. */
int sha_memo_win_stamp(void *ctx, plat_file f, sha_memo_stamp *out) {
    (void)ctx;
    plat_stat st;
    int e = plat_file_stat(f, &st);
    if (e) return e;
    FILE_BASIC_INFO b;
    if (!GetFileInformationByHandleEx((HANDLE)f.h, FileBasicInfo, &b, sizeof b)) return EIO;
    if (b.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) return EINVAL;
    out->size = st.size;
    out->mtime = b.LastWriteTime.QuadPart;
    out->ctime = b.ChangeTime.QuadPart;
    out->id[0] = st.id.vol;
    out->id[1] = st.id.file[0];
    out->id[2] = st.id.file[1];
    return 0;
}
#endif

void sha_memo_init(sha_memo *m) {
    memset(m, 0, sizeof *m);
    plat_mutex_init(&m->mu);
}

void sha_memo_free(sha_memo *m) {
    for (int i = 0; i < SHA_MEMO_MAX; i++) free(m->e[i].path);
    plat_mutex_destroy(&m->mu);
    memset(m->e, 0, sizeof m->e);
}

static int stamp_of(sha_memo *m, plat_file f, sha_memo_stamp *out) {
    memset(out, 0, sizeof *out);
    if (m->stat) return m->stat(m->ctx, f, out);
    plat_stat st;
    int e = plat_file_stat(f, &st);
    if (e) return e;
    if (st.kind != PLAT_FILE) return EINVAL;
    out->size = st.size;
    out->mtime = st.mtime;
    out->id[0] = st.id.vol;
    out->id[1] = st.id.file[0];
    out->id[2] = st.id.file[1];
    return 0;
}

static int same(const sha_memo_stamp *a, const sha_memo_stamp *b) {
    return a->size == b->size && a->mtime == b->mtime && a->ctime == b->ctime && a->id[0] == b->id[0] && a->id[1] == b->id[1] &&
           a->id[2] == b->id[2];
}

/* under mu: the entry for path, or NULL */
static sha_memo_entry *find(sha_memo *m, const char *path) {
    for (int i = 0; i < SHA_MEMO_MAX; i++)
        if (m->e[i].path && !strcmp(m->e[i].path, path)) return &m->e[i];
    return NULL;
}

/* f's sha256 from where it stands, hex, into out[65]: 0, or an errno. m: its between_reads hook
 * (NULL: none) */
static int hash_open(sha_memo *m, plat_file f, char out[65]) {
    enum { CHUNK = 1 << 20 };
    unsigned char *buf = malloc(CHUNK);
    if (!buf) return ENOMEM;
    plat_sha256_ctx h;
    int e = plat_sha256_init(&h);
    if (!e) {
        size_t got = 0;
        while (!(e = plat_read(f, buf, CHUNK, &got)) && got) {
            plat_sha256_update(&h, buf, got);
            if (m && m->between_reads) m->between_reads(m->ctx);
        }
        unsigned char d[32];
        if (plat_sha256_final(&h, d) || e) e = e ? e : EIO;
        else
            for (int i = 0; i < 32; i++) {
                out[2 * i] = "0123456789abcdef"[d[i] >> 4];
                out[2 * i + 1] = "0123456789abcdef"[d[i] & 15];
            }
        out[64] = 0;
    }
    free(buf);
    return e;
}

int sha_memo_file(sha_memo *m, const char *path, char out[65]) {
    plat_file f;
    sha_memo_stamp before, after;
    if (plat_file_open_path(path, &f)) return -1;
    if (stamp_of(m, f, &before)) {
        plat_file_close(f);
        return -1;
    }
    plat_mutex_lock(&m->mu);
    sha_memo_entry *hit = find(m, path);
    int found = hit && same(&hit->st, &before);
    if (found) memcpy(out, hit->sha, 65);
    plat_mutex_unlock(&m->mu);
    if (found) {
        plat_file_close(f);
        return 0;
    }
    int e = hash_open(m, f, out);
    int unchanged = !e && !stamp_of(m, f, &after) && same(&before, &after);
    plat_file_close(f);
    if (e) return -1;
    plat_mutex_lock(&m->mu);
    m->hashed++;
    sha_memo_entry *x = find(m, path);
    if (!unchanged) {   /* written while hashed, or its stamp unreadable: no hash, neither stamp memoised */
        if (x) {
            free(x->path);
            memset(x, 0, sizeof *x);
        }
        plat_mutex_unlock(&m->mu);
        return -1;
    }
    if (!x) {
        size_t n = strlen(path) + 1;
        char *p = malloc(n);
        if (p) {
            memcpy(p, path, n);
            x = &m->e[m->next];
            m->next = (m->next + 1) % SHA_MEMO_MAX;
            free(x->path);
            x->path = p;
        }
    }
    if (x) {
        x->st = before;
        memcpy(x->sha, out, 65);
    }
    plat_mutex_unlock(&m->mu);
    return 0;
}

int sha_file(const char *path, char out[65]) {
    plat_file f;
    if (plat_file_open_path(path, &f)) return -1;
    int e = hash_open(NULL, f, out);
    plat_file_close(f);
    return e ? -1 : 0;
}
