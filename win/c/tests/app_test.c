/* The app's own platform calls (W6a: volumes.c, lifecycle.c, report.c, shell.c) on the Mac: the
 * pure rules with fakes, the size walk and the WER walk on real files in a scratch folder through
 * plat_mac.c, CriticalWork with real threads, and a seeded fuzz of the text parsers (no crash,
 * and each one's own property), under run.sh's sanitizers. Its own binary, beside winc-test.
 *
 *   winc-app-test SCRATCH [FUZZ_ITERATIONS [WIN_C_SRC]]     SCRATCH: an empty folder */
#include "../lifecycle.h"
#include "core_win.h"   /* the core DLL exports the one-copy answers under its names */
#include "../report.h"
#include "../shell.h"
#include "../unzip.h"
#include "../stub.h"
#include "../volumes.h"
#include "../watch.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* one minimum for the app (lifecycle.h), the watcher (watch.h) and the stub (stub.h) */
_Static_assert(LIFE_MIN_BUILD == WATCH_MIN_BUILD && LIFE_MIN_BUILD == STUB_MIN_BUILD, "the Windows build minimums differ");
_Static_assert(LIFE_MIN_NET_RELEASE == WATCH_MIN_NET_RELEASE, "the .NET Framework minimums differ");

static int checks, failures;

_Static_assert(CORE_WIN_ONE_HELD == LIFE_ONE_HELD && CORE_WIN_ONE_HANDED == LIFE_ONE_HANDED && CORE_WIN_ONE_BUSY == LIFE_ONE_BUSY
                   && CORE_WIN_ONE_FAILED == LIFE_ONE_FAILED,
               "core_win.h's one-copy answers are life_one_copy's");
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

static const char G[] = "\\\\?\\Volume{0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0}\\";   /* a volume GUID path's root */

/* ------------------------------------------------------------------ volumes: the external rule */

static vol_facts base(int bus) {
    vol_facts f;
    memset(&f, 0, sizeof f);
    strcpy(f.guid, G);
    strcpy(f.mount, "E:\\");
    strcpy(f.fstype, "exfat");
    f.drive_type = VOL_DRIVE_FIXED;
    f.disks = 1;
    f.bus = bus;
    f.hotplug = 0;
    return f;
}

static void node(vol_facts *f, const char *enumerator, int removable, int tunnel) {
    vol_node *v = &f->node[f->nodes++];
    snprintf(v->enumerator, sizeof v->enumerator, "%s", enumerator);
    v->removable = removable;
    v->tunnel = tunnel;
}

static void test_external(void) {
    CHECK(vol_tunnel_text("PCI\\VEN_8086&DEV_15EF&SUBSYS_00000000&REV_06 Intel(R) JHL7540 Thunderbolt 3 Bridge"), "Thunderbolt");
    CHECK(vol_tunnel_text("USB4(TM) Host Router"), "USB4");
    CHECK(vol_tunnel_text("PCI\\VEN_8086&DEV_9A1B&CC_0C0340 PCI\\CC_0C03"), "the USB4 host interface's class");
    CHECK(vol_tunnel_text("pci\\cc_0c0340"), "case");
    CHECK(!vol_tunnel_text("PCI\\VEN_8086&DEV_A352&CC_010601 Standard SATA AHCI Controller storahci"), "AHCI");
    CHECK(!vol_tunnel_text("USB\\VID_0781&PID_5581 USBSTOR"), "a USB stick is no tunnel");
    CHECK(!vol_tunnel_text("") && !vol_tunnel_text(NULL), "empty");

    vol_facts f = base(VOL_BUS_USB);
    f.drive_type = VOL_DRIVE_REMOVABLE;
    CHECK(vol_external(&f), "a USB stick");
    f = base(VOL_BUS_USB);
    CHECK(vol_external(&f), "a USB SSD reporting DRIVE_FIXED");
    f = base(VOL_BUS_1394);
    CHECK(vol_external(&f), "1394");
    f = base(VOL_BUS_SD);
    CHECK(vol_external(&f), "an SD card");
    f = base(VOL_BUS_MMC);
    CHECK(vol_external(&f), "MMC");

    /* I89: a Thunderbolt NVMe enclosure: BusTypeNvme, its controller removable under a TB bridge */
    f = base(VOL_BUS_NVME);
    node(&f, "SCSI", 0, 0);
    node(&f, "PCI", 1, 0);      /* the NVMe controller, in a hot-plug slot */
    node(&f, "PCI", 1, 0);      /* the enclosure's bridge */
    node(&f, "PCI", 0, 1);      /* the host's Thunderbolt downstream port */
    node(&f, "PCI", 0, 0);
    node(&f, "ACPI", 0, 0);
    CHECK(vol_external(&f), "a Thunderbolt NVMe enclosure");
    CHECK(vol_refusal(&f) == VOL_OK, "it may hold the folder: %d", vol_refusal(&f));
    f = base(VOL_BUS_NVME);
    node(&f, "SCSI", 0, 0);
    node(&f, "PCI", 1, 1);
    CHECK(vol_external(&f), "the removable node itself a USB4 one");
    /* J28: SATA on a hot-plug port: removable (or DeviceHotplug), no USB or tunnel node above */
    f = base(VOL_BUS_SATA);
    f.hotplug = 1;
    node(&f, "SCSI", 0, 0);
    node(&f, "PCI", 0, 0);      /* the AHCI controller */
    node(&f, "ACPI", 0, 0);
    CHECK(!vol_external(&f), "an internal SATA disk on a hot-plug port");
    CHECK(vol_refusal(&f) == VOL_REFUSE_INTERNAL, "refused as internal");
    f = base(VOL_BUS_SATA);
    node(&f, "SCSI", 1, 0);
    node(&f, "PCI", 0, 0);
    CHECK(!vol_external(&f), "the disk node removable (AHCI hot-plug), still internal");
    f = base(VOL_BUS_RAID);
    node(&f, "SCSI", 1, 0);
    node(&f, "PCI", 1, 0);
    node(&f, "ACPI", 0, 0);
    CHECK(!vol_external(&f), "RAID behind a removable PCI node, no tunnel");
    f = base(VOL_BUS_NVME);
    node(&f, "SCSI", 0, 0);
    node(&f, "PCI", 0, 0);
    node(&f, "PCI", 0, 1);
    CHECK(!vol_external(&f), "an internal NVMe: nothing removable (a tunnel node alone isn't enough)");
    f = base(VOL_BUS_NVME);
    node(&f, "SCSI", 0, 1);     /* a tunnel below the first removable node doesn't count */
    node(&f, "PCI", 0, 0);
    node(&f, "PCI", 1, 0);
    node(&f, "ACPI", 0, 0);
    CHECK(!vol_external(&f), "a tunnel only below the removable node");
    f = base(-1);
    node(&f, "USBSTOR", 0, 0);
    node(&f, "USB", 1, 0);
    CHECK(vol_external(&f), "a USB disk whose bus couldn't be read: its removable USB node");
    f = base(VOL_BUS_SATA);
    f.hotplug = 1;
    node(&f, "UASPSTOR", 0, 0);
    CHECK(vol_external(&f), "a UAS disk, hot-plug at the disk itself");
    f = base(VOL_BUS_SATA);
    f.hotplug = 1;
    CHECK(!vol_external(&f), "hot-plug with no device tree: unknown, internal");

    f = base(VOL_BUS_USB);
    f.system = 1;
    CHECK(!vol_external(&f) && vol_refusal(&f) == VOL_REFUSE_INTERNAL, "the system volume, even on USB (Windows To Go)");
    f = base(VOL_BUS_USB);
    f.drive_type = VOL_DRIVE_REMOTE;
    CHECK(!vol_external(&f) && vol_refusal(&f) == VOL_REFUSE_NETWORK, "a network drive");
    f = base(VOL_BUS_USB);
    f.disks = 2;
    CHECK(!vol_external(&f), "a volume spanning two disks");
    f.disks = -1;
    CHECK(!vol_external(&f), "disks unknown");
    f.disks = 0;
    CHECK(!vol_external(&f), "no disk (a RAM disk)");
    CHECK(!vol_external(NULL), "NULL");

    CHECK(vol_fs_allowed("ntfs") && vol_fs_allowed("exfat") && vol_fs_allowed("fat32"), "NTFS, exFAT, FAT32");
    CHECK(!vol_fs_allowed("fat") && !vol_fs_allowed("refs") && !vol_fs_allowed("udf") && !vol_fs_allowed("NTFS") && !vol_fs_allowed("")
              && !vol_fs_allowed(NULL),
          "the others (lower case only: the reader lowers it)");

    /* locationRefusalFor's order */
    CHECK(vol_refusal(NULL) == VOL_REFUSE_UNREADABLE, "unreadable");
    f = base(VOL_BUS_USB);
    f.drive_type = VOL_DRIVE_REMOTE;
    f.readonly = 1;
    strcpy(f.fstype, "smb");
    CHECK(vol_refusal(&f) == VOL_REFUSE_NETWORK, "network before the rest");
    f = base(VOL_BUS_SATA);
    f.readonly = 1;
    strcpy(f.fstype, "refs");
    CHECK(vol_refusal(&f) == VOL_REFUSE_INTERNAL, "internal before the file system and read-only");
    f = base(VOL_BUS_USB);
    f.readonly = 1;
    strcpy(f.fstype, "udf");
    CHECK(vol_refusal(&f) == VOL_REFUSE_FILE_SYSTEM, "the file system before read-only");
    f = base(VOL_BUS_USB);
    f.readonly = 1;
    strcpy(f.fstype, "ntfs");
    CHECK(vol_refusal(&f) == VOL_REFUSE_READ_ONLY, "read-only");
    f = base(VOL_BUS_USB);
    strcpy(f.fstype, "fat32");
    CHECK(vol_refusal(&f) == VOL_OK, "fine");

    char line[512];
    f = base(VOL_BUS_NVME);
    node(&f, "SCSI", 0, 0);
    node(&f, "PCI", 1, 0);
    node(&f, "PCI", 0, 1);
    vol_facts_line(&f, line, sizeof line);
    CHECK(strstr(line, "nodes=SCSI>PCI(r)>PCI(tb) external=1 refusal=0") && strstr(line, "bus=17") && strstr(line, "fs=exfat"), "%s", line);
    CHECK(!strstr(line, "Volume{") && !strstr(line, "E:"), "no GUID, no mount in the line: %s", line);
    f = base(VOL_BUS_SATA);
    vol_facts_line(&f, line, sizeof line);
    CHECK(strstr(line, "nodes=- external=0 refusal=3"), "%s", line);
    vol_facts_line(NULL, line, sizeof line);
    CHECK(!strcmp(line, "volume: unreadable"), "%s", line);
    char tiny[12];
    vol_facts_line(&f, tiny, sizeof tiny);
    CHECK(strlen(tiny) < sizeof tiny, "cut short and terminated");
}

/* ------------------------------------------------------------------ volumes: paths and events */

static int display_mounted(const char *guid_path, char *out, size_t n) {
    return vol_join_mount(guid_path, "E:\\", out, n);
}

static int display_unmounted(const char *guid_path, char *out, size_t n) {
    (void)guid_path;
    (void)out;
    (void)n;
    return ENOENT;
}

/* Lane C (F7): the uninstall shows a mount path if available, or the full saved path */
static void test_uninstall_path(void) {
    char dir[256], out[256];
    snprintf(dir, sizeof dir, "%sMusic\\RB Stems Plus Cache.noindex", G);
    const char *shown = vol_uninstall_path(dir, display_mounted, out, sizeof out);
    CHECK(!strcmp(shown, "E:\\Music\\RB Stems Plus Cache.noindex"), "mounted: %s", shown);
    shown = vol_uninstall_path(dir, display_unmounted, out, sizeof out);
    CHECK(!strcmp(shown, dir), "unmounted: %s", shown);
}

