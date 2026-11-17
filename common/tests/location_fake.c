/* location.c's checks on a fake platform layer (review BW3): the states a real volume can't be put
 * in on demand, each in both path forms (the Mac's and Windows'):
 *  - the folder opened on another volume than the one walked (an ancestor swapped for a junction
 *    or a link after the walk): "folder" (TC1), unless any (the bridge's test override);
 *  - the volume's root unreadable with an error other than ENOENT/ENOTDIR (ENXIO) on a volume no
 *    longer mounted: "not-connected" (TC4); still mounted: the error's reason; a permission
 *    refusal: "no-permission";
 *  - Windows only: a mapped network drive, which the table of volumes doesn't list: "not-local"
 *    (plat_win_drive_remote, T3c); a drive no table lists and that isn't a network drive:
 *    "not-connected".
 * Linked with location.c, marker.c and path_rules.c in place of plat_mac.c / plat_win.c. Run on
 * the Mac by common/tests/run.sh; built for Windows by common/tests/win.sh (for the VM).
 *
 *   location-fake-test       exits 1 when a check fails */
#include "../location.h"
#include "../marker.h"
#include "../path_rules.h"
#if defined(_WIN32)
#include "../plat_win.h"
#endif
#include "check.h"

#include <errno.h>

#define ID "0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F"

/* the fake volume: what each call answers */
static struct {
    int holding_err, system, remote;     /* plat_volume_holding's error and system flag; plat_win_drive_remote */
    const char *mount;                   /* the volume plat_volume_holding and plat_volume_info name */
    int root_err, mounted;               /* plat_stat_path of the mount (0: a folder); plat_volume_mounted */
    uint64_t vol, final_vol;             /* the walk's volume; the volume of the folder plat_dir_open opens */
} fk;
static const char marker[] = "id=" ID "\nformat=" MARKER_FORMAT "\n";

int plat_volume_holding(const char *path, char *mount, size_t n, int *system) {
    (void)path;
    if (fk.holding_err) return fk.holding_err;
    snprintf(mount, n, "%s", fk.mount);
    *system = fk.system;
    return 0;
}
int plat_volume_mounted(const char *mount) {
    (void)mount;
    return fk.mounted;
}
int plat_stat_path(const char *path, int follow, plat_stat *st) {
    (void)follow;
    if (fk.root_err && !strcmp(path, fk.mount)) return fk.root_err;
    memset(st, 0, sizeof *st);
    st->kind = PLAT_DIR;
    st->id.vol = fk.vol;
    st->id.file[0] = strlen(path);
    return 0;
}
int plat_volume_info(const char *path, plat_volume *v) {
    (void)path;
    memset(v, 0, sizeof *v);
    snprintf(v->mount, sizeof v->mount, "%s", fk.mount);
    v->local = 1;
    return 0;
}
int plat_dir_open(const char *path, plat_dir *out) {
    (void)path;
    memset(out, 0, sizeof *out);
    return 0;
}
int plat_dir_stat(plat_dir d, plat_stat *st) {
    (void)d;
    memset(st, 0, sizeof *st);
    st->kind = PLAT_DIR;
    st->id.vol = fk.final_vol;
    st->id.file[0] = 7;
    return 0;
}
int plat_dir_writable(plat_dir d) {
    (void)d;
    return 0;
}
void plat_dir_close(plat_dir d) { (void)d; }
int plat_file_open_at(plat_dir d, const char *name, int64_t max, int64_t *size, plat_file *out) {
    (void)d;
    (void)max;
    if (strcmp(name, MARKER_NAME)) return ENOENT;
    memset(out, 0, sizeof *out);
    *size = (int64_t)strlen(marker);
    return 0;
}
int plat_read_all(plat_file f, void *buf, size_t n) {
    (void)f;
    if (n != strlen(marker)) return EIO;
    memcpy(buf, marker, n);
    return 0;
}
void plat_file_close(plat_file f) { (void)f; }
#if defined(_WIN32)
int plat_win_drive_remote(const char *path) {
    (void)path;
    return fk.remote;
}
#endif

