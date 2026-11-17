/* The fake admin_os and compat_os (fake_os.h), over plat_mac.c's descriptors. */
#include "fake_os.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* -- compat_os */

static int f_entries(void *ctx, compat_uninstall *out, int max, int *incomplete) {
    fake *f = ctx;
    *incomplete = 0;
    int n = f->n_entries < max ? f->n_entries : max;
    memcpy(out, f->entries, (size_t)n * sizeof *out);
    return n;
}

/* AlphaTheta's: the file's bytes hold "ALPHATHETA-SIGNED" and not the bridge's text */
static int f_pin(void *ctx, const char *path, plat_file pf) {
    fake *f = ctx;
    (void)path;
    f->pin_calls++;
    if (f->pin_error) return COMPAT_PIN_ERROR;
    if (f->pin_ok_left < 0) return COMPAT_PIN_NO;
    if (f->pin_ok_left > 0 && --f->pin_ok_left == 0) f->pin_ok_left = -1;   /* this one still as usual */
    struct stat st;
    if (fstat(pf.fd, &st)) return COMPAT_PIN_ERROR;
    size_t n = (size_t)st.st_size;
    char *b = malloc(n + 1);
    if (!b) return COMPAT_PIN_ERROR;
    ssize_t got = pread(pf.fd, b, n, 0);
    int r = COMPAT_PIN_NO;
    if (got == (ssize_t)n) {
        b[n] = 0;
        int signed_ = memmem(b, n, "ALPHATHETA-SIGNED", 17) != NULL, bridge = memmem(b, n, "rbstems bridge: ", 16) != NULL;
        r = signed_ && !bridge ? COMPAT_PIN_OK : COMPAT_PIN_NO;
    }
    free(b);
    return r;
}

static int f_procs(void *ctx, compat_proc *out, int max) {
    fake *f = ctx;
    if (f->procs_fail) return -1;
    int n = f->n_procs < max ? f->n_procs : max;
    memcpy(out, f->procs, (size_t)n * sizeof *out);
    return n;
}

static uint16_t f_native(void *ctx) {
    (void)ctx;
    return COMPAT_MACHINE_AMD64;
}

/* -- admin_os */

static int dir_path(plat_dir d, char out[PATH_MAX]) { return fcntl(d.fd, F_GETPATH, out) == -1 ? errno : 0; }

static int bad_acl(fake *f, const char *path) {
    for (int i = 0; i < f->n_bad_acl; i++) {
        char real[PATH_MAX];
        const char *want = realpath(f->bad_acl[i], real) ? real : f->bad_acl[i];
        if (!strcmp(want, path)) return 1;
    }
    return 0;
}

static int f_hold_dir(void *ctx, plat_dir parent, const char *name, plat_dir *out) {
    fake *f = ctx;
    int e = plat_dir_ok(parent) ? plat_dir_open_at(parent, name, out) : plat_dir_open(name, out);
    if (e) return e;
    char p[PATH_MAX];
    if ((e = dir_path(*out, p)) || bad_acl(f, p)) {
        plat_dir_close(*out);
        out->fd = -1;
        return e ? e : EACCES;
    }
    return 0;
}

static int f_root_open(void *ctx, const char *path, int create, plat_dir *out) {
    fake *f = ctx;
    struct stat st;
    out->fd = -1;
    int made = 0;
    if (lstat(path, &st)) {
        if (errno != ENOENT || !create) return errno;
        if (mkdir(path, 0755)) return errno;
        made = 1;
    }
    int e = f_hold_dir(ctx, (plat_dir){-1}, path, out);
    if (!e && !made && f->root_not_exact) {
        plat_dir_close(*out);
        out->fd = -1;
        return EACCES;
    }
    return e;
}

static int f_file_admin_only(void *ctx, plat_dir d, const char *name) {
    fake *f = ctx;
    struct stat st;
    if (fstatat(d.fd, name, &st, AT_SYMLINK_NOFOLLOW)) return errno;
    if (S_ISLNK(st.st_mode)) return ELOOP;
    char p[PATH_MAX], full[PATH_MAX + 300];
    if (dir_path(d, p)) return EIO;
    snprintf(full, sizeof full, "%s/%s", p, name);
    return bad_acl(f, full) ? EACCES : 0;
}

static int f_mkdir_at(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    return plat_mkdir_at(d, name);
}

static int f_rmdir_at(void *ctx, plat_dir d, const char *name) {
    (void)ctx;
    struct stat st;
    if (fstatat(d.fd, name, &st, AT_SYMLINK_NOFOLLOW)) return errno;
    if (unlinkat(d.fd, name, S_ISLNK(st.st_mode) ? 0 : AT_REMOVEDIR)) return errno;
    return 0;
}

static int f_rename_noreplace(void *ctx, plat_dir d, const char *from, const char *to) {
    (void)ctx;
    return renameatx_np(d.fd, from, d.fd, to, RENAME_EXCL) ? errno : 0;
}

static int f_lock(void *ctx, plat_dir root, void **token) {
    (void)ctx;
    int fd = openat(root.fd, "lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return errno;
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        int e = errno;
        close(fd);
        return e == EWOULDBLOCK ? EBUSY : e;
    }
    *token = (void *)(intptr_t)(fd + 1);
    return 0;
}