static void test_paths(void) {
    char p[256], out[256];
    CHECK(vol_guid_root(G) == VOL_GUID_LEN, "%zu", vol_guid_root(G));
    snprintf(p, sizeof p, "%sMusic\\RB Stems Plus Cache", G);
    CHECK(vol_guid_root(p) == VOL_GUID_LEN, "with a rest");
    CHECK(!vol_guid_root("\\\\?\\Volume{0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0}"), "without its last separator");
    CHECK(!vol_guid_root("\\\\?\\Volume{0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1fg}\\"), "not hex");
    CHECK(!vol_guid_root("E:\\") && !vol_guid_root("\\\\?\\E:\\") && !vol_guid_root("") && !vol_guid_root(NULL), "other paths");

    static const char both[] = "C:\\mnt\\gls\\\0E:\\\0F:\\\0";
    CHECK(!vol_pick_mount(both, sizeof both, out, sizeof out) && !strcmp(out, "E:\\"), "a drive letter first: %s", out);
    static const char folder[] = "C:\\mnt\\gls\\\0";
    CHECK(!vol_pick_mount(folder, sizeof folder, out, sizeof out) && !strcmp(out, "C:\\mnt\\gls\\"), "a folder mount: %s", out);
    static const char none[] = "\0";
    CHECK(vol_pick_mount(none, sizeof none, out, sizeof out) == -1, "mounted nowhere");
    static const char junk[] = {'E', ':', '\\'};
    CHECK(vol_pick_mount(junk, sizeof junk, out, sizeof out) == -1, "unterminated");
    CHECK(vol_pick_mount(both, sizeof both, out, 3) == -1, "doesn't fit");
    static const char lower[] = "e:\\\0";
    CHECK(!vol_pick_mount(lower, sizeof lower, out, sizeof out) && !strcmp(out, "e:\\"), "as listed");

    CHECK(!vol_join_mount(p, "E:\\", out, sizeof out) && !strcmp(out, "E:\\Music\\RB Stems Plus Cache"), "%s", out);
    CHECK(!vol_join_mount(G, "E:\\", out, sizeof out) && !strcmp(out, "E:\\"), "the root");
    CHECK(!vol_join_mount(p, "C:\\mnt\\gls\\", out, sizeof out) && !strcmp(out, "C:\\mnt\\gls\\Music\\RB Stems Plus Cache"), "%s", out);
    CHECK(vol_join_mount(p, "E:", out, sizeof out) == -1, "a mount without its separator");
    CHECK(vol_join_mount("E:\\Music", "E:\\", out, sizeof out) == -1, "not a GUID path");
    CHECK(vol_join_mount(p, "E:\\", out, 8) == -1, "doesn't fit");

    CHECK(!vol_plain_dos("\\\\?\\E:\\Music\\X", out, sizeof out) && !strcmp(out, "E:\\Music\\X"), "%s", out);
    CHECK(!vol_plain_dos("E:\\Music\\X", out, sizeof out) && !strcmp(out, "E:\\Music\\X"), "already plain");
    CHECK(vol_plain_dos("\\\\?\\UNC\\server\\share\\x", out, sizeof out) == -1, "UNC");
    CHECK(vol_plain_dos(p, out, sizeof out) == -1, "a GUID path");
    CHECK(vol_plain_dos("\\\\.\\PhysicalDrive1", out, sizeof out) == -1, "a device");
    CHECK(vol_plain_dos("\\\\?\\E:\\Music\\X", out, 5) == -1, "doesn't fit");

    /* network paths (Q2): the network's refusal, not the internal one */
    CHECK(vol_path_remote("\\\\nas\\music\\RB Stems Plus Cache") && vol_path_remote("//nas/music"), "a UNC path");
    CHECK(vol_path_remote("\\\\?\\UNC\\nas\\music\\x") && vol_path_remote("\\\\?\\unc\\nas\\music"), "a long UNC path");
    CHECK(!vol_path_remote("\\\\?\\Z:\\music") && !vol_path_remote(G) && !vol_path_remote("\\\\.\\PhysicalDrive1")
              && !vol_path_remote("\\\\?\\Volume{0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0}\\x"),
          "a drive, a volume, a device");
    CHECK(!vol_path_remote("Z:\\music") && !vol_path_remote("\\music") && !vol_path_remote("\\\\") && !vol_path_remote("\\\\\\x")
              && !vol_path_remote("") && !vol_path_remote(NULL),
          "a drive path (a mapped letter is told by its drive type), rooted, empty");
    {
        vol_facts r;
        memset(&r, 0x5a, sizeof r);
        vol_remote_facts(&r);
        CHECK(r.drive_type == VOL_DRIVE_REMOTE && r.disks == -1 && r.bus == -1 && r.hotplug == -1 && r.nodes == 0 && !r.guid[0] && !r.mount[0]
                  && !r.fstype[0] && !r.system && !r.readonly,
              "the remote facts");
        CHECK(vol_refusal(&r) == VOL_REFUSE_NETWORK && !vol_external(&r), "refused as a network drive");
    }

    /* device events: the core's "mount" and "unmount" (events.def) */
    char ev[512];
    int n = vol_device_events(VOL_DBT_ARRIVAL, VOL_DBT_VOLUME, (1u << 4) | (1u << 5), NULL, ev, sizeof ev);
    CHECK(n == 2 && !strcmp(ev, "{\"ev\":\"mount\",\"path\":\"E:\\\\\"}\n{\"ev\":\"mount\",\"path\":\"F:\\\\\"}\n"), "%d %s", n, ev);
    n = vol_device_events(VOL_DBT_REMOVE_COMPLETE, VOL_DBT_VOLUME, 1u << 25, NULL, ev, sizeof ev);
    CHECK(n == 1 && !strcmp(ev, "{\"ev\":\"unmount\",\"path\":\"Z:\\\\\"}\n"), "%d %s", n, ev);
    n = vol_device_events(VOL_DBT_ARRIVAL, VOL_DBT_INTERFACE, 0, "\\\\?\\STORAGE#Volume#{x}#{53f5630d-b6bf-11d0-94f2-00a0c91efb8b}", ev,
                          sizeof ev);
    CHECK(n == 1 && !strcmp(ev, "{\"ev\":\"mount\",\"path\":\"\\\\\\\\?\\\\STORAGE#Volume#{x}#{53f5630d-b6bf-11d0-94f2-00a0c91efb8b}\"}\n"),
          "%d %s", n, ev);
    n = vol_device_events(VOL_DBT_ARRIVAL, VOL_DBT_INTERFACE, 0, "a\"b\x01", ev, sizeof ev);
    CHECK(n == 1 && !strcmp(ev, "{\"ev\":\"mount\",\"path\":\"a\\\"b\\u0001\"}\n"), "escaped: %s", ev);
    CHECK(vol_device_events(0x8001, VOL_DBT_VOLUME, 0x10, NULL, ev, sizeof ev) == 0 && !ev[0], "a query, not a change");
    CHECK(vol_device_events(VOL_DBT_ARRIVAL, 3, 0x10, NULL, ev, sizeof ev) == 0, "a port");
    CHECK(vol_device_events(VOL_DBT_ARRIVAL, VOL_DBT_VOLUME, 0, NULL, ev, sizeof ev) == 0, "no letter");
    CHECK(vol_device_events(VOL_DBT_ARRIVAL, VOL_DBT_INTERFACE, 0, "", ev, sizeof ev) == 0, "no name");
    CHECK(vol_device_events(VOL_DBT_ARRIVAL, VOL_DBT_VOLUME, 0x3FFFFFF, NULL, ev, 64) == -1 && !ev[0], "doesn't fit");
    char exact[64];
    n = vol_device_events(VOL_DBT_ARRIVAL, VOL_DBT_VOLUME, 1u << 4, NULL, exact, 30);
    CHECK(n == 1 && strlen(exact) == 29, "fits exactly: %d %zu", n, strlen(exact));
    CHECK(vol_device_events(VOL_DBT_ARRIVAL, VOL_DBT_VOLUME, 1u << 4, NULL, exact, 29) == -1, "one byte short");
}

/* ------------------------------------------------------------------ volumes: size on disk */

static const char K1[] = "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9";   /* a model key */
static const char E1[] = "0aff00112233445566778899aabbccddeeff00112233445566778899aabbccdd";   /* an entry key, prefix 0a */

static void spit(const char *path, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "can't write %s\n", path); abort(); }
    for (size_t i = 0; i < n; i++) fputc((int)(i & 0xff), f);
    fclose(f);
}

static int64_t blocks(const char *path) {
    struct stat st;
    return lstat(path, &st) ? -1 : (int64_t)st.st_blocks * 512;
}

static void test_size(const char *scratch) {
    char e[65], name[1024], p[1024], top[1024], model[1024], pre[1024];
    snprintf(e, sizeof e, "%s", E1);
    CHECK(vol_counts_in_entry_size(strcat(strcpy(name, e), ".flac"), "0a"), "flac");
    CHECK(vol_counts_in_entry_size(strcat(strcpy(name, e), ".desc"), "0a"), "desc");
    snprintf(name, sizeof name, "._%s.flac", e);
    CHECK(vol_counts_in_entry_size(name, "0a"), "._ flac");
    snprintf(name, sizeof name, "._%s.desc", e);
    CHECK(vol_counts_in_entry_size(name, "0a"), "._ desc");
    snprintf(name, sizeof name, "%s.flac", e);
    CHECK(!vol_counts_in_entry_size(name, "0b"), "another prefix");
    snprintf(name, sizeof name, "._._%s.flac", e);
    CHECK(!vol_counts_in_entry_size(name, "0a"), "two companions' prefixes");
    snprintf(name, sizeof name, "%s.FLAC", e);
    CHECK(!vol_counts_in_entry_size(name, "0a"), "upper-case suffix");
    snprintf(name, sizeof name, "0AFF%s.flac", e + 4);
    CHECK(!vol_counts_in_entry_size(name, "0a"), "upper-case hex");
    snprintf(name, sizeof name, "%.63s.flac", e);
    CHECK(!vol_counts_in_entry_size(name, "0a"), "63 digits");
    snprintf(name, sizeof name, "%s0.flac", e);
    CHECK(!vol_counts_in_entry_size(name, "0a"), "65 digits");
    snprintf(name, sizeof name, "%s.flac.tmp", e);
    CHECK(!vol_counts_in_entry_size(name, "0a"), "a temp file");
    CHECK(!vol_counts_in_entry_size("", "0a") && !vol_counts_in_entry_size("._", "0a"), "empty");

    snprintf(top, sizeof top, "%s/RB Stems Plus Cache", scratch);
    CHECK(vol_entries_size(top) == 0, "not there: 0");
    mkdir(top, 0755);
    CHECK(vol_entries_size(top) == 0, "empty: 0");
    snprintf(model, sizeof model, "%s/%s", top, K1);
    mkdir(model, 0755);
    snprintf(pre, sizeof pre, "%s/0a", model);
    mkdir(pre, 0755);
    int64_t want = 0;
    snprintf(p, sizeof p, "%s/%s.flac", pre, e);
    spit(p, 100000);
    want += blocks(p);
    snprintf(p, sizeof p, "%s/%s.desc", pre, e);
    spit(p, 300);
    want += blocks(p);
    snprintf(p, sizeof p, "%s/._%s.flac", pre, e);
    spit(p, 4096);
    want += blocks(p);
    snprintf(p, sizeof p, "%s/._%s.desc", pre, e);
    spit(p, 10);
    want += blocks(p);
    /* what doesn't count */
    snprintf(p, sizeof p, "%s/%s.flac.rbsp-tmp", pre, e);
    spit(p, 50000);
    snprintf(p, sizeof p, "%s/marker", top);
    spit(p, 40);
    snprintf(p, sizeof p, "%s/ff%s.flac", pre, e + 2);
    spit(p, 50000);                                        /* in the wrong prefix folder */
    snprintf(p, sizeof p, "%s/0b", model);
    mkdir(p, 0755);
    snprintf(name, sizeof name, "%s/0b%s.flac", p, e + 2);
    char link[1024];
    snprintf(link, sizeof link, "%s/%s.flac", pre, "0aaa00112233445566778899aabbccddeeff00112233445566778899aabbccdd");
    CHECK(symlink(name, link) == 0, "a link named like an entry");
    spit(name, 70000);
    want += blocks(name);                                  /* 0b/0b...flac counts */
    snprintf(p, sizeof p, "%s/0A", model);
    mkdir(p, 0755);
    snprintf(name, sizeof name, "%s/0Acc%s.flac", p, e + 4);   /* (a case-blind volume: the same folder as 0a) */
    spit(name, 60000);                                     /* an upper-case prefix folder: no */
    snprintf(p, sizeof p, "%s/%.63s", top, K1);
    mkdir(p, 0755);                                        /* a short model folder: no */
    snprintf(p, sizeof p, "%s/%s", top, "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f8");
    spit(p, 9000);                                         /* a file named like a model folder: no */
    snprintf(name, sizeof name, "%s/%s.desc", pre, "0abb00112233445566778899aabbccddeeff00112233445566778899aabbccdd");
    mkdir(name, 0755);                                     /* a folder named like an entry: no */
    int64_t got = vol_entries_size(top);
    CHECK(got == want && want > 100000, "size %lld, want %lld", (long long)got, (long long)want);

    snprintf(p, sizeof p, "%s/a-file", scratch);
    spit(p, 10);
    CHECK(vol_entries_size(p) == -1, "a file: can't be read");
    snprintf(link, sizeof link, "%s/linked", scratch);
    CHECK(symlink(top, link) == 0 && vol_entries_size(link) == -1, "a link to the folder: -1");
    CHECK(chmod(pre, 0000) == 0, "make one prefix folder unreadable");
    if (geteuid() != 0) CHECK(vol_entries_size(top) == -1, "an unreadable prefix makes the size unknown even with readable siblings");
    CHECK(chmod(pre, 0755) == 0, "restore prefix permissions");
    CHECK(vol_entries_size(top) == want, "again");
}

