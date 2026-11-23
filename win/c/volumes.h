/* volumes.c, volumes_win.c: the app's own volume calls on Windows (app.md §5.8, W6a), the facts
 * Location.swift and LocationMonitor.swift give the Mac's core:
 *  - the volumes mounted now and the facts that decide whether one may hold the stems folder
 *    (getmntinfo and volumeFacts' twin): its volume GUID path, mount point, label, file system,
 *    drive type, read-only, the system volume, and the device's facts;
 *  - the external rule (I89, J28): the disk's bus is USB, 1394, SD or MMC; or the volume isn't
 *    the system's and the disk, or one of its ancestors in the device tree, is removable or
 *    hot-pluggable, on an external bus (that node's enumerator USB, or it or a node above it a
 *    Thunderbolt or USB4 device). SATA, RAID or NVMe on a hot-plug port without such a node is
 *    internal;
 *  - GUID paths: a folder's final path (links followed) as a volume GUID path, the form
 *    config.ini's cache_dir takes (contract with the bridge), and back to a drive-letter path for
 *    the texts;
 *  - device events: WM_DEVICECHANGE's volume arrivals and removals as the core's "mount" and
 *    "unmount" events (the shell's window is W6b's);
 *  - size on disk: the saved stems' allocated size, by the entry-size rule's four name forms
 *    (entriesSize, Location.swift:431), through common/plat.h's listing (AllocationSize on
 *    Windows, st_blocks * 512 in the Mac tests).
 *
 * The rules are pure C (volumes.c), run on the Mac by tests/app_test.c; volumes_win.c reads the
 * facts from Windows. Every call that touches a volume runs on a worker thread, never the UI
 * thread: a sleeping USB disk can block GetVolumeInformation for seconds (app.md §2 rule 1). */
#ifndef RBSP_WIN_VOLUMES_H
#define RBSP_WIN_VOLUMES_H

#include <stddef.h>
#include <stdint.h>
#include "plat.h"

enum {
    VOL_PATH_MAX = 1024,
    VOL_GUID_MAX = 64,       /* "\\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\" is 49 bytes */
    VOL_GUID_LEN = 49,
    VOL_LABEL_MAX = 132,     /* 32 UTF-16 units, at most 4 bytes each in UTF-8 */
    VOL_NODES_MAX = 16,
};

/* GetDriveType's answers (winbase.h) */
enum { VOL_DRIVE_UNKNOWN = 0, VOL_DRIVE_NO_ROOT = 1, VOL_DRIVE_REMOVABLE = 2, VOL_DRIVE_FIXED = 3, VOL_DRIVE_REMOTE = 4,
       VOL_DRIVE_CDROM = 5, VOL_DRIVE_RAMDISK = 6 };

/* STORAGE_BUS_TYPE (winioctl.h): the ones the rule names */
enum { VOL_BUS_1394 = 0x4, VOL_BUS_USB = 0x7, VOL_BUS_RAID = 0x8, VOL_BUS_SATA = 0xB, VOL_BUS_SD = 0xC, VOL_BUS_MMC = 0xD,
       VOL_BUS_FILE_BACKED_VIRTUAL = 0xF, VOL_BUS_SPACES = 0x10, VOL_BUS_NVME = 0x11 };

/* One device node, from the disk up: node[0] is the disk, node[i + 1] node[i]'s parent, to the
 * root (configuration manager's tree). */
typedef struct {
    char enumerator[32];   /* CM_DRP_ENUMERATOR_NAME, upper case: "USB", "USBSTOR", "SCSI", "PCI"... */
    int removable;         /* CM_DEVCAP_REMOVABLE in its capabilities */
    int tunnel;            /* a Thunderbolt or USB4 node: vol_tunnel_text on its hardware and compatible
                            * IDs, service and description */
} vol_node;

/* What decides whether a volume may hold the stems folder (VolumeFacts' twin). The device's facts
 * are -1 (or nodes 0) when they couldn't be read. */
