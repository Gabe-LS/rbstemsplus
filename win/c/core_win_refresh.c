/* Feed the shell the same registry generation the inventory cache uses. */
#include "core_win.h"
#include "compat.h"

CORE_API uint64_t CORE_CALL core_win_rekordbox_generation(void) { return compat_uninstall_generation(); }
