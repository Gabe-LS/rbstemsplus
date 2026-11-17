/* A chosen location's marker (bridge.md §0a), moved from the bridge's marker_ok and marker_check:
 * MARKER_NAME in the location's folder, written once by the app: an id= line (the location's
 * cache_id) and a format= line (MARKER_FORMAT). tests/marker-fixtures.txt is the oracle shared
 * with the app's reader (Location.swift). The check takes the folder's handle, so the Swift app's
 * removeInLocation can call it on a folder it opened itself (I13). */
#ifndef RBSP_MARKER_H
#define RBSP_MARKER_H

#include "api.h"
#include "plat.h"

#include <stddef.h>

#define MARKER_NAME ".rbstemsplus-cache"
#define MARKER_MAX 4096
#define MARKER_FORMAT "rbstems-v1"

/* the marker's text: an id= line equal to want (ignoring case) and a format= line equal to
 * MARKER_FORMAT, and no other value of either; lines split at "\n", nothing trimmed, no NUL */
COMMON_API int marker_ok(const char *b, size_t n, const char *want);
/* The verdict when the marker couldn't be opened or read, from the error: "no-permission",
 * "io-error", "marker" (missing, not a regular file, too big), or "" (no verdict: a shortage of
 * descriptors or memory, or EBUSY: a marker another process holds open, which Windows reports for
 * a sharing violation (bridge.md §1, I83); not the location's fault). */
COMMON_API const char *marker_why(int err);
/* NULL when the folder d holds the marker of the location `want`, else why not, as marker_why
 * (a read that fails after the open is "io-error"; a marker of another id or format, "marker") */
COMMON_API const char *marker_check_at(plat_dir d, const char *want);

#endif