/* ------------------------------------------------------------------ lifecycle: one copy */

typedef struct {
    int create;               /* what create answers */
    int owned_after;          /* wait answers OWNED from this call on (0: never) */
    int wait_fail_at;         /* wait answers FAILED at this call (0: never) */
    int window_after;         /* find answers the pid from this call on (0: never) */
    int hand_off_fails;       /* hand_off fails this many times first */
    int ack;                  /* hand_off as os_hand_off decides it: a send that went through, answered 0 (not 'RBSP') */
    int waits, finds, hands;
    uint32_t handed_pid, last_slice;
    uint64_t now;
} one_fake;

static int f_create(void *c) { return ((one_fake *)c)->create; }
static int f_wait(void *c, uint32_t ms) {
    one_fake *f = c;
    f->waits++;
    f->last_slice = ms;
    f->now += ms;
    if (f->wait_fail_at && f->waits >= f->wait_fail_at) return LIFE_MUTEX_FAILED;
    if (f->owned_after && f->waits >= f->owned_after) return LIFE_MUTEX_OWNED;
    return LIFE_MUTEX_TIMEOUT;
}
static uint32_t f_find(void *c) {
    one_fake *f = c;
    f->finds++;
    return f->window_after && f->finds >= f->window_after ? 4242 : 0;
}
static int f_hand(void *c, uint32_t pid) {
    one_fake *f = c;
    f->hands++;
    f->handed_pid = pid;
    if (f->ack) return life_one_acked(1, 0) ? 0 : -1;   /* accepted, not acknowledged */
    return f->hand_off_fails-- > 0 ? -1 : 0;
}
static uint64_t f_now(void *c) { return ((one_fake *)c)->now; }

static one_fake fake(int create, int owned_after, int wait_fail_at, int window_after, int hand_off_fails) {
    one_fake f;
    memset(&f, 0, sizeof f);
    f.create = create;
    f.owned_after = owned_after;
    f.wait_fail_at = wait_fail_at;
    f.window_after = window_after;
    f.hand_off_fails = hand_off_fails;
    return f;
}

static int one(one_fake *f, uint32_t wait) {
    life_one_ops ops = {f, f_create, f_wait, f_find, f_hand, f_now};
    return life_one_copy(&ops, wait);
}

static void test_one_copy(void) {
    one_fake f = fake(LIFE_MUTEX_OWNED, 0, 0, 0, 0);
    CHECK(one(&f, 10000) == LIFE_ONE_HELD && f.finds == 0 && f.waits == 0, "the first copy");
    f = fake(LIFE_MUTEX_FAILED, 0, 0, 0, 0);
    CHECK(one(&f, 10000) == LIFE_ONE_FAILED, "the mutex refused");
    f = fake(LIFE_MUTEX_EXISTS, 0, 0, 1, 0);
    CHECK(one(&f, 10000) == LIFE_ONE_HANDED && f.handed_pid == 4242 && f.waits == 0, "a copy open: signalled at once");
    f = fake(LIFE_MUTEX_EXISTS, 0, 0, 4, 0);
    CHECK(one(&f, 10000) == LIFE_ONE_HANDED && f.waits == 3, "its window appears a moment later: %d", f.waits);
    f = fake(LIFE_MUTEX_EXISTS, 3, 0, 0, 0);
    CHECK(one(&f, 10000) == LIFE_ONE_HELD && f.waits == 3, "a copy quitting (no window): its mutex comes free");
    f = fake(LIFE_MUTEX_EXISTS, 0, 0, 0, 0);
    CHECK(one(&f, 10000) == LIFE_ONE_BUSY && f.now == 10000 && f.waits == 100, "held, no window, 10 s: busy (%d waits)", f.waits);
    f = fake(LIFE_MUTEX_EXISTS, 0, 0, 0, 0);
    CHECK(one(&f, 250) == LIFE_ONE_BUSY && f.waits == 3 && f.last_slice == 50, "the last slice is what's left: %u", f.last_slice);
    f = fake(LIFE_MUTEX_EXISTS, 0, 0, 0, 0);
    CHECK(one(&f, 0) == LIFE_ONE_BUSY && f.finds == 1 && f.waits == 0, "no wait: one look");
    f = fake(LIFE_MUTEX_EXISTS, 0, 2, 0, 0);
    CHECK(one(&f, 10000) == LIFE_ONE_FAILED, "the wait fails");
    f = fake(LIFE_MUTEX_EXISTS, 5, 0, 1, 1000);
    CHECK(one(&f, 10000) == LIFE_ONE_HELD && f.hands == 5, "its window going away (the signal fails), then its mutex free");
    /* Q3 (WC5): a window that accepts the signal without acknowledging it is no hand-off */
    CHECK(life_one_acked(1, LIFE_ONE_ACK) && !life_one_acked(1, 0) && !life_one_acked(1, LIFE_ONE_ACK + 1) && !life_one_acked(0, LIFE_ONE_ACK),
          "handed only on a send that went through and answered 'RBSP'");
    f = fake(LIFE_MUTEX_EXISTS, 0, 0, 1, 1000);
    f.ack = 1;
    CHECK(one(&f, 10000) == LIFE_ONE_BUSY && f.hands >= 2 && f.handed_pid == 4242, "accepted, not acknowledged (a copy that is quitting): never handed, busy after the wait (%d sends)", f.hands);
    f = fake(LIFE_MUTEX_EXISTS, 4, 0, 1, 1000);
    f.ack = 1;
    CHECK(one(&f, 10000) == LIFE_ONE_HELD && f.hands == 4, "accepted, not acknowledged, then its mutex free: this start runs");
}

/* ------------------------------------------------------------------ lifecycle: the rest */

static void test_lifecycle(void) {
    CHECK(life_os_verdict(10, 22621, 1, 533320) == LIFE_OS_OK, "22H2 with 4.8.1");
    CHECK(life_os_verdict(10, 26100, 1, 533325) == LIFE_OS_OK, "24H2");
    CHECK(life_os_verdict(10, 22620, 1, 533320) == LIFE_OS_WINDOWS_OLD, "one build short");
    CHECK(life_os_verdict(10, 19045, 0, -1) == LIFE_OS_WINDOWS_OLD, "Windows 10");
    CHECK(life_os_verdict(6, 99999, 1, 600000) == LIFE_OS_WINDOWS_OLD, "6.x");
    CHECK(life_os_verdict(0, 0, 0, -1) == LIFE_OS_WINDOWS_OLD, "unknown: refused");
    CHECK(life_os_verdict(11, 1, 1, 533320) == LIFE_OS_OK, "a later major");
    CHECK(life_os_verdict(10, 26100, 1, 533319) == LIFE_OS_NET_OLD, "4.8");
    CHECK(life_os_verdict(10, 26100, 1, -1) == LIFE_OS_NET_OLD, ".NET missing");
    CHECK(life_os_verdict(10, 26100, 0, -1) == LIFE_OS_OK, "net=0 (the stub): .NET not asked");
    CHECK(life_os_verdict(10, 22000, 1, -1) == LIFE_OS_WINDOWS_OLD, "Windows first");

    CHECK(life_token_verdict(3, 0, 0) == LIFE_TOKEN_OK, "an admin's normal start");
    CHECK(life_token_verdict(1, 0, 0) == LIFE_TOKEN_OK, "a standard user");
    CHECK(life_token_verdict(2, 1, 0) == LIFE_TOKEN_ELEVATED, "Run as administrator");
    CHECK(life_token_verdict(1, 1, 0) == LIFE_TOKEN_UAC_OFF, "UAC off");
    CHECK(life_token_verdict(1, 1, 1) == LIFE_TOKEN_SERVICE && life_token_verdict(3, 0, 1) == LIFE_TOKEN_SERVICE, "a service account");
    CHECK(life_token_verdict(1, -1, 0) == LIFE_TOKEN_UNKNOWN && life_token_verdict(0, 0, 0) == LIFE_TOKEN_UNKNOWN
              && life_token_verdict(7, 0, 0) == LIFE_TOKEN_UNKNOWN,
          "unknown");

    static const char stub[] = "C:\\Users\\Zoë\\AppData\\Local\\Programs\\RB Stems Plus\\RB Stems Plus.exe";
    char v[512];
    CHECK(!life_run_value(stub, v, sizeof v) && !strcmp(v, "\"C:\\Users\\Zoë\\AppData\\Local\\Programs\\RB Stems Plus\\RB Stems Plus.exe\" --watcher"),
          "%s", v);
    CHECK(life_run_value("C:\\a\"b.exe", v, sizeof v) == -1 && life_run_value("", v, sizeof v) == -1, "a quote, empty");
    CHECK(life_run_value(stub, v, 20) == -1, "doesn't fit");
    CHECK(!life_run_value(stub, v, sizeof v) && life_run_names(v, stub), "its own value");
    CHECK(life_run_names("  \"c:/users/Zoë/appdata/local/programs/rb stems plus/RB STEMS PLUS.EXE\"\t--watcher  ", stub),
          "ASCII case, '/' and spaces");
    CHECK(!life_run_names("\"C:\\Users\\ZOË\\AppData\\Local\\Programs\\RB Stems Plus\\RB Stems Plus.exe\" --watcher", stub),
          "a non-ASCII letter's case differs: repaired, harmlessly");
    CHECK(life_run_names("C:\\x\\stub.exe --watcher", "C:\\x\\stub.exe"), "unquoted, no space");
    CHECK(!life_run_names("C:\\Users\\Zoë\\AppData\\Local\\Programs\\RB Stems Plus\\RB Stems Plus.exe --watcher", stub),
          "unquoted with spaces: Windows would run C:\\Users\\Zoë\\...\\RB");
    CHECK(!life_run_names("\"C:\\x\\stub.exe\" --watcher --other", "C:\\x\\stub.exe"), "another argument");
    CHECK(!life_run_names("\"C:\\x\\stub.exe\"", "C:\\x\\stub.exe") && !life_run_names("\"C:\\x\\stub.exe\" ", "C:\\x\\stub.exe"), "none");
    CHECK(!life_run_names("\"C:\\x\\stub.exe\" --watcherx", "C:\\x\\stub.exe"), "--watcherx");
    CHECK(!life_run_names("\"C:\\x\\stub.exe\"--watcher", "C:\\x\\stub.exe"), "no space");
    CHECK(!life_run_names("\"C:\\x\\stub.ex\" --watcher", "C:\\x\\stub.exe") && !life_run_names("\"C:\\x\\stub.exe2\" --watcher", "C:\\x\\stub.exe"),
          "another path");
    CHECK(!life_run_names("\"C:\\x\\stub.exe --watcher", "C:\\x\\stub.exe"), "an open quote");
    CHECK(!life_run_names("\"\" --watcher", "") && !life_run_names("", "C:\\x\\stub.exe"), "empty");

    static const unsigned char on[12] = {0x02}, on6[12] = {0x06}, off[12] = {0x03, 0, 0, 0, 0x10, 0x20}, off7[1] = {0x07};
    CHECK(!life_approved_off(on, sizeof on) && !life_approved_off(on6, sizeof on6), "on");
    CHECK(life_approved_off(off, sizeof off) && life_approved_off(off7, 1), "off");
    CHECK(!life_approved_off(off, 0) && !life_approved_off(NULL, 12), "empty");

    CHECK(!life_uninstall_string(stub, v, sizeof v) && strstr(v, "RB Stems Plus.exe\" --uninstall") && v[0] == '"', "%s", v);
    CHECK(life_uninstall_string("a\"b", v, sizeof v) == -1, "a quote");
    CHECK(life_size_kb(0) == 0 && life_size_kb(1) == 1 && life_size_kb(1024) == 1 && life_size_kb(1025) == 2
              && life_size_kb(6800000) == 6641 && life_size_kb(UINT64_MAX) == 0xFFFFFFFFu,
          "KB");
}