static void f_unlock(void *ctx, void *token) {
    (void)ctx;
    close((int)(intptr_t)token - 1);
}

static int f_mapped(void *ctx, plat_dir d, const char *name) {
    fake *f = ctx;
    (void)d;
    (void)name;
    return f->mapped;
}

static int f_windows(void *ctx) { return ((fake *)ctx)->windows; }

static int f_load_check(void *ctx, const char *path, const char *symbol) {
    fake *f = ctx;
    f->load_calls++;
    if (f->on_load) f->on_load(f);
    if (f->load_fail) return -1;
    struct stat st;
    (void)symbol;
    return stat(path, &st) ? -1 : 0;
}

static int f_self_path(void *ctx, char *out, size_t n) {
    fake *f = ctx;
    if (!f->self[0] || strlen(f->self) >= n) return -1;
    memcpy(out, f->self, strlen(f->self) + 1);
    return 0;
}

static int f_delete_self(void *ctx, const char *self, const char *const folders[3], int *reboot) {
    fake *f = ctx;
    *reboot = 0;
    f->n_scheduled = 0;
    if (!f->reboot) {
        if (unlink(self)) return errno;
        for (int i = 0; i < 3; i++)
            if (f->rmdir_fail || rmdir(folders[i])) {
                if (f->schedule_fail) *reboot |= ADMIN_FOLDERS_STAY;
                else {
                    snprintf(f->scheduled[f->n_scheduled++], 1024, "%s", folders[i]);
                    *reboot |= ADMIN_GONE_AT_RESTART;
                }
            }
        return 0;
    }
    char del[1100];
    snprintf(del, sizeof del, "%s.%04d.del", self, f->rnd++);
    if (rename(self, del)) return errno;
    snprintf(f->scheduled[f->n_scheduled++], 1024, "%s", del);
    *reboot = ADMIN_GONE_AT_RESTART;
    for (int i = 0; i < 3; i++) {
        if (f->schedule_fail) *reboot |= ADMIN_FOLDERS_STAY;
        else snprintf(f->scheduled[f->n_scheduled++], 1024, "%s", folders[i]);
    }
    return 0;
}

static void f_say(void *ctx, const char *line) {
    fake *f = ctx;
    size_t n = strlen(line);
    if (f->verbose) fprintf(stderr, "  | %s\n", line);
    if (f->said_n + n + 2 < sizeof f->said) {
        memcpy(f->said + f->said_n, line, n);
        f->said_n += n;
        f->said[f->said_n++] = '\n';
        f->said[f->said_n] = 0;
    }
}

static void f_sleep(void *ctx, uint32_t ms) {
    fake *f = ctx;
    f->clock += ms;
    f->sleeps++;
    if (f->rekordbox_exits_after && f->sleeps >= f->rekordbox_exits_after) f->n_procs = 0;
}

static uint64_t f_now(void *ctx) { return ((fake *)ctx)->clock; }

static void f_random_hex(void *ctx, char *out, size_t n) {
    fake *f = ctx;
    static const char h[] = "0123456789abcdef";
    unsigned v = (unsigned)f->rnd++ * 2654435761u;
    for (size_t i = 0; i < n; i++) {
        out[i] = h[v & 15];
        v = v * 1103515245u + 12345u;
    }
    out[n] = 0;
}

void fake_init(fake *f) {
    memset(f, 0, sizeof *f);
    f->windows = 0;
    f->mapped = 0;
    f->verbose = getenv("ADMIN_TEST_VERBOSE") != NULL;
    f->compat = (compat_os){f_entries, f_pin, f_procs, f_native, f};
    f->os = (admin_os){&f->compat,        f,          f_root_open, f_hold_dir, f_file_admin_only, f_mkdir_at, f_rmdir_at,
                       f_rename_noreplace, f_lock,     f_unlock,    f_mapped,   f_windows,         f_load_check, f_self_path,
                       f_delete_self,      f_say,      f_sleep,     f_now,      f_random_hex};
}

int fake_said(const fake *f, const char *s) { return strstr(f->said, s) != NULL; }

void fake_clear_said(fake *f) {
    f->said_n = 0;
    f->said[0] = 0;
}

void fake_entry(fake *f, const char *folder, const char *dv) {
    compat_uninstall *e = &f->entries[f->n_entries++];
    memset(e, 0, sizeof *e);
    const char *name = strrchr(folder, '/');
    snprintf(e->key, sizeof e->key, "Pioneer %s", name ? name + 1 : folder);
    snprintf(e->display_name, sizeof e->display_name, "%s 64bit", name ? name + 1 : folder);
    snprintf(e->display_version, sizeof e->display_version, "%s", dv);
    snprintf(e->install_location, sizeof e->install_location, "%s", folder);
}

void fake_proc(fake *f, const char *name, const char *path) {
    compat_proc *p = &f->procs[f->n_procs++];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    snprintf(p->path, sizeof p->path, "%s", path);
    p->machine = COMPAT_MACHINE_AMD64;
}
