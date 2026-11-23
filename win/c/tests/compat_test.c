/* compat.c on the Mac: the pure rules, the PE reader on exes the pinned toolchain built
 * (run.sh: pe/rekordbox.rc, pe/ort.rc), and the file facts, rekordbox's folders and R1's facts on
 * real files in a scratch folder, through plat_mac.c and a fake compat_os. */
#include "check.h"

#include "../compat.h"
#include "../inventory.h"
#include "../native_facts.h"
#include "../native_steps.h"

#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* -- files */

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(len > 0 ? (size_t)len : 1);
    if (b && len > 0 && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *n = len > 0 ? (size_t)len : 0;
    return b;
}

static void spit(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "can't write %s\n", path); abort(); }
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

static void sha_hex(const void *d, size_t n, char out[65]) {
    unsigned char h[32];
    CC_SHA256(d, (CC_LONG)n, h);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}

typedef struct {
    const unsigned char *b;
    size_t n;
} mem;
static int mem_read(void *ctx, uint64_t off, void *buf, size_t n) {
    const mem *m = ctx;
    if (off > m->n || n > m->n - off) return -1;
    memcpy(buf, m->b + off, n);
    return 0;
}

/* -- the fake OS */

typedef struct {
    compat_uninstall entries[COMPAT_ENTRIES_MAX];
    int n_entries;
    const char *signed_paths[8];   /* AlphaTheta's, by exact path */
    int n_signed;
    int pin_error, pin_errors_once, pin_calls;   /* pin_errors_once: that many ERRORs, then the real answer */
    const char *pin_error_path;                  /* an ERROR for this path only */
    compat_proc procs[8];
    int n_procs;
    const char *swap_from;   /* renamed over the path while it is pinned (once) */
    int entry_calls;         /* uninstall_entries asked (the registry walk) */
    const char *remove_trigger, *remove_target;   /* remove_target unlinked while remove_trigger is pinned (once) */
    const char *rn_trigger, *rn_src, *rn_dst;     /* rn_src renamed over rn_dst while rn_trigger is pinned (once) */
    const char *grow_trigger, *grow_path;           /* grow_path grown by a byte, its times kept, while grow_trigger is pinned (once) */
    uint64_t gen;            /* compat_uninstall_generation's stand-in, passed to the cache */
    int incomplete;          /* review RC4: the enumeration reported partial (a subkey unreadable) */
} fake;

static int fake_entries(void *ctx, compat_uninstall *out, int max, int *incomplete) {
    fake *f = ctx;
    f->entry_calls++;
    *incomplete = f->incomplete;
    int n = f->n_entries < max ? f->n_entries : max;
    memcpy(out, f->entries, (size_t)n * sizeof *out);
    return n;
}
static int fake_pin(void *ctx, const char *path, plat_file pf) {
    fake *f = ctx;
    (void)pf;
    f->pin_calls++;
    if (f->remove_trigger && !strcmp(path, f->remove_trigger)) {
        if (unlink(f->remove_target)) abort();
        f->remove_trigger = NULL;
    }
    if (f->rn_trigger && !strcmp(path, f->rn_trigger)) {
        if (rename(f->rn_src, f->rn_dst)) abort();
        f->rn_trigger = NULL;
    }
    if (f->grow_trigger && !strcmp(path, f->grow_trigger)) {
        struct stat o;
        struct timeval tv[2];
        if (stat(f->grow_path, &o)) abort();
        FILE *g = fopen(f->grow_path, "ab");
        if (!g || fputc('x', g) == EOF || fclose(g)) abort();
        tv[0].tv_sec = o.st_atime;
        tv[0].tv_usec = 0;
        tv[1].tv_sec = o.st_mtime;
        tv[1].tv_usec = 0;
        if (utimes(f->grow_path, tv)) abort();   /* the same mtime: only the size check can catch it */
        f->grow_trigger = NULL;
    }
    if (f->swap_from) {   /* another file renamed in while this one is pinned: the old one is unsigned */
        rename(f->swap_from, path);
        f->swap_from = NULL;
        return COMPAT_PIN_NO;
    }
    if (f->pin_error || (f->pin_error_path && !strcmp(path, f->pin_error_path))) return COMPAT_PIN_ERROR;
    if (f->pin_errors_once > 0) {
        f->pin_errors_once--;
        return COMPAT_PIN_ERROR;
    }
    for (int i = 0; i < f->n_signed; i++)
        if (!strcmp(f->signed_paths[i], path)) return COMPAT_PIN_OK;
    return COMPAT_PIN_NO;
}
static int fake_procs(void *ctx, compat_proc *out, int max) {
    fake *f = ctx;
    int n = f->n_procs < max ? f->n_procs : max;
    memcpy(out, f->procs, (size_t)n * sizeof *out);
    return n;
}
static uint16_t fake_native(void *ctx) {
    (void)ctx;
    return COMPAT_MACHINE_ARM64;
}

/* review RC5: this file opens but its read fails */
static const char *read_fail_path;
static int read_fails(const char *path) {
    return read_fail_path && !strcmp(path, read_fail_path);
}

/* the C1 hook: renames swap_src over the live DLL between R1's two reads, once */
static const char *swap_src;
static void swap_between(const char *path) {
    if (swap_src) rename(swap_src, path);
    swap_src = NULL;
}

static void entry(fake *f, const char *key, const char *dn, const char *dv, const char *us, const char *icon, const char *loc) {
    compat_uninstall *e = &f->entries[f->n_entries++];
    memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", key);
    snprintf(e->display_name, sizeof e->display_name, "%s", dn);
    snprintf(e->display_version, sizeof e->display_version, "%s", dv);
    snprintf(e->uninstall_string, sizeof e->uninstall_string, "%s", us);
    snprintf(e->display_icon, sizeof e->display_icon, "%s", icon);
    snprintf(e->install_location, sizeof e->install_location, "%s", loc);
}

static void pure(void) {
    CHECK(compat_same_short_version("7.2.19.0342", "7.2.19") && compat_same_short_version("7.2.19", "7.2.19.1")
              && compat_same_short_version("7..2.19", "7.2.19") && !compat_same_short_version("7.2.19", "7.2.1")
              && !compat_same_short_version("", "") && !compat_same_short_version("7.2.190", "7.2.19"),
          "shortVersion's first three parts");
    char v[64];
    CHECK(compat_folder_version("C:/Program Files/rekordbox/rekordbox 7.2.19", v, sizeof v) && !strcmp(v, "7.2.19"), "a folder's version");
    CHECK(compat_folder_version("Rekordbox 7.2.19", v, sizeof v), "the name, case-blind");
    CHECK(!compat_folder_version("rekordbox 7.2", v, sizeof v) && !compat_folder_version("rekordbox 7.2.19.1", v, sizeof v)
              && !compat_folder_version("rekordbox 7.2.19 (old)", v, sizeof v) && !compat_folder_version("rekordbox7.2.19", v, sizeof v)
              && !compat_folder_version("rekordbox 7.2.x", v, sizeof v),
          "not a version folder");

    compat_uninstall e;
    memset(&e, 0, sizeof e);
    snprintf(e.key, sizeof e.key, "Pioneer rekordbox 7.2.19");
    CHECK(compat_entry_is_rekordbox(&e), "by the key");
    snprintf(e.key, sizeof e.key, "{GUID}");
    snprintf(e.display_name, sizeof e.display_name, "rekordbox 7.2.19 64bit");
    CHECK(compat_entry_is_rekordbox(&e), "by the DisplayName");
    snprintf(e.display_name, sizeof e.display_name, "rekordboxAgent");
    CHECK(!compat_entry_is_rekordbox(&e), "another program");

    /* the folder an entry names: phase0-probes §2.4's entry (InstallLocation empty) */
    const char *root = "C:\\Program Files\\rekordbox";
    char out[COMPAT_PATH_MAX];
    memset(&e, 0, sizeof e);
    snprintf(e.uninstall_string, sizeof e.uninstall_string, "C:\\Program Files\\rekordbox\\rekordbox 7.2.19\\Uninstall rekordbox.exe");
    CHECK(compat_entry_folder(&e, root, out, sizeof out) && !strcmp(out, "C:/Program Files/rekordbox/rekordbox 7.2.19"),
          "from UninstallString: %s", out);
    snprintf(e.uninstall_string, sizeof e.uninstall_string, "\"C:\\Program Files\\rekordbox\\rekordbox 7.2.19\\Uninstall rekordbox.exe\" /S");
    CHECK(compat_entry_folder(&e, root, out, sizeof out) && !strcmp(out, "C:/Program Files/rekordbox/rekordbox 7.2.19"), "quoted, with an argument");
    snprintf(e.uninstall_string, sizeof e.uninstall_string, "C:\\Program Files\\rekordbox\\rekordbox 7.2.19\\Uninstall rekordbox.exe /S");
    CHECK(compat_entry_folder(&e, root, out, sizeof out) && !strcmp(out, "C:/Program Files/rekordbox/rekordbox 7.2.19"), "unquoted, with an argument");
    snprintf(e.uninstall_string, sizeof e.uninstall_string, "C:\\Program Files\\rekordbox\\Uninstall.exe");
    snprintf(e.display_icon, sizeof e.display_icon, "c:\\program files\\REKORDBOX\\rekordbox 7.2.18\\rekordbox.exe,0");
    CHECK(compat_entry_folder(&e, root, out, sizeof out) && !strcmp(out, "c:/program files/REKORDBOX/rekordbox 7.2.18"),
          "from DisplayIcon, its index dropped, case-blind: %s", out);
    snprintf(e.install_location, sizeof e.install_location, "C:\\Program Files\\rekordbox\\rekordbox 7.2.17\\");
    CHECK(compat_entry_folder(&e, root, out, sizeof out) && !strcmp(out, "C:/Program Files/rekordbox/rekordbox 7.2.17"), "InstallLocation first");
    static const char *const outside[] = {
        "C:\\Program Files\\rekordbox\\rekordbox 7.2.19\\sub\\Uninstall rekordbox.exe",   /* not a direct child */
        "C:\\Program Files\\rekordbox2\\rekordbox 7.2.19\\Uninstall rekordbox.exe",
        "D:\\Program Files\\rekordbox\\rekordbox 7.2.19\\Uninstall rekordbox.exe",
        "C:\\Program Files\\rekordbox\\..\\rekordbox 7.2.19\\Uninstall rekordbox.exe",
        "C:\\Program Files\\rekordbox\\rekordbox\\Uninstall rekordbox.exe",
        "C:\\Program Files\\rekordbox\\Uninstall rekordbox.exe",
        "C:\\Program Files\\rekordboxXrekordbox 7.2.19\\Uninstall rekordbox.exe",   /* the root's name runs on: no separator (C17) */
        "C:\\Program Files\\rekordboxrekordbox 7.2.19\\Uninstall rekordbox.exe",
        "",
    };
    for (size_t i = 0; i < sizeof outside / sizeof outside[0]; i++) {
        memset(&e, 0, sizeof e);
        snprintf(e.uninstall_string, sizeof e.uninstall_string, "%s", outside[i]);
        CHECK(!compat_entry_folder(&e, root, out, sizeof out), "not a version folder under the root: %s", outside[i]);
    }

    CHECK(compat_pin_subject("AlphaTheta Corporation", "0104-01-151827"), "the pin's subject");
    CHECK(!compat_pin_subject("AlphaTheta Corporation", "0104-01-151828") && !compat_pin_subject("AlphaTheta Corp", "0104-01-151827")
              && !compat_pin_subject("alphatheta corporation", "0104-01-151827") && !compat_pin_subject(NULL, "0104-01-151827")
              && !compat_pin_subject("Pioneer DJ Corporation", "0104-01-151827"),
          "anything else");

    CHECK(!strcmp(compat_machine_words(COMPAT_MACHINE_AMD64, COMPAT_MACHINE_ARM64), "x64 under emulation on ARM64")
              && !strcmp(compat_machine_words(COMPAT_MACHINE_AMD64, COMPAT_MACHINE_AMD64), "native")
              && !strcmp(compat_machine_words(COMPAT_MACHINE_ARM64, COMPAT_MACHINE_ARM64), "native")
              && !strcmp(compat_machine_words(COMPAT_MACHINE_UNKNOWN, COMPAT_MACHINE_ARM64), ""),
          "the report's machine words");

    /* the resampler check */
    static payload_manifest m;
    memset(&m, 0, sizeof m);
    m.has_windows = 1;
    m.n_resampler_proved = 2;
    snprintf(m.resampler_proved[0].product_version, PAYLOAD_NAME_MAX, "7.2.19");
    memset(m.resampler_proved[0].rekordbox_sha256, '7', 64);
    snprintf(m.resampler_proved[1].product_version, PAYLOAD_NAME_MAX, "7.2.20.0001");
    memset(m.resampler_proved[1].rekordbox_sha256, '8', 64);
    char s7[65], s8[65];
    memset(s7, '7', 64), s7[64] = 0;
    memset(s8, '8', 64), s8[64] = 0;
    CHECK(compat_resample_proved(&m, "7.2.19.0342", s7) && compat_resample_proved(&m, "7.2.19", s7), "a proved build");
    CHECK(!compat_resample_proved(&m, "7.2.19.0342", s8), "a listed version with another exe");
    CHECK(!compat_resample_proved(&m, "7.2.18.0001", s7), "another version with the exe's sha");
    CHECK(compat_resample_proved(&m, "7.2.20.0001", s8) && !compat_resample_proved(&m, "7.2.20.0002", s8), "a four-part entry matches only itself");
    m.has_windows = 0;
    CHECK(!compat_resample_proved(&m, "7.2.19.0342", s7) && !compat_resample_proved(NULL, "7.2.19", s7), "no windows block: nothing proved");

    /* processes */
    compat_proc p[4];
    memset(p, 0, sizeof p);
    snprintf(p[0].name, sizeof p[0].name, "rekordboxAgent.exe");
    snprintf(p[1].name, sizeof p[1].name, "REKORDBOX.EXE");
    snprintf(p[1].path, sizeof p[1].path, "C:/Program Files/rekordbox/rekordbox 7.2.18/rekordbox.exe");
    p[1].machine = COMPAT_MACHINE_AMD64;
    compat_procs pr;
    compat_procs_classify(p, 2, "C:/Program Files/rekordbox/rekordbox 7.2.19", &pr);
    CHECK(pr.running && !pr.running_here && !pr.updating && pr.machine == COMPAT_MACHINE_AMD64, "another version's rekordbox runs");
    compat_procs_classify(p, 2, "c:/program files/rekordbox/rekordbox 7.2.18", &pr);
    CHECK(pr.running && pr.running_here, "this version's (case-blind)");
    p[1].path[0] = 0;
    compat_procs_classify(p, 2, "C:/Program Files/rekordbox/rekordbox 7.2.19", &pr);
    CHECK(pr.running && pr.running_here, "another account's, its path unreadable: counts as this folder's");
    snprintf(p[2].name, sizeof p[2].name, "Upmgr rekordbox.exe");
    compat_procs_classify(p, 3, NULL, &pr);
    CHECK(pr.updating, "the updater");
    snprintf(p[2].name, sizeof p[2].name, "Uninstall rekordbox.exe");
    compat_procs_classify(p, 3, NULL, &pr);
    CHECK(pr.updating, "the uninstaller");
    compat_procs_classify(p, 1, NULL, &pr);
    CHECK(!pr.running && !pr.updating, "rekordboxAgent alone isn't rekordbox");
}

