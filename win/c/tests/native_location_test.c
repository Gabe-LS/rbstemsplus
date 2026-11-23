/* native_location.c on plat_mac.c (review host-facts HF5): probeLocation step for step on a
 * scratch folder given as a volume, measureCheapState over a scratch support folder, and the
 * bridge's line rules. */
#include "check.h"

#include "../native_location.h"
#include "../native_steps.h"
#include "../volumes.h"
#include "cache_owner.h"
#include "marker.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char loc_id[] = "0E8A1D84-5C2B-4B7A-9F3E-6D1C2B3A4F50";
/* Also supplied to the release-policy build to check that it ignores the fixture opt-in. */
int vol_test_internal_ok;

/* the fake host */
static struct {
    char mount[1100];
    int vol_err, system, local, readonly, remote;
    vol_facts device;
    int cache, says, running;
    int32_t pid;
    int64_t start;
    int volume_calls;
    uint64_t avail, total;   /* the space op's answer */
    char label[64], letter[8];   /* the volume name op's answer, or name_err */
    int name_err, name_calls;
    int bridge_calls, hold, hold_first_only;
} fk;

/* the live bridge: fk.cache its presence, fk.says its location=1 (asked with the bridge in), -1 none */
static int fk_bridge(void *ctx, int *present, int *location) {
    (void)ctx;
    int call = __atomic_add_fetch(&fk.bridge_calls, 1, __ATOMIC_SEQ_CST);
    while ((!fk.hold_first_only || call == 1) && __atomic_load_n(&fk.hold, __ATOMIC_SEQ_CST)) usleep(1000);
    *present = fk.cache > 0;
    *location = fk.cache > 0 && fk.says > 0;
    return fk.cache < 0 || (fk.cache > 0 && fk.says < 0) ? -1 : 0;
}
static int fk_running(void *ctx) {
    (void)ctx;
    return fk.running;
}
static int64_t fk_start(void *ctx, int32_t pid) {
    (void)ctx;
    return pid == fk.pid ? fk.start : -1;
}
static int fk_volname(void *ctx, const char *dir, char *label, size_t ln, char *letter, size_t lt) {
    (void)ctx, (void)dir;
    fk.name_calls++;
    if (fk.name_err) return fk.name_err;
    snprintf(label, ln, "%s", fk.label);
    snprintf(letter, lt, "%s", fk.letter);
    return 0;
}
static int fk_remote(void *ctx, const char *dir) {
    (void)ctx, (void)dir;
    return fk.remote;
}
static int fk_volume(void *ctx, const char *dir, char *mount, size_t n, vol_facts *facts) {
    (void)ctx, (void)dir;
    fk.volume_calls++;
    if (fk.vol_err) return fk.vol_err;
    snprintf(mount, n, "%s", fk.mount);
    *facts = fk.device;
    facts->system = fk.system;
    if (!fk.local) facts->drive_type = VOL_DRIVE_REMOTE;
    facts->readonly = fk.readonly;
    return 0;
}
static void fk_space(void *ctx, const char *path, uint64_t *avail, uint64_t *total) {
    (void)ctx, (void)path;
    *avail = fk.avail;
    *total = fk.total;
}
static const native_location_ops ops = {NULL, fk_bridge, fk_running, fk_start, fk_volname, fk_remote, fk_volume, fk_space,
                                        native_pc_cache_probe};

static void fk_reset(const char *mount) {
    memset(&fk, 0, sizeof fk);
    snprintf(fk.mount, sizeof fk.mount, "%s", mount);
    fk.local = 1;
    fk.device.drive_type = VOL_DRIVE_FIXED;
    fk.device.disks = 1;
    fk.device.bus = VOL_BUS_USB;
    strcpy(fk.device.fstype, "ntfs");
    snprintf(fk.label, sizeof fk.label, "Stick");
    snprintf(fk.letter, sizeof fk.letter, "E:");
}

static int bridge_calls(void) {
    return __atomic_load_n(&fk.bridge_calls, __ATOMIC_SEQ_CST);
}

static void spit(const char *path, const char *text, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    fwrite(text, 1, n, f);
    fclose(f);
}

/* the whole of a small file, or "" */
static void slurp(const char *path, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t k = fread(out, 1, n - 1, f);
    out[k] = 0;
    fclose(f);
}

/* a location folder at parent/RB Stems Plus Cache.noindex with the marker of id */
static void make_loc(const char *parent, const char *id, char *loc, size_t n) {
    mkdir(parent, 0755);
    snprintf(loc, n, "%s/%s", parent, PATHS_CACHE_FOLDER);
    mkdir(loc, 0755);
    char p[1400], m[128];
    snprintf(p, sizeof p, "%s/.rbstemsplus-cache", loc);
    int k = snprintf(m, sizeof m, "id=%s\nformat=rbstems-v1\n", id);
    spit(p, m, (size_t)k);
}