/* ------------------------------------------------------------------ report */

static void test_report_pure(void) {
    char q[200];
    CHECK(!report_event_query(30, q, sizeof q)
              && !strcmp(q, "*[System[(Level=1 or Level=2) and TimeCreated[timediff(@SystemTime) <= 1800000]]]"),
          "%s", q);
    CHECK(report_event_query(30, q, 20) == -1, "doesn't fit");

    static const char xml[] = "<Event><System><Provider Name='Application Error'/><Computer>DESKTOP-ZOE</Computer>"
                              "<Security UserID='S-1-5-21-1-2-3-1001'/></System><EventData><Data>rekordbox.exe</Data></EventData></Event>";
    CHECK(report_event_wanted(xml, sizeof xml - 1), "rekordbox");
#define WANTED(t) report_event_wanted(t, sizeof t - 1)
    /* the review's four (Q1), as report_win.c sees them: cleaned first */
    {
        char pc[] = "<Event><System><Provider Name='Application Error'/><EventID>1000</EventID><Computer>rekordbox.exe-PC</Computer>"
                    "</System><EventData><Data>chrome.exe</Data><Data>C:\\Program Files\\Chrome\\chrome.exe</Data></EventData></Event>";
        size_t l = report_event_clean(pc, strlen(pc));
        CHECK(!report_event_wanted(pc, l), "the name only in <Computer>: not wanted");
    }
    CHECK(!WANTED("<Event><System><Provider Name='Application Error'/></System><EventData><Data>Backup.exe</Data>"
                  "<Data>D:\\Music\\rekordbox\\x.db</Data></EventData></Event>"),
          "another app's Data naming a rekordbox folder: not wanted");
    CHECK(WANTED("<Event><System><Provider Name='Application Error'/><EventID>1000</EventID></System><EventData><Data>rekordbox.exe</Data>"
                 "<Data>7.2.19.342</Data><Data>C:\\Program Files\\rekordbox\\rekordbox 7.2.19\\rekordbox.exe</Data></EventData></Event>"),
          "1000 with rekordbox.exe: wanted");
    CHECK(WANTED("<Event><System><Provider Name='.NET Runtime'/><EventID>1026</EventID></System><EventData><Data>Application: RB Stems Plus.exe"
                 "&#xD;Framework Version: v4.0.30319</Data></EventData></Event>"),
          "1026 Application: RB Stems Plus.exe: wanted");
    CHECK(WANTED("<Event><EventData><Data>RBSP-WATCH-ARM64.EXE</Data></EventData></Event>"), "the watcher, case");
    CHECK(WANTED("<Event><EventData><Data>c:\\x\\rbsp-core.dll</Data></EventData></Event>"), "the core DLL");
    CHECK(WANTED("<Event><System><Provider Name=\"rbsp-watch.exe\"/></System></Event>"), "a provider named after our image");
    CHECK(!WANTED("<EventData><Data>rekordbox.exe.old</Data></EventData>"), "the image's name must end at a boundary");
    CHECK(!WANTED("<Provider Name='RB Stems Plus.exeHelper'/>"), "a provider with a longer name");
    CHECK(!WANTED("<EventData><Data>rbsp-core.dllx</Data></EventData>"), "an extension prefix");
    CHECK(WANTED("<EventData><Data>'rekordbox.exe' crashed</Data></EventData>"), "a quoted image");
    CHECK(WANTED("<EventData><Data>rbsp-core.dll crashed</Data></EventData>"), "whitespace after the image");
    CHECK(!WANTED("<Event><EventData><Data>myrekordbox.exe</Data><Data>rbsp-.exe</Data><Data>rbsp-x.ex</Data></EventData></Event>"),
          "inside a longer word, no name, no extension");
    CHECK(!WANTED("<Event><System><Provider Name='rekordbox'/></System><EventData><Data>rekordbox</Data></EventData></Event>"),
          "no image name");
    CHECK(!WANTED("<Event><EventData><Data>chrome.exe</Data></EventData></Event> rekordbox.exe"), "outside EventData");
    CHECK(!WANTED("<EventData><Data>rekordbox.ex"), "cut short");
    CHECK(!report_event_wanted("<EventData><Data>rekordbox.exe</Data></EventData>", 29), "only within n");
#undef WANTED

    char x[sizeof xml];
    memcpy(x, xml, sizeof xml);
    size_t n = report_event_clean(x, sizeof xml - 1);
    CHECK(n == strlen(x) && !strstr(x, "S-1-5-21") && !strstr(x, "DESKTOP") && strstr(x, "<Computer></Computer>") && strstr(x, "<Security/>")
              && strstr(x, "rekordbox.exe"),
          "%s", x);
    char y[] = "<Security UserID=\"S-1-5-18\" Other='1'/><Computer>A</Computer><Computer>B</Computer> UserID='x'";
    n = report_event_clean(y, strlen(y));
    CHECK(!strcmp(y, "<Security Other='1'/><Computer></Computer><Computer></Computer>"), "%s", y);
    char z[] = "<Security UserID='open";
    n = report_event_clean(z, strlen(z));
    CHECK(!strcmp(z, "<Security UserID='open"), "an open quote stays: %s", z);
    char w[] = "<Computer>no end";
    n = report_event_clean(w, strlen(w));
    CHECK(!strcmp(w, "<Computer>no end") && n == strlen(w), "an open element stays");
    char u[] = "UserID= nothing";
    report_event_clean(u, strlen(u));
    CHECK(!strcmp(u, "UserID= nothing"), "no leading space, no quote");
    char v[] = "rbsp- UserID=\"x";
    report_event_clean(v, strlen(v));
    CHECK(!strcmp(v, "rbsp- UserID=\"x"), "an open quote at the end stays: %s", v);
    char t2[] = "a UserID=\"open b UserID='S-1-5-21-9' c";
    report_event_clean(t2, strlen(t2));
    CHECK(!strcmp(t2, "a UserID=\"open b c"), "an open quote doesn't stop the next one's removal (Q9): %s", t2);

    CHECK(report_wer_wanted("AppCrash_rekordbox.exe_7c2e1f_cab_0f1e2d3c"), "rekordbox's crash");
    CHECK(report_wer_wanted("AppHang_RB Stems Plus.e_1234_abcd"), "ours, cut short");
    CHECK(report_wer_wanted("appcrash_RBSP-watch-arm6_99"), "the watcher, case");
    CHECK(!report_wer_wanted("AppCrash_chrome.exe_1"), "someone else's");
    CHECK(!report_wer_wanted("Critical_rekordbox.exe_1") && !report_wer_wanted("rekordbox") && !report_wer_wanted(""), "other kinds");

    size_t l;
    char *t;
    static const unsigned char u16[] = {0xFF, 0xFE, 'A', 0, '=', 0, 0xE9, 0, 0x3D, 0xD8, 0x00, 0xDE, 0x00, 0xD8, 'B', 0, 0, 0, 0x00, 0xDC, 'C'};
    t = report_text_utf8(u16, sizeof u16, &l);
    CHECK(t && !strcmp(t, "A=\xC3\xA9\xF0\x9F\x98\x80\xEF\xBF\xBD" "B\xEF\xBF\xBD") && l == strlen(t), "UTF-16LE: %s", t ? t : "");
    free(t);
    static const unsigned char u16n[] = {'V', 0, 'e', 0, 'r', 0, '=', 0, '1', 0};
    t = report_text_utf8(u16n, sizeof u16n, &l);
    CHECK(t && !strcmp(t, "Ver=1"), "UTF-16LE without a BOM: %s", t ? t : "");
    free(t);
    static const unsigned char u8[] = {0xEF, 0xBB, 0xBF, 'a', 0xC3, 0xA9, 0xC0, 0x80, 0xED, 0xA0, 0x80, 0xF4, 0x90, 0x80, 0x80, 0, 'z', 0xE2, 0x82};
    t = report_text_utf8(u8, sizeof u8, &l);
    CHECK(t && !strcmp(t, "a\xC3\xA9" "\xEF\xBF\xBD\xEF\xBF\xBD" "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
                         "z\xEF\xBF\xBD\xEF\xBF\xBD"),
          "UTF-8 repaired: %s", t ? t : "");
    free(t);
    t = report_text_utf8((const unsigned char *)"", 0, &l);
    CHECK(t && l == 0 && !t[0], "empty");
    free(t);
    t = report_text_utf8((const unsigned char *)"x", 1, &l);
    CHECK(t && !strcmp(t, "x"), "one byte");
    free(t);
}

static void set_mtime(const char *path, time_t t) {
    struct timeval tv[2] = {{t, 0}, {t, 0}};
    utimes(path, tv);
}

static void wer_folder(const char *root, const char *name, const unsigned char *text, size_t n, time_t when) {
    char d[1024], p[1100];
    snprintf(d, sizeof d, "%s/%s", root, name);
    mkdir(d, 0755);
    snprintf(p, sizeof p, "%s/Report.wer", d);
    FILE *f = fopen(p, "wb");
    if (!f) abort();
    fwrite(text, 1, n, f);
    fclose(f);
    set_mtime(p, when);
}

