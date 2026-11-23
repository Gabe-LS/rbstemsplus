/* The fake admin_os and compat_os (fake_os.h), over plat_mac.c's descriptors. */
#include "fake_os.h"

#include "../../c/receipt.h"

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
    *incomplete = f->entries_partial || f->n_entries > max;
    int n = f->n_entries < max ? f->n_entries : max;
    memcpy(out, f->entries, (size_t)n * sizeof *out);
    return n;
}

/* AlphaTheta's: the file's bytes hold "ALPHATHETA-SIGNED" and not the bridge's text */
static int f_pin(void *ctx, const char *path, plat_file pf) {
    fake *f = ctx;
    f->pin_calls++;
    if (f->on_pin) f->on_pin(f, path);
    if (f->pin_error) return COMPAT_PIN_ERROR;
    if (f->pin_no_sub && strlen(path) >= strlen(f->pin_no_sub) && !strcmp(path + strlen(path) - strlen(f->pin_no_sub), f->pin_no_sub)) return COMPAT_PIN_NO;
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
        if (memmem(b, n, "ALPHATHETA-UNCHECKABLE", 21)) r = COMPAT_PIN_ERROR;
        else if (signed_ && !bridge) r = COMPAT_PIN_OK;
    }
    free(b);
    return r;
}