static void probes(const char *scratch) {
    char vol[1100], loc[1300], p[1400], q[1400];
    snprintf(vol, sizeof vol, "%s/vol", scratch);
    mkdir(vol, 0755);
    fk_reset(vol);
    snprintf(p, sizeof p, "%s/a", vol);
    make_loc(p, loc_id, loc, sizeof loc);
    CHECK(native_location_probe(&ops, loc, loc_id) == PROBE_AVAILABLE, "probe: a local folder with our marker, writable: available");
    CHECK(native_location_probe(&ops, loc, "11111111-2222-3333-4444-555555555555") == PROBE_MARKER_MISSING_OR_WRONG, "probe: another id's marker");
    char lower[64];
    for (size_t i = 0; i <= strlen(loc_id); i++) lower[i] = (char)(loc_id[i] >= 'A' && loc_id[i] <= 'Z' ? loc_id[i] + 32 : loc_id[i]);
    CHECK(native_location_probe(&ops, loc, lower) == PROBE_AVAILABLE, "probe: the id in another case is the same (lowercased())");
    /* the volume */
    fk.vol_err = ENOENT;
    CHECK(native_location_probe(&ops, loc, loc_id) == PROBE_NOT_CONNECTED, "probe: no volume holds it: not connected");
    fk.remote = 1;
    CHECK(native_location_probe(&ops, loc, loc_id) == PROBE_NETWORK, "probe: none holds it, a network drive: network");
    fk.vol_err = EIO;
    CHECK(native_location_probe(&ops, loc, loc_id) == PROBE_NOT_CONNECTED, "probe: the table can't be read: not connected");
    fk_reset(vol);
    fk.system = 1;
    CHECK(native_location_probe(&ops, loc, loc_id) == PROBE_NOT_CONNECTED, "probe: this PC's own volume: not connected (as the Mac's mountHolding)");

    vol_test_internal_ok = 1;
#ifdef VOL_TEST_INTERNAL_OK
    const int internal = PROBE_AVAILABLE, internal_readonly = PROBE_READ_ONLY;
#else
    const int internal = PROBE_NOT_CONNECTED, internal_readonly = PROBE_NOT_CONNECTED;
#endif
    CHECK(native_location_probe(&ops, loc, loc_id) == internal, "internal-ok: a marked folder on the system volume obeys the build's policy");
    fk_reset(vol);
    fk.device.bus = VOL_BUS_FILE_BACKED_VIRTUAL;
    strcpy(fk.device.fstype, "exfat");
    CHECK(native_location_probe(&ops, loc, loc_id) == internal, "internal-ok: a marked folder on an internal exFAT VHD obeys the build's policy");
    fk.readonly = 1;
    CHECK(native_location_probe(&ops, loc, loc_id) == internal_readonly, "internal-ok: a read-only internal VHD is never available");
    fk.readonly = 0;
    strcpy(fk.device.fstype, "refs");
    CHECK(native_location_probe(&ops, loc, loc_id) == PROBE_NOT_CONNECTED, "internal-ok: an unsupported filesystem stays ineligible");
    vol_test_internal_ok = 0;

    fk_reset(vol);
    fk.local = 0;
    CHECK(native_location_probe(&ops, loc, loc_id) == PROBE_NETWORK, "probe: a volume that isn't local: network");
    fk_reset(vol);
    fk.readonly = 1;
    CHECK(native_location_probe(&ops, loc, loc_id) == PROBE_READ_ONLY, "probe: a read-only volume: read-only");
    CHECK(native_location_probe(&ops, loc, "11111111-2222-3333-4444-555555555555") == PROBE_MARKER_MISSING_OR_WRONG,
          "probe: read-only with another marker: the marker first, as the Mac");
    fk_reset(vol);
    /* the components */
    snprintf(p, sizeof p, "%s/missing/%s", vol, PATHS_CACHE_FOLDER);
    CHECK(native_location_probe(&ops, p, loc_id) == PROBE_FOLDER_MISSING, "probe: a folder that isn't there: folder missing");
    snprintf(p, sizeof p, "%s/file", vol);
    spit(p, "x", 1);
    snprintf(q, sizeof q, "%s/file/%s", vol, PATHS_CACHE_FOLDER);
    CHECK(native_location_probe(&ops, q, loc_id) == PROBE_FOLDER_MISSING, "probe: a file on the way: folder missing");
    snprintf(p, sizeof p, "%s/a", vol);
    snprintf(q, sizeof q, "%s/linked", vol);
    CHECK(!symlink(p, q), "a link on the way");
    snprintf(q, sizeof q, "%s/linked/%s", vol, PATHS_CACHE_FOLDER);
    CHECK(native_location_probe(&ops, q, loc_id) == PROBE_LINK, "probe: a link on the way: link (never followed)");
    snprintf(p, sizeof p, "%s/c", vol);
    char loc2[1300];
    make_loc(p, loc_id, loc2, sizeof loc2);
    snprintf(q, sizeof q, "%s/.rbstemsplus-cache", loc2);
    unlink(q);
    CHECK(native_location_probe(&ops, loc2, loc_id) == PROBE_MARKER_MISSING_OR_WRONG, "probe: no marker: missing or wrong");
    if (geteuid() != 0) {
        chmod(p, 0);
        CHECK(native_location_probe(&ops, loc2, loc_id) == PROBE_NO_PERMISSION, "probe: a component that can't be looked at: no permission");
        chmod(p, 0755);
        chmod(loc, 0555);
        CHECK(native_location_probe(&ops, loc, loc_id) == PROBE_NO_PERMISSION, "probe: a folder that can't be written: no permission");
        chmod(loc, 0755);
        snprintf(q, sizeof q, "%s/.rbstemsplus-cache", loc);
        chmod(q, 0);
        CHECK(native_location_probe(&ops, loc, loc_id) == PROBE_NO_PERMISSION, "probe: a marker that can't be read: no permission");
        chmod(q, 0644);
    }
    /* outside the volume the table names: not below its mount */
    snprintf(p, sizeof p, "%s/elsewhere", scratch);
    make_loc(p, loc_id, loc2, sizeof loc2);
    CHECK(native_location_probe(&ops, loc2, loc_id) == PROBE_FOLDER_MISSING, "probe: a path not below the volume's mount: folder missing");
}

static void config(const char *sup, const char *text) {
    char p[1300];
    snprintf(p, sizeof p, "%s/config.ini", sup);
    if (text) spit(p, text, strlen(text));
    else unlink(p);
}

static void put_bridge_state(const char *sup, const char *text) {
    char p[1300];
    snprintf(p, sizeof p, "%s/Logs", sup);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Logs/bridge-state", sup);
    if (text) spit(p, text, strlen(text));
    else unlink(p);
}

