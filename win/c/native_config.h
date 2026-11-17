/* native_config.c: config.ini's writer and configSets over the file's bytes (Config.swift's
 * readConfigBytes, configLines, configSets and writeConfigValues), for the core DLL's CONFIG_WRITE
 * and CONFIG_SETS. The reader is common/cfg_ini.c's (the same grammar); the writer becomes the
 * core's once the Mac's switches (mac-core M5). On Windows config.ini is
 * %LOCALAPPDATA%\rbstemsplus\config.ini (bridge.md §0). */
#ifndef RBSP_WIN_NATIVE_CONFIG_H
#define RBSP_WIN_NATIVE_CONFIG_H

#include <stddef.h>

typedef struct {
    const char *key;     /* lower case, as written */
    const char *value;   /* as written ("1.5", "\"D:\\Stems\"") */
} native_config_pair;

/* readConfigBytes' rule for bytes read whole: 1 readable (at most CFG_MAX bytes, no NUL, strict
 * UTF-8), 0 unreadable */
int native_config_readable(const char *b, size_t n);

/* configSets: whether a line in our scope (the top, [cache]) of readable bytes sets key (lower
 * case), whatever its value. b NULL: no file, 0 */
int native_config_sets(const char *b, size_t n, const char *key);

/* writeConfigValues' new text for readable bytes b (NULL: no file): each of values' keys replaced
 * on every line of it in our scope, or added before the first [section]; remove's keys lose every
 * line of theirs in our scope; every other line kept as it is, a byte order mark kept at offset 0;
 * when the location (cache_dir, cache_id) is written or removed over a file with a section line
 * over the limit, that line becomes "[unreadable-section]" and location keys outside our scope go.
 * 0 with *out (malloc'd; free it) and *out_n, or -1 (no memory, or the result would be over CFG_MAX). */
int native_config_rewrite(const char *b, size_t n, const native_config_pair *values, int nvalues, const char *const *remove, int nremove,
                          char **out, size_t *out_n);

/* CONFIG_WRITE's arguments (key, value, ...) as pairs: an even count, 2 to 16; rekordbox_model
 * as given; max_gb and max_days a number as typed with "." for a decimal mark (Double(value):
 * finite, nothing before or after it), written as configNumber writes it, into nums[i]; any other
 * key refused. The count of pairs (their strings point into args and nums), or -1 (review S46) */
int native_config_pairs(const char *const *args, int nargs, native_config_pair pairs[8], char nums[8][32]);

#endif
