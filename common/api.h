/* The linkage of every function common/ declares (bridge.md §0a).
 *
 * Compiled on its own (the helper, the core, the Windows worker), a common/ source gives its
 * functions external linkage. The bridge stays one translation unit: it defines COMMON_API as
 * `static __attribute__((unused))` and #includes the common/ sources it uses, so their functions
 * are static there (fuzz.c still reaches them, the compiler may inline the thin Mac wrappers, and
 * the ones it doesn't call are left out of the dylib). */
#ifndef RBSP_COMMON_API_H
#define RBSP_COMMON_API_H

#ifndef COMMON_API
#define COMMON_API
#endif

#endif