static void test_report_files(const char *scratch) {
    char a[1024], b[1024], none[1024], p[1100];
    snprintf(a, sizeof a, "%s/user-archive", scratch);
    snprintf(b, sizeof b, "%s/machine-archive", scratch);
    snprintf(none, sizeof none, "%s/missing", scratch);
    mkdir(a, 0755);
    mkdir(b, 0755);
    time_t now = time(NULL);
    static const unsigned char rep[] = {0xFF, 0xFE, 'V', 0, '=', 0, '1', 0, '\r', 0, '\n', 0, 'A', 0, 'p', 0, 'p', 0};
    wer_folder(a, "AppCrash_rekordbox.exe_1", rep, sizeof rep, now - 3600);
    wer_folder(a, "AppHang_RB Stems Plus.e_2", rep, sizeof rep, now - 7200);
    wer_folder(a, "AppCrash_chrome.exe_3", rep, sizeof rep, now - 60);           /* not ours */
    wer_folder(a, "AppCrash_rekordbox.exe_old", rep, sizeof rep, now - 10 * 86400);   /* too old */
    wer_folder(b, "AppCrash_rbsp-watch-x64._4", rep, sizeof rep, now - 600);
    wer_folder(b, "AppCrash_rekordbox.exe_5", rep, sizeof rep, now - 5000);
    wer_folder(b, "AppCrash_rekordbox.exe_6", rep, sizeof rep, now - 6000);
    wer_folder(b, "AppCrash_rekordbox.exe_7", rep, sizeof rep, now - 8000);
    /* a dump beside a report is never listed; a Report.wer that is a link isn't either */
    snprintf(p, sizeof p, "%s/AppCrash_rekordbox.exe_1/memory.hdmp", a);
    spit(p, 100);
    snprintf(p, sizeof p, "%s/AppCrash_rekordbox.exe_8", b);
    mkdir(p, 0755);
    char target[1100];
    snprintf(target, sizeof target, "%s/AppCrash_rekordbox.exe_1/Report.wer", a);
    snprintf(p, sizeof p, "%s/AppCrash_rekordbox.exe_8/Report.wer", b);
    CHECK(symlink(target, p) == 0, "a linked Report.wer");
    snprintf(p, sizeof p, "%s/AppCrash_rekordbox.exe_9", b);
    spit(p, 10);                                           /* a file, not a folder */

    const char *roots[3] = {none, a, b};
    report_wer out[REPORT_WER_MAX];
    int found = -1;
    int n = report_wer_find(roots, 3, now - REPORT_WER_DAYS * 86400, out, REPORT_WER_MAX, &found);
    CHECK(n == 5 && found == 6, "the newest 5 of 6: %d of %d", n, found);
    CHECK(n == 5 && strstr(out[0].path, "rbsp-watch") && strstr(out[1].path, "_1/Report.wer") && strstr(out[2].path, "_5/")
              && strstr(out[3].path, "_6/") && strstr(out[4].path, "_2/"),
          "newest first");
    for (int i = 1; i < n; i++) CHECK(out[i - 1].mtime >= out[i].mtime, "ordered at %d", i);
    n = report_wer_find(roots, 3, now - REPORT_WER_DAYS * 86400, out, 2, &found);
    CHECK(n == 2 && found == 6 && strstr(out[1].path, "_1/"), "max 2");
    n = report_wer_find(roots, 1, 0, out, 5, &found);
    CHECK(n == 0 && found == 0, "only the missing root");

    char *t = NULL;
    size_t l = 0;
    CHECK(!report_wer_text(target, REPORT_TEXT_MAX, &t, &l) && t && !strcmp(t, "V=1\r\nApp"), "the text: %s", t ? t : "");
    free(t);
    CHECK(!report_wer_text(target, 6, &t, &l) && t && !strcmp(t, "V="), "cut at max bytes: %s", t ? t : "");
    free(t);
    snprintf(p, sizeof p, "%s/AppCrash_rekordbox.exe_1/memory.hdmp", a);
    CHECK(report_wer_text(p, REPORT_TEXT_MAX, &t, &l) == EINVAL && !t, "never a dump");
    snprintf(p, sizeof p, "%s/AppCrash_rekordbox.exe_8/Report.wer", b);
    CHECK(report_wer_text(p, REPORT_TEXT_MAX, &t, &l) != 0 && !t, "never through a link");
    snprintf(p, sizeof p, "%s/AppCrash_x/Report.wer", none);
    CHECK(report_wer_text(p, REPORT_TEXT_MAX, &t, &l) == ENOENT, "missing");
    CHECK(report_wer_text("Report.wer", REPORT_TEXT_MAX, &t, &l) == EINVAL, "no folder");
}

/* WER's queue (reports not sent yet) is read as its archive is: same names, limits and texts */
static void test_report_queue(const char *scratch) {
    char local[1024], data[1024], roots[REPORT_WER_ROOTS][REPORT_PATH_MAX], p[1200];
    snprintf(local, sizeof local, "%s/q-local", scratch);
    snprintf(data, sizeof data, "%s/q-data", scratch);
    const char *bases[3] = {local, NULL, data};
    int nr = report_wer_root_paths(bases, 3, roots, REPORT_WER_ROOTS);
    CHECK(nr == 4, "four roots: %d", nr);
    snprintf(p, sizeof p, "%s/Microsoft/Windows/WER/ReportArchive", local);
    CHECK(nr == 4 && !strcmp(roots[0], p), "the account's archive: %s", roots[0]);
    snprintf(p, sizeof p, "%s/Microsoft/Windows/WER/ReportQueue", local);
    CHECK(nr == 4 && !strcmp(roots[1], p), "the account's queue: %s", roots[1]);
    snprintf(p, sizeof p, "%s/Microsoft/Windows/WER/ReportArchive", data);
    CHECK(nr == 4 && !strcmp(roots[2], p), "the machine's archive: %s", roots[2]);
    snprintf(p, sizeof p, "%s/Microsoft/Windows/WER/ReportQueue", data);
    CHECK(nr == 4 && !strcmp(roots[3], p), "the machine's queue: %s", roots[3]);
    char two[2][REPORT_PATH_MAX];
    CHECK(report_wer_root_paths(bases, 3, two, 2) == 2 && strstr(two[1], "ReportQueue"), "at most cap");
    const char *nob[2] = {NULL, ""};
    CHECK(report_wer_root_paths(nob, 2, roots, REPORT_WER_ROOTS) == 0, "no base: none");
    nr = report_wer_root_paths(bases, 3, roots, REPORT_WER_ROOTS);

    for (int i = 0; i < nr; i++) {   /* mkdir -p */
        snprintf(p, sizeof p, "%s", roots[i]);
        for (char *c = p + strlen(scratch) + 1; *c; c++)
            if (*c == '/') {
                *c = 0;
                mkdir(p, 0755);
                *c = '/';
            }
        mkdir(p, 0755);
    }
    time_t now = time(NULL);
    static const unsigned char rep[] = {0xFF, 0xFE, 'Q', 0, '=', 0, '1', 0};
    wer_folder(roots[1], "AppCrash_rekordbox.exe_q1", rep, sizeof rep, now - 60);        /* not sent yet */
    wer_folder(roots[1], "AppCrash_chrome.exe_q2", rep, sizeof rep, now - 30);           /* not ours */
    wer_folder(roots[1], "AppHang_rekordbox.exe_q3", rep, sizeof rep, now - 9 * 86400);  /* too old */
    wer_folder(roots[3], "AppCrash_RB Stems Plus.e_q4", rep, sizeof rep, now - 120);
    wer_folder(roots[0], "AppCrash_rekordbox.exe_a1", rep, sizeof rep, now - 90);
    wer_folder(roots[2], "AppCrash_rekordbox.exe_a2", rep, sizeof rep, now - 200);
    wer_folder(roots[2], "AppCrash_rekordbox.exe_a3", rep, sizeof rep, now - 300);
    wer_folder(roots[3], "AppCrash_rbsp-watch-arm6_q5", rep, sizeof rep, now - 400);
    snprintf(p, sizeof p, "%s/AppCrash_rekordbox.exe_q1/memory.hdmp", roots[1]);
    spit(p, 100);
    const char *r[REPORT_WER_ROOTS] = {roots[0], roots[1], roots[2], roots[3]};
    report_wer out[REPORT_WER_MAX];
    int found = -1, n = report_wer_find(r, nr, now - REPORT_WER_DAYS * 86400, out, REPORT_WER_MAX, &found);
    CHECK(n == 5 && found == 6, "the newest 5 of 6, archives and queues together: %d of %d", n, found);
    CHECK(n == 5 && strstr(out[0].path, "ReportQueue/AppCrash_rekordbox.exe_q1/Report.wer") && strstr(out[1].path, "_a1/")
              && strstr(out[2].path, "ReportQueue/AppCrash_RB Stems Plus.e_q4/") && strstr(out[3].path, "_a2/") && strstr(out[4].path, "_a3/"),
          "newest first, the queue's among the archive's");
    char *t = NULL;
    size_t l = 0;
    CHECK(n >= 1 && !report_wer_text(out[0].path, REPORT_TEXT_MAX, &t, &l) && t && !strcmp(t, "Q=1"), "a queued report's text: %s", t ? t : "");
    free(t);
    snprintf(p, sizeof p, "%s/AppCrash_rekordbox.exe_q1/memory.hdmp", roots[1]);
    t = NULL;
    CHECK(report_wer_text(p, REPORT_TEXT_MAX, &t, &l) == EINVAL && !t, "never a queued dump");
}

/* ------------------------------------------------------------------ CriticalWork */

static critical_work work;

static uint64_t ms_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static void *step(void *arg) {
    usleep((useconds_t)(uintptr_t)arg * 1000);
    critical_end(&work);
    return NULL;
}

static critical_work *other_process;
static void *process_of(void *arg) {
    (void)arg;
    other_process = critical_process();
    return NULL;
}

static void test_critical(void) {
    critical_init(&work);
    CHECK(!critical_running(&work) && critical_wait_idle(&work, 1000) == 1, "idle: at once");
    critical_begin(&work);
    critical_begin(&work);
    CHECK(critical_running(&work), "two steps");
    critical_end(&work);
    CHECK(critical_running(&work), "one left");
    uint64_t t = ms_now();
    CHECK(critical_wait_idle(&work, 80) == 0 && ms_now() - t >= 75, "times out: %llu ms", (unsigned long long)(ms_now() - t));
    pthread_t th;
    pthread_create(&th, NULL, step, (void *)(uintptr_t)50);
    t = ms_now();
    CHECK(critical_wait_idle(&work, 5000) == 1 && ms_now() - t < 2000, "returns when the step ends: %llu ms", (unsigned long long)(ms_now() - t));
    pthread_join(th, NULL);
    CHECK(!critical_running(&work), "idle");
    critical_end(&work);
    CHECK(!critical_running(&work) && critical_wait_idle(&work, 0) == 1, "an extra end doesn't go below 0");
    critical_begin(&work);
    CHECK(critical_wait_idle(&work, 0) == 0, "no wait");
    critical_end(&work);
    /* the process's one, as core_win_critical_* read it: the same from any thread, idle at first */
    critical_work *p = critical_process();
    pthread_t th2;
    pthread_create(&th2, NULL, process_of, NULL);
    pthread_join(th2, NULL);
    CHECK(p && p == critical_process() && p == other_process && p != &work, "one per process, from any thread");
    CHECK(!critical_running(p) && critical_wait_idle(p, 0) == 1, "the process's: idle at first");
    critical_begin(p);
    CHECK(critical_running(critical_process()) && !critical_running(&work), "a step counts in the process's only");
    critical_end(p);
    CHECK(!critical_running(p), "and ends");
    /* admission (WC1, with the model steps): once the quit is decided no new step starts; one begun
     * before still counts, and so does a plain begin (the worker past its prompt) */
    critical_work a;
    critical_init(&a);
    CHECK(critical_admit(&a) && critical_try_begin(&a) && critical_running(&a), "open: a step begins");
    critical_close(&a);
    CHECK(!critical_admit(&a) && !critical_try_begin(&a), "closed: no new step admitted");
    CHECK(critical_running(&a) && critical_wait_idle(&a, 0) == 0, "the step begun before the close still counts");
    critical_begin(&a);
    critical_end(&a);
    critical_end(&a);
    CHECK(!critical_running(&a) && critical_wait_idle(&a, 0) == 1, "ended: idle (the refused one never counted)");
    critical_close(&a);
    CHECK(!critical_admit(&a), "closed stays closed");
}