static int f_procs(void *ctx, compat_proc *out, int max) {
    fake *f = ctx;
    f->procs_seen++;
    if (f->on_procs) f->on_procs(f, f->procs_seen);
    if (f->procs_fail) return -1;
    if (f->procs_from_call && ++f->proc_calls < f->procs_from_call) return 0;
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

static int f_root_open(void *ctx, const char *path, int create, plat_dir *out, int *made_out) {
    fake *f = ctx;
    struct stat st;
    out->fd = -1;
    int made = 0;
    *made_out = 0;
    if (lstat(path, &st)) {
        if (errno != ENOENT || !create) return errno;
        if (f->on_root_create) f->on_root_create(f);
        /* as Windows' CreateDirectoryW: one made meanwhile is opened, never made */
        int me = mkdir(path, 0755) ? errno : 0;
        if (me && me != EEXIST) return me;
        made = !me;
    }
    int e = f_hold_dir(ctx, (plat_dir){-1}, path, out);
    if (!e && !made && f->root_not_exact) {
        plat_dir_close(*out);
        out->fd = -1;
        return EACCES;
    }
    *made_out = made;
    return e;
}

static int f_file_admin_only(void *ctx, plat_dir d, const char *name) {
    fake *f = ctx;
    char p[PATH_MAX], full[PATH_MAX + 300];
    if (dir_path(d, p)) return EIO;
    snprintf(full, sizeof full, "%s/%s", p, name);
    if (f->on_admin_only) f->on_admin_only(f, full);   /* before the file is looked at: it may go */
    struct stat st;
    if (fstatat(d.fd, name, &st, AT_SYMLINK_NOFOLLOW)) return errno;
    if (S_ISLNK(st.st_mode)) return ELOOP;
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

static int f_lock(void *ctx, plat_dir root, void **token, int *made) {
    (void)ctx;
    int fd = openat(root.fd, "lock", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    *made = fd >= 0;
    if (fd < 0 && errno == EEXIST) fd = openat(root.fd, "lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
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
    if (f->on_mapped) f->on_mapped(f);
    return f->mapped;
}

static int f_windows(void *ctx) { return ((fake *)ctx)->windows; }

static int f_load_check(void *ctx, const char *path, const char *symbol) {
    fake *f = ctx;
    f->load_calls++;
    if (f->on_load) f->on_load(f);
    if (f->load_fail || (f->load_fail_sub && strstr(path, f->load_fail_sub))) return -1;
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
    if (f->on_random) f->on_random(f);
    static const char h[] = "0123456789abcdef";
    unsigned v = (unsigned)f->rnd++ * 2654435761u;
    for (size_t i = 0; i < n; i++) {
        out[i] = h[v & 15];
        v = v * 1103515245u + 12345u;
    }
    out[n] = 0;
}

static int f_hold_file(void *ctx, plat_dir d, const char *name, void **token) {
    fake *f = ctx;
    char p[PATH_MAX], full[PATH_MAX + 300];
    struct stat st;
    *token = NULL;
    if (dir_path(d, p)) return EIO;
    if (fstatat(d.fd, name, &st, AT_SYMLINK_NOFOLLOW)) return errno;
    if (S_ISLNK(st.st_mode)) return ELOOP;
    snprintf(full, sizeof full, "%s/%s", p, name);
    for (int i = 0; i < 4; i++)
        if (!f->held_file[i][0]) {
            snprintf(f->held_file[i], sizeof f->held_file[i], "%s", full);
            f->file_holds++;
            *token = (void *)(intptr_t)(i + 1);
            return 0;
        }
    return EMFILE;
}

static void f_release_file(void *ctx, void *token) {
    fake *f = ctx;
    intptr_t i = (intptr_t)token - 1;
    if (i >= 0 && i < 4) f->held_file[i][0] = 0;
}

int fake_file_held(const fake *f, const char *path) {
    char real[PATH_MAX];
    const char *want = realpath(path, real) ? real : path;
    for (int i = 0; i < 4; i++)
        if (f->held_file[i][0] && !strcmp(f->held_file[i], want)) return 1;
    return 0;
}

void fake_init(fake *f) {
    memset(f, 0, sizeof *f);
    f->windows = 0;
    f->mapped = 0;
    f->verbose = getenv("ADMIN_TEST_VERBOSE") != NULL;
    f->compat = (compat_os){f_entries, f_pin, f_procs, f_native, f};
    f->os = (admin_os){&f->compat,        f,          f_root_open, f_hold_dir, f_file_admin_only, f_mkdir_at, f_rmdir_at,
                       f_rename_noreplace, f_lock,     f_unlock,    f_mapped,   f_windows,         f_load_check, f_self_path,
                       f_delete_self,      f_say,      f_sleep,     f_now,      f_random_hex,      f_hold_file,  f_release_file};
}

#ifdef ADMIN_TEST_HOLD
/* worker.h's test-build hold: recorded and said, the case's hook run, never a wait */
void admin_test_mid_hold(const admin_os *os, const char *where, const char *version) {
    fake *f = os->ctx;
    if (f->mid_target && (!version || strcmp(f->mid_target, version))) return;
    f->mid_holds++;
    snprintf(f->mid_where, sizeof f->mid_where, "%s", where);
    char line[320];
    snprintf(line, sizeof line, "test build: held 0 s mid-work: %s", where);
    f_say(f, line);
    if (f->on_mid_hold) f->on_mid_hold(f);
}

int admin_test_fail_target(const admin_os *os, const char *version) {
    const fake *f = os->ctx;
    return f->fail_target && !strcmp(f->fail_target, version);
}
#endif

int fake_said(const fake *f, const char *s) { return strstr(f->said, s) != NULL; }

void fake_clear_said(fake *f) {
    f->said_n = 0;
    f->said[0] = 0;
}

void fake_entry(fake *f, const char *folder, const char *dv) {
    if (f->n_entries == FAKE_MAX) abort();
    compat_uninstall *e = &f->entries[f->n_entries++];
    memset(e, 0, sizeof *e);
    const char *name = strrchr(folder, '/');
    snprintf(e->key, sizeof e->key, "Pioneer %s", name ? name + 1 : folder);
    snprintf(e->display_name, sizeof e->display_name, "%s 64bit", name ? name + 1 : folder);
    snprintf(e->display_version, sizeof e->display_version, "%s", dv);
    snprintf(e->install_location, sizeof e->install_location, "%s", folder);
}

void fake_stock_dll(const char *version, char *out, size_t n) {
    if ((size_t)snprintf(out, n, "MZ onnxruntime-1.18 #cert %s ALPHATHETA-SIGNED", version) >= n) abort();
}

void fake_stock_code(char out[65]) {
    static const char code[] = "MZ onnxruntime-1.18";
    unsigned char d[32];
    if (plat_sha256(code, sizeof code - 1, d)) abort();
    for (size_t i = 0; i < sizeof d; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void fixture_dir(const char *path) {
    if (mkdir(path, 0755) && errno != EEXIST) abort();
}

static void fixture_file(const char *path, const void *bytes, size_t n) {
    FILE *fp = fopen(path, "wb");
    if (!fp || fwrite(bytes, 1, n, fp) != n || fclose(fp)) abort();
}

static void fixture_sha(const char *bytes, char out[65]) {
    unsigned char digest[32];
    if (plat_sha256(bytes, strlen(bytes), digest)) abort();
    for (size_t i = 0; i < sizeof digest; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

static void fixture_exe(const fake_inventory *inventory, const fake_install *install, const char *path) {
    static const unsigned char v19[] = {'7', 0, '.', 0, '2', 0, '.', 0, '1', 0, '9', 0};
    const char *mark = install->trust == FAKE_RB_SIGNED ? "ALPHATHETA-SIGNED" :
                       install->trust == FAKE_RB_UNVERIFIABLE ? "ALPHATHETA-UNCHECKABLE" : "";
    size_t n = inventory->exe_size;
    unsigned char *exe = malloc(n + strlen(mark));
    if (!exe || strlen(install->version) != sizeof v19 / 2) abort();
    memcpy(exe, inventory->exe_7219, n);
    for (size_t i = 0; i + sizeof v19 <= n; i++)
        if (!memcmp(exe + i, v19, sizeof v19))
            for (size_t k = 0; k < sizeof v19 / 2; k++) exe[i + 2 * k] = (unsigned char)install->version[k];
    memcpy(exe + n, mark, strlen(mark));
    fixture_file(path, exe, n + strlen(mark));
    free(exe);
}

void fake_inventory_add(fake *f, const fake_inventory *inventory, fake_install install) {
    char folder[COMPAT_PATH_MAX], path[COMPAT_PATH_MAX + 32], version[COMPAT_VERSION_MAX], stock[96];
    if ((size_t)snprintf(folder, sizeof folder, "%s/rekordbox %s", inventory->root, install.version) >= sizeof folder) abort();
    snprintf(version, sizeof version, "%s.0342", install.version);
    if (install.views & FAKE_REG_64) fake_entry(f, folder, version), f->entries[f->n_entries - 1].view = COMPAT_VIEW_64;
    if (install.views & FAKE_REG_32) fake_entry(f, folder, version), f->entries[f->n_entries - 1].view = COMPAT_VIEW_32;
    if (install.registry_only) return;
    fixture_dir(inventory->root);
    fixture_dir(folder);
    snprintf(path, sizeof path, "%s/rekordbox.exe", folder);
    fixture_exe(inventory, &install, path);
    fake_stock_dll(install.version, stock, sizeof stock);
    snprintf(path, sizeof path, "%s/onnxruntime.dll", folder);
    if (install.live == FAKE_RB_STOCK) fixture_file(path, stock, strlen(stock));
    if (install.live == FAKE_RB_BRIDGED) fixture_file(path, inventory->bridge, strlen(inventory->bridge));
    if (install.live == FAKE_RB_INTERRUPTED) {
        if (unlink(path) && errno != ENOENT) abort();
        snprintf(path, sizeof path, "%s/onnxruntime.dll.rbsp-new", folder);
        fixture_file(path, inventory->bridge, strlen(inventory->bridge));
    }
    if (install.live == FAKE_RB_STOCK) return;

    fixture_dir(inventory->stems_root);
    snprintf(path, sizeof path, "%s/pioneer", inventory->stems_root);
    fixture_dir(path);
    if ((size_t)snprintf(folder, sizeof folder, "%s/pioneer/%s", inventory->stems_root, install.version) >= sizeof folder) abort();
    fixture_dir(folder);
    snprintf(path, sizeof path, "%s/onnxruntime.dll", folder);
    fixture_file(path, stock, strlen(stock));
    char sha[65], sums[96];
    fixture_sha(stock, sha);
    snprintf(sums, sizeof sums, "%s  onnxruntime.dll\n", sha);
    snprintf(path, sizeof path, "%s/SHA256SUMS", folder);
    fixture_file(path, sums, strlen(sums));
    if (install.live == FAKE_RB_BRIDGED) {
        snprintf(path, sizeof path, "%s/ort", inventory->stems_root);
        fixture_dir(path);
        snprintf(path, sizeof path, "%s/ort/onnxruntime.dll", inventory->stems_root);
        if (access(path, F_OK)) fixture_file(path, stock, strlen(stock));
        /* the completed install's receipt: this folder, its rekordbox.exe, the bridge, the saved
         * original, the runtime */
        receipt rc;
        memset(&rc, 0, sizeof rc);
        char vfolder[COMPAT_PATH_MAX], text[RECEIPT_MAX + 1];
        snprintf(vfolder, sizeof vfolder, "%s/rekordbox %s", inventory->root, install.version);
        snprintf(rc.folder_name, sizeof rc.folder_name, "rekordbox %s", install.version);
        snprintf(rc.version, sizeof rc.version, "%s", install.version);
        snprintf(rc.payload_version, sizeof rc.payload_version, "1.5.0");
        plat_stat st;
        if (plat_stat_path(vfolder, 0, &st)) abort();
        plat_id_hex(st.id, rc.folder_id);
        char *b = NULL;
        size_t bn = 0;
        snprintf(path, sizeof path, "%s/rekordbox.exe", vfolder);
        if (plat_read_file(path, 64 << 20, &b, &bn)) abort();
        b = realloc(b, bn + 1);
        b[bn] = 0;
        unsigned char d[32];
        if (plat_sha256(b, bn, d)) abort();
        for (size_t k = 0; k < 32; k++) snprintf(rc.exe_sha256 + 2 * k, 3, "%02x", d[k]);
        free(b);
        fixture_sha(stock, rc.original_sha256);
        fixture_sha(inventory->bridge, rc.bridge_sha256);
        fake_stock_code(rc.runtime_code_sha256);   /* ort\'s copy: a stock DLL, every version's the same code */
        int tn = receipt_format(&rc, text, sizeof text);
        snprintf(path, sizeof path, "%s/receipts", inventory->stems_root);
        fixture_dir(path);
        snprintf(path, sizeof path, "%s/receipts/%s", inventory->stems_root, install.version);
        if (tn < 0) abort();
        fixture_file(path, text, (size_t)tn);
    }
}

void fake_proc(fake *f, const char *name, const char *path) {
    compat_proc *p = &f->procs[f->n_procs++];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    snprintf(p->path, sizeof p->path, "%s", path);
    p->machine = COMPAT_MACHINE_AMD64;
}