typedef struct {
    char guid[VOL_GUID_MAX];       /* "\\?\Volume{...}\" */
    char mount[VOL_PATH_MAX];      /* its first drive letter ("E:\"), else its first folder mount; "" none */
    char label[VOL_LABEL_MAX];     /* "" when it has none */
    char fstype[16];               /* lower case: ntfs, exfat, fat32, fat, refs, udf... */
    int drive_type;                /* VOL_DRIVE_* */
    int readonly;                  /* FILE_READ_ONLY_VOLUME */
    int system;                    /* the volume holding %SystemRoot% */
    int disks;                     /* how many disk extents' disks it spans (IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS) */
    int bus;                       /* the disk's STORAGE_BUS_TYPE (IOCTL_STORAGE_QUERY_PROPERTY) */
    int hotplug;                   /* IOCTL_STORAGE_GET_HOTPLUG_INFO's DeviceHotplug */
    int nodes;                     /* how many in node[] */
    vol_node node[VOL_NODES_MAX];
} vol_facts;

/* -- pure rules (volumes.c) */

/* Whether the text (a hardware or compatible ID, a service, a description) names a Thunderbolt or
 * USB4 device: "THUNDERBOLT", "USB4" or the USB4 host interface's class code "CC_0C0340", case
 * ignored. Unverified against real hardware until the real-PC round (J28): the facts line below
 * records what each node said. */
int vol_tunnel_text(const char *s);
/* The external rule (I89, J28). 0 for the system volume, a remote one, one spanning several disks
 * or none, or when the device's facts are unknown. */
int vol_external(const vol_facts *f);
/* whether the file system may hold the stems folder: NTFS, exFAT, FAT32 */
int vol_fs_allowed(const char *fstype);
/* What refuses a volume for the stems folder, in locationRefusalFor's order (Location.swift:231):
 * a network drive, then an internal one (the system volume included), then the file system, then
 * read-only. f NULL: unreadable. The path rules (links, our folder above, its length) are the
 * core's. */
enum { VOL_OK, VOL_REFUSE_UNREADABLE, VOL_REFUSE_NETWORK, VOL_REFUSE_INTERNAL, VOL_REFUSE_FILE_SYSTEM, VOL_REFUSE_READ_ONLY };
int vol_refusal(const vol_facts *f);
/* The picker and saved locations use this verdict. Only the location test build can opt in
 * to checking paths on internal volumes. */
int vol_location_refusal(const vol_facts *f);
/* One line for the log and the real-PC round (J28): the drive type, bus, hot-plug, disks and each
 * node's enumerator and flags, then the verdict. No ID, serial or label (the report masks
 * nothing here). out: n bytes, always terminated. */
void vol_facts_line(const vol_facts *f, char *out, size_t n);

/* A volume GUID path's root length (VOL_GUID_LEN: "\\?\Volume{...}\"), or 0 when p doesn't start
 * with one */
size_t vol_guid_root(const char *p);
/* The mount point to show a volume by, from its mount points as GetVolumePathNamesForVolumeName
 * lists them (NUL-separated, an empty one last; len bytes): the first drive letter ("E:\"), else
 * the first folder mount. 0 with out; -1 when there is none or it doesn't fit. */
int vol_pick_mount(const char *mounts, size_t len, char *out, size_t n);
/* A volume GUID path shown through its mount point: "\\?\Volume{...}\Music\X" with mount "E:\"
 * gives "E:\Music\X". 0, or -1 (not a GUID path, or too long). */
int vol_join_mount(const char *guid_path, const char *mount, char *out, size_t n);
/* The cache folder in the uninstall texts (review N3R16): out from vol_display_path when the
 * GUID volume has a mount point, else the path below its root. Other paths pass through. */
const char *vol_uninstall_path(const char *dir, int (*display)(const char *, char *, size_t), char *out, size_t n);
/* Whether p is a network path by its form: "\\server\share..." (or with '/'), "\\?\UNC\...";
 * not "\\?\X:\...", a GUID path or a device ("\\.\..."). A mapped drive letter is told by
 * its drive type (vol_facts_of). */
int vol_path_remote(const char *p);
/* A network volume's facts: drive_type VOL_DRIVE_REMOTE, the device's facts unknown (-1, nodes 0),
 * no GUID, mount, label or file system; vol_refusal gives VOL_REFUSE_NETWORK */
