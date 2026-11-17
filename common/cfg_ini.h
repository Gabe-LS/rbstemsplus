/* config.ini's reader (bridge.md §0a), moved from the bridge's parse_config: one grammar for the
 * bridge and, once it switches, the app's core (tests/config-fixtures.txt is the oracle for both).
 * The writer is the core's and lands with its switch (mac-core M5).
 *
 * The grammar, the same as the app's reader (app/Sources/Config.swift):
 *  - the whole file at once: at most CFG_MAX bytes, valid UTF-8, no NUL. A file that breaks that,
 *    or that can't be read for any reason but not existing, is unreadable: nothing is cached (the
 *    app says so, and won't rewrite it). No file: the defaults, stems saved on this Mac.
 *  - a UTF-8 byte order mark only at offset 0; lines end at "\n" and hold at most CFG_LINE_MAX
 *    bytes (a "\r" counts); leading spaces, tabs and "\r" are skipped; '#' starts a comment line.
 *  - ours are the top (before any [section]) and [cache]: the name between '[' and the first ']',
 *    spaces and tabs around it and case ignored. Other sections are someone else's ([watcher] has
 *    an enabled of its own). An over-long section line leaves its scope unknown: not ours.
 *  - key=value: the key is what comes before the first '=', case ignored; the value is quoted
 *    ("..." or '...', ending at its own quote) or ends at the first space, tab, "\r" or '#'. An
 *    over-long line's key still counts, never its value.
 *  - enabled, rekordbox_model, near_hits, max_gb, max_days: unknown keys are ignored; a bad or
 *    out-of-range value is ignored with a log line, and so is an over-long line.
 *  - cache_dir and cache_id: a chosen location (paths_location_ok_os: the OS's form; cfg_uuid_ok). Either key in our
 *    scope means one is chosen, and then both must be there, valid, and the same on every line,
 *    or nothing is cached: a mistake never means this Mac. So does an over-long section line in a
 *    file that has either key anywhere (its scope can't be told). */
#ifndef RBSP_CFG_INI_H
#define RBSP_CFG_INI_H

#include "api.h"
#include "path_rules.h"

#include <stddef.h>

#define CFG_MAX 65536
#define CFG_LINE_MAX 1024

typedef struct {
    int enabled, rekordbox_model, near_hits;
    long long max_bytes;
    double max_days;
    int location;                       /* 0: this Mac; 1: a chosen location; -1: one is set, wrongly */
    char dir[PATHS_LOCATION_MAX + 1], id[37];
} cfg_config;

/* where the reader's notes go (an ignored value or line), printf-style; NULL: nowhere */
typedef void (*cfg_log_fn)(const char *fmt, ...);

/* config.ini's bytes into c, which holds the defaults; -1: unreadable (a NUL, invalid UTF-8) */
COMMON_API int cfg_parse(const char *b, size_t n, cfg_config *c, cfg_log_fn log);
/* UTF-8 in its shortest forms, no surrogates, at most U+10FFFF */
COMMON_API int cfg_utf8_ok(const unsigned char *s, size_t n);
/* 8-4-4-4-12 hex digits, either case */
COMMON_API int cfg_uuid_ok(const char *s);
/* 1 or 0 for a yes/no value, -1 when it's neither */
COMMON_API int cfg_yes_no(const char *v);

#endif