static void pe_files(const char *dir) {
    static const char *const arch[2] = {"x64", "arm64"};
    static const uint16_t machine[2] = {COMPAT_MACHINE_AMD64, COMPAT_MACHINE_ARM64};
    for (int a = 0; a < 2; a++) {
        char path[1024];
        size_t n;
        compat_pe pe;
        snprintf(path, sizeof path, "%s/rekordbox-%s.exe", dir, arch[a]);
        unsigned char *b = slurp(path, &n);
        CHECK(b != NULL, "%s", path);
        if (!b) continue;
        mem m = {b, n};
        CHECK(compat_pe_read(mem_read, &m, &pe) == 0 && pe.machine == machine[a] && !strcmp(pe.product_version, "7.2.19.0342") &&
                  !strcmp(pe.product_name, "rekordbox") && pe.has_fixed
                  && pe.fixed_product[0] == 7 && pe.fixed_product[1] == 2 && pe.fixed_product[2] == 19 && pe.fixed_product[3] == 342,
              "%s: machine %04x, ProductVersion \"%s\"", arch[a], pe.machine, pe.product_version);
        /* every truncation either reads or refuses; none reads past the end */
        int bad = 0;
        for (size_t k = 0; k < n; k += 1 + k / 64) {
            mem t = {b, k};
            if (compat_pe_read(mem_read, &t, &pe) == 0 && *pe.product_version && strcmp(pe.product_version, "7.2.19.0342")) bad++;
        }
        CHECK(bad == 0, "%s: truncated copies never give another version", arch[a]);
        free(b);

        snprintf(path, sizeof path, "%s/ort-%s.exe", dir, arch[a]);
        b = slurp(path, &n);
        m = (mem){b, n};
        CHECK(b && compat_pe_read(mem_read, &m, &pe) == 0 && !strcmp(pe.product_version, "Internal Build") && pe.has_fixed
                  && !pe.fixed_product[0] && !pe.fixed_product[3],
              "%s: the ORT's \"Internal Build\"", arch[a]);
        free(b);
        snprintf(path, sizeof path, "%s/plain-%s.exe", dir, arch[a]);
        b = slurp(path, &n);
        m = (mem){b, n};
        CHECK(b && compat_pe_read(mem_read, &m, &pe) == 0 && pe.machine == machine[a] && !*pe.product_version && !pe.has_fixed,
              "%s: no version resource", arch[a]);
        free(b);
    }
    mem none = {(const unsigned char *)"MZ not a PE at all, just text that is long enough to have an e_lfanew field.....", 80};
    compat_pe pe;
    CHECK(compat_pe_read(mem_read, &none, &pe) == -1, "not a PE");
}

static int digest_calls;
static int fake_code_digest(compat_read_fn read, void *ctx, uint64_t size, payload_sha out) {
    char b[5];
    if (size != sizeof b || read(ctx, 0, b, sizeof b) || memcmp(b, "hello", sizeof b)) return -1;
    digest_calls++;
    memset(out, 'a', 64);
    out[64] = 0;
    return 0;
}

static void t_ort_file_facts(const char *scratch) {
    char path[COMPAT_PATH_MAX];
    snprintf(path, sizeof path, "%s/ort-code-fixture", scratch);
    spit(path, "hello", 5);
    compat_file file;
    CHECK(!compat_file_facts(NULL, path, 0, &file) && file.readable && !file.code_sha256[0], "ordinary scan has no code digest");
    CHECK(!compat_ort_file_facts(NULL, path, 0, fake_code_digest, &file) && file.code_sha256[0] == 'a' && digest_calls == 1,
          "an ordinary memo entry cannot satisfy a request for a code digest");
    CHECK(!compat_ort_file_facts(NULL, path, 0, fake_code_digest, &file) && file.code_sha256[0] == 'a' && digest_calls == 1,
          "the code digest is memoised with the file stamp");
    unlink(path);
}

static const char *unlisted_v22;   /* review RE1: the folder whose stat fails */
static int re1_stat_fails(const char *path) {
    return unlisted_v22 && !strcmp(path, unlisted_v22);
}

static int t_inventory_current_equivalence(const compat_os *os, const char *root, compat_rekordbox *rb, int max,
                                           int *cur, int *state) {
    int n = compat_find_rekordbox(os, root, rb, max, cur, state);
    inv_list *list = malloc(sizeof *list);
    compat_rekordbox other[8];
    int inv_cur = -1, inv_state = -1;
    int nr = list && inv_read(os, root, 0, list) == 0 ? inv_current(list, other, max, &inv_cur, &inv_state) : -1;
    int same = nr == n && inv_cur == *cur && inv_state == *state;
    for (int i = 0; same && i < n; i++) {
        same = !strcmp(other[i].folder, rb[i].folder) && other[i].candidate == rb[i].candidate
            && other[i].ok == rb[i].ok && other[i].agree == rb[i].agree && other[i].unknown == rb[i].unknown;
    }
    CHECK(same, "inventory current equals compatibility walk: n %d/%d current %d/%d state %d/%d",
          nr, n, inv_cur, *cur, inv_state, *state);
    free(list);
    return n;
}

