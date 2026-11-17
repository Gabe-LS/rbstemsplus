/* A chosen location's checks (bridge.md §1, T3c): the bridge's check_location and gone_why over
 * plat.h, for either OS's path form (path_rules.h), so the Mac's tests run the logic Windows runs.
 * The bridge's reasons ("not-connected", "not-local", "read-only", "folder", "no-permission",
 * "io-error", and marker_check_at's "marker") are written to bridge-state as they are.
 *
 * Where the forms differ:
 *  - the volume: the longest mount point holding the location's parent folder, from the table of
 *    mounts (plat_volume_holding: no lookup that could follow a link). Mac: a mount point path;
 *    Windows: the volume's GUID path, so a drive letter that now names another drive is another
 *    volume. This computer's own volume (the Mac's "/" or Data volume, the volume holding
 *    %SystemRoot%) is "not-connected" (any: a test's override). A Windows network drive, which the
 *    table doesn't list, is "not-local".
 *  - the walk: Mac, every component below the mount point; Windows, every component below the
 *    path's own root (X:\ or \\?\Volume{GUID}\), so a volume mounted in a folder ("C:\mnt\x\")
 *    is refused through that path ("folder": its mount point is a reparse point); the app stores
 *    the GUID path. Each component a real folder on that volume, no link or reparse point. */
#ifndef RBSP_LOCATION_H
#define RBSP_LOCATION_H

#include "api.h"
#include "plat.h"

typedef struct {
    plat_id id;                         /* the folder's identity, to pin it (re-checked at each use) */
    char mount[PLAT_MOUNT_MAX];         /* its volume (Windows: \\?\Volume{GUID}\), for location_gone */
} location_pin;

/* NULL when dir (an absolute path in the OS's form) can be used: its volume connected, local, not
 * this computer's and not read-only; every component a real folder on it; our marker of id want in
 * it; the folder writable (plat_dir_writable). *pin then holds its identity and volume. Else the
 * reason. Nothing is ever created: the app makes the folder and the marker. any: any volume,
 * this computer's included, and components on other volumes (the bridge's test override). */
COMMON_API const char *location_check(const char *dir, const char *want, int any, location_pin *pin);
/* why a chosen location's folder failed with errno e: "not-connected" when its volume (mount, as
 * location_check pinned it) is no longer mounted (unless any), else location_why(e) */
COMMON_API const char *location_gone(const char *mount, int any, int e);
/* an errno as a reason: ENOENT, ENOTDIR, ELOOP, EXDEV "folder"; EACCES, EPERM "no-permission";
 * EROFS "read-only"; anything else "io-error" */
COMMON_API const char *location_why(int e);

#endif
