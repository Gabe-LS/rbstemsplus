/* win.c: the core's Windows-only pure decisions (W-core, app.md §9). Compiled into the Windows
 * builds and the host test builds, never into the Mac archive: core/build.sh lib builds $SRCS
 * only, and this file is in $WIN_SRCS. Under CORE_WIN_POLICY only, so even a Mac test build
 * compiles nothing of it. */
#include "internal.h"

#ifdef CORE_WIN_POLICY

/* The Windows in-front policy (mac-core §3.6, app.md §4.1, §4.3), from front_facts' raw bits read
 * live at the decision: the Mac's inFront() (PrepareView.swift:293: the tab's view loaded, the
 * window not minimized, the app active) with WPF's facts: "active" is an activated app, as the
 * activation aggregator sees it (Application.Activated: any of our windows, dialogs included);
 * the tab asked about is the one the window shows now (the first release creates only Manage, so
 * WINDOW_LOADED is the tab's content); and nothing is in front while a UAC elevation is in flight
 * (CORE_FRONT_ELEVATING: the secure desktop is up or the worker's exit isn't handled yet). Windows
 * has no privacy prompt to keep off rekordbox, but the same rule keeps a playlist's check from
 * reading a drive while the DJ is in rekordbox. held: the held click's check (PrepareView.swift:670)
 * also needs no dialog up (modal_open). 1 in front, 0 not. */
int win_in_front(uint32_t front, int32_t modal, int32_t tab, int held) {
    if (!(front & CORE_FRONT_ACTIVE) || !(front & CORE_FRONT_WINDOW_LOADED)) return 0;
    if (front & (CORE_FRONT_MINIATURIZED | CORE_FRONT_ELEVATING)) return 0;
    if ((int32_t)((front & CORE_FRONT_TAB_MASK) >> CORE_FRONT_TAB_SHIFT) != tab) return 0;
    return !(held && modal);
}

#else
typedef int core_win_c_is_windows_only;   /* ISO C: a translation unit isn't empty */
#endif