/* a connected local volume that isn't this computer's, walked and opened on volume 2 */
static void reset(const char *mount) {
    memset(&fk, 0, sizeof fk);
    fk.mount = mount;
    fk.mounted = 1;
    fk.vol = fk.final_vol = 2;
}
static const char *check(const char *dir, int any, location_pin *pin) {
    const char *w = location_check(dir, ID, any, pin);
    return w ? w : "ok";
}

static void cases(const char *form, const char *dir, const char *mount) {
    location_pin pin;
    const char *w;
    /* the control: everything as it should be */
    reset(mount);
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "ok") && !strcmp(pin.mount, mount) && pin.id.vol == 2 && pin.id.file[0] == 7, "%s: a usable location: ok (%s)", form, w);
    /* TC1: the folder opened is on another volume than the one walked */
    fk.final_vol = 1;
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "folder"), "%s: the folder opened on another volume: folder (%s)", form, w);
    w = check(dir, 1, &pin);
    CHECK(!strcmp(w, "ok"), "%s: the same, any (the test override): ok (%s)", form, w);
    /* TC4: the root unreadable (ENXIO) */
    reset(mount);
    fk.root_err = ENXIO;
    fk.mounted = 0;
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "not-connected"), "%s: the root ENXIO, the volume unmounted: not-connected (%s)", form, w);
    CHECK(!strcmp(location_gone(mount, 0, ENXIO), "not-connected") && !strcmp(location_gone(mount, 1, ENXIO), "io-error"),
          "%s: gone with ENXIO, the volume unmounted: not-connected (any: io-error)", form);
    fk.mounted = 1;
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "io-error"), "%s: the root ENXIO, the volume still mounted: io-error (%s)", form, w);
    CHECK(!strcmp(location_gone(mount, 0, ENXIO), "io-error"), "%s: gone with ENXIO, the volume still mounted: io-error", form);
    fk.root_err = EACCES;
    fk.mounted = 0;
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "no-permission"), "%s: the root refused (EACCES): no-permission (%s)", form, w);
    fk.root_err = ENOENT;
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "not-connected"), "%s: the root gone (ENOENT): not-connected (%s)", form, w);
    /* no volume holds it, and it isn't a network drive */
    reset(mount);
    fk.holding_err = ENOENT;
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "not-connected"), "%s: no volume holds it, not a network drive: not-connected (%s)", form, w);
}

int main(void) {
    cases("Mac", "/Volumes/RBSPX/Sets/" PATHS_CACHE_FOLDER, "/Volumes/RBSPX");
    cases("Windows, a drive path", "D:\\Sets\\" PATHS_CACHE_FOLDER, "\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f}\\");
    cases("Windows, a GUID path", "\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f}\\Sets\\" PATHS_CACHE_FOLDER,
          "\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f}\\");
#if defined(_WIN32)
    /* a mapped network drive: no table lists it */
    location_pin pin;
    const char *w;
    reset("\\\\?\\Volume{0f8c2d3a-1b2c-4d5e-8f90-1a2b3c4d5e6f}\\");
    fk.holding_err = ENOENT;
    fk.remote = 1;
    w = check("Z:\\Sets\\" PATHS_CACHE_FOLDER, 0, &pin);
    CHECK(!strcmp(w, "not-local"), "a mapped network drive: not-local (%s)", w);
    w = check("Z:\\Sets\\" PATHS_CACHE_FOLDER, 1, &pin);
    CHECK(!strcmp(w, "not-local"), "a mapped network drive, any: not-local (%s)", w);
    fk.holding_err = EIO;
    w = check("Z:\\Sets\\" PATHS_CACHE_FOLDER, 0, &pin);
    CHECK(!strcmp(w, "io-error"), "the table unreadable, a network drive: io-error (%s)", w);
#else
    printf("note: the mapped network drive case is Windows' (plat_win_drive_remote): built by common/tests/win.sh\n");
#endif
    printf("location_fake: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
