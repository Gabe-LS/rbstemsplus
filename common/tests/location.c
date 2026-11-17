/* location.c's tests on the Mac (T3c), against plat_mac.c, on a scratch folder: this Mac's own
 * volume, so a location there is "not-connected" unless any (the bridge's test override) is set.
 * Windows runs the same function in common/tests/location_win.c (location-win-test.exe, the VM).
 * With hdiutil (outside a sandbox), a small exFAT image adds the cases with any=0 on another
 * volume (review TC5); skipped, and said so, when it can't be attached.
 *
 *   location-test SCRATCH       exits 1 when a check fails */
#include "../location.h"
#include "../marker.h"
#include "../path_rules.h"
#include "check.h"

#include <sys/stat.h>
#include <unistd.h>

#define ID "0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F"
static char root[1024];

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static const char *check(const char *dir, int any, location_pin *pin) {
    const char *w = location_check(dir, ID, any, pin);
    return w ? w : "ok";
}

/* hdiutil with its output kept out of the test's: 0 when it succeeded */
static int hdi(const char *args) {
    char cmd[3000];
    snprintf(cmd, sizeof cmd, "/usr/bin/hdiutil %s > /dev/null 2>&1", args);
    return system(cmd);
}

/* another volume, any=0 (review TC5): an exFAT image attached at a mount point of our own (as a
 * volume mounted in a folder: the Mac's walk starts below its mount point, wherever that is) */
