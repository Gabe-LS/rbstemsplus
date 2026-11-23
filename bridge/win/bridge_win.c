/* The Windows bridge's one translation unit: the Mac bridge's own code (bridge/rbstems_bridge.c,
 * its _WIN32 branches) with common/plat_win.c and the entry points (entry_win.c, which that file
 * includes after the platform layer). Every common/ function is static here, as in the Mac bridge
 * (common/api.h), so the ones it doesn't call are left out of the DLL. bridge/win/build.sh builds
 * it. RBSP_WIN_ENTRY_ONLY (-D) builds the entry points alone, with entry_win.c's stand-in core
 * that caches nothing, as before rbstems_bridge.c built for Windows. */
#ifdef RBSP_WIN_ENTRY_ONLY
#define COMMON_API static __attribute__((unused))
#include "../../common/plat_win.c"
#include "entry_win.c"
#else
#include "../rbstems_bridge.c"
#include "../../win/c/volumes.c"
#include "../../win/c/volumes_win.c"
#endif