static void t_inventory(const char *scratch, const char *pedir) {
    char root[1024], folder19[1200], folder20[1200], folder9[1200], stale[1200], exe19[1300], source[1200];
    snprintf(root, sizeof root, "%s/inventory", scratch);
    snprintf(folder19, sizeof folder19, "%s/rekordbox 7.2.19", root);
    snprintf(folder20, sizeof folder20, "%s/rekordbox 7.2.20", root);
    snprintf(folder9, sizeof folder9, "%s/rekordbox 7.2.9", root);
    snprintf(stale, sizeof stale, "%s/rekordbox 7.2.8", root);
    snprintf(exe19, sizeof exe19, "%s/rekordbox.exe", folder19);
    snprintf(source, sizeof source, "%s/rekordbox-x64.exe", pedir);
    size_t n = 0;
    unsigned char *exe = slurp(source, &n);
    inv_list *list = malloc(sizeof *list);
    CHECK(exe && list && !mkdir(root, 0755) && !mkdir(folder19, 0755)
          && !mkdir(folder20, 0755) && !mkdir(folder9, 0755), "inventory fixture");
    if (!exe || !list) { free(exe); free(list); return; }
    spit(exe19, exe, n);
    fake f = {0};
    compat_os os = {fake_entries, fake_pin, fake_procs, fake_native, &f};
    f.signed_paths[f.n_signed++] = exe19;
    entry(&f, "Pioneer rekordbox 7.2.19", "", "7.2.19.0342", "", "", folder19);
    entry(&f, "Pioneer rekordbox 7.2.19", "", "7.2.19.0343", "", "", folder19);
    entry(&f, "Pioneer rekordbox 7.2.20", "", "7.2.20.0354", "", "", folder20);
    entry(&f, "Pioneer rekordbox 7.2.8", "", "7.2.8", "", "", stale);
    f.entries[0].view = COMPAT_VIEW_64;
    f.entries[1].view = COMPAT_VIEW_32;
    CHECK(inv_read(&os, root, 0, list) == 0 && list->n == 4
          && !strcmp(list->it[0].version, "7.2.20") && !strcmp(list->it[1].version, "7.2.19")
          && !strcmp(list->it[2].version, "7.2.9") && !strcmp(list->it[3].version, "7.2.8"),
          "inventory sorts numeric versions and deduplicates both views");
    CHECK(list->it[1].trust == INV_VERIFIED && list->it[1].views == (INV_VIEW_64 | INV_VIEW_32),
          "both registry views are retained, build suffix alone isn't a conflict");
    plat_stat dir;
    CHECK(plat_stat_path(folder19, 0, &dir) == 0 && plat_id_same(dir.id, list->it[1].dir_id),
          "the verified row records its directory identity");
    CHECK(list->it[0].trust == INV_SETTLING && list->it[2].trust == INV_DISK_ONLY && list->it[3].trust == INV_STALE
          && list->registry_complete && list->disk_complete && !list->complete, "settling, disk-only and stale are distinct");
    strcpy(f.entries[1].display_version, "7.2.18");
    f.entries[2] = f.entries[--f.n_entries];     /* the settling folder is now disk-only */
    CHECK(inv_read(&os, root, 0, list) == 0 && list->complete && list->it[1].trust == INV_UNVERIFIED,
          "conflicting short display versions are unverified, with a complete walk");
    compat_rekordbox legacy[8];
    int cur, state;
    CHECK(compat_find_rekordbox(&os, root, legacy, 8, &cur, &state) == 2
          && cur == -1 && state == COMPAT_RB_UNVERIFIED, "the compatibility view uses the inventory's conflict verdict");
    f.incomplete = 1;
    CHECK(inv_read(&os, root, 0, list) == 0 && !list->registry_complete && !list->complete, "partial registry");
    f.incomplete = 0;
    int had = f.n_entries;
    f.n_entries = COMPAT_ENTRIES_MAX;
    CHECK(inv_read(&os, root, 0, list) == 0 && !list->registry_complete && !list->complete, "full buffer can be truncated");
    f.n_entries = had;
    compat_test_entries_alloc_fails = 1;
    CHECK(inv_read(&os, root, 0, list) == -1 && !list->registry_complete && !list->complete, "entry allocation failed");
    compat_test_entries_alloc_fails = 0;
    unlisted_v22 = root;
    compat_test_stat_fails = re1_stat_fails;
    CHECK(inv_read(&os, root, 0, list) == 0 && list->registry_complete && !list->disk_complete && !list->complete,
          "registry survives a failed root listing");
    compat_test_stat_fails = NULL;
    unlisted_v22 = NULL;
    strcpy(f.entries[1].display_version, "7.2.19.0343");
    f.pin_error = 1;
    CHECK(inv_read(&os, root, COMPAT_FRESH, list) == 0 && list->it[1].trust == INV_NO_ANSWER && !list->complete,
          "fresh inventory cannot reuse an EXE pin after the pin stops answering");

    inv_cache cache = INV_CACHE_INIT;
    f.pin_error = 0;
    CHECK(inv_read_cached(&cache, &os, 17, root, list) == 0 && list->complete && list->gen == 17
          && inv_read_cached(&cache, &os, 17, root, list) == 0 && cache.walks == 1, "whole inventory reused");
    unlink(exe19);
    CHECK(inv_read_cached(&cache, &os, 17, root, list) == 0 && list->it[1].trust == INV_SETTLING
          && inv_read_cached(&cache, &os, 17, root, list) == 0 && cache.walks == 3, "settling is never cached");
    inv_cache_clear(&cache);
    free(exe);
    free(list);
    compat_memo_clear();
}

static void t_inventory_rules(void) {
    static payload_manifest m;
    m.has_windows = 1;
    m.n_win_ort_bind = 2;
    strcpy(m.win_ort_bind[0].rekordbox, "7.2.19");
    strcpy(m.win_ort_bind[1].rekordbox, "7.2.20");
    memset(m.win_ort_bind[0].sha256, 'a', 64);
    memset(m.win_ort_bind[1].sha256, 'b', 64);
    m.n_win_ort_sha256 = 2;
    strcpy(m.win_ort_sha256[0], m.win_ort_bind[0].sha256);
    strcpy(m.win_ort_sha256[1], m.win_ort_bind[1].sha256);
    m.n_win_rekordbox = 2;
    strcpy(m.win_rekordbox[0], "7.2.19");
    strcpy(m.win_rekordbox[1], "7.2.20");
    memset(m.win_ort_code_sha256, 'c', 64);
    memset(m.win_bridge.sha256, 'd', 64);
    compat_file original = {.exists = 1, .present = 1, .readable = 1, .pin = COMPAT_PIN_OK};
    strcpy(original.sha256, m.win_ort_bind[0].sha256);
    strcpy(original.code_sha256, m.win_ort_code_sha256);
    CHECK(inv_original_bound(&m, "7.2.19.0342", &original)
          && !inv_original_bound(&m, "7.2.20.0354", &original), "same code is bound to different originals per version");
    strcpy(original.sha256, m.win_ort_bind[1].sha256);
    CHECK(inv_original_bound(&m, "7.2.20.0354", &original)
          && !inv_original_bound(&m, "7.2.19.0342", &original), "the re-signed twin is bound only to its own version");
    original.code_sha256[0] = 'a';
    CHECK(!inv_original_bound(&m, "7.2.20.0354", &original), "exact original hash does not substitute for code proof");
    strcpy(original.code_sha256, m.win_ort_code_sha256);
    original.pin = COMPAT_PIN_NO;
    CHECK(!inv_original_bound(&m, "7.2.20.0354", &original), "an exact hash still needs the signer pin");
    original.pin = COMPAT_PIN_OK;
    original.marker = 1;
    CHECK(!inv_original_bound(&m, "7.2.20.0354", &original), "a bridge is never a bound original");

    inv_list *l = calloc(1, sizeof *l);
    CHECK(l, "inventory rows allocated");
    if (!l) return;
    l->n = 5;
    l->complete = 1;
    strcpy(l->it[0].version, "7.2.20");
    strcpy(l->it[0].display_version, "7.2.20.0354");
    strcpy(l->it[1].version, "7.2.19");
    l->it[2].trust = INV_STALE;
    l->it[3].trust = l->it[4].trust = INV_DISK_ONLY;
    inv_assess_row a[5] = {
        {.live = COMPAT_LIVE_PIONEER, .listed = 1, .original_bound = 1, .running = 1},
        {.live = COMPAT_LIVE_OURS, .listed = 1, .original_bound = 1, .receipt_ok = 1},
        {.live = COMPAT_LIVE_OURS},
        {.live = COMPAT_LIVE_PIONEER},
        {.live = COMPAT_LIVE_OURS},
    };
    inv_row_view rows[5];
    int n = inv_rows(l, a, &m, rows, 5);
    CHECK(n == 3 && rows[0].state == INV_ROW_READY && rows[0].running && !strcmp(rows[0].display_version, "7.2.20.0354")
          && rows[1].state == INV_ROW_ON && rows[2].state == INV_ROW_REPAIR, "UI keeps real rows and disk-only bridges");
    a[1].receipt_ok = 0;
    n = inv_rows(l, a, &m, rows, 5);
    CHECK(n == 3 && rows[1].state == INV_ROW_REPAIR && rows[1].why == WHY_NOT_RECORDED,
          "D13: repair for a bridge without its matching receipt");
    a[1].listed = 0;
    n = inv_rows(l, a, &m, rows, 5);
    CHECK(n == 3 && rows[1].state == INV_ROW_REPAIR && rows[1].why == WHY_DELISTED_BRIDGE, "delisted bridge requires repair");
    a[0].original_bound = 0;
    strcpy(a[0].original_code, m.win_ort_code_sha256);
    n = inv_rows(l, a, &m, rows, 5);
    CHECK(n == 3 && rows[0].state == INV_ROW_UNSUPPORTED && rows[0].why == WHY_BUILD_UNLISTED,
          "a listed build with the wrong original hash and matching code is build-unlisted");
    a[0].original_code[0] = 'a';
    n = inv_rows(l, a, &m, rows, 5);
    CHECK(n == 3 && rows[0].state == INV_ROW_UNSUPPORTED && rows[0].why == WHY_ORT_UNLISTED,
          "a listed build with different ORT code is ORT-unlisted");
    l->it[0].trust = INV_NO_ANSWER;
    n = inv_rows(l, a, &m, rows, 5);
    CHECK(n == 3 && rows[0].state == INV_ROW_UNVERIFIED && rows[0].why == WHY_NO_ANSWER,
          "pin errors remain visible as no answer");
    l->n = 2;
    l->it[0].trust = l->it[1].trust = INV_VERIFIED;
    memset(l->it[0].exe.sha256, '2', 64);
    memset(l->it[1].exe.sha256, '1', 64);
    a[0] = (inv_assess_row){.live = COMPAT_LIVE_PIONEER, .listed = 1, .original_bound = 1};
    a[1] = (inv_assess_row){.live = COMPAT_LIVE_OURS, .listed = 1, .original_bound = 1, .receipt_ok = 1};
    memset(a[0].live_sha, '3', 64);
    strcpy(a[1].live_sha, m.win_bridge.sha256);
    CHECK(!*inv_outdated_bridge(l, a, m.win_bridge.sha256), "newest stock DLL is Ready, not a bridge update");
    a[0].listed = 0;
    CHECK(!*inv_outdated_bridge(l, a, m.win_bridge.sha256), "newest stock DLL is Unsupported, not a bridge update");
    a[1].live = COMPAT_LIVE_OURS_UNLISTED;
    memset(a[1].live_sha, '4', 64);
    CHECK(!strcmp(inv_outdated_bridge(l, a, m.win_bridge.sha256), a[1].live_sha), "select the older covered bridge behind the stock DLL");
    a[1].receipt_ok = 0;
    CHECK(!*inv_outdated_bridge(l, a, m.win_bridge.sha256), "a receipt-less bridge needs repair, not an update offer");
    a[0].listed = a[1].receipt_ok = 1;
    a[1].live = COMPAT_LIVE_OURS;
    admin_target t[2];
    CHECK(inv_targets(INV_VERB_INSTALL, l, a, &m, t, 2) == 1
          && !strcmp(t[0].version, "7.2.20") && !strcmp(t[0].exe16, "2222222222222222"),
          "only the not-yet-covered version in install scope");
    CHECK(inv_targets(INV_VERB_REINSTALL, l, a, &m, t, 2) == 2
          && !strcmp(t[0].version, "7.2.19") && !strcmp(t[1].version, "7.2.20"),
          "force covers both versions in ascending order");
    a[1].live = COMPAT_LIVE_OURS_UNLISTED;
    CHECK(inv_targets(INV_VERB_INSTALL, l, a, &m, t, 2) == 2, "a receipt-backed older bridge is eligible for upgrade");
    a[1].receipt_ok = 0;
    CHECK(inv_targets(INV_VERB_REINSTALL, l, a, &m, t, 2) == 1
          && !strcmp(t[0].version, "7.2.20"), "D13 excludes receipt-less bridges even under force");
    l->complete = 0;
    a[1].r1 = NF_R1_ORT_MISSING;
    a[0].r1 = -1;
    CHECK(inv_targets(INV_VERB_INSTALL, l, a, &m, t, 2) == -1, "incomplete inventory blocks swaps");
    CHECK(inv_targets(INV_VERB_RESTORE, l, a, &m, t, 2) == 1 && !strcmp(t[0].version, "7.2.19"),
          "restore scopes the known repair despite incomplete inventory and skips no-answer rows");

    /* uninstall (D3): complete, every bridge and repair row (disk-only, no exe hash too);
     * incomplete, only our bridges with their receipts, never -1 */
    l->n = 3;
    l->complete = 1;
    strcpy(l->it[2].version, "7.2.18");
    l->it[2].trust = INV_DISK_ONLY;
    memset(l->it[2].exe.sha256, 0, sizeof l->it[2].exe.sha256);
    a[0] = (inv_assess_row){.live = COMPAT_LIVE_PIONEER, .listed = 1, .original_bound = 1};
    a[1] = (inv_assess_row){.live = COMPAT_LIVE_OURS, .listed = 1, .original_bound = 1, .receipt_ok = 1};
    a[2] = (inv_assess_row){.live = COMPAT_LIVE_OURS_UNLISTED};
    admin_target u[3];
    CHECK(inv_targets(INV_VERB_UNINSTALL, l, a, &m, u, 3) == 2 && !strcmp(u[0].version, "7.2.18") && !u[0].exe16[0]
          && !strcmp(u[1].version, "7.2.19") && !strcmp(u[1].exe16, "1111111111111111"),
          "uninstall, complete: both bridges, the disk-only one without an exe hash");
    a[0].r1 = NF_R1_LIVE_MISSING;
    CHECK(inv_targets(INV_VERB_UNINSTALL, l, a, &m, u, 3) == 3, "uninstall, complete: a repair row is affected too");
    CHECK(inv_targets(INV_VERB_UNINSTALL, l, a, NULL, u, 3) == 3, "uninstall needs no payload");
    l->complete = 0;
    CHECK(inv_targets(INV_VERB_UNINSTALL, l, a, &m, u, 3) == 1 && !strcmp(u[0].version, "7.2.19"),
          "uninstall, incomplete: only the bridge with its receipt (D3), not the repair row nor the receipt-less bridge");
    a[1].receipt_ok = 0;
    CHECK(inv_targets(INV_VERB_UNINSTALL, l, a, &m, u, 3) == 0, "uninstall, incomplete, no receipt-backed bridge: nothing");
    l->it[1].trust = INV_DISK_ONLY;
    a[1].receipt_ok = 1;
    CHECK(inv_targets(INV_VERB_UNINSTALL, l, a, &m, u, 3) == 1 && !strcmp(u[0].version, "7.2.19"),
          "uninstall, incomplete: an on-disk folder with our bridge and its receipt is restored");
    free(l);
}