/* ------------------------------------------------------------------ fuzz */

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) {
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545F4914F6CDD1DULL) >> 32);
}

static int valid_utf8(const char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        int more = c < 0x80 ? 0 : c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : -1;
        if (more < 0 || n - i <= (size_t)more || c == 0) return 0;
        for (int j = 1; j <= more; j++)
            if (((unsigned char)s[i + j] & 0xC0) != 0x80) return 0;
        i += (size_t)more + 1;
    }
    return 1;
}

static void test_fuzz(long iterations) {
#define SEED(x) {x, sizeof x - 1}
    static const struct { const char *s; size_t n; } seeds[] = {
        SEED("\"C:\\x\\stub.exe\" --watcher"), SEED("<Security UserID='S-1-5-21'/><Computer>PC</Computer>"),
        SEED("AppCrash_rekordbox.exe_1"), SEED("\xFF\xFE" "A\0B\0"), SEED("\xEF\xBB\xBF" "abc\xC3\xA9"), SEED("C:\\mnt\\\0E:\\\0\0"),
        SEED("\\\\?\\Volume{0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0}\\x")};
#undef SEED
    unsigned char buf[256];
    char out[2048];
    for (long it = 0; it < iterations; it++) {
        size_t pick = rnd() % (sizeof seeds / sizeof *seeds), n = seeds[pick].n;
        memcpy(buf, seeds[pick].s, n);
        for (int k = 1 + (int)(rnd() % 6); k > 0; k--) {
            size_t at = rnd() % n;
            switch (rnd() % 4) {
            case 0: buf[at] = (unsigned char)rnd(); break;
            case 1: buf[at] = "\"\\ '=<>\0\xFF\xFE\xD8\xDC"[rnd() % 12]; break;
            case 2: if (n > 1) n--; break;
            default: if (n < sizeof buf - 1) n++; buf[n - 1] = (unsigned char)rnd(); break;
            }
        }
        buf[n] = 0;
        size_t l = 0;
        char *t = report_text_utf8(buf, n, &l);
        if (t) {
            CHECK(l == strlen(t) && valid_utf8(t, l), "report_text_utf8 gives valid UTF-8 (iteration %ld)", it);
            free(t);
        }
        char c[sizeof buf];
        memcpy(c, buf, n + 1);
        size_t cl = report_event_clean(c, strlen(c));
        CHECK(cl == strlen(c) && cl <= strlen((char *)buf), "clean shrinks (iteration %ld)", it);
        (void)life_run_names((char *)buf, "C:\\x\\stub.exe");
        (void)report_wer_wanted((char *)buf);
        (void)report_event_wanted((char *)buf, n);
        (void)vol_counts_in_entry_size((char *)buf, "0a");
        (void)vol_guid_root((char *)buf);
        (void)vol_pick_mount((char *)buf, n, out, sizeof out);
        (void)vol_plain_dos((char *)buf, out, sizeof out);
        (void)vol_join_mount((char *)buf, "E:\\", out, sizeof out);
        int k = vol_device_events(VOL_DBT_ARRIVAL, VOL_DBT_INTERFACE, 0, (char *)buf, out, 1 + rnd() % sizeof out);
        CHECK(k <= 1 && (k != 1 || out[strlen(out) - 1] == '\n'), "one event or none (iteration %ld)", it);
    }
}

/* ------------------------------------------------------------------ Create Report (report_build.c) */

static int has_sub(const char *s, const char *sub) { return s && strstr(s, sub) != NULL; }