static void measures(const char *scratch) {
    char vol[1100], loc[1300], sup[1100], cache[1200], p[1400], text[2048];
    snprintf(vol, sizeof vol, "%s/vol", scratch);
    snprintf(sup, sizeof sup, "%s/support", scratch);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    mkdir(sup, 0755);
    snprintf(p, sizeof p, "%s/m", vol);
    make_loc(p, loc_id, loc, sizeof loc);
    native_location_env e = {sup, cache, &ops};
    native_location l;
    fk_reset(vol);

    /* no config.ini: this PC, its Cache in use */
    config(sup, NULL);
    CHECK(!native_location_measure(&e, &l) && l.kind == LOC_THIS_MAC && l.probe == PROBE_AVAILABLE && !l.has_status_drive && l.has_in_use
              && !strcmp(l.in_use, cache) && !l.rekordbox_running && !l.has_bridge && fk.volume_calls == 0,
          "measure: no config.ini: this PC, Cache in use, local probe passed (kind %d)", l.kind);
    /* a chosen location, a bridge that saves there */
    snprintf(text, sizeof text, "[cache]\ncache_dir=\"%s\"\ncache_id=%s\n", loc, loc_id);
    config(sup, text);
    fk.cache = 1;
    fk.says = 1;
    CHECK(!native_location_measure(&e, &l) && l.kind == LOC_CHOSEN && l.probe == PROBE_AVAILABLE && l.has_status_drive && !strcmp(l.status_drive, "Stick")
              && l.has_in_use && !strcmp(l.in_use, loc),
          "measure: chosen, available: in use there, its drive named (kind %d, probe %d)", l.kind, l.probe);
    fk.device.bus = VOL_BUS_SATA;   /* the same marked disk on an internal port */
    CHECK(vol_refusal(&fk.device) == VOL_REFUSE_INTERNAL && !native_location_measure(&e, &l)
              && l.probe != PROBE_AVAILABLE && !l.has_in_use,
          "saved_internal_volume: the picker and the measure both refuse an internal disk");
    fk.device.bus = VOL_BUS_USB;
    strcpy(fk.device.fstype, "refs");   /* the marked directory was restored after reformat */
    CHECK(vol_refusal(&fk.device) == VOL_REFUSE_FILE_SYSTEM && !native_location_measure(&e, &l)
              && l.probe != PROBE_AVAILABLE && !l.has_in_use,
          "saved_unsupported_filesystem: the picker and the measure both refuse an unsupported filesystem");
    strcpy(fk.device.fstype, "ntfs");
    /* chosen, not available: nothing in use */
    snprintf(text, sizeof text, "[cache]\ncache_dir=\"%s\"\ncache_id=11111111-2222-3333-4444-555555555555\n", loc);
    config(sup, text);
    CHECK(!native_location_measure(&e, &l) && l.kind == LOC_CHOSEN && l.probe == PROBE_MARKER_MISSING_OR_WRONG && !l.has_in_use,
          "measure: chosen, another marker: nothing in use (probe %d)", l.probe);
    /* review NR4: another drive at the letter (the marker isn't ours) never overwrites the label kept */
    char hint[1200], want[1400], got[1400];
    snprintf(hint, sizeof hint, "%s/location-drive", sup);
    int hk = snprintf(want, sizeof want, "dir=%s\nlabel=A\n", loc);
    spit(hint, want, (size_t)hk);
    snprintf(fk.label, sizeof fk.label, "B");
    CHECK(!native_location_measure(&e, &l) && l.probe == PROBE_MARKER_MISSING_OR_WRONG, "measure: NR4: another marker (probe %d)", l.probe);
    slurp(hint, got, sizeof got);
    CHECK(!strcmp(got, want), "measure: NR4: the marker not ours: the hint keeps A (%s)", got);
    snprintf(fk.label, sizeof fk.label, "Stick");
    /* a bridge without location=1: this PC's folder for now */
    snprintf(text, sizeof text, "[cache]\ncache_dir=\"%s\"\ncache_id=%s\n", loc, loc_id);
    config(sup, text);
    fk.says = 0;
    CHECK(!native_location_measure(&e, &l) && l.kind == LOC_BRIDGE_TOO_OLD && l.has_status_drive && l.has_in_use && !strcmp(l.in_use, cache),
          "measure: the bridge too old: Cache in use, the drive still named (kind %d)", l.kind);
    /* no bridge installed: chosen (it would save there once installed) */
    fk.cache = 0;
    CHECK(!native_location_measure(&e, &l) && l.kind == LOC_CHOSEN, "measure: no bridge: chosen (kind %d)", l.kind);
    /* malformed: one key without the other */
    snprintf(text, sizeof text, "[cache]\ncache_dir=\"%s\"\n", loc);
    config(sup, text);
    CHECK(!native_location_measure(&e, &l) && l.kind == LOC_CONFIG_MALFORMED && !l.has_in_use && !l.has_status_drive,
          "measure: a location set wrongly: malformed, nothing in use (kind %d)", l.kind);
    /* unreadable: a NUL */
    config(sup, NULL);
    snprintf(p, sizeof p, "%s/config.ini", sup);
    spit(p, "enabled=1\n\0x", 12);
    CHECK(!native_location_measure(&e, &l) && l.kind == LOC_CONFIG_UNREADABLE && !l.has_in_use, "measure: config.ini unreadable (kind %d)", l.kind);
    config(sup, NULL);
    /* no answer from an input: -1 */
    fk.cache = -1;
    CHECK(native_location_measure(&e, &l) == -1, "measure: the bridge's presence unknown: no answer");
    fk.cache = 1;
    fk.says = -1;
    CHECK(native_location_measure(&e, &l) == -1, "measure: its location=1 unknown: no answer");
    fk.cache = 0;
    CHECK(!native_location_measure(&e, &l) && l.kind == LOC_THIS_MAC, "measure: no bridge: its location=1 not asked (kind %d)", l.kind);
    fk.cache = 1;
    fk.says = 1;
    fk.running = -1;
    CHECK(native_location_measure(&e, &l) == -1, "measure: the process list unknown: no answer");

    /* rekordbox runs: the bridge's state when it speaks for this rekordbox and this location */
    fk.running = 1;
    fk.pid = 4242;
    fk.start = 1700000000;
    snprintf(text, sizeof text, "[cache]\ncache_dir=\"%s\"\ncache_id=%s\n", loc, loc_id);
    config(sup, text);
    snprintf(text, sizeof text, "pid=4242\nstart=1700000000\nlocation=%s\nstate=unavailable\nreason=read-only\n", loc);
    put_bridge_state(sup, text);
    CHECK(!native_location_measure(&e, &l) && l.rekordbox_running && l.has_bridge && !strcmp(l.bridge_state, "unavailable")
              && !strcmp(l.bridge_reason, "read-only") && l.has_bridge_drive && !strcmp(l.bridge_drive, "Stick"),
          "measure: a live bridge state: its state, reason and drive");
    fk.start = 1700000001;
    CHECK(!native_location_measure(&e, &l) && l.rekordbox_running && !l.has_bridge, "measure: another process at its pid: none live");
    /* review NL3: a pid with no process (-1) never matches a bridge-state saying start=-1 */
    snprintf(text, sizeof text, "pid=999\nstart=-1\nlocation=%s\nstate=unavailable\nreason=read-only\n", loc);
    put_bridge_state(sup, text);
    CHECK(!native_location_measure(&e, &l) && l.rekordbox_running && !l.has_bridge, "measure: NL3: start=-1 and no such process: none live");
    fk.start = 1700000000;
    snprintf(text, sizeof text, "pid=4242\nstart=1700000000\nlocation=/other/%s\nstate=available\nreason=\n", PATHS_CACHE_FOLDER);
    put_bridge_state(sup, text);
    CHECK(!native_location_measure(&e, &l) && !l.has_bridge, "measure: a state for another location: none live");
    config(sup, NULL);
    put_bridge_state(sup, "pid=4242\nstart=1700000000\nlocation=\nstate=pending\nreason=\n");
    CHECK(!native_location_measure(&e, &l) && l.has_bridge && !strcmp(l.bridge_state, "pending") && !l.has_bridge_drive,
          "measure: this PC's state: live, no drive");
    fk.running = 0;
    CHECK(!native_location_measure(&e, &l) && !l.rekordbox_running && !l.has_bridge, "measure: rekordbox not running: no bridge state read");
    put_bridge_state(sup, "garbage");
    fk.running = 1;
    CHECK(!native_location_measure(&e, &l) && !l.has_bridge, "measure: a state that doesn't parse: none");
    /* the floor: the bridge's measure. review NL6: 10 GB free on a volume of unknown total (a SUBST
     * or mapped drive): the 50 GB floor, as the bridge pauses there */
    fk.avail = 10000000000ULL;
    fk.total = 0;
    CHECK(!native_location_measure(&e, &l) && l.paused_floor == 50000000000LL, "measure: NL6: unknown total: the 50e9 floor (%lld)",
          (long long)l.paused_floor);
    /* the plat calls themselves (no space op), on the nearest folder that exists (Cache isn't there yet) */
    native_location_ops plat_ops = ops;
    plat_ops.space = NULL;
    native_location_env pe = {sup, cache, &plat_ops};
    CHECK(!native_location_measure(&pe, &l) && (l.paused_floor == -1 || l.paused_floor > 0),
          "measure: the floor from this disk: -1 (not paused) or a size (%lld)", (long long)l.paused_floor);
    fk.avail = 60000000000ULL;
    CHECK(!native_location_measure(&e, &l) && l.paused_floor == -1, "measure: 60 GB free, total unknown: not paused, no floor");
}