static void facts(const char *scratch, const char *pedir) {
    char root[1024], rbsp[1024], ortdir[1024], p[1200], q[1200];
    snprintf(root, sizeof root, "%s/Program Files/rekordbox", scratch);
    snprintf(rbsp, sizeof rbsp, "%s/Program Files/RB Stems Plus", scratch);
    snprintf(ortdir, sizeof ortdir, "%s/ort", rbsp);
    snprintf(p, sizeof p, "%s/Program Files", scratch);
    mkdir(p, 0755);
    mkdir(root, 0755);
    mkdir(rbsp, 0755);
    size_t exe_n, ort_n;
    snprintf(p, sizeof p, "%s/rekordbox-x64.exe", pedir);
    unsigned char *exe = slurp(p, &exe_n);
    snprintf(p, sizeof p, "%s/ort-x64.exe", pedir);
    unsigned char *ort = slurp(p, &ort_n);
    CHECK(exe && ort, "the PE fixtures");
    if (!exe || !ort) return;
    const char *bridge = "MZ... rbstems bridge: 1.5.0 ... rbstems-cache: rebuild=1 location=1;";

    char v19[1024], v18[1024], exe19[1200], exe18[1200], live[1200], ortf[1200];
    snprintf(v19, sizeof v19, "%s/rekordbox 7.2.19", root);
    snprintf(v18, sizeof v18, "%s/rekordbox 7.2.18", root);
    mkdir(v19, 0755);
    mkdir(v18, 0755);
    snprintf(exe19, sizeof exe19, "%s/rekordbox.exe", v19);
    snprintf(exe18, sizeof exe18, "%s/rekordbox.exe", v18);
    snprintf(live, sizeof live, "%s/onnxruntime.dll", v19);
    snprintf(ortf, sizeof ortf, "%s/onnxruntime.dll", ortdir);
    spit(exe19, exe, exe_n);
    spit(exe18, exe, exe_n);   /* its ProductVersion says 7.2.19: the versions don't agree */
    snprintf(p, sizeof p, "%s/rekordbox 7.2.20", root);
    symlink(v19, p);           /* a link in place of a version folder */

    fake f;
    memset(&f, 0, sizeof f);
    compat_os os = {fake_entries, fake_pin, fake_procs, fake_native, &f};
    f.signed_paths[f.n_signed++] = exe19;
    f.signed_paths[f.n_signed++] = exe18;
    snprintf(p, sizeof p, "%s/rekordbox 7.2.19/Uninstall rekordbox.exe", root);
    entry(&f, "Pioneer rekordbox 7.2.19", "rekordbox 7.2.19 64bit", "7.2.19.0342", p, "", "");
    snprintf(p, sizeof p, "%s/rekordbox 7.2.18/rekordbox.exe,0", root);
    entry(&f, "Pioneer rekordbox 7.2.18", "rekordbox 7.2.18 64bit", "7.2.18.0100", "", p, "");
    snprintf(p, sizeof p, "%s/rekordbox 7.2.20/Uninstall rekordbox.exe", root);
    entry(&f, "Pioneer rekordbox 7.2.20", "rekordbox 7.2.20 64bit", "7.2.20.0001", p, "", "");
    snprintf(p, sizeof p, "%s/rekordbox 7.2.19/Uninstall rekordbox.exe", root);
    entry(&f, "{dup}", "rekordbox 7.2.19 64bit", "7.2.19.0342", p, "", "");   /* the other registry view */
    entry(&f, "Other", "Another program", "1.0", p, "", "");

    compat_memo_clear();
    compat_rekordbox rb[8];
    int cur, st;
    int n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    CHECK(n == 3, "three version folders named (the duplicate once): %d", n);
    CHECK(n == 3 && !strcmp(rb[0].folder_version, "7.2.20") && !strcmp(rb[1].folder_version, "7.2.19") && !strcmp(rb[2].folder_version, "7.2.18"),
          "newest first");
    CHECK(cur == 1 && rb[1].ok && rb[1].agree && rb[1].exe.pin == COMPAT_PIN_OK && !strcmp(rb[1].exe.pe.product_version, "7.2.19.0342"),
          "the current one is 7.2.19 (%d)", cur);
    CHECK(n == 3 && !rb[0].ok && !rb[0].exe.exists, "7.2.20: a link, not a version folder");
    CHECK(n == 3 && !rb[2].ok && !rb[2].agree && rb[2].exe.pin == COMPAT_PIN_OK, "7.2.18: its exe's version doesn't agree");
    char sha[65];
    sha_hex(exe, exe_n, sha);
    CHECK(n == 3 && !strcmp(rb[1].exe.sha256, sha) && rb[1].exe.size == (int64_t)exe_n, "rekordbox.exe's sha256, for the resampler check");
    int calls = f.pin_calls;
    n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    CHECK(f.pin_calls == calls && cur == 1, "again: memoised, no new pin (%d calls)", f.pin_calls - calls);
    f.n_signed = 1;   /* 7.2.18's exe no longer AlphaTheta's... but memoised: its size and time didn't change */
    spit(exe19, exe, exe_n - 1);   /* 7.2.19's exe changed: read again */
    t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    CHECK(f.pin_calls == calls + 1, "a changed file is checked again (%d)", f.pin_calls - calls);
    spit(exe19, exe, exe_n);
    f.n_signed = 2;
    compat_memo_clear();
    f.pin_error = 1;
    t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    CHECK(cur == -1, "the pin can't be checked: no current rekordbox");
    f.pin_error = 0;
    calls = f.pin_calls;
    t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    CHECK(cur == 1 && f.pin_calls > calls, "the pin answers again: asked again, not taken from the memo (C7)");

    /* C8, owner decided: the newest folder holding rekordbox.exe decides, with no fallback */
    char v21[1024], exe21[1200];
    snprintf(v21, sizeof v21, "%s/rekordbox 7.2.21", root);
    snprintf(exe21, sizeof exe21, "%s/rekordbox.exe", v21);
    mkdir(v21, 0755);
    spit(exe21, exe, exe_n);   /* its ProductVersion says 7.2.19: the versions disagree */
    f.signed_paths[f.n_signed++] = exe21;
    snprintf(p, sizeof p, "%s/Uninstall rekordbox.exe", v21);
    entry(&f, "Pioneer rekordbox 7.2.21", "rekordbox 7.2.21 64bit", "7.2.21.0001", p, "", "");
    n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    CHECK(n == 4 && cur == -1 && st == COMPAT_RB_UNVERIFIED && rb[0].candidate && !strcmp(rb[0].folder_version, "7.2.21") && !rb[2].candidate,
          "the newest disagrees: couldn't verify rekordbox 7.2.21, no fallback to 7.2.19 (cur %d, state %d)", cur, st);
    char r1f[1024] = "x", r1v[64] = "x";
    CHECK(native_r1_current(&os, root, r1f, sizeof r1f, r1v, sizeof r1v) == 0 && !r1f[0] && !r1v[0],
          "CW1: R1's folder and version: none while rekordbox is unverified");
    unlink(exe21);
    n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    CHECK(cur == 2 && st == COMPAT_RB_OK && rb[2].candidate && !strcmp(rb[2].folder_version, "7.2.19"),
          "a newest folder without rekordbox.exe is passed over: 7.2.19 (cur %d)", cur);
    compat_memo_clear();
    f.signed_paths[0] = "nobody's";   /* 7.2.19's exe no longer AlphaTheta's */
    t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    CHECK(cur == -1 && st == COMPAT_RB_UNVERIFIED && rb[2].candidate && !rb[3].candidate, "its pin NO: couldn't verify, 7.2.18 not taken");
    compat_memo_clear();
    f.pin_error = 1;
    t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    CHECK(cur == -1 && st == COMPAT_RB_NO_ANSWER, "its pin ERROR: no answer");
    f.pin_error = 0;
    chmod(exe19, 0);
    compat_memo_clear();
    t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    chmod(exe19, 0644);
    CHECK(cur == -1 && st == COMPAT_RB_NO_ANSWER && rb[2].candidate, "its rekordbox.exe unreadable: no answer, no fallback");
    f.signed_paths[0] = exe19;
    f.n_entries--;
    f.n_signed--;
    rmdir(v21);
    compat_memo_clear();
    n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
    CHECK(n == 3 && cur == 1 && st == COMPAT_RB_OK, "back to 7.2.19");
    CHECK(native_r1_folder(&os, root, r1f, sizeof r1f) && !strcmp(r1f, v19), "CW1: R1's folder: the verified current one (%s)", r1f);
    CHECK(native_r1_current(&os, root, r1f, sizeof r1f, r1v, sizeof r1v) == 1 && !strcmp(r1f, v19) && !strcmp(r1v, "7.2.19"),
          "CW1: R1's folder and version: the verified current ones (%s, %s)", r1f, r1v);
    int had = f.n_entries;
    f.n_entries = 0;   /* rekordbox uninstalled: no entry names a folder */
    strcpy(r1v, "x");
    CHECK(native_r1_current(&os, root, r1f, sizeof r1f, r1v, sizeof r1v) == 0 && !r1f[0] && !r1v[0],
          "CW1: R1's folder and version: none when rekordbox is gone");
    f.n_entries = had;
    snprintf(q, sizeof q, "%s.gone", v19);
    rename(v19, q);   /* the current version folder removed (an update, an uninstall): no fallback */
    compat_memo_clear();
    CHECK(!native_r1_folder(&os, root, r1f, sizeof r1f) && !r1f[0], "CW1: R1's folder: none when the current version folder is gone");
    rename(q, v19);
    compat_memo_clear();
    f.signed_paths[f.n_signed] = NULL;
    compat_memo_clear();

    /* review RC4 (C8's no fallback): a partial Uninstall enumeration (a subkey that couldn't be
     * read) may have missed the newest version's entry: a newer version folder holding a
     * rekordbox.exe that no entry names means no answer, never the older folder */
    {
        char v22[1024], exe22[1200], v10[1024], exe10[1200];
        snprintf(v22, sizeof v22, "%s/rekordbox 7.2.22", root);
        snprintf(exe22, sizeof exe22, "%s/rekordbox.exe", v22);
        snprintf(v10, sizeof v10, "%s/rekordbox 7.2.10", root);
        snprintf(exe10, sizeof exe10, "%s/rekordbox.exe", v10);
        f.incomplete = 1;
        n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
        CHECK(n == 3 && cur == 1 && st == COMPAT_RB_OK, "RC4: partial, no newer folder on disk: 7.2.19 stands (cur %d, state %d)", cur, st);
        mkdir(v22, 0755);
        spit(exe22, exe, exe_n);   /* 7.2.22 installed, its Uninstall key the one that couldn't be read */
        n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
        CHECK(n == 3 && cur == -1 && st == COMPAT_RB_NO_ANSWER, "RC4: partial, a newer unlisted folder with rekordbox.exe: no answer, 7.2.19 not current (cur %d, state %d)",
              cur, st);
        {
            int cand = 0;
            for (int i = 0; i < n; i++) cand += rb[i].candidate;
            CHECK(cand == 0, "RE3: no answer from an unlisted newer folder: the older one isn't the candidate (%d)", cand);
        }
        unlink(exe22);
        rmdir(v22);
        mkdir(v22, 0755);
        unlisted_v22 = v22;
        compat_test_stat_fails = re1_stat_fails;
        n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
        CHECK(cur == -1 && st == COMPAT_RB_NO_ANSWER, "RE1: partial, an unlisted newer folder that can't be stat'ed: no answer (cur %d, state %d)", cur, st);
        f.incomplete = 0;
        n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
        CHECK(cur == 1 && st == COMPAT_RB_OK, "RE1: whole, the same folder unlisted: not rekordbox's (cur %d, state %d)", cur, st);
        {
            static char v19s[1200];
            snprintf(v19s, sizeof v19s, "%s", rb[1].folder);
            unlisted_v22 = v19s;
        }
        n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
        CHECK(cur == -1 && st == COMPAT_RB_NO_ANSWER && rb[1].candidate && rb[1].unknown,
              "RE1: whole, the listed 7.2.19 that can't be stat'ed: no answer, never 7.2.18 (cur %d, state %d)", cur, st);
        compat_test_stat_fails = NULL;
        unlisted_v22 = NULL;
        f.incomplete = 1;
        spit(exe22, exe, exe_n);
        f.incomplete = 0;
        n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
        CHECK(cur == 1 && st == COMPAT_RB_OK, "RC4: a whole enumeration: a folder no entry names isn't rekordbox's (cur %d)", cur);
        f.incomplete = 1;
        unlink(exe22);
        n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
        CHECK(cur == 1 && st == COMPAT_RB_OK, "RC4: partial, the newer folder without rekordbox.exe: passed over, as the walk does (cur %d)", cur);
        mkdir(v10, 0755);
        spit(exe10, exe, exe_n);   /* an older unlisted one: whatever its key said, 7.2.19 is newer */
        n = t_inventory_current_equivalence(&os, root, rb, 8, &cur, &st);
        CHECK(cur == 1 && st == COMPAT_RB_OK, "RC4: partial, only an older unlisted folder: 7.2.19 stands (cur %d)", cur);
        unlink(exe10);
        rmdir(v10);

        /* the cache never keeps an answer from a partial enumeration */
        static compat_rb_cache pc = COMPAT_RB_CACHE_INIT;
        compat_rekordbox rp[8];
        int pcur, pst;
        f.gen = 77;
        f.entry_calls = 0;
        compat_find_rekordbox_cached(&pc, &os, f.gen, root, rp, 8, &pcur, &pst);
        compat_find_rekordbox_cached(&pc, &os, f.gen, root, rp, 8, &pcur, &pst);
        CHECK(f.entry_calls == 2 && pcur == 1 && pst == COMPAT_RB_OK, "RC4: a partial answer isn't cached: walked each time (%d)", f.entry_calls);
        f.incomplete = 0;
        compat_find_rekordbox_cached(&pc, &os, f.gen, root, rp, 8, &pcur, &pst);
        compat_find_rekordbox_cached(&pc, &os, f.gen, root, rp, 8, &pcur, &pst);
        CHECK(f.entry_calls == 3 && pcur == 1, "RC4: whole again: kept, the next call from the cache (%d)", f.entry_calls);
        rmdir(v22);
        f.gen = 0;
        compat_memo_clear();
    }

    /* review HF6: the discovery cached while the Uninstall views and every listed exe hold */
    {
        static compat_rb_cache cache = COMPAT_RB_CACHE_INIT;
        compat_rekordbox rc[8];
        int cc, cs;
        f.gen = 0;
        f.entry_calls = 0;
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        CHECK(f.entry_calls == 2 && cc == 1, "HF6: no notice (generation 0): walked each time (%d)", f.entry_calls);
        f.gen = 5;
        f.entry_calls = 0;
        n = compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        int calls0 = f.pin_calls;
        int n2 = compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        CHECK(f.entry_calls == 1 && n2 == n && n == 3 && cc == 1 && cs == COMPAT_RB_OK && !strcmp(rc[1].folder_version, "7.2.19") && f.pin_calls == calls0,
              "HF6: the same generation: the second answer from the cache, the same (%d walks)", f.entry_calls);
        CHECK(compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 2, &cc, &cs) == 2 && f.entry_calls == 2, "HF6: another max: walked again");
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        f.gen = 6;
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        CHECK(f.entry_calls == 4, "HF6: the Uninstall views changed: walked again (%d)", f.entry_calls);
        spit(exe19, exe, exe_n - 1);   /* the current rekordbox.exe changed */
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        CHECK(f.entry_calls == 5, "HF6: the current exe's stat changed: walked again (%d)", f.entry_calls);
        spit(exe19, exe, exe_n);
        compat_memo_clear();
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        unlink(exe18);   /* another listed folder's exe went */
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        CHECK(f.entry_calls == 7 && cc == 1, "HF6: another listed exe went: walked again (%d)", f.entry_calls);
        spit(exe18, exe, exe_n);
        /* a pin ERROR is never kept: asked again next time (C7) */
        compat_memo_clear();
        f.pin_error = 1;
        f.entry_calls = 0;
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        f.pin_error = 0;
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        CHECK(f.entry_calls == 2 && cc == 1, "HF6: an answer with a pin ERROR isn't kept: walked again, then answered (%d)", f.entry_calls);
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        CHECK(f.entry_calls == 2 && cc == 1, "HF6: then from the cache");
        compat_rb_cache_clear(&cache);
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        CHECK(f.entry_calls == 3 && cache.walks == 1, "HF6: cleared: walked again");
        /* review NW3: the entries' list can't be allocated: no answer, never "not installed", and not kept */
        compat_rb_cache_clear(&cache);
        compat_test_entries_alloc_fails = 1;
        f.entry_calls = 0;
        n = compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        compat_test_entries_alloc_fails = 0;
        CHECK(n == 0 && cc == -1 && cs == COMPAT_RB_NO_ANSWER && f.entry_calls == 0, "NW3: out of memory: no answer (%d %d %d)", n, cc, cs);
        n = compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        CHECK(n == 3 && cc == 1 && cs == COMPAT_RB_OK && f.entry_calls == 1 && cache.walks == 2, "NW3: not kept: the next call walks again (%d walks)",
              cache.walks);
        compat_find_rekordbox_cached(&cache, &os, f.gen, root, rc, 8, &cc, &cs);
        CHECK(f.entry_calls == 1 && cc == 1, "NW3: then from the cache");
        compat_memo_clear();
    }

    /* review RC1, RC2, RC3, RC5: the cache's stamp, against a fresh walk */
    {
        static compat_rb_cache cache = COMPAT_RB_CACHE_INIT;
        compat_rekordbox warm[8], fresh[8];
        int wc, ws, fc, fs;
        plat_stat before, after;
        struct timeval tv[2] = {{1700000000, 0}, {1700000000, 0}};
        char cp[1200];
        snprintf(cp, sizeof cp, "%s/copy19", root);

        /* RC1: 7.2.19's exe removed while 7.2.18 is pinned: the walk's answer isn't kept */
        compat_memo_clear();
        f.remove_trigger = exe18;
        f.remove_target = exe19;
        f.entry_calls = 0;
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        int walks = f.entry_calls;
        compat_memo_clear();
        compat_find_rekordbox(&os, root, fresh, 8, &fc, &fs);
        CHECK(walks == 2 && wc == fc && ws == fs, "RC1: an exe removed during the walk isn't kept (walks %d, cached %d/%d, fresh %d/%d)", walks, wc, ws,
              fc, fs);
        spit(exe19, exe, exe_n);

        /* RC1: the same size and mtime, a new file id, renamed in while 7.2.18 is pinned */
        compat_rb_cache_clear(&cache);
        compat_memo_clear();
        spit(cp, exe, exe_n);
        utimes(exe19, tv);
        utimes(cp, tv);
        CHECK(plat_stat_path(exe19, 0, &before) == 0 && plat_stat_path(cp, 0, &after) == 0 && before.size == after.size
                  && before.mtime == after.mtime && !plat_id_same(before.id, after.id),
              "RC1: the copy differs only in its file id");
        f.rn_trigger = exe18;
        f.rn_src = cp;
        f.rn_dst = exe19;
        f.entry_calls = 0;
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 2, "RC1: a same-size, same-mtime replacement during the walk isn't kept (%d walks)", f.entry_calls);
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 2 && wc == 1 && ws == COMPAT_RB_OK, "RC1: then the next walk's answer is kept");

        /* RC5: an id-only replacement after the walk */
        spit(cp, exe, exe_n);
        utimes(cp, tv);
        CHECK(plat_stat_path(exe19, 0, &before) == 0 && plat_stat_path(cp, 0, &after) == 0 && before.size == after.size
                  && before.mtime == after.mtime && !plat_id_same(before.id, after.id) && rename(cp, exe19) == 0,
              "RC5: replaced by a file that differs only in its id");
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 3 && wc == 1, "RC5: an id-only replacement: walked again (%d)", f.entry_calls);

        /* RC5: an mtime-only change */
        tv[0].tv_sec = tv[1].tv_sec = 1700000100;
        utimes(exe19, tv);
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 4 && wc == 1, "RC5: an mtime-only change: walked again (%d)", f.entry_calls);

        /* RC5: another root walks, and the root after it too */
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 4, "RC5: then from the cache");
        compat_find_rekordbox_cached(&cache, &os, 42, ortdir, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 5 && wc == -1 && ws == COMPAT_RB_NOT_INSTALLED, "RC5: another root: walked, its own answer (%d)", f.entry_calls);
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 6 && wc == 1, "RC5: back to the first root: walked again (%d)", f.entry_calls);

        /* RC5: the NO_ANSWER guard alone: the current exe opens but its read fails, its pin a definite NO */
        compat_rb_cache_clear(&cache);
        compat_memo_clear();
        f.signed_paths[0] = "nobody's";
        read_fail_path = exe19;
        compat_test_read_fails = read_fails;
        f.entry_calls = 0;
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(ws == COMPAT_RB_NO_ANSWER && !warm[1].unknown && warm[1].exe.exists && !warm[1].exe.readable && warm[1].exe.pin == COMPAT_PIN_NO,
              "RC5: a read failure with a pin NO: no answer, nothing unknown");
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 2, "RC5: no answer isn't kept: walked again (%d)", f.entry_calls);
        compat_test_read_fails = NULL;
        read_fail_path = NULL;
        f.signed_paths[0] = exe19;

        /* RC5: an older entry that couldn't be looked at: its folder not searchable, so its exe
         * doesn't stat either and the stamp agrees; only the unknown guard refuses it */
        compat_rb_cache_clear(&cache);
        compat_memo_clear();
        chmod(v18, 0);
        f.entry_calls = 0;
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(wc == 1 && ws == COMPAT_RB_OK && warm[2].unknown, "RC5: 7.2.18 unknown, 7.2.19 current");
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 2, "RC5: an older unknown entry isn't kept: walked again (%d)", f.entry_calls);
        chmod(v18, 0755);

        /* RC5: a pin ERROR on an older entry only */
        compat_rb_cache_clear(&cache);
        compat_memo_clear();
        f.pin_error_path = exe18;
        f.entry_calls = 0;
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(wc == 1 && ws == COMPAT_RB_OK && warm[2].exe.pin == COMPAT_PIN_ERROR, "RC5: 7.2.18's pin ERROR, 7.2.19 current");
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 2, "RC5: an older entry's pin ERROR isn't kept: walked again (%d)", f.entry_calls);
        f.pin_error_path = NULL;

        /* RC2: the version folder replaced by a link to it: every exe stat field stays the same */
        compat_rb_cache_clear(&cache);
        compat_memo_clear();
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(plat_stat_path(exe19, 0, &before) == 0, "RC2: the exe before");
        snprintf(q, sizeof q, "%s/held-version", root);
        CHECK(rename(v19, q) == 0 && symlink(q, v19) == 0, "RC2: the version folder replaced by a link to it");
        CHECK(plat_stat_path(exe19, 0, &after) == 0 && plat_id_same(before.id, after.id) && before.size == after.size
                  && before.mtime == after.mtime && before.kind == after.kind,
              "RC2: the exe stats the same through the link");
        f.entry_calls = 0;
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        walks = f.entry_calls;
        compat_memo_clear();
        compat_find_rekordbox(&os, root, fresh, 8, &fc, &fs);
        CHECK(walks == 1 && wc == fc && ws == fs && ws != COMPAT_RB_OK, "RC2: a linked folder: walked again, its fresh answer (cached %d/%d, fresh %d/%d)",
              wc, ws, fc, fs);
        unlink(v19);
        CHECK(rename(q, v19) == 0, "RC2: the folder back");

        /* RC3, by design: read access lost after the walk isn't checked on a hit (acting code reads FRESH) */
        compat_rb_cache_clear(&cache);
        compat_memo_clear();
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(plat_stat_path(exe19, 0, &before) == 0 && chmod(exe19, 0) == 0 && plat_stat_path(exe19, 0, &after) == 0
                  && plat_id_same(before.id, after.id) && before.size == after.size && before.mtime == after.mtime,
              "RC3: read access gone, the stat the same");
        f.entry_calls = 0;
        compat_find_rekordbox_cached(&cache, &os, 42, root, warm, 8, &wc, &ws);
        CHECK(f.entry_calls == 0 && wc == 1 && ws == COMPAT_RB_OK, "RC3: a hit stays a hit (%d walks)", f.entry_calls);
        compat_memo_clear();
        compat_find_rekordbox(&os, root, fresh, 8, &fc, &fs);
        CHECK(fs == COMPAT_RB_NO_ANSWER, "RC3: a fresh walk sees it");
        chmod(exe19, 0644);
        compat_rb_cache_clear(&cache);
        compat_memo_clear();
    }

    /* rr1 probes: rb_stamp's other walk-time branches */
    {
        static compat_rb_cache cache2 = COMPAT_RB_CACHE_INIT;
        compat_rekordbox w[8], fr[8];
        int a, b, c2, d2;
        char cp2[1200], tgt[1200], held[1200];
        snprintf(cp2, sizeof cp2, "%s/copy19b", root);
        snprintf(tgt, sizeof tgt, "%s/elsewhere.exe", root);
        snprintf(held, sizeof held, "%s/held19", root);
#define RR1(label) do { \
            compat_find_rekordbox_cached(&cache2, &os, 42, root, w, 8, &a, &b); \
            compat_find_rekordbox_cached(&cache2, &os, 42, root, w, 8, &a, &b); \
            int wk = f.entry_calls; \
            compat_memo_clear(); \
            compat_find_rekordbox(&os, root, fr, 8, &c2, &d2); \
            CHECK(wk == 2 && a == c2 && b == d2, "RR1 %s (%d walks, cached %d/%d, fresh %d/%d)", label, wk, a, b, c2, d2); \
            compat_rb_cache_clear(&cache2); compat_memo_clear(); f.entry_calls = 0; } while (0)
        compat_rb_cache_clear(&cache2); compat_memo_clear(); f.entry_calls = 0;
        /* present && !exists: exe19 a link at the walk, a regular file renamed over it during 7.2.18's pin */
        unlink(exe19); spit(tgt, exe, exe_n); CHECK(symlink(tgt, exe19) == 0, "rr1 link");
        spit(cp2, exe, exe_n);
        f.rn_trigger = exe18; f.rn_src = cp2; f.rn_dst = exe19;
        RR1("link replaced by a file during the walk");
        /* !present: exe19 missing at the walk, created during 7.2.18's pin */
        unlink(exe19); spit(cp2, exe, exe_n);
        f.rn_trigger = exe18; f.rn_src = cp2; f.rn_dst = exe19;
        RR1("exe appeared during the walk");
        /* exists: exe19 grown in place (same id) during 7.2.18's pin */
        f.grow_trigger = exe18; f.grow_path = exe19;
        RR1("exe grown in place during the walk");
        spit(exe19, exe, exe_n);
        /* not a dir at the stamp: 7.2.19's folder moved away during 7.2.18's pin */
        f.rn_trigger = exe18; f.rn_src = v19; f.rn_dst = held;
        RR1("folder moved away during the walk");
        CHECK(rename(held, v19) == 0, "rr1 folder back");
        unlink(tgt);