void vol_remote_facts(vol_facts *out);
/* A final DOS path ("\\?\E:\Music\X") as a drive path ("E:\Music\X"); a path in that form already
 * is copied. 0, or -1: a UNC path ("\\?\UNC\..."), another device, or too long. */
int vol_plain_dos(const char *p, char *out, size_t n);

/* WM_DEVICECHANGE's codes (dbt.h) */
enum { VOL_DBT_ARRIVAL = 0x8000, VOL_DBT_REMOVE_COMPLETE = 0x8004, VOL_DBT_VOLUME = 2, VOL_DBT_INTERFACE = 5 };
/* The core's events (events.def) for one device change, as JSON objects, each followed by "\n",
 * into out (n bytes): {"ev":"mount","path":...} on an arrival, "unmount" on a removal; one per
 * drive letter in unitmask ("E:\") for a volume broadcast, and one naming iface (the volume
 * interface's path) for a volume interface's notification (volumes without a letter). How many,
 * 0 for any other message, -1 when out is too small (out then ""). */
int vol_device_events(uint32_t event, uint32_t devtype, uint32_t unitmask, const char *iface, char *out, size_t n);

/* Whether a file in prefix folder `prefix` counts in an entry's size (countsInEntrySize,
 * Location.swift:459): <key>.flac, <key>.desc, or either's "._" companion, the key 64 lower-case
 * hex digits starting with the prefix. Moves onto common/'s entry-size rule (entries.c, bridge
 * T1a) when that lands. */
int vol_counts_in_entry_size(const char *name, const char *prefix);
/* entriesSize (Location.swift:431) through common/plat.h: per entry the allocated size of the
 * files that count, under <64 hex>/<2 hex>/. 0 when the root isn't there; -1 when the root or
 * a model or prefix directory in the walk can't be opened or listed. */
int64_t vol_entries_size(const char *folder);

/* -- from Windows (volumes_win.c; a worker thread only) */

/* The facts of the volume at GUID path guid ("\\?\Volume{...}\"); device: read the device's facts
 * too (the disk, its bus, the device tree). 0, or an errno (ENXIO: not ready). */
int vol_facts_read(const char *guid, int device, vol_facts *out);
/* The picker's rules applied to the GUID root of a saved Windows path. */
int vol_saved_refusal(const char *path);
/* For the bridge's process-lifetime cache, watch the disk used to get the verdict.
 * Register before querying eligibility. The callback must not do I/O. */
typedef struct {
    void *registration;
    void (*invalidated)(void);
} vol_watch;
int vol_saved_watch(const char *path, vol_watch *watch);
/* The volumes mounted now (FindFirstVolume), at most max, without the device's facts; how many,
 * or -1 when the list can't be had. */
int vol_list(vol_facts *out, int max);
/* The facts of the volume holding the folder at path, after links (its final path): 0, or an
 * errno. A network path (vol_path_remote), a mapped drive letter, or a folder whose final path is
 * on a share gives vol_remote_facts (0), so the refusal is the network's. */
int vol_facts_of(const char *path, vol_facts *out);
/* The folder's final path (links followed, GetFinalPathNameByHandle) as a volume GUID path
 * ("\\?\Volume{...}\Music\X", the form cache_dir takes); 0, or an errno. */
int vol_guid_path(const char *path, char *out, size_t n);
/* The GUID of the volume holding this open directory. */
int vol_dir_guid(plat_dir dir, char out[VOL_GUID_MAX]);
int vol_dir_on_volume(plat_dir dir, const char *guid_path);
/* The same as a drive path ("E:\Music\X"), for the texts and the realpath comparison; 0, or an
 * errno (ENOENT: no drive letter or folder mount shows it; EXDEV: a network path,
 * "\\?\UNC\..."). */
int vol_dos_path(const char *path, char *out, size_t n);
/* A GUID path shown through its volume's mount point (vol_pick_mount): 0, or an errno (ENOENT:
 * not mounted anywhere now). */
int vol_display_path(const char *guid_path, char *out, size_t n);
/* vol_device_events for a WM_DEVICECHANGE message as the window procedure got it (W6b's window
 * calls this). */
int vol_device_message(uintptr_t wparam, intptr_t lparam, char *out, size_t n);

#endif
