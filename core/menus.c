/* The menu table (mac-core §3.8), static constant data from menus.def. The rules are in core.c
 * (core_menu_state). */
#include "internal.h"

enum { NONE = -1 };
#define CORE_MENU_KIND_item CORE_MENU_ITEM
#define CORE_MENU_KIND_separator CORE_MENU_SEPARATOR
#define CORE_MENU_KIND_native CORE_MENU_NATIVE
#define CORE_OSBIT_all 3
#define CORE_OSBIT_mac 1
#define CORE_OSBIT_win 2

/* in core_menu_id order: row i is item i */
const menu_row core_menu_rows[CORE_MENU_COUNT > 0 ? CORE_MENU_COUNT : 1] = {
#define MENU(id, os, menu, kind, title, key_mac, key_win, mods_mac, mods_win, event) \
    CORE_KEEP_##os([id] = {id, menu, CORE_MENU_KIND_##kind, title, key_mac, key_win, mods_mac, mods_win, event, CORE_OSBIT_##os},)
#include "menus.def"
};
const int32_t core_menu_rows_n = CORE_MENU_COUNT;

static int os_bit(core_os os) {
    if (os == CORE_OS_MAC) return CORE_OSBIT_mac;
#ifdef CORE_WIN_POLICY
    if (os == CORE_OS_WIN) return CORE_OSBIT_win;
#endif
    return 0;
}

int32_t core_menu_count(core_os os) {
    int bit = os_bit(os), n = 0;
    for (int32_t i = 0; i < core_menu_rows_n; i++) n += (core_menu_rows[i].os & bit) != 0;
    return n;
}

int32_t core_menu_at(core_os os, int32_t i, core_menu_item *out) {
    int bit = os_bit(os);
    if (!out || i < 0 || !bit) return CORE_E_ARG;
    for (int32_t k = 0; k < core_menu_rows_n; k++) {
        const menu_row *r = &core_menu_rows[k];
        if (!(r->os & bit) || i--) continue;
        out->id = r->id;
        out->menu = r->parent;
        out->kind = r->kind;
        out->title = r->title;
        out->key = os == CORE_OS_MAC ? r->key_mac : r->key_win;
        out->mods = os == CORE_OS_MAC ? r->mods_mac : r->mods_win;
        out->reserved = 0;
        out->event = r->event;
        return CORE_OK;
    }
    return CORE_E_ARG;
}

/* main.swift validateMenuItem, unchanged: the window's items as their buttons are (off while an
 * action runs); the app delegate's Help items (home page, troubleshooting, issues) aren't the
 * controller's, so nothing turns them off; a submenu's own item and the OS's items stay on. */
uint32_t menu_rule(int32_t id, const menu_facts *f, int32_t *title) {
    if ((unsigned)id >= (unsigned)CORE_MENU_COUNT) return 0;
    const menu_row *r = &core_menu_rows[id];
    const gate_out *g = &f->buttons;
    int idle = !f->action_running;
    if (title) *title = r->title;
#ifdef CORE_WIN_POLICY
    /* the first Windows release (I10, I86): no Status and Prepare tabs, so none of their rows; not
     * enabled either, so their shortcuts and menu events do nothing */
    if (f->tabs_hidden && (id == M_VIEW_STATUS || id == M_VIEW_PREPARE || id == M_PREPARE || r->parent == M_PREPARE))
        return CORE_MENU_HIDDEN;
#endif
    switch (id) {
    case M_VIEW_STATUS: case M_VIEW_PREPARE: case M_VIEW_MANAGE: case M_MANAGE_SHOW_LOG: return CORE_MENU_ENABLED;
    case M_PREP_CHECK: return idle && !f->preparing && f->prep_selected ? CORE_MENU_ENABLED : 0;
    case M_PREP_PAUSE:
        if (title) *title = f->paused ? T_MENU_RESUME : T_MENU_PAUSE;
        return f->preparing ? CORE_MENU_ENABLED : 0;
    case M_PREP_STOP: return f->preparing ? CORE_MENU_ENABLED : 0;
    case M_MANAGE_PLUS_INSTALL: return idle && g->install_plus ? CORE_MENU_ENABLED : 0;
    case M_MANAGE_CACHE_INSTALL: return idle && g->install_cache ? CORE_MENU_ENABLED : 0;
    case M_MANAGE_PLUS_REINSTALL: return idle && g->reinstall_plus ? CORE_MENU_ENABLED : 0;
    case M_MANAGE_CACHE_REINSTALL: return idle && g->reinstall_cache ? CORE_MENU_ENABLED : 0;
    case M_MANAGE_PLUS_UNINSTALL: return idle && g->uninstall_plus ? CORE_MENU_ENABLED : 0;
    case M_MANAGE_CACHE_UNINSTALL: return idle && g->uninstall_cache ? CORE_MENU_ENABLED : 0;
    case M_APP_UNINSTALL: return idle && g->uninstall_all ? CORE_MENU_ENABLED : 0;
    case M_APP_UPDATE: return (idle ? CORE_MENU_ENABLED : 0) | (f->update_visible ? 0 : CORE_MENU_HIDDEN);
    case M_HELP_HOME: case M_HELP_TROUBLESHOOTING: case M_HELP_ISSUES: return CORE_MENU_ENABLED;
    default: break;
    }
    /* an item without its own action (a menu, a submenu's item), a separator, the OS's own items */
    if (r->kind != CORE_MENU_ITEM || !*r->event) return CORE_MENU_ENABLED;
    return idle ? CORE_MENU_ENABLED : 0;   /* the controller's other items: Prepare Playlist…, Settings…, Create Report */
}