#undef RR1
    }

    /* R1's facts */
    static payload_manifest m;
    memset(&m, 0, sizeof m);
    m.has_windows = 1;
    char ortsha[65], bridgesha[65];
    sha_hex(ort, ort_n, ortsha);
    sha_hex(bridge, strlen(bridge), bridgesha);
    payload_sha known[1];
    memcpy(known[0], bridgesha, 65);
    compat_r1 r, cr;
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_MISSING && r.offer_restore && r.ort == COMPAT_ORT_NONE, "the live DLL missing (a quarantine): offered");
    spit(live, ort, ort_n);
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_FOREIGN && r.offer_restore && r.ort == COMPAT_ORT_NONE, "neither ours nor AlphaTheta's: offered");
    f.signed_paths[f.n_signed++] = live;
    compat_memo_clear();   /* the pin is memoised with the file: the same file is signed now (a test's change) */
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_PIONEER && !r.offer_restore && r.ort == COMPAT_ORT_UNLISTED && !strcmp(r.ort_sha256, ortsha),
          "Pioneer's, its sha unlisted");
    memcpy(m.win_ort_sha256[0], ortsha, 65);
    m.n_win_ort_sha256 = 1;
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_PIONEER && r.ort == COMPAT_ORT_OK, "Pioneer's, listed");
    compat_memo_clear();
    f.pin_errors_once = 1;
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_UNREADABLE && !r.offer_restore && r.ort == COMPAT_ORT_NONE, "the pin can't answer: no verdict, no offer (C7)");
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_PIONEER && r.ort == COMPAT_ORT_OK, "then it answers: asked again (C7)");
    spit(live, bridge, strlen(bridge));
    compat_r1_facts(&os, v19, rbsp, NULL, 0, &m, &r);
    CHECK(r.live == COMPAT_LIVE_OURS_UNLISTED && r.offer_restore, "the bridge's text, its sha unlisted: ours unlisted, offered (C10)");
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_OURS && !r.ort_copy && r.offer_restore && r.ort == COMPAT_ORT_NONE, "ours (listed), ort\\ missing: offered");
    mkdir(ortdir, 0755);
    spit(ortf, ort, ort_n);
    compat_r1_facts(&os, v19, rbsp, NULL, 0, &m, &r);
    CHECK(r.live == COMPAT_LIVE_OURS_UNLISTED && r.ort_copy && r.offer_restore, "ours unlisted, ort\\ there: still offered (C10)");
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_OURS && r.ort_copy && !r.offer_restore && r.ort == COMPAT_ORT_NOT_ORIGINAL, "ort\\'s copy there but not AlphaTheta's");
    f.signed_paths[f.n_signed++] = ortf;
    compat_memo_clear();
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_OURS && r.ort_copy && !r.offer_restore && r.ort == COMPAT_ORT_OK, "ours, with Pioneer's ORT in ort\\");
    compat_memo_clear();
    f.pin_error = 1;
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_OURS && r.ort_copy && !r.offer_restore && r.ort == COMPAT_ORT_NONE, "ort\\'s pin can't answer: no ORT verdict (C7)");
    f.pin_error = 0;
    compat_memo_clear();
    chmod(ortdir, 0);
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    compat_r1_facts(&os, v19, rbsp, NULL, 0, &m, &cr);
    chmod(ortdir, 0755);
    CHECK(r.live == COMPAT_LIVE_OURS && !r.offer_restore && r.ort == COMPAT_ORT_NONE, "ort\\ refused: no answer, no offer (C15)");
    CHECK(cr.live == COMPAT_LIVE_OURS_UNLISTED && cr.offer_restore, "ort\\ refused, ours unlisted: still offered for the unlisted bridge");
    const char *quiet = "a bridge build without the text";
    spit(live, quiet, strlen(quiet));
    sha_hex(quiet, strlen(quiet), known[0]);
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_OURS, "ours by its sha256");
    unlink(live);
    symlink(ortf, live);
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_FOREIGN && r.offer_restore, "a link in its place: not Pioneer's");
    unlink(live);
    mkdir(live, 0755);
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_FOREIGN && r.offer_restore, "a folder in its place");
    rmdir(live);
    /* C1: a bridge renamed over the live DLL while the old one is pinned: no verdict, no offer */
    memcpy(known[0], bridgesha, 65);
    compat_memo_clear();
    const char *plain = "an unsigned DLL, not ours";
    spit(live, plain, strlen(plain));
    snprintf(q, sizeof q, "%s/bridge.new", scratch);
    spit(q, bridge, strlen(bridge));
    f.swap_from = q;
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(!f.swap_from && r.live != COMPAT_LIVE_FOREIGN && !r.offer_restore, "renamed over while pinned: not foreign, no offer (live %d)", r.live);
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_OURS && !r.offer_restore, "and next time it reads as the bridge it now is");
    /* C1: our bridge renamed over between R1's unpinned and pinned reads: classified from the
     * pinned read, as ours (not as Pioneer's by the pin on the path) */
    compat_memo_clear();
    spit(live, plain, strlen(plain));
    snprintf(q, sizeof q, "%s/bridge.new", scratch);
    spit(q, bridge, strlen(bridge));
    swap_src = q;
    compat_test_between_reads = swap_between;
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    compat_test_between_reads = NULL;
    CHECK(!swap_src && r.live == COMPAT_LIVE_OURS && !r.offer_restore, "swapped between the reads: ours, from the pinned read (live %d)", r.live);
    /* C17: a live DLL that can't be read at all: no answer, no offer */
    compat_memo_clear();
    chmod(live, 0);
    compat_r1_facts(&os, v19, rbsp, known, 1, &m, &r);
    chmod(live, 0644);
    CHECK(r.live == COMPAT_LIVE_UNREADABLE && !r.offer_restore, "the live DLL unreadable: no answer, no offer (%d)", r.live);
    unlink(live);
    snprintf(q, sizeof q, "%s/no such folder", root);
    compat_r1_facts(&os, q, rbsp, known, 1, &m, &r);
    CHECK(r.live == COMPAT_LIVE_MISSING, "the version folder gone");

    /* C2: a same-size rewrite with its time put back; the memo, and COMPAT_FRESH */
    compat_file cf;
    {
        compat_memo_clear();
        const char *one = "contents number one", *two = "contents number two";   /* the same size */
        spit(live, one, strlen(one));
        struct stat before;
        stat(live, &before);
        CHECK(compat_file_facts(&os, live, 0, &cf) == 0 && cf.readable, "read once");
        char sha1[65], sha2[65];
        sha_hex(one, strlen(one), sha1);
        sha_hex(two, strlen(two), sha2);
        struct timeval tv[2] = {{before.st_atime, 0}, {before.st_mtime, 0}};
        snprintf(q, sizeof q, "%s/rewrite.new", scratch);   /* replaced by a rename: another file */
        spit(q, two, strlen(two));
        utimes(q, tv);
        rename(q, live);
        CHECK(compat_file_facts(&os, live, 0, &cf) == 0 && !strcmp(cf.sha256, sha2), "replaced by a rename, its time put back: read again");
        spit(live, one, strlen(one));                       /* rewritten in place: the same file */
        utimes(live, tv);
        compat_file_facts(&os, live, 0, &cf);
        int stale = !strcmp(cf.sha256, sha2);
        CHECK(compat_file_facts(&os, live, COMPAT_FRESH, &cf) == 0 && !strcmp(cf.sha256, sha1),
              "rewritten in place, its time put back: FRESH reads it again (the memo %s)", stale ? "was stale, as documented" : "saw it");
        CHECK(compat_file_facts(&os, live, 0, &cf) == 0 && !strcmp(cf.sha256, sha1), "and FRESH's result is stored");
        unlink(live);
    }

    /* the file facts themselves */
    spit(live, bridge, strlen(bridge));
    CHECK(compat_file_facts(&os, live, 0, &cf) == 0 && cf.exists && cf.readable && cf.marker && !strcmp(cf.sha256, bridgesha)
              && cf.pin == COMPAT_PIN_ERROR,
          "a bridge: its text and sha256, no pin asked");
    /* the marker across a 1 MB read boundary */
    size_t big = (1u << 20) + 64;
    unsigned char *bb = calloc(1, big);
    if (!bb) abort();
    memcpy(bb + (1u << 20) - 7, "rbstems bridge: ", 16);
    spit(live, bb, big);
    CHECK(compat_file_facts(&os, live, 0, &cf) == 0 && cf.marker && cf.size == (int64_t)big, "the text across a read boundary");
    memset(bb, 'x', big);
    spit(live, bb, big);
    compat_memo_clear();   /* rewritten in place, same size, maybe the same second: plat_stat's mtime is whole seconds */
    CHECK(compat_file_facts(&os, live, 0, &cf) == 0 && !cf.marker, "no text: none");
    free(bb);
    unlink(live);
    CHECK(compat_file_facts(&os, live, 0, &cf) == 0 && !cf.present, "gone");

    /* processes through the OS */
    f.n_procs = 1;
    snprintf(f.procs[0].name, sizeof f.procs[0].name, "rekordbox.exe");
    snprintf(f.procs[0].path, sizeof f.procs[0].path, "%s", exe19);
    f.procs[0].machine = COMPAT_MACHINE_AMD64;
    compat_procs pr;
    CHECK(compat_processes(&os, v19, &pr) == 0 && pr.running && pr.running_here
              && !strcmp(compat_machine_words(pr.machine, os.native_machine(os.ctx)), "x64 under emulation on ARM64"),
          "rekordbox from this folder, x64 under emulation");
    free(exe);
    free(ort);
}

