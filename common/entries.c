/* The cache's layout and size rule (entries.h), moved from the bridge. */
#include "entries.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

COMMON_API int entries_is_hex(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

COMMON_API int entries_is_pid_tmp(const char *s) {
    if (*s++ != '.' || !isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s)) s++;
    return !strcmp(s, ".tmp");
}

COMMON_API int entries_is_memo_tmp(const char *n) {
    if (strncmp(n, "model-", 6) || !isdigit((unsigned char)n[6])) return 0;
    for (n += 6; isdigit((unsigned char)*n); n++) {}
    return !strncmp(n, ".sha256", 7) && entries_is_pid_tmp(n + 7);
}

COMMON_API int entries_layout(const char *n, int level, int dir, int file, const char *parent, int memos) {
    size_t len = strlen(n);
    if (file && n[0] == '.' && n[1] == '_') {               /* a companion: of a folder, an entry or an entry's temp */
        if (level < 3) {
            int k = entries_layout(n + 2, level, 1, 0, parent, memos);
            return k == ENT_MODEL_DIR || k == ENT_PREFIX_DIR ? ENT_COMP_DIR : ENT_OTHER;
        }
        int k = entries_layout(n + 2, level, 0, 1, parent, memos);
        return k == ENT_FLAC ? ENT_COMP_FLAC : k == ENT_DESC ? ENT_COMP_DESC : k == ENT_TMP ? ENT_COMP_TMP : ENT_OTHER;
    }
    if (level == 1) {                                       /* <model sha256>/, model-<inode>.sha256.<pid>.tmp */
        if (dir) return len == 64 && entries_is_hex(n, 64) ? ENT_MODEL_DIR : ENT_OTHER;
        return file && memos && entries_is_memo_tmp(n) ? ENT_MEMO_TMP : ENT_OTHER;
    }
    if (level == 2) return dir && len == 2 && entries_is_hex(n, 2) ? ENT_PREFIX_DIR : ENT_OTHER;   /* <key[0:2]>/ */
    if (level != 3 || !file || len < 69 || !parent || strlen(parent) != 2 || !entries_is_hex(n, 64) || strncmp(n, parent, 2))
        return ENT_OTHER;
    if (!strcmp(n + 64, ".flac")) return ENT_FLAC;          /* <key>.flac, <key>.desc, either's .<pid>.tmp */
    if (!strcmp(n + 64, ".desc")) return ENT_DESC;
    return (!strncmp(n + 64, ".flac", 5) || !strncmp(n + 64, ".desc", 5)) && entries_is_pid_tmp(n + 69) ? ENT_TMP : ENT_OTHER;
}

COMMON_API void entries_file(char *out, size_t n, const char *key, int i) {
    snprintf(out, n, "%s%s%s", i >= 2 ? "._" : "", key, i % 2 ? ".desc" : ".flac");   /* %s.flac %s.desc ._%s.flac ._%s.desc */
}

COMMON_API long long entries_size_at(plat_dir x, const char *key) {
    long long sum = 0;
    for (int i = 0; i < ENTRIES_FILES; i++) {
        char name[80];
        plat_stat st;
        entries_file(name, sizeof name, key, i);
        if (plat_stat_at(x, name, &st) == 0 && st.kind == PLAT_FILE) sum += st.alloc;
    }
    return sum;
}