/* review NL1, NL4: driveName on Windows paths, with the remembered name of an unplugged drive */
static void drive_names(const char *scratch) {
    char sup[1100], other[1100], hint[1200], out[NL_DRIVE_MAX];
    snprintf(sup, sizeof sup, "%s/names", scratch);
    snprintf(other, sizeof other, "%s/names-other", scratch);
    snprintf(hint, sizeof hint, "%s/location-drive", sup);
    mkdir(sup, 0755);
    mkdir(other, 0755);
    native_location_env e = {sup, "", &ops}, e2 = {other, "", &ops};
    const char *guid = "\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\music\\" PATHS_CACHE_FOLDER;
    const char *guid2 = "\\\\?\\Volume{fedcba98-7654-3210-fedc-ba9876543210}\\music\\" PATHS_CACHE_FOLDER;
    fk_reset("");

    /* plugged in: the label, remembered for that folder */
    native_location_drive_name(&e, guid, 1, out, sizeof out);
    struct stat a, b;
    CHECK(!strcmp(out, "Stick") && stat(hint, &a) == 0, "NL1: a labelled drive: its label (%s), remembered", out);
    native_location_drive_name(&e, guid, 1, out, sizeof out);
    CHECK(stat(hint, &b) == 0 && a.st_ino == b.st_ino, "NL1: the same name again: the hint isn't rewritten");
    /* unplugged: the remembered name */
    fk.name_err = ENOENT;
    native_location_drive_name(&e, guid, 1, out, sizeof out);
    CHECK(!strcmp(out, "Stick"), "NL1: unplugged: the name remembered (%s)", out);
    /* another folder unplugged: no name (never "Volume{...}" or a folder name) */
    native_location_drive_name(&e, guid2, 1, out, sizeof out);
    CHECK(!strcmp(out, ""), "NL1: another GUID folder unplugged, never seen: no name (%s)", out);
    /* a drive-letter path unplugged and never seen: no name, never its letter (owner: letters get reassigned) */
    native_location_drive_name(&e, "f:\\Music\\" PATHS_CACHE_FOLDER, 1, out, sizeof out);
    CHECK(!strcmp(out, ""), "NL1: a drive-letter path unplugged: no name, not its letter (%s)", out);

    /* no label: the drive letter while connected (HF5), and the old label cleared */
    fk.name_err = 0;
    fk.label[0] = 0;
    native_location_drive_name(&e, guid, 1, out, sizeof out);
    CHECK(!strcmp(out, "E:"), "NL1: an unlabelled drive: its letter (%s)", out);
    fk.name_err = ENOENT;
    native_location_drive_name(&e, guid, 1, out, sizeof out);
    CHECK(!strcmp(out, ""), "NL1: the unlabelled drive unplugged: no name, never the letter (%s)", out);
    /* neither a label nor a letter (a folder-mounted volume): no name */
    fk.name_err = 0;
    fk.letter[0] = 0;
    native_location_drive_name(&e, guid, 1, out, sizeof out);
    CHECK(!strcmp(out, ""), "NL1: no label, no letter: no name (%s)", out);
    /* an unlabelled drive never labelled: no hint written at all */
    char sup3[1100];
    snprintf(sup3, sizeof sup3, "%s/names-bare", scratch);
    mkdir(sup3, 0755);
    native_location_env e3 = {sup3, "", &ops};
    snprintf(fk.letter, sizeof fk.letter, "E:");
    native_location_drive_name(&e3, guid, 1, out, sizeof out);
    snprintf(hint, sizeof hint, "%s/location-drive", sup3);
    CHECK(!strcmp(out, "E:") && stat(hint, &a) != 0, "NL1: an unlabelled drive: nothing to remember, no hint written");

    /* without remember (the bridge's location): nothing written */
    fk.name_err = 0;
    snprintf(fk.label, sizeof fk.label, "Stick");
    native_location_drive_name(&e2, guid, 0, out, sizeof out);
    fk.name_err = ENOENT;
    native_location_drive_name(&e2, guid, 0, out, sizeof out);
    snprintf(hint, sizeof hint, "%s/location-drive", other);
    CHECK(!strcmp(out, "") && stat(hint, &a) != 0, "NL1: remember 0: no hint written, unplugged: no name (%s)", out);

    /* review NR3: a hand-edited hint that is malformed is no hint: the unplugged drive gets no name */
    char bad[1400];
    int bk = snprintf(bad, sizeof bad, "dir=%s\nlabel=", guid);
    for (int i = 0; i < 64; i++) bk += snprintf(bad + bk, sizeof bad - (size_t)bk, "\xF0\x9F\x8E\xB5");
    bk += snprintf(bad + bk, sizeof bad - (size_t)bk, "\n");
    spit(hint, bad, (size_t)bk);
    native_location_drive_name(&e2, guid, 0, out, sizeof out);
    CHECK(!strcmp(out, ""), "NR3: a label of 64 x U+1F3B5 (256 bytes): no name, never cut mid-character (%zu bytes)", strlen(out));
    bk = snprintf(bad, sizeof bad, "dir=%s", guid);
    memcpy(bad + bk, "\0x\nlabel=Other\n", 15);
    spit(hint, bad, (size_t)bk + 15);
    native_location_drive_name(&e2, guid, 0, out, sizeof out);
    CHECK(!strcmp(out, ""), "NR3: a NUL in dir: no hint (%s)", out);
    bk = snprintf(bad, sizeof bad, "dir=%s\nlabel=", guid);
    for (int i = 0; i < 256; i++) bad[bk++] = 'L';
    bad[bk++] = '\n';
    spit(hint, bad, (size_t)bk);
    native_location_drive_name(&e2, guid, 0, out, sizeof out);
    CHECK(!strcmp(out, ""), "NR3: a label of 256 ASCII bytes: no hint (%zu bytes)", strlen(out));
    bk = snprintf(bad, sizeof bad, "dir=%s\nlabel=Other\n", guid);
    spit(hint, bad, (size_t)bk);
    native_location_drive_name(&e2, guid, 0, out, sizeof out);
    CHECK(!strcmp(out, "Other"), "NR3: the same hint well formed: its label (%s)", out);

    /* NL4: a 100 x U+1F3B5 folder above an unplugged GUID folder never comes out, whole or cut */
    char path[1400];
    int k = snprintf(path, sizeof path, "\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\");
    for (int i = 0; i < 100; i++) k += snprintf(path + k, sizeof path - (size_t)k, "\xF0\x9F\x8E\xB5");
    snprintf(path + k, sizeof path - (size_t)k, "\\%s", PATHS_CACHE_FOLDER);
    native_location_drive_name(&e, path, 1, out, sizeof out);
    CHECK(!strcmp(out, ""), "NL4: no folder name for a GUID path (%zu bytes)", strlen(out));
}

static void lines(void) {
    static const char a[] = "xx rbstems bridge: 1.5\0yy rbstems-cache: rebuild=1 location=1 pioneer=abc,def; zz";
    static const char b[] = "xx rbstems bridge: 1.5\0yy rbstems-cache: rebuild=1 pioneer=abc location=1; zz";
    static const char c[] = "xx rbstems-cache: rebuild=1 location=1 pioneer=abc;";
    static const char d[] = "xx rbstems bridge: 1.5 rbstems-cache: rebuild=1 location=1";
    static const char f[] = "rbstems bridge: rbstems-cache: rebuild=1 location=10;";
    CHECK(native_location_bridge_says(a, sizeof a - 1) && native_location_is_bridge(a, sizeof a - 1), "location=1 before pioneer=: says");
    CHECK(!native_location_bridge_says(b, sizeof b - 1), "location=1 after pioneer=: doesn't");
    CHECK(!native_location_bridge_says(c, sizeof c - 1) && !native_location_is_bridge(c, sizeof c - 1), "no bridge line: neither");
    CHECK(!native_location_bridge_says(d, sizeof d - 1), "no ';': doesn't");
    CHECK(!native_location_bridge_says(f, sizeof f - 1), "location=10 isn't location=1");
    CHECK(!native_location_bridge_says(NULL, 0) && !native_location_is_bridge(NULL, 0), "nothing: neither");
    /* the line alone (compat_file's cache_line, a NUL among its bytes) */
    static const char g[] = "rebuild=1\0 location=1 pioneer=abc";
    CHECK(native_location_line_says("rebuild=1 location=1 pioneer=abc", 32) && native_location_line_says("location=1", 10),
          "line: location=1 before pioneer=, or alone: says");
    CHECK(native_location_line_says(g, sizeof g - 1), "line: a NUL before it: still a token of the line");
    CHECK(!native_location_line_says("rebuild=1 pioneer=abc location=1", 32) && !native_location_line_says("location=10", 11)
              && !native_location_line_says("xlocation=1", 11) && !native_location_line_says("", 0) && !native_location_line_says(NULL, 0),
          "line: after pioneer=, location=10, a longer token, empty, NULL: doesn't");
}

/* the worker: never on the UI thread, same-generation checks share a job, a bounded wait */
typedef struct {
    native_location_runner *r;
    const native_location_env *e;
    const plat_thread_id *ui;
    native_location l;
    int rc;
} nl_call;
static void *nl_call_run(void *v) {
    nl_call *c = v;
    c->rc = native_location_measure_bounded(c->r, c->e, c->ui, 2000000000ull, &c->l);
    return NULL;
}
/* review NW2: wakes the waiter every 5 ms, as spurious wakeups would, until told to stop */
typedef struct {
    native_location_runner *r;
    int stop;
} nl_poker;
static void *nl_poke_run(void *v) {
    nl_poker *p = v;
    while (!__atomic_load_n(&p->stop, __ATOMIC_SEQ_CST)) {
        plat_mutex_lock(&p->r->lock);
        plat_cond_broadcast(&p->r->done);
        plat_mutex_unlock(&p->r->lock);
        usleep(5000);
    }
    return NULL;
}
static int runner_busy(native_location_runner *r) {
    plat_mutex_lock(&r->lock);
    int b = 0;
    for (int i = 0; i < NL_MAX_JOBS; i++) b |= r->jobs[i] != NULL;
    plat_mutex_unlock(&r->lock);
    return b;
}

static void worker(const char *scratch) {
    static native_location_runner r = NATIVE_LOCATION_RUNNER_INIT;   /* a measure may outlive its call */
    char sup[1100], cache[1200];
    snprintf(sup, sizeof sup, "%s/worker", scratch);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    mkdir(sup, 0755);
    config(sup, NULL);
    fk_reset(scratch);
    native_location_env e = {sup, cache, &ops};
    native_location l, want;
    CHECK(!native_location_measure(&e, &want) && want.kind == LOC_THIS_MAC, "worker: the measure itself");

    /* answered, off the UI thread: the measure's own answer */
    plat_thread_id me = plat_thread_self();
    nl_call c = {&r, &e, &me, {0}, 99};
    pthread_t t;
    CHECK(!pthread_create(&t, NULL, nl_call_run, &c) && !pthread_join(t, NULL), "worker: a thread");
    CHECK(c.rc == 0 && !memcmp(&c.l, &want, sizeof want), "worker: off the UI thread: the measure's answer (%d)", c.rc);
    CHECK(native_location_measure_bounded(&r, &e, NULL, 2000000000ull, &l) == 0 && !memcmp(&l, &want, sizeof want),
          "worker: no UI thread given: answered");

    /* on the UI thread: refused, nothing measured */
    int calls = bridge_calls();
    CHECK(native_location_measure_bounded(&r, &e, &me, 2000000000ull, &l) == NL_ON_UI && bridge_calls() == calls && l.probe == -1
              && l.paused_floor == -1 && !l.has_in_use,
          "worker: on the UI thread: NL_ON_UI, no measure, nothing out");

    /* no answer from an input, through the worker: no answer, nothing out */
    fk.cache = -1;
    CHECK(native_location_measure_bounded(&r, &e, NULL, 2000000000ull, &l) == NL_NO_ANSWER && !l.has_in_use,
          "worker: the live bridge has no answer: NL_NO_ANSWER");
    fk.cache = 0;

    /* a slow measure: the wait runs out (NL_TIMEOUT). A call for other paths finds it
     * still running (NL_BUSY); one for the same paths joins it with its own wait (review N3R18). */
    __atomic_store_n(&fk.hold, 1, __ATOMIC_SEQ_CST);
    char *hs = strdup(sup), *hc = strdup(cache);   /* freed while the measure runs: it holds its own copies (ASan) */
    native_location_env eh = {hs, hc, &ops};
    uint64_t t0 = plat_now_mono_ns();
    int rc = native_location_measure_bounded(&r, &eh, NULL, 50000000ull, &l);
    CHECK(rc == NL_TIMEOUT && !l.has_in_use && l.probe == -1,
          "worker: a measure past its wait: NL_TIMEOUT, nothing out");
    uint64_t waited = plat_now_mono_ns() - t0;
    free(hs);
    free(hc);
    CHECK(waited >= 40000000ull && waited < 1500000000ull, "worker: waited about 50 ms (%llu ms)", (unsigned long long)(waited / 1000000));
    native_location_env other = {sup, "", &ops};
    native_location_target target;
    int64_t size_kind;
    const char *dir = "E:\\Music\\" PATHS_CACHE_FOLDER;
    CHECK(!native_location_now_facts(1, rc ? NULL : &l, dir, loc_id, "", dir, &target, &size_kind)
              && !strcmp(target.dir, dir) && !target.gate_id[0] && size_kind == -1,
          "LOCATION_NOW: a timed-out measure keeps the chosen folder without a gate or size (review N3R3)");
    rc = native_location_measure_bounded(&r, &other, NULL, 50000000ull, &l);
    CHECK(rc == NL_BUSY, "worker: a measure for other paths still running: NL_BUSY");
    CHECK(!native_location_now_facts(1, rc ? NULL : &l, dir, loc_id, "Remembered", dir, &target, &size_kind)
              && !strcmp(target.dir, dir) && !strcmp(target.drive, "Remembered") && !target.gate_id[0] && size_kind == -1,
          "LOCATION_NOW: a busy measure keeps the chosen folder and remembered label (review NS11, N3R3)");
    CHECK(native_location_now_facts(1, rc ? NULL : &l, NULL, "", "", cache, &target, &size_kind) == 1,
          "LOCATION_NOW: busy with This PC returns 1 (review N3R3)");
    t0 = plat_now_mono_ns();
    rc = native_location_measure_bounded(&r, &e, NULL, 50000000ull, &l);
    waited = plat_now_mono_ns() - t0;
    CHECK(rc == NL_TIMEOUT && !l.has_in_use && waited >= 40000000ull && waited < 1500000000ull,
          "worker: a joined call gets its own bounded wait (review N3R18)");
    __atomic_store_n(&fk.hold, 0, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 2000 && runner_busy(&r); i++) usleep(1000);
    CHECK(!runner_busy(&r), "worker: the late measure ended (its answer dropped, its job freed)");
    CHECK(native_location_measure_bounded(&r, &e, NULL, 2000000000ull, &l) == 0 && !memcmp(&l, &want, sizeof want),
          "worker: after it: answered again");

    /* review NW2: wakeups that aren't the job's end don't restart the wait: still about 50 ms */
    __atomic_store_n(&fk.hold, 1, __ATOMIC_SEQ_CST);
    nl_poker pk = {&r, 0};
    pthread_t pt;
    CHECK(!pthread_create(&pt, NULL, nl_poke_run, &pk), "worker: the poking thread");
    t0 = plat_now_mono_ns();
    int prc = native_location_measure_bounded(&r, &e, NULL, 50000000ull, &l);
    waited = plat_now_mono_ns() - t0;
    __atomic_store_n(&pk.stop, 1, __ATOMIC_SEQ_CST);
    pthread_join(pt, NULL);
    CHECK(prc == NL_TIMEOUT && waited < 120000000ull, "worker: woken every 5 ms: NL_TIMEOUT in under 120 ms (%d, %llu ms)", prc,
          (unsigned long long)(waited / 1000000));
    __atomic_store_n(&fk.hold, 0, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 2000 && runner_busy(&r); i++) usleep(1000);
    CHECK(!runner_busy(&r), "worker: the poked measure ended");
}

/* CORE_SLOW_LOCATION's release behavior and its disabled fallback. A timeout, a busy worker,
 * no answer and out of memory are CORE_SLOW_NOT_NOW. The UI thread and missing paths return -1. */
typedef struct {
    int writes, logs;
    char last[128];
    native_location result;
} nl_sink;
static int sink_write(void *ctx, const native_location *l) {
    nl_sink *k = ctx;
    k->writes++;
    k->result = *l;
    return l->kind == LOC_THIS_MAC ? 0 : 1;
}
static void sink_log(void *ctx, const char *line) {
    nl_sink *k = ctx;
    k->logs++;
    snprintf(k->last, sizeof k->last, "%s", line);
}
static void slow_answers(const char *scratch) {
    static native_location_runner r = NATIVE_LOCATION_RUNNER_INIT;   /* a measure may outlive its call */
    char sup[1100], cache[1200];
    snprintf(sup, sizeof sup, "%s/slow", scratch);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    mkdir(sup, 0755);
    config(sup, NULL);
    fk_reset(scratch);
    native_location_env e = {sup, cache, &ops}, none = {NULL, cache, &ops};
    nl_sink k = {0};
    plat_thread_id me = plat_thread_self();
    int calls = bridge_calls();
    native_location_test_off = 1;
    CHECK(native_location_slow_fact(&r, &e, NULL, 2000000000ull, sink_write, sink_log, &k) == -1 && !k.writes && !k.logs
              && bridge_calls() == calls,
          "slow fact: the gate off: -1, nothing measured");
    native_location_test_off = 0;
    CHECK(native_location_live(), "Settings, LOCATION_NOW and reports use the live release gate");
    put_bridge_state(sup, "pid=4242\nstart=1700000000\nlocation=\nstate=available\nreason=\n");
    fk.pid = 4242;
    fk.start = 1700000000;
    fk.avail = 10000000000ULL;
    CHECK(native_location_slow_fact(&r, &e, NULL, 2000000000ull, sink_write, sink_log, &k) == 0 &&
              k.writes == 1 && !k.result.has_bridge && native_location_checked(&k.result) &&
              k.result.has_in_use && k.result.paused_floor == 50000000000LL,
          "slow fact: absent offline This PC cache is checked and uses the disk floor");
    if (mkdir(cache, 0755)) abort();
    k = (nl_sink){0};
    fk.running = 1;
    put_bridge_state(sup, "pid=4242\nstart=1700000000\nlocation=/other/RB Stems Plus\nstate=available\nreason=\n");
    CHECK(native_location_slow_fact(&r, &e, NULL, 2000000000ull, sink_write, sink_log, &k) == 0 &&
              k.writes == 1 && !k.result.has_bridge && !native_location_checked(&k.result) &&
              k.result.probe == PROBE_MARKER_MISSING_OR_WRONG && !k.result.has_in_use,
          "slow fact: an existing unmarked slot reports a folder problem even with a live result for another folder");
    k = (nl_sink){0};
    calls = bridge_calls();
    put_bridge_state(sup, "pid=4242\nstart=1700000000\nlocation=\nstate=available\nreason=\n");
    CHECK(native_location_slow_fact(&r, &e, NULL, 2000000000ull, sink_write, sink_log, &k) == 0 && k.writes == 1 && !k.logs
              && bridge_calls() == calls + 1,
          "slow fact: This PC with a matching live result writes the answer (%d)", k.writes);
    plat_dir d;
    plat_stat st;
    if (plat_dir_open(cache, &d) || plat_dir_stat(d, &st)) abort();
    plat_dir_close(d);
    char marker[1300], identity[CACHE_OWNER_IDENTITY_MAX];
    snprintf(marker, sizeof marker, "%s/%s", cache, CACHE_OWNER_MARKER);
    cache_owner_identity(identity, 0, st.id);
    spit(marker, identity, strlen(identity));
    fk.running = 0;
    nl_sink offline = {0};
    CHECK(native_location_slow_fact(&r, &e, NULL, 2000000000ull, sink_write, sink_log, &offline) == 0 &&
              offline.writes == 1 && !offline.result.has_bridge && native_location_checked(&offline.result) &&
              offline.result.has_in_use && offline.result.paused_floor == 50000000000LL,
          "slow fact: marked offline This PC cache is checked and uses the disk floor");
    unlink(marker);
    rmdir(cache);
    CHECK(native_location_slow_fact(&r, &none, NULL, 2000000000ull, sink_write, sink_log, &k) == -1 && k.writes == 1,
          "slow fact: no support path: -1");
    none.support = sup;
    none.cache_default = NULL;
    CHECK(native_location_slow_fact(&r, &none, NULL, 2000000000ull, sink_write, sink_log, &k) == -1 && k.writes == 1,
          "slow fact: no default cache path: -1");
    CHECK(native_location_slow_fact(&r, &e, &me, 2000000000ull, sink_write, sink_log, &k) == -1 && k.writes == 1 && k.logs == 1
              && strstr(k.last, "UI thread"),
          "slow fact: on the UI thread: -1, a log line (%s)", k.last);
    fk.cache = -1;
    CHECK(native_location_slow_fact(&r, &e, NULL, 2000000000ull, sink_write, sink_log, &k) == CORE_SLOW_NOT_NOW && k.writes == 1
              && k.logs == 2 && strstr(k.last, "had no answer"),
          "slow fact: no answer: not now (%s)", k.last);
    fk.cache = 0;
    native_location_test_no_memory = 1;
    calls = bridge_calls();
    CHECK(native_location_slow_fact(&r, &e, NULL, 2000000000ull, sink_write, sink_log, &k) == CORE_SLOW_NOT_NOW && k.writes == 1
              && bridge_calls() == calls,
          "slow fact: out of memory: not now, nothing measured");
    native_location_test_no_memory = 0;
    __atomic_store_n(&fk.hold, 1, __ATOMIC_SEQ_CST);
    native_location_env other = {sup, "", &ops};
    CHECK(native_location_slow_fact(&r, &e, NULL, 50000000ull, sink_write, sink_log, &k) == CORE_SLOW_NOT_NOW && k.writes == 1
              && strstr(k.last, "over 15 s"),
          "slow fact: a timeout: not now (%s)", k.last);
    CHECK(native_location_slow_fact(&r, &other, NULL, 50000000ull, sink_write, sink_log, &k) == CORE_SLOW_NOT_NOW && k.writes == 1
              && strstr(k.last, "still running"),
          "slow fact: busy: not now (%s)", k.last);
    __atomic_store_n(&fk.hold, 0, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 2000 && runner_busy(&r); i++) usleep(1000);
    CHECK(!runner_busy(&r), "slow fact: the late measure ended");
    CHECK(native_location_slow_fact(&r, &e, NULL, 2000000000ull, sink_write, sink_log, &k) == 0 && k.writes == 2,
          "slow fact: after it: answered again");
}

typedef struct {
    native_location_runner *r;
    const native_location_env *e;
    native_location l;
    int rc, writes;
} nl_memo;

static int memo_write(void *ctx, const native_location *l) {
    nl_memo *m = ctx;
    m->l = *l;
    m->writes++;
    return 0;
}
static void memo_log(void *ctx, const char *line) {
    (void)ctx;
    (void)line;
}
static void *memo_run(void *ctx) {
    nl_memo *m = ctx;
    m->rc = native_location_slow_fact(m->r, m->e, NULL, 2000000000ull, memo_write, memo_log, m);
    return NULL;
}
static int wait_for_waiters(native_location_runner *r, int n) {
    for (int i = 0; i < 500; i++) {
        if (native_location_test_waiters(r) == n && bridge_calls() == 1) return 1;
        usleep(1000);
    }
    return 0;
}

/* review N3R18: the memo and the flow share a measure in either order. Once the old config has
 * been read, change just one of the two keys to show that each invalidates its answer. */
static void shared_measure(const char *scratch) {
    static native_location_runner r = NATIVE_LOCATION_RUNNER_INIT;
    char sup[1100], cache[1200], a[1200], b[1200], dir[1200], other[1200], ini[1500];
    snprintf(sup, sizeof sup, "%s/share", scratch);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    snprintf(a, sizeof a, "%s/share-a", scratch);
    snprintf(b, sizeof b, "%s/share-b", scratch);
    mkdir(sup, 0755);
    make_loc(a, loc_id, dir, sizeof dir);
    make_loc(b, loc_id, other, sizeof other);
    native_location_env e = {sup, cache, &ops};
    for (int row = 0; row < 4; row++) {
        int memo_first = row == 0 || row == 2;
        fk_reset(scratch);
        snprintf(ini, sizeof ini, "cache_dir=\"%s\"\ncache_id=%s\n", dir, loc_id);
        config(sup, ini);
        __atomic_store_n(&fk.hold, 1, __ATOMIC_SEQ_CST);
        nl_call flow = {&r, &e, NULL, {0}, 99};
        nl_memo memo = {.r = &r, .e = &e, .rc = 99};
        pthread_t ft, mt;
        if (memo_first ? pthread_create(&mt, NULL, memo_run, &memo) : pthread_create(&ft, NULL, nl_call_run, &flow)) abort();
        int started = wait_for_waiters(&r, 1);
        if (memo_first ? pthread_create(&ft, NULL, nl_call_run, &flow) : pthread_create(&mt, NULL, memo_run, &memo)) abort();
        int shared = wait_for_waiters(&r, 2);
        if (row == 2) snprintf(ini, sizeof ini, "cache_dir=\"%s\"\ncache_id=%s\n", other, loc_id);
        if (row == 3) snprintf(ini, sizeof ini, "cache_dir=\"%s\"\ncache_id=01234567-89ab-cdef-0123-456789abcdef\n", dir);
        if (row >= 2) config(sup, ini);
        __atomic_store_n(&fk.hold, 0, __ATOMIC_SEQ_CST);
        if (pthread_join(ft, NULL) || pthread_join(mt, NULL)) abort();
        if (row < 2) {
            CHECK(started && shared && bridge_calls() == 1 && flow.rc == 0 && memo.rc == 0 && memo.writes == 1
                      && flow.l.probe == PROBE_AVAILABLE && native_location_matches_config(&flow.l, dir, loc_id)
                      && !strcmp(flow.l.in_use, memo.l.in_use),
                  "N3R18: the %s measure answers both callers", memo_first ? "memo's" : "flow's");
        } else {
            CHECK(started && shared && bridge_calls() == 1 && flow.rc == NL_NO_ANSWER && !flow.l.has_in_use
                      && memo.rc == CORE_SLOW_NOT_NOW && !memo.writes,
                  "N3R18: a change to %s discards the shared answer", row == 2 ? "cache_dir" : "cache_id");
        }
    }
}

static void t_location_config_generation(const char *scratch) {
    static native_location_runner r = NATIVE_LOCATION_RUNNER_INIT;
    char sup[1100], cache[1200], parent[1200], dir[1200], ini[1500];
    snprintf(sup, sizeof sup, "%s/generation", scratch);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    snprintf(parent, sizeof parent, "%s/generation-drive", scratch);
    mkdir(sup, 0755);
    make_loc(parent, loc_id, dir, sizeof dir);
    snprintf(ini, sizeof ini, "cache_dir=\"%s\"\ncache_id=%s\n", dir, loc_id);
    config(sup, ini);
    fk_reset(scratch);
    fk.hold_first_only = 1;
    __atomic_store_n(&fk.hold, 1, __ATOMIC_SEQ_CST);
    native_location_env e = {sup, cache, &ops};
    native_location l;
    CHECK(native_location_measure_bounded(&r, &e, NULL, 50000000ull, &l) == NL_TIMEOUT,
          "generation: the first chosen-folder check times out");
    nl_call old = {&r, &e, NULL, {0}, 99};
    pthread_t waiter;
    if (pthread_create(&waiter, NULL, nl_call_run, &old)) abort();
    CHECK(wait_for_waiters(&r, 1), "generation: a waiter joined the old check");

    config(sup, NULL);
    int rc = native_location_measure_bounded(&r, &e, NULL, 250000000ull, &l);
    CHECK(rc == 0 && l.kind == LOC_THIS_MAC && l.has_in_use && !strcmp(l.in_use, cache),
          "generation: This PC answers while the old target is still hung (%d)", rc);
    config(sup, ini);
    rc = native_location_measure_bounded(&r, &e, NULL, 50000000ull, &l);
    CHECK(rc == NL_TIMEOUT && !l.has_in_use && bridge_calls() == 2,
          "F8: switching back joins the hung check for the same folder and marker (%d)", rc);
    config(sup, NULL);
    rc = native_location_measure_bounded(&r, &e, NULL, 250000000ull, &l);
    CHECK(rc == 0 && l.kind == LOC_THIS_MAC && l.has_in_use && bridge_calls() == 3,
          "F8: This PC still has a worker while the chosen folder is hung (%d)", rc);
    __atomic_store_n(&fk.hold, 0, __ATOMIC_SEQ_CST);
    if (pthread_join(waiter, NULL)) abort();
    CHECK(old.rc == NL_NO_ANSWER && !old.l.has_in_use, "generation: the first generation cannot publish after switching back (%d)", old.rc);

    config(sup, ini);
    fk_reset(scratch);
    __atomic_store_n(&fk.hold, 1, __ATOMIC_SEQ_CST);
    CHECK(native_location_measure_bounded(&r, &e, NULL, 250000000ull, &l) == NL_TIMEOUT,
          "generation: one hung check occupies one worker");
    config(sup, NULL);
    CHECK(native_location_measure_bounded(&r, &e, NULL, 250000000ull, &l) == NL_TIMEOUT && bridge_calls() == 2,
          "generation: a second hung check occupies the other worker");
    config(sup, ini);
    CHECK(native_location_measure_bounded(&r, &e, NULL, 50000000ull, &l) == NL_TIMEOUT && bridge_calls() == 2 && !l.has_in_use,
          "F8: a matching job can be joined even when both workers are busy");
    __atomic_store_n(&fk.hold, 0, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 2000 && runner_busy(&r); i++) usleep(1000);
    CHECK(!runner_busy(&r), "generation: both workers become available again");
    CHECK(native_location_measure_bounded(&r, &e, NULL, 2000000000ull, &l) == 0 && l.probe == PROBE_AVAILABLE,
          "generation: checks resume when a worker is available");
}

typedef struct {
    const char *dir;
    int entered, released;
} held_space;

static void hold_chosen_space(void *ctx, const char *path, uint64_t *avail, uint64_t *total) {
    held_space *h = ctx;
    if (!strcmp(path, h->dir)) {
        __atomic_store_n(&h->entered, 1, __ATOMIC_SEQ_CST);
        while (!__atomic_load_n(&h->released, __ATOMIC_SEQ_CST)) usleep(1000);
    }
    fk_space(NULL, path, avail, total);
}

static void t_location_rejoin_stale_health(const char *scratch) {
    static native_location_runner r = NATIVE_LOCATION_RUNNER_INIT;
    char sup[1100], cache[1200], parent[1200], dir[1200], ini[1500], marker[1300];
    snprintf(sup, sizeof sup, "%s/rejoin", scratch);
    snprintf(cache, sizeof cache, "%s/Cache", sup);
    snprintf(parent, sizeof parent, "%s/rejoin-drive", scratch);
    mkdir(sup, 0755);
    make_loc(parent, loc_id, dir, sizeof dir);
    snprintf(ini, sizeof ini, "cache_dir=\"%s\"\ncache_id=%s\n", dir, loc_id);
    snprintf(marker, sizeof marker, "%s/%s", dir, MARKER_NAME);
    config(sup, ini);
    fk_reset(scratch);
    held_space hold = {.dir = dir};
    native_location_ops held_ops = ops;
    held_ops.ctx = &hold;
    held_ops.space = hold_chosen_space;
    native_location_env e = {sup, cache, &held_ops};
    native_location l;
    CHECK(native_location_measure_bounded(&r, &e, NULL, 50000000ull, &l) == NL_TIMEOUT &&
              __atomic_load_n(&hold.entered, __ATOMIC_SEQ_CST),
          "F8: the first A worker passed the marker probe and stalled in the space query");
    config(sup, NULL);
    CHECK(native_location_measure_bounded(&r, &e, NULL, 500000000ull, &l) == 0 && l.kind == LOC_THIS_MAC,
          "F8: This PC answers while A still occupies its slot");
    if (unlink(marker)) abort();
    config(sup, ini);
    nl_call newer = {&r, &e, NULL, {0}, 99};
    pthread_t waiter;
    if (pthread_create(&waiter, NULL, nl_call_run, &newer)) abort();
    for (int i = 0; i < 500 && native_location_test_waiters(&r) != 1; i++) usleep(1000);
    CHECK(native_location_test_waiters(&r) == 1 && bridge_calls() == 2, "F8: the newer A waiter waits for A's slot");
    __atomic_store_n(&hold.released, 1, __ATOMIC_SEQ_CST);
    if (pthread_join(waiter, NULL)) abort();
    CHECK(newer.rc == 0 && newer.l.probe == PROBE_MARKER_MISSING_OR_WRONG && !newer.l.has_in_use && bridge_calls() == 3,
          "F8: the newer A waiter rechecks the marker after the slot frees (rc %d, probe %d, calls %d)",
          newer.rc, newer.l.probe, bridge_calls());
}

static int stalled_name(void *ctx, const char *dir, char *label, size_t ln, char *letter, size_t lt) {
    int *calls = ctx;
    ++*calls;
    plat_sleep_ms(250);
    return fk_volname(NULL, dir, label, ln, letter, lt);
}

static void stalled_space(void *ctx, const char *path, uint64_t *avail, uint64_t *total) {
    int *calls = ctx;
    if (++*calls > 1) plat_sleep_ms(250);
    fk_space(NULL, path, avail, total);
}

static void t_display_uses_known_label(const char *support) {
    const char *dir = "/previously-checked-drive";
    int calls = 0;
    native_location_ops drive_ops = {.ctx = &calls, .volume_name = stalled_name};
    native_location_env e = {support, support, &drive_ops};
    native_location l = {.has_status_drive = 1, .status_drive = "Measured"};
    snprintf(l.config_dir, sizeof l.config_dir, "%s", dir);
    snprintf(l.config_id, sizeof l.config_id, "%s", loc_id);
    char hint[NL_PATH_MAX];
    snprintf(hint, sizeof hint, "%s/location-drive", support);
    const char saved[] = "dir=/previously-checked-drive\nlabel=Remembered\n";
    spit(hint, saved, sizeof saved - 1);
    native_location_details d;
    native_location_describe(&e, &l, dir, loc_id, &d);
    CHECK(!calls && !strcmp(d.drive, "Measured"),
          "t_display_uses_known_label: display must use the measured label without another drive call");
    native_location_describe(&e, &l, dir, "changed-id", &d);
    CHECK(!calls && !strcmp(d.drive, "Remembered") && !d.has_disk,
          "t_display_uses_known_label: a stale measure cannot name the current drive");
    native_location_describe(&e, NULL, dir, loc_id, &d);
    CHECK(!calls && !strcmp(d.drive, "Remembered"),
          "t_display_uses_known_label: unchecked display must use only the remembered label");
    unlink(hint);
}

static void t_report_reuses_disk_space(const char *support) {
    int calls = 0;
    fk_reset(support);
    fk.avail = 1234567890;
    fk.total = 98765432100;
    native_location_ops space_ops = ops;
    space_ops.ctx = &calls;
    space_ops.space = stalled_space;
    char cache[NL_PATH_MAX];
    snprintf(cache, sizeof cache, "%s/Cache", support);
    native_location_env e = {support, cache, &space_ops};
    native_location l;
    CHECK(!native_location_measure(&e, &l) && calls == 1, "the location measurement gets one disk answer");
    native_location_details d;
    native_location_describe(&e, &l, "", "", &d);
    CHECK(calls == 1 && d.has_disk && d.available == fk.avail && d.total == fk.total,
          "t_report_reuses_disk_space: report must use the measured values without a second disk call");
    native_location_describe(&e, NULL, "", "", &d);
    CHECK(calls == 1 && !d.has_disk, "t_report_reuses_disk_space: a missing measure leaves disk space unknown");
}

static atomic_int config_open_count, config_open_returned;
static int block_config_open_at;

static void block_config_open(const char *support) {
    (void)support;
    if (atomic_fetch_add(&config_open_count, 1) + 1 != block_config_open_at) return;
    plat_sleep_ms(250);
    atomic_store(&config_open_returned, 1);
}

static void t_config_reads_deadline(const char *scratch) {
    static native_location_runner r = NATIVE_LOCATION_RUNNER_INIT;
    char support[1100], cache[1200];
    for (int read = 1; read <= 2; read++) {
        snprintf(support, sizeof support, "%s/blocked-config-%d", scratch, read);
        snprintf(cache, sizeof cache, "%s/Cache", support);
        if (mkdir(support, 0755)) abort();
        fk_reset(scratch);
        native_location_env e = {support, cache, &ops};
        native_location l;
        block_config_open_at = read;
        atomic_store(&config_open_count, 0);
        atomic_store(&config_open_returned, 0);
        native_location_test_config_read = block_config_open;
        int rc = native_location_measure_bounded(&r, &e, NULL, 100000000, &l);
        CHECK(rc == NL_TIMEOUT && atomic_load(&config_open_count) == read && !atomic_load(&config_open_returned) &&
              !l.has_in_use && !native_location_checked(&l),
              "R2: measure config read %d observes the deadline and publishes no checked state (%d)", read, rc);
        for (int i = 0; i < 500 && !atomic_load(&config_open_returned); i++) plat_sleep_ms(1);
        if (!atomic_load(&config_open_returned)) abort();
        native_location_test_config_read = NULL;
    }
}

void test_native_location(const char *scratch) {
    /* config.ini's cache_dir takes plain components only: a TMPDIR ending in '/' gives "//" */
    char real[PATH_MAX];
    int have = realpath(scratch, real) != NULL;
    CHECK(have, "the scratch folder's real path");
    lines();
    if (!have) return;
    probes(real);
    measures(real);
    drive_names(real);
    worker(real);
    slow_answers(real);
    shared_measure(real);
    t_location_config_generation(real);
    t_location_rejoin_stale_health(real);
    t_display_uses_known_label(real);
    t_report_reuses_disk_space(real);
    t_config_reads_deadline(real);
}
