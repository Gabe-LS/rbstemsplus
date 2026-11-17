/* A chosen location's marker (marker.h), moved from the bridge's marker_ok and marker_check. */
#include "marker.h"

#include <string.h>
#include <strings.h>

COMMON_API int marker_ok(const char *b, size_t n, const char *want) {
    int id = 0, fmt = 0;
    if (memchr(b, 0, n)) return 0;
    for (size_t i = 0; i < n;) {
        const char *s = b + i, *nl = memchr(s, '\n', n - i);
        size_t len = nl ? (size_t)(nl - s) : n - i;
        i += len + 1;
        if (len > 3 && !strncmp(s, "id=", 3)) {
            if (len != 39 || strncasecmp(s + 3, want, 36)) return 0;
            id = 1;
        } else if (len > 7 && !strncmp(s, "format=", 7)) {
            if (len != 7 + strlen(MARKER_FORMAT) || strncmp(s + 7, MARKER_FORMAT, strlen(MARKER_FORMAT))) return 0;
            fmt = 1;
        }
    }
    return id && fmt;
}

COMMON_API const char *marker_why(int err) {
    return err == EACCES || err == EPERM ? "no-permission" : err == EIO || err == ENXIO || err == ENODEV ? "io-error"
         : err == EMFILE || err == ENFILE || err == ENOMEM || err == EINTR || err == EAGAIN || err == EBUSY ? "" : "marker";
}

COMMON_API const char *marker_check_at(plat_dir d, const char *want) {
    char buf[MARKER_MAX];
    int64_t size = 0;
    plat_file m;
    int err = plat_file_open_at(d, MARKER_NAME, MARKER_MAX, &size, &m);
    if (err) return marker_why(err);
    int r = plat_read_all(m, buf, (size_t)size);
    plat_file_close(m);
    if (r) return "io-error";
    return marker_ok(buf, (size_t)size, want) ? NULL : "marker";
}