/* review HR5: single-flight in compat_file_facts. The first read of sf_path holds until another
 * caller is waiting for it (compat_test_flight_waits moved), so the two overlap for sure; with
 * sf_swap_src set it first renames that file over sf_path (its own handle stays the old file). */
static const char *sf_path, *sf_swap_src;
static int sf_reads, sf_append;   /* sf_append: the file grows in place while the other caller waits */
static void sf_before(const char *path) {
    if (!sf_path || strcmp(path, sf_path)) return;
    if (__atomic_load_n(&sf_reads, __ATOMIC_SEQ_CST) == 0 && sf_swap_src && rename(sf_swap_src, sf_path)) abort();
    if (__atomic_add_fetch(&sf_reads, 1, __ATOMIC_SEQ_CST) != 1) return;
    for (int i = 0; i < 5000 && __atomic_load_n(&compat_test_flight_waits, __ATOMIC_SEQ_CST) == 0; i++) usleep(1000);
    if (sf_append) {
        FILE *x = fopen(path, "ab");
        if (!x || fwrite("more", 1, 4, x) != 4) abort();
        fclose(x);
    }
}
typedef struct {
    const compat_os *os;
    compat_file f;
    int r;
} sf_call;
static void *sf_thread(void *v) {
    sf_call *c = v;
    c->r = compat_file_facts(c->os, sf_path, 0, &c->f);
    return NULL;
}
/* main's own call, once the thread's read has started */
static void sf_second(sf_call *mine) {
    for (int i = 0; i < 5000 && __atomic_load_n(&sf_reads, __ATOMIC_SEQ_CST) == 0; i++) usleep(1000);
    mine->r = compat_file_facts(mine->os, sf_path, 0, &mine->f);
}
static int reenter_pin(void *ctx, const char *path, plat_file f) {
    (void)f;
    compat_file inner;
    *(int *)ctx = compat_file_facts(NULL, path, 0, &inner) == 0 && inner.readable;
    return COMPAT_PIN_OK;
}