static void test_report_text(void) {
    char *c = report_cap_bytes("abc", 3);
    CHECK(c && !strcmp(c, "abc"), "fits: whole");
    free(c);
    c = report_cap_bytes("abcdef", 5);
    CHECK(c && !strcmp(c, "ab\xE2\x80\xA6"), "cut with an ellipsis in 5 bytes: %s", c);
    free(c);
    c = report_cap_bytes("\xC3\xA9\xC3\xA9\xC3\xA9", 6);   /* ééé: 6 bytes, fits */
    CHECK(c && strlen(c) == 6, "6 bytes fit in 6");
    free(c);
    c = report_cap_bytes("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 6);
    CHECK(c && !strcmp(c, "\xC3\xA9\xE2\x80\xA6"), "a code point kept whole: %s", c);
    free(c);

    static const char A64[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char text[400];
    snprintf(text, sizeof text, "model %s, commit %.40s, upper %s, long %sff, short %.20s", A64, A64,
             "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF", A64, A64);
    char **list = NULL;
    int n = 0;
    CHECK(!report_known_checksums(text, strlen(text), &list, &n), "the checksums");
    CHECK(n == 2 && !strcmp(list[0], A64) && strlen(list[1]) == 40, "64 and 40 hex digits only, lower case (the upper one is the first), each once, never "
                                                                      "inside a longer run: %d", n);
    report_strings_free(list, n);

    const char *facts[] = {"RB Stems Plus: 1.5.0 (x64); lights: Demucs v4 green (on)", "Windows: 10.0 build 26100", "line\nwith a break"};
    char *d = report_issue_details("", facts, 3);
    CHECK(d && !strcmp(d, "RB Stems Plus: 1.5.0 (x64); lights: Demucs v4 green (on)\nWindows: 10.0 build 26100\nline with a break"),
          "no problem: the facts one a line: %s", d);
    free(d);
    d = report_issue_details("Last problem:\nx", facts, 1);
    CHECK(d && !strncmp(d, "Last problem:\nx\n\nRB Stems Plus", 30), "the problem, a blank line, the facts: %s", d);
    free(d);

    char *u = report_issue_url("Problem report: a+b", "1+1 & = ? #");
    CHECK(u && !strncmp(u, REPORT_ISSUES_URL "?title=Problem%20report%3A%20a%2Bb&body=", 80) && has_sub(u, "1%2B1%20%26%20%3D%20%3F%20%23"),
          "every byte but the unreserved ones encoded: a + stays a +: %s", u);
    CHECK(has_sub(u, "%23%23%23%20What%20happened%3F%0A%0A%0A%0A%23%23%23%20Technical%20details%0APlease%20also%20drag%20in"),
          "the user's text first, then the technical details");
    free(u);
    /* the worst case: characters that take 9 bytes in the link each */
    char wide[6001], big[9000];
    size_t k = 0;
    for (int i = 0; i < 1000; i++) k += (size_t)snprintf(wide + k, sizeof wide - k, "\xC3\xA9\xE2\x80\x94");
    snprintf(big, sizeof big, "Last problem:\n2026-11-18 10:00:00  %s", wide);
    const char *f4[40];
    char lines[40][700];
    for (int i = 0; i < 40; i++) {
        snprintf(lines[i], sizeof lines[i], "fact %d: %.600s", i, wide);
        f4[i] = lines[i];
    }
    d = report_issue_details(big, f4, 40);
    u = d ? report_issue_url(wide, d) : NULL;
    CHECK(u && strlen(u) <= REPORT_URL_MAX, "the worst case link is at most %d bytes: %zu", REPORT_URL_MAX, u ? strlen(u) : 0);
    CHECK(has_sub(u, "Last%20problem%3A") && has_sub(u, "fact%200%3A") && has_sub(u, "fact%203%3A") && !has_sub(u, "fact%2039%3A"),
          "the last problem and the first four facts fit; the bottom ones go");
    free(d);
    free(u);

    const char *names[] = {"SAMSUNG"};
    char *m = report_mask_location("D:\\Stems: saving to E:\\Music\\RB Stems Plus\\x.flac (on \"SAMSUNG\"), C:\\Users\\dj\\x and \\\\nas\\share\\y",
                                   "D:\\Stems", 'C', names, 1);
    CHECK(m && has_sub(m, "saving to <drive>:\\<path>") && has_sub(m, "C:\\Users\\dj\\x") && has_sub(m, "<stems folder>") && !has_sub(m, "Music")
              && !has_sub(m, "nas") && !has_sub(m, "SAMSUNG"),
          "another drive's paths, a share and the drive's name go; the system drive's stays (redact's): %s", m);
    free(m);
    m = report_mask_location("\\\\音楽\\share\\x\nD:/x\nC:/Windows", "", 'C', NULL, 0);
    CHECK(m && !strcmp(m, "\\\\<drive>\\<path>\n<drive>:\\<path>\nC:/Windows"), "a Unicode UNC host and a drive with slash separators: %s", m);
    free(m);

    CHECK(!strcmp(report_light_colour("0"), "green") && !strcmp(report_light_colour("1"), "yellow") && !strcmp(report_light_colour("2"), "red"),
          "the lights' colours");
    char a[32];
    report_ago(-1, a, sizeof a);
    CHECK(!strcmp(a, "never"), "never checked");
    report_ago(600, a, sizeof a);
    CHECK(!strcmp(a, "10 min ago"), "%s", a);
    report_ago(5 * 3600, a, sizeof a);
    CHECK(!strcmp(a, "5 h ago"), "%s", a);
    report_ago(3 * 86400, a, sizeof a);
    CHECK(!strcmp(a, "3 days ago"), "%s", a);
    report_gb(25600000000, a, sizeof a);
    CHECK(!strcmp(a, "25.6 GB"), "%s", a);
}

/* a log of (seconds before now, message) lines, as app.log writes them */
static char *mklog(time_t now, const int *ago, const char *const *msg, int n) {
    size_t cap = 64;
    for (int i = 0; i < n; i++) cap += strlen(msg[i]) + 32;
    char *b = malloc(cap);
    size_t k = 0;
    b[0] = 0;
    for (int i = 0; i < n; i++) {
        time_t t = now - ago[i];
        struct tm tm;
        localtime_r(&t, &tm);
        k += (size_t)snprintf(b + k, cap - k, "%04d-%02d-%02d %02d:%02d:%02d  %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                              tm.tm_sec, msg[i]);
    }
    return b;
}
static void stamp(time_t t, char out[20]) {
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, 20, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

#define LP(app, bridge)                                                                                \
    do {                                                                                               \
        free(title);                                                                                   \
        free(text);                                                                                    \
        title = text = NULL;                                                                           \
        CHECK(!report_last_problem(app, strlen(app), bridge, strlen(bridge), lnow, &title, &text), "read"); \
    } while (0)

static void test_report_problem(void) {
    time_t now = time(NULL);
    int64_t lnow = report_local_s(now);
    char *title = NULL, *text = NULL, *l = NULL, want[600], at[20];
    LP("", "");
    CHECK(title && !title[0] && text && !text[0], "no logs: no last problem");
    {
        int ago[] = {4 * 3600};
        const char *m[] = {"download failed: nothing changed"};
        l = mklog(now, ago, m, 1);
        LP(l, "");
        CHECK(!title[0], "a failure 4 hours old is left out");
        free(l);
    }
    {
        int ago[] = {3600, 3000};
        const char *m[] = {"download failed: nothing changed", "state: rekordbox 7.2.19"};
        l = mklog(now, ago, m, 2);
        LP(l, "");
        stamp(now - 3600, at);
        snprintf(want, sizeof want, "Last problem:\n%s  download failed: nothing changed", at);
        CHECK(!strcmp(title, "download failed: nothing changed") && !strcmp(text, want), "a failure 1 hour old is in, dated: %s", text);
        free(l);
    }
    LP("download failed: nothing changed\n10:00 refused\n", "");
    CHECK(!title[0], "lines without a date are skipped");
    {
        int ago[] = {1000, 990, 900, 890, 800, 790};
        const char *m[] = {"admin run (install), attempt 1", "  FAILED: Operation not permitted", "admin run (install), attempt 2",
                           "  FAILED: Operation not permitted", "admin run (install), attempt 3", "  FAILED: Operation not permitted"};
        l = mklog(now, ago, m, 6);
        LP(l, "");
        stamp(now - 990, at);
        char last[20];
        stamp(now - 790, last);
        snprintf(want, sizeof want, "Last problem:\n%s  admin run (install) FAILED: Operation not permitted (3 times, last at %.5s)", at, last + 11);
        CHECK(!strcmp(title, "admin run (install) failed") && !strcmp(text, want), "three identical failures are one line: %s", text);
        free(l);
    }
    {
        int ago[] = {1000, 990, 900, 890};
        const char *m[] = {"admin run (install), attempt 1", "  FAILED: Operation not permitted", "admin run (install), attempt 2", "  ok: installed"};
        l = mklog(now, ago, m, 4);
        LP(l, "");
        CHECK(!title[0], "a failure then a success of the same run: none");
        free(l);
        const char *m2[] = {"admin run (install), attempt 1", "  FAILED: Operation not permitted", "admin run (uninstall), attempt 1", "  ok: removed"};
        l = mklog(now, ago, m2, 4);
        LP(l, "");
        CHECK(!strcmp(title, "admin run (install) failed"), "another run's success doesn't hide it");
        free(l);
    }
    {
        int ago[] = {700, 600};
        const char *m[] = {"Demucs v4 not installed: the STEMS Engine isn't downloaded", "Demucs v4: installed (0cc50d877629)"};
        l = mklog(now, ago, m, 2);
        LP(l, "");
        CHECK(!title[0], "Demucs v4 not installed, then installed: none");
        free(l);
        l = mklog(now, ago, m, 1);
        LP(l, "");
        CHECK(!strcmp(title, "Demucs v4 not installed: the STEMS Engine isn't downloaded"), "not installed is a problem");
        free(l);
        const char *m2[] = {"Stems Cache not installed: rekordbox 7.3.0 isn't supported", "admin run (install), attempt 1"};
        l = mklog(now, ago, m2, 1);
        LP(l, "");
        CHECK(!strcmp(title, "Stems Cache not installed: rekordbox 7.3.0 isn't supported"), "Stems Cache not installed is one");
        free(l);
    }
    {
        int ago[] = {500, 400, 300};
        const char *m[] = {"ERROR no real ONNX Runtime library: rekordbox's analysis will fail", "bridge loaded (real library x)", "hit  abc"};
        l = mklog(now, ago, m, 2);
        LP("", l);
        CHECK(!title[0], "a loading error, then loaded: none");
        free(l);
        const char *m2[] = {"ERROR writing C:\\x\\y.flac", "bridge loaded (real library x)", "hit  abc"};
        l = mklog(now, ago, m2, 3);
        LP("", l);
        stamp(now - 500, at);
        snprintf(want, sizeof want, "Last problem:\nbridge.log:\n%s  ERROR writing", at);
        CHECK(!strcmp(title, "Stems Cache: writing C:\\x\\y.flac") && !strncmp(text, want, strlen(want)), "another bridge error stays: %s", text);
        free(l);
    }
    {
        int ago[8];
        const char *m[8];
        char buf[8][32];
        for (int i = 0; i < 8; i++) {
            ago[i] = 900 - i * 10;
            snprintf(buf[i], sizeof buf[i], "step %d failed", i);
            m[i] = buf[i];
        }
        l = mklog(now, ago, m, 8);
        LP(l, "");
        int lines = 1;
        for (const char *p = text; *p; p++) lines += *p == '\n';
        CHECK(lines == 6 && has_sub(text, "step 7 failed") && !has_sub(text, "step 2 failed"), "app.log's newest 5 only (%d lines)", lines);
        free(l);
        for (int i = 0; i < 6; i++) {
            snprintf(buf[i], sizeof buf[i], "ERROR writing %d.flac", i);
            ago[i] = 600 - i * 10;
        }
        l = mklog(now, ago, m, 6);
        LP("", l);
        lines = 1;
        for (const char *p = text; *p; p++) lines += *p == '\n';
        CHECK(lines == 5 && has_sub(text, "5.flac") && !has_sub(text, "2.flac"), "the bridge's newest 3 only (%d lines)", lines);
        free(l);
    }
    {
        int ago[] = {60};
        const char *m[] = {"report: couldn't save it on the Desktop (error 13)"};
        l = mklog(now, ago, m, 1);
        LP(l, "");
        CHECK(!title[0], "the report's own lines aren't problems");
        free(l);
        char longm[200];
        memset(longm, 'x', 100);
        snprintf(longm + 100, sizeof longm - 100, " failed");
        const char *m2[] = {longm};
        l = mklog(now, ago, m2, 1);
        LP(l, "");
        CHECK(strlen(title) == 59 + 3 && !strcmp(title + 59, "\xE2\x80\xA6"), "a long title is cut to 60 characters: %zu", strlen(title));
        free(l);
        const char *m3[] = {"the update couldn't start", "Errors: 0", "terrorist", "cannot", "notfailed here"};
        int ago3[] = {50, 40, 30, 20, 10};
        l = mklog(now, ago3, m3, 5);
        LP(l, "");
        CHECK(has_sub(text, "couldn't start") && has_sub(text, "  cannot") && !has_sub(text, "terrorist") && !has_sub(text, "notfailed") && !has_sub(text, "Errors"),
              "the words whole, case ignored: %s", text);
        free(l);
    }
    free(title);
    free(text);
}
#undef LP

static void test_report_zip(const char *scratch) {
    report_zip z;
    report_zip_init(&z);
    struct tm when = {.tm_year = 126, .tm_mon = 10, .tm_mday = 18, .tm_hour = 10, .tm_min = 15, .tm_sec = 30};
    CHECK(!report_zip_add(&z, "top/a.txt", "hello", 5, &when) && !report_zip_add(&z, "top/crash-reports/b.wer.txt", "", 0, &when), "two entries");
    CHECK(report_zip_add(&z, "../x", "x", 1, &when) == 0, "a name the reader refuses is still written (the caller's names are fixed)");
    report_zip_free(&z);
    report_zip_init(&z);
    report_zip_add(&z, "top/a.txt", "hello", 5, &when);
    report_zip_add(&z, "top/crash-reports/b.wer.txt", "", 0, &when);
    CHECK(report_zip_add(&z, "top\\c", "x", 1, &when) == EINVAL && report_zip_end(&z) == EINVAL, "a backslash: refused, and the zip with it");
    report_zip_free(&z);
    report_zip_init(&z);
    report_zip_add(&z, "top/a.txt", "hello", 5, &when);
    report_zip_add(&z, "top/crash-reports/b.wer.txt", "", 0, &when);
    CHECK(!report_zip_end(&z) && unzip_check(z.b, z.n) == UNZIP_OK, "the updater's strict reader takes it");
    char dest[1100], p[1200];
    snprintf(dest, sizeof dest, "%s/zip-out", scratch);
    mkdir(dest, 0755);
    int err = 0;
    CHECK(unzip_extract(z.b, z.n, dest, &err) == UNZIP_OK, "extracted (%d)", err);
    snprintf(p, sizeof p, "%s/top/a.txt", dest);
    FILE *f = fopen(p, "rb");
    char got[16] = "";
    if (f) {
        got[fread(got, 1, sizeof got - 1, f)] = 0;
        fclose(f);
    }
    CHECK(!strcmp(got, "hello"), "its bytes: %s", got);
    CHECK(report_crc32(0, "123456789", 9) == 0xCBF43926u, "zip's CRC-32");
    report_zip_free(&z);
}

typedef struct {
    report_mask_names names;
    const char *disappears;
} mask_fixture;

static char *mask_report(void *ctx, const char *s, size_t n, const char *const *keep, int nkeep, size_t *len) {
    mask_fixture *m = ctx;
    /* All candidates were scanned by the time the first one's text is masked. */
    if (m->disappears && n == 4 && !memcmp(s, "V=1\n", n)) {
        CHECK(unlink(m->disappears) == 0, "a WER candidate disappears between scan and read");
        m->disappears = NULL;
    }
    return report_mask_with(&m->names, s, n, keep, nkeep, len);
}
static int fake_events(void *ctx, char **out, size_t *len) {
    (void)ctx;
    *out = strdup("<Event>rekordbox.exe at /Users/dj/x</Event>\n");
    *len = strlen(*out);
    return 0;
}
static void put_file(const char *dir, const char *name, const char *text) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) abort();
    fputs(text, f);
    fclose(f);
}
static char *slurp_bytes(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *b = calloc(1, 1 << 20);
    size_t n = fread(b, 1, (1 << 20) - 1, f);
    if (len) *len = n;
    b[n] = 0;
    fclose(f);
    return b;
}
static char *slurp(const char *dir, const char *name) {
    char p[1300];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return slurp_bytes(p, NULL);
}
static int count_entries(const char *dir) {
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}
static void report_says(void *ctx, const char *line) { strncat((char *)ctx, line, 1000); }

static void test_report_build(const char *scratch) {
    char sup[1024], logs[1100], desk[1024], fb[1100], wer[1024], wer2[1024], p[1200], said[2048] = "";
    snprintf(sup, sizeof sup, "%s/support", scratch);
    snprintf(logs, sizeof logs, "%s/Logs", sup);
    snprintf(fb, sizeof fb, "%s/reports", logs);
    snprintf(desk, sizeof desk, "%s/Desktop", scratch);
    snprintf(wer, sizeof wer, "%s/wer", scratch);
    snprintf(wer2, sizeof wer2, "%s/wer2", scratch);
    mkdir(sup, 0755);
    mkdir(logs, 0755);
    mkdir(desk, 0755);
    mkdir(wer, 0755);
    mkdir(wer2, 0755);
    snprintf(p, sizeof p, "%s/payload", sup);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/kept", sup);
    mkdir(p, 0755);
    time_t now = time(NULL);
    int ago[] = {120};
    const char *m[] = {"download failed for dj at /Users/dj/AppData on E:\\Music\\x"};
    char *app = mklog(now, ago, m, 1);
    put_file(logs, "app.log", app);
    free(app);
    put_file(logs, "watcher.log", "watcher: started\n");
    put_file(sup, "installed", "model=1\ncache=0\n");
    put_file(sup, "config.ini", "max_gb = 20\n");
    snprintf(p, sizeof p, "%s/payload", sup);
    put_file(p, "payload.json", "{\"model\":{\"sha256\":\"feedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedface\"}}");
    snprintf(p, sizeof p, "%s/kept", sup);
    put_file(p, "Playlist.kept", "a track name.mp3\n");             /* kept sets: never in the report */
    put_file(sup, "master.db", "not ours to send");                   /* nor anything else in the folder */
    put_file(sup, "shown-version", "1.5.0\n1.5.0\n");
    static const unsigned char rep[] = {'V', '=', '1', '\n'};
    wer_folder(wer, "AppCrash_rekordbox.exe_1", rep, sizeof rep, now - 600);
    wer_folder(wer, "AppCrash_rekordbox.exe_gone", rep, sizeof rep, now - 660);
    char gone[1100];
    snprintf(gone, sizeof gone, "%s/AppCrash_rekordbox.exe_gone/Report.wer", wer);
    mask_fixture mask = {{.home = "/Users/dj", .short_home = "C:\\Users\\DANAJO~1", .full = "Dana Jones", .user = "Dana", .folder_name = "dj"}, gone};
    report_facts f = {0};
    report_fact_add(&f, 1, "RB Stems Plus: 1.5.0 (x64); lights: Demucs v4 %s (on)", report_light_colour("0"));
    report_fact_add(&f, 1, "saved stems location: thisPC; folder E:\\Stems\\RB Stems Plus");
    report_fact_add(&f, 0, "model in rekordbox, full: %s", "feedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedface");
    report_fact_add(&f, 1, "account: Dana Jones");
    report_fact_add(&f, 1, "profile: c:\\users\\danajo~1\\Logs");
    report_fact_add(&f, 0, "other folder: C:\\Users\\DANAJO~10\\Logs; bare: DANAJO~1");
    const char *roots[] = {wer, wer2};
    report_env e;
    memset(&e, 0, sizeof e);
    e.ctx = &mask;
    e.mask = mask_report;
    e.events = fake_events;
    e.log = report_says;
    e.log_ctx = said;
    e.support = sup;
    e.logs = logs;
    e.wer_roots = roots;
    e.nwer = 1;
    e.desktop = desk;
    e.fallback = fb;
    e.facts = &f;
    e.location_dir = "";
    e.sysdrive = 'C';
    e.now = now;
    report_result r;
    CHECK(report_build(&e, &r) == 0, "built");
    CHECK(!strncmp(r.path, desk, strlen(desk)) && has_sub(r.path, "/RB Stems Plus report "), "on the Desktop: %s", r.path);
    CHECK(!mask.disappears && has_sub(said, "report: 2 crash or hang reports from the last 7 days, 1 included"), "the missing candidate: %s", said);
    CHECK(r.title && !strcmp(r.title, "Problem report: download failed for <user> at ~/AppData on <drive>:\\<path>"),
          "the title: the last problem, masked, no drive's path: %s", r.title);
    CHECK(r.details && has_sub(r.details, "Last problem:\n") && has_sub(r.details, "RB Stems Plus: 1.5.0 (x64); lights: Demucs v4 green (on)")
              && has_sub(r.details, "folder <drive>:\\<path>") && !has_sub(r.details, "model in rekordbox, full"),
          "the details: the problem, then the link's facts only, masked: %s", r.details);
    char *url = report_issue_url(r.title, r.details);
    CHECK(url && has_sub(url, "account%3A%20%3Cuser%3E") && !has_sub(url, "Jones"), "the full name is masked in the issue URL");
    CHECK(url && has_sub(url, "profile%3A%20~%5CLogs") && !has_sub(url, "danajo"), "the short profile path is masked in the issue URL");
    free(url);
    /* what the zip holds: exactly these */
    size_t zn = 0;
    char *zb = slurp_bytes(r.path, &zn);
    CHECK(zb && unzip_check(zb, zn) == UNZIP_OK, "a zip the strict reader takes");
    char out[1100], top[1300];
    snprintf(out, sizeof out, "%s/out", scratch);
    mkdir(out, 0755);
    int err = 0;
    CHECK(zb && unzip_extract(zb, zn, out, &err) == UNZIP_OK, "extracted");
    free(zb);
    const char *slash = strrchr(r.path, '/');
    snprintf(top, sizeof top, "%s/%.*s", out, (int)(strlen(slash + 1) - 4), slash + 1);
    CHECK(count_entries(out) == 1, "one folder at the top");
    CHECK(count_entries(top) == 8, "summary, app.log, watcher.log, installed, config.ini, payload.json, the event log, crash-reports: %d (no bridge.log, "
                                   "busy, request or dismissed here)",
          count_entries(top));
    char *t = slurp(top, "summary.txt");
    CHECK(t && !strncmp(t, "RB Stems Plus report, ", 22) && has_sub(t, "\nRB Stems Plus: 1.5.0 (x64)") && has_sub(t, "full: feedfacefeedface"),
          "summary.txt: every fact, the checksums it prints kept: %s", t);
    CHECK(t && has_sub(t, "account: <user>\n") && !has_sub(t, "Jones"), "the full name is masked in summary.txt");
    CHECK(t && has_sub(t, "profile: ~\\Logs") && has_sub(t, "other folder: C:\\Users\\DANAJO~10\\Logs; bare: DANAJO~1"),
          "the short profile path is masked at a component boundary: %s", t);
    free(t);
    t = slurp(top, "app.log");
    CHECK(t && has_sub(t, "for <user> at ~/AppData") && !has_sub(t, "/Users/dj"), "the logs masked: %s", t);
    free(t);
    t = slurp(top, "payload.json");
    CHECK(t && has_sub(t, "feedfacefeedfacefeedface"), "payload.json's checksums kept");
    free(t);
    t = slurp(top, "event-log-errors.txt");
    CHECK(t && has_sub(t, "at ~/x"), "the event log masked: %s", t);
    free(t);
    snprintf(p, sizeof p, "%s/crash-reports", top);
    t = slurp(p, "0-AppCrash_rekordbox.exe_1.wer.txt");
    CHECK(t && !strcmp(t, "V=1\n"), "the crash report: %s", t ? t : "(none)");
    free(t);
    CHECK(!slurp(top, "master.db") && !slurp(top, "Playlist.kept") && !slurp(top, "shown-version"), "nothing else from support");
    report_result_free(&r);

    /* The same folder name can occur under two different WER roots. */
    static const unsigned char rep2[] = {'V', '=', '2', '\n'};
    wer_folder(wer2, "AppCrash_rekordbox.exe_1", rep2, sizeof rep2, now - 720);
    e.nwer = 2;
    said[0] = 0;
    CHECK(report_build(&e, &r) == 0, "built with two WER reports of the same name");
    zb = slurp_bytes(r.path, &zn);
    CHECK(zb && unzip_check(zb, zn) == UNZIP_OK && has_sub(said, "2 included"), "two WER roots produce distinct ZIP entries");
    free(zb);
    report_result_free(&r);

    /* The Desktop can be opened by itself but leave no room for a ZIP name in r.path. */
    char long_desk[REPORT_PATH_MAX];
    snprintf(long_desk, sizeof long_desk, "%s", desk);
    while (strlen(long_desk) < sizeof long_desk - 20) strcat(long_desk, "/.");
    e.desktop = long_desk;
    said[0] = 0;
    e.now++;
    CHECK(report_build(&e, &r) == 0 && !strncmp(r.path, fb, strlen(fb)), "a Desktop result path that can't fit uses the fallback");
    CHECK(count_entries(desk) == 1, "a path that can't be returned never writes a new ZIP on the Desktop");
    report_result_free(&r);

    /* the Desktop refused: next to the logs; both refused: not made */
    snprintf(p, sizeof p, "%s/no-desktop", scratch);
    e.desktop = p;
    said[0] = 0;
    CHECK(report_build(&e, &r) == 0 && !strncmp(r.path, fb, strlen(fb)), "the fallback: %s", r.path);
    CHECK(has_sub(said, "report: couldn't save it on the Desktop"), "said: %s", said);
    report_result_free(&r);
    e.fallback = "";
    CHECK(report_build(&e, &r) != 0 && !r.title && !r.details, "nowhere to save it: an error, nothing kept");
    e.fallback = fb;
    e.desktop = desk;
    e.events = NULL;
    e.logs = NULL;
    CHECK(report_build(&e, &r) == 0 && r.title && !strcmp(r.title, "Problem report"), "no logs, no event log: still a report, a plain title");
    report_result_free(&r);

    /* Leave room for "<user>…" at each cut, with the full name extending past it. */
    char line_prefix[200 - 21 - 7 + 1], title_prefix[60 - 7 + 1], long_line[220], long_title[80];
    snprintf(line_prefix, sizeof line_prefix, "%-*s", (int)sizeof line_prefix - 1, "download failed:");
    snprintf(title_prefix, sizeof title_prefix, "%-*s", (int)sizeof title_prefix - 1, "download failed:");
    snprintf(long_line, sizeof long_line, "%s%s while loading", line_prefix, mask.names.full);
    snprintf(long_title, sizeof long_title, "%s%s while loading", title_prefix, mask.names.full);
    int boundary_ago[] = {120, 60};
    const char *boundary_msgs[] = {long_line, long_title};
    app = mklog(now, boundary_ago, boundary_msgs, 2);
    put_file(logs, "app.log", app);
    free(app);
    e.logs = logs;
    CHECK(report_build(&e, &r) == 0, "built with a full name across both cuts");
    snprintf(long_line, sizeof long_line, "%s<user>…\n", line_prefix);
    snprintf(long_title, sizeof long_title, "Problem report: %s<user>…", title_prefix);
    CHECK(has_sub(r.details, long_line), "the last problem line masks a full name across the 200-character cut");
    CHECK(r.title && !strcmp(r.title, long_title), "the title masks a full name across the 60-character cut");
    report_result_free(&r);
    report_facts_free(&f);
}

/* The host can check these Win32 lifetime rules in source. Runtime coverage needs Windows. */
static void test_windows_startup(const char *src) {
    char *s = slurp(src, "notify_win.c");
    char *worker = s ? strstr(s, "static DWORD WINAPI nt_thread(") : NULL;
    char *init = worker ? strstr(worker, "static BOOL WINAPI nt_init(") : NULL;
    char *show = init ? strstr(init, "int notify_show(") : NULL;
    CHECK(init && strstr(init, "DuplicateHandle(") && !strstr(init, "nt_thread, &"), "the worker owns a separate event handle");
    CHECK(init && strstr(worker, "SetEvent(ready);") && strstr(worker, "CloseHandle(ready);"),
          "the worker signals and closes its event even after the caller times out");
    CHECK(show && strstr(worker, "InterlockedExchangePointer(&nt_hwnd") && strstr(show, "InterlockedCompareExchangePointer(&nt_hwnd"),
          "the window is published and read atomically");
    CHECK(init && strstr(init, "if (!ready) return FALSE;") && strstr(init, "wait == WAIT_TIMEOUT ||"),
          "a failed startup can retry, while a timed-out worker can still publish its window");
    free(s);

    s = slurp(src, "update_win.c");
    char *scan = s ? strstr(s, "static int win_watchers_in(") : NULL;
    char *end = scan ? strstr(scan, "\n}") : NULL;
    if (end) *end = 0;
    CHECK(end && strstr(scan, "malloc(NT_MAX * sizeof *path)") && !strstr(scan, "static wchar_t path") && strstr(scan, "free(path);"),
          "each watcher scan owns its path buffer");
    free(s);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s SCRATCH [FUZZ_ITERATIONS [WIN_C_SRC]]\n", argv[0]);
        return 2;
    }
    char sub[1024];
    test_external();
    test_paths();
    test_uninstall_path();
    snprintf(sub, sizeof sub, "%s/size", argv[1]);
    mkdir(sub, 0755);
    test_size(sub);
    test_one_copy();
    test_lifecycle();
    test_report_pure();
    snprintf(sub, sizeof sub, "%s/wer", argv[1]);
    mkdir(sub, 0755);
    test_report_files(sub);
    test_report_queue(sub);
    test_report_text();
    test_report_problem();
    snprintf(sub, sizeof sub, "%s/report", argv[1]);
    mkdir(sub, 0755);
    test_report_zip(sub);
    test_report_build(sub);
    test_windows_startup(argc > 3 ? argv[3] : "win/c");
    test_critical();
    test_fuzz(argc > 2 ? atol(argv[2]) : 20000);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