static void t_image(void) {
    char img[1100], mnt[1100], a[3000], dir[1300], top[1300], p[1400];
    location_pin pin;
    const char *w;
    snprintf(img, sizeof img, "%s/loc.dmg", root);
    snprintf(mnt, sizeof mnt, "%s/mnt", root);
    mkdir(mnt, 0755);
    snprintf(a, sizeof a, "create -size 16m -fs ExFAT -volname RBSPLOC -o '%s'", img);
    if (hdi(a)) { printf("note: hdiutil can't make an image here (a sandbox?): the other-volume cases skipped\n"); return; }
    snprintf(a, sizeof a, "attach -nobrowse -mountpoint '%s' '%s'", mnt, img);
    if (hdi(a)) { printf("note: hdiutil can't attach here (a sandbox?): the other-volume cases skipped\n"); return; }
    snprintf(top, sizeof top, "%s/" PATHS_CACHE_FOLDER, mnt);
    snprintf(p, sizeof p, "%s/Sets", mnt);
    mkdir(p, 0755);
    snprintf(dir, sizeof dir, "%s/Sets/" PATHS_CACHE_FOLDER, mnt);
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "folder"), "image: no folder yet: folder (%s)", w);
    mkdir(dir, 0755);
    mkdir(top, 0755);
    snprintf(p, sizeof p, "%s/" MARKER_NAME, dir);
    put(p, "id=" ID "\nformat=" MARKER_FORMAT "\n");
    snprintf(p, sizeof p, "%s/" MARKER_NAME, top);
    put(p, "id=" ID "\nformat=" MARKER_FORMAT "\n");
    plat_stat st;
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "ok") && !strcmp(pin.mount, mnt) && plat_stat_path(dir, 0, &st) == 0 && plat_id_same(pin.id, st.id),
          "image: below its root, any=0: ok, its volume %s (%s)", pin.mount, w);
    w = check(top, 0, &pin);
    CHECK(!strcmp(w, "ok"), "image: at its root, any=0: ok (%s)", w);
    snprintf(p, sizeof p, "%s/Gone/" PATHS_CACHE_FOLDER, mnt);
    w = check(p, 0, &pin);
    CHECK(!strcmp(w, "folder"), "image: a missing folder: folder (%s)", w);
    /* read-only: attached again with -readonly */
    snprintf(a, sizeof a, "detach '%s'", mnt);
    hdi(a);
    snprintf(a, sizeof a, "attach -nobrowse -readonly -mountpoint '%s' '%s'", mnt, img);
    if (!hdi(a)) {
        w = check(dir, 0, &pin);
        CHECK(!strcmp(w, "read-only"), "image: attached read-only: read-only (%s)", w);
        snprintf(a, sizeof a, "detach '%s'", mnt);
        hdi(a);
    } else CHECK(0, "image: attached again read-only");
    /* detached: not connected, by the check and by gone */
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "not-connected") && !strcmp(location_gone(mnt, 0, ENOENT), "not-connected"),
          "image: detached: not-connected, and gone says so (%s)", w);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s SCRATCH\n", argv[0]); return 2; }
    if (!realpath(argv[1], root)) { perror(argv[1]); return 2; }   /* /var is a link to /private/var */
    char dir[1100], p[1200];
    location_pin pin;
    const char *w;

    /* the reasons of an errno */
    CHECK(!strcmp(location_why(ENOENT), "folder") && !strcmp(location_why(ENOTDIR), "folder") && !strcmp(location_why(ELOOP), "folder")
              && !strcmp(location_why(EXDEV), "folder") && !strcmp(location_why(EACCES), "no-permission")
              && !strcmp(location_why(EPERM), "no-permission") && !strcmp(location_why(EROFS), "read-only") && !strcmp(location_why(EIO), "io-error")
              && !strcmp(location_why(EBUSY), "io-error"),
          "location_why");

    snprintf(dir, sizeof dir, "%s/drive/%s", root, PATHS_CACHE_FOLDER);
    w = check(dir, 1, &pin);
    CHECK(!strcmp(w, "folder"), "no folder: folder (%s)", w);
    snprintf(p, sizeof p, "%s/drive", root);
    mkdir(p, 0755);
    mkdir(dir, 0755);
    w = check(dir, 1, &pin);
    CHECK(!strcmp(w, "marker"), "no marker: marker (%s)", w);
    snprintf(p, sizeof p, "%s/" MARKER_NAME, dir);
    put(p, "id=1F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F\nformat=" MARKER_FORMAT "\n");
    w = check(dir, 1, &pin);
    CHECK(!strcmp(w, "marker"), "another location's marker: marker (%s)", w);
    put(p, "id=" ID "\nformat=" MARKER_FORMAT "\n");
    memset(&pin, 0, sizeof pin);
    w = check(dir, 1, &pin);
    plat_stat st;
    CHECK(!strcmp(w, "ok") && plat_stat_path(dir, 0, &st) == 0 && plat_id_same(pin.id, st.id) && pin.mount[0] == '/',
          "ours: ok, pinned, its volume %s (%s)", pin.mount, w);
    /* this Mac's own volume: not connected, unless any */
    w = check(dir, 0, &pin);
    CHECK(!strcmp(w, "not-connected"), "on this Mac's own volume: not-connected (%s)", w);
    w = check("RB Stems Plus Cache.noindex", 1, &pin);
    CHECK(!strcmp(w, "not-connected"), "a relative path: not-connected (%s)", w);
    /* a component that isn't a real folder: a link, a file */
    snprintf(p, sizeof p, "%s/link", root);
    symlink("drive", p);
    snprintf(p, sizeof p, "%s/link/%s", root, PATHS_CACHE_FOLDER);
    w = check(p, 1, &pin);
    CHECK(!strcmp(w, "folder"), "a link on the way: folder (%s)", w);
    snprintf(p, sizeof p, "%s/file", root);
    put(p, "x");
    snprintf(p, sizeof p, "%s/file/%s", root, PATHS_CACHE_FOLDER);
    w = check(p, 1, &pin);
    CHECK(!strcmp(w, "folder"), "a file on the way: folder (%s)", w);
    snprintf(p, sizeof p, "%s/link2", root);
    snprintf(dir, sizeof dir, "%s/drive/%s", root, PATHS_CACHE_FOLDER);
    symlink(dir, p);
    w = check(p, 1, &pin);
    CHECK(!strcmp(w, "folder"), "the folder itself a link: folder (%s)", w);
    /* not writable; not searchable */
    chmod(dir, 0555);
    w = check(dir, 1, &pin);
    CHECK(geteuid() == 0 || !strcmp(w, "no-permission"), "a folder without write permission: no-permission (%s)", w);
    chmod(dir, 0755);
    snprintf(p, sizeof p, "%s/drive", root);
    chmod(p, 0644);
    w = check(dir, 1, &pin);
    CHECK(geteuid() == 0 || !strcmp(w, "no-permission"), "a parent without search permission: no-permission (%s)", w);
    chmod(p, 0755);
    /* gone: its volume no longer mounted, else the error's reason */
    w = check(dir, 1, &pin);
    CHECK(!strcmp(location_gone(pin.mount, 0, ENOENT), "folder") && !strcmp(location_gone(pin.mount, 0, EACCES), "no-permission"),
          "gone, its volume still there: the error's reason");
    CHECK(!strcmp(location_gone("/Volumes/rbsp-not-mounted", 0, ENOENT), "not-connected")
              && !strcmp(location_gone("/Volumes/rbsp-not-mounted", 1, ENOENT), "folder"),
          "gone, its volume unmounted: not-connected (any: the error's)");
    /* too long for a location */
    char lp[PATHS_LOCATION_MAX + 64];
    int k = snprintf(lp, sizeof lp, "%s/", root);
    while (k < PATHS_LOCATION_MAX + 10) lp[k++] = 'a';
    lp[k] = 0;
    CHECK(!strcmp(check(lp, 1, &pin), "folder"), "a path over %d bytes: folder", PATHS_LOCATION_MAX);
    t_image();
    printf("location: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