/* review SF1: HR3's re-entering pin. The outer pin asks the same file again, pinned, on its own
 * thread (that inner read pins OK), then answers ERROR: the outer read keeps the inner OK. */
static int hr3_outer = 1;
static int hr3_pin(void *ctx, const char *path, plat_file f) {
    (void)f;
    if (!hr3_outer) return COMPAT_PIN_OK;
    hr3_outer = 0;
    compat_file in;
    *(int *)ctx = compat_file_facts(&(compat_os){NULL, hr3_pin, NULL, NULL, ctx}, path, COMPAT_PIN, &in) ? -2 : in.pin;
    return COMPAT_PIN_ERROR;
}
typedef struct {
    const char *path;
    compat_file f;
    int r;
} sf_path_call;
static void *sf_path_thread(void *v) {
    sf_path_call *c = v;
    c->r = compat_file_facts(NULL, c->path, 0, &c->f);
    return NULL;
}

/* review SF5: a pin held until another caller waits on its flight */
static int gate_pins;
static int gate_pin(void *ctx, const char *path, plat_file f) {
    (void)ctx;
    (void)path;
    (void)f;
    __atomic_add_fetch(&gate_pins, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 5000 && __atomic_load_n(&compat_test_flight_waits, __ATOMIC_SEQ_CST) == 0; i++) usleep(1000);
    return COMPAT_PIN_OK;
}
static const compat_os gate_os = {NULL, gate_pin, NULL, NULL, NULL};
static void *gate_thread(void *v) {
    sf_path_call *c = v;
    c->r = compat_file_facts(&gate_os, c->path, COMPAT_PIN, &c->f);
    return NULL;
}

/* review SF6: a flight wait that never ends fails the suite instead of hanging it */
static void flight_hang(int sig) {
    (void)sig;
    static const char m[] = "FAIL compat flight tests: a flight wait never ended\n";
    if (write(2, m, sizeof m - 1) < 0) {}
    _exit(1);
}

static void single_flight(const char *scratch) {
    char p[1100], q[1100];
    static unsigned char a[300000], b[200000];
    for (size_t i = 0; i < sizeof a; i++) a[i] = (unsigned char)(i * 7);
    for (size_t i = 0; i < sizeof b; i++) b[i] = (unsigned char)(i * 13 + 1);
    char sha_a[65], sha_b[65];
    sha_hex(a, sizeof a, sha_a);
    sha_hex(b, sizeof b, sha_b);
    snprintf(p, sizeof p, "%s/flight.bin", scratch);
    snprintf(q, sizeof q, "%s/flight-new.bin", scratch);
    spit(p, a, sizeof a);
    compat_test_before_read = sf_before;
    sf_path = p;

    /* two callers of one unchanged file: one read, the same answer */
    compat_memo_clear();
    sf_reads = 0;
    __atomic_store_n(&compat_test_flight_waits, 0, __ATOMIC_SEQ_CST);
    sf_call t = {NULL, {0}, -1}, m = {NULL, {0}, -1};
    pthread_t th;
    CHECK(!pthread_create(&th, NULL, sf_thread, &t), "HR5: a thread");
    sf_second(&m);
    pthread_join(th, NULL);
    CHECK(t.r == 0 && m.r == 0 && t.f.readable && m.f.readable && !strcmp(t.f.sha256, sha_a) && !strcmp(m.f.sha256, sha_a),
          "HR5: both answered, the same file's sha256");
    CHECK(sf_reads == 1 && compat_test_flight_waits >= 1, "HR5: one read, the other waited for it (%d reads, %d waits)", sf_reads,
          compat_test_flight_waits);

    /* the file replaced while the second caller waits: its own handle's stat doesn't match the
     * memo the first read left, so it reads the new file itself */
    compat_memo_clear();
    spit(q, b, sizeof b);
    sf_swap_src = q;
    sf_reads = 0;
    __atomic_store_n(&compat_test_flight_waits, 0, __ATOMIC_SEQ_CST);
    sf_call t2 = {NULL, {0}, -1}, m2 = {NULL, {0}, -1};
    CHECK(!pthread_create(&th, NULL, sf_thread, &t2), "HR5: a thread");
    sf_second(&m2);
    pthread_join(th, NULL);
    sf_swap_src = NULL;
    CHECK(t2.r == 0 && !strcmp(t2.f.sha256, sha_a) && t2.f.size == (int64_t)sizeof a, "HR5: the first caller: the file it opened");
    CHECK(m2.r == 0 && m2.f.readable && !strcmp(m2.f.sha256, sha_b) && m2.f.size == (int64_t)sizeof b,
          "HR5: the waiter: the new file's facts, not the memo's");
    CHECK(sf_reads == 2 && compat_test_flight_waits >= 1, "HR5: a change during the wait: read again (%d reads, %d waits)", sf_reads,
          compat_test_flight_waits);

    /* the same file grows in place while the second caller waits (one file, one id): the waiter's
     * stat taken again after the wait doesn't match the memo's, so it reads it itself */
    compat_memo_clear();
    spit(p, a, sizeof a);
    sf_append = 1;
    sf_reads = 0;
    __atomic_store_n(&compat_test_flight_waits, 0, __ATOMIC_SEQ_CST);
    sf_call t3 = {NULL, {0}, -1}, m3 = {NULL, {0}, -1};
    CHECK(!pthread_create(&th, NULL, sf_thread, &t3), "HR5: a thread");
    sf_second(&m3);
    pthread_join(th, NULL);
    sf_append = 0;
    CHECK(m3.r == 0 && m3.f.readable && m3.f.size == (int64_t)sizeof a + 4 && sf_reads == 2 && compat_test_flight_waits >= 1,
          "HR5: changed in place during the wait: stat again, read again (size %lld, %d reads)", (long long)m3.f.size, sf_reads);
    spit(p, a, sizeof a);

    /* a pin that asks the same file again on the reading thread reads it, never waits on itself */
    compat_memo_clear();
    sf_path = NULL;
    int inner_ok = 0;
    compat_os os = {NULL, reenter_pin, NULL, NULL, &inner_ok};
    compat_file f;
    CHECK(compat_file_facts(&os, p, COMPAT_PIN, &f) == 0 && f.pin == COMPAT_PIN_OK && inner_ok, "HR5: re-entered from its own pin: answered");

    /* review SF1: HR3's keep (the outer pin's ERROR keeps the inner OK) ends the outer read's
     * flight too: a new version renamed over the file, read from another thread, doesn't wait */
    compat_memo_clear();
    int inner_pin = -1;
    compat_os hr3 = {NULL, hr3_pin, NULL, NULL, &inner_pin};
    hr3_outer = 1;
    CHECK(compat_file_facts(&hr3, p, COMPAT_PIN, &f) == 0 && f.pin == COMPAT_PIN_OK && inner_pin == COMPAT_PIN_OK,
          "SF1: the outer pin's ERROR keeps the inner read's OK (outer %d, inner %d)", f.pin, inner_pin);
    spit(q, b, sizeof b);
    CHECK(!rename(q, p), "SF1: a new version renamed over the file");
    int w0 = __atomic_load_n(&compat_test_flight_waits, __ATOMIC_SEQ_CST);
    sf_path_call o = {p, {0}, -1};
    CHECK(!pthread_create(&th, NULL, sf_path_thread, &o), "SF1: a thread");
    pthread_join(th, NULL);
    CHECK(o.r == 0 && o.f.readable && !strcmp(o.f.sha256, sha_b), "SF1: another thread reads the new version");
    CHECK(__atomic_load_n(&compat_test_flight_waits, __ATOMIC_SEQ_CST) == w0, "SF1: and waits on no flight (%d waits)",
          __atomic_load_n(&compat_test_flight_waits, __ATOMIC_SEQ_CST) - w0);
    spit(p, a, sizeof a);

    /* review SF4: an unreadable file on the UI thread is remembered for the UI thread only: a
     * worker's read of the same file and stat reads it again */
    compat_memo_clear();
    read_fail_path = p;
    compat_test_read_fails = read_fails;
    compat_main_reads(1, NULL, NULL);
    CHECK(compat_file_facts(NULL, p, 0, &f) == 0 && f.exists && !f.readable, "SF4: the UI thread: unreadable");
    compat_test_read_fails = NULL;
    read_fail_path = NULL;
    compat_main_reads(0, NULL, NULL);
    int64_t s0 = compat_test_scanned;
    CHECK(compat_file_facts(NULL, p, 0, &f) == 0 && f.readable && !strcmp(f.sha256, sha_a) && compat_test_scanned > s0,
          "SF4: a worker reads it again (readable %d, %lld bytes)", f.readable, (long long)(compat_test_scanned - s0));

    /* review SF5: a remembered unpinned file, then two pinned reads of it at once: one pins, the
     * other waits on its flight and takes the pin remembered */
    compat_memo_clear();
    CHECK(compat_file_facts(NULL, p, 0, &f) == 0 && f.readable, "SF5: an unpinned read first");
    __atomic_store_n(&gate_pins, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&compat_test_flight_waits, 0, __ATOMIC_SEQ_CST);
    sf_path_call g1 = {p, {0}, -1}, g2 = {p, {0}, -1};
    CHECK(!pthread_create(&th, NULL, gate_thread, &g1), "SF5: a thread");
    for (int i = 0; i < 5000 && __atomic_load_n(&gate_pins, __ATOMIC_SEQ_CST) == 0; i++) usleep(1000);
    g2.r = compat_file_facts(&gate_os, p, COMPAT_PIN, &g2.f);
    pthread_join(th, NULL);
    CHECK(g1.r == 0 && g2.r == 0 && g1.f.pin == COMPAT_PIN_OK && g2.f.pin == COMPAT_PIN_OK && !strcmp(g2.f.sha256, sha_a),
          "SF5: both pinned OK (%d, %d)", g1.f.pin, g2.f.pin);
    CHECK(gate_pins == 1 && compat_test_flight_waits >= 1, "SF5: one pin, the other waited for it (%d pins, %d waits)", gate_pins,
          compat_test_flight_waits);

    compat_test_before_read = NULL;
    compat_memo_clear();
}

void test_compat(const char *scratch, const char *pedir) {
    pure();
    pe_files(pedir);
    facts(scratch, pedir);
    t_inventory(scratch, pedir);
    t_inventory_rules();
    t_ort_file_facts(scratch);
    signal(SIGALRM, flight_hang);
    alarm(30);
    single_flight(scratch);
    alarm(0);
    signal(SIGALRM, SIG_DFL);
}
