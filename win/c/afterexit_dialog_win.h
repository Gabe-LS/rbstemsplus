#ifndef RBSP_AFTEREXIT_DIALOG_WIN_H
#define RBSP_AFTEREXIT_DIALOG_WIN_H

/* The kept dialog can open long after the starter quits. Windows keeps a popup owned by a
 * topmost window topmost independently of foreground activation. The owner stays hidden. */
static inline int ax_win_kept_message_box(const wchar_t *message, const wchar_t *title, DWORD *error) {
    HWND owner = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", NULL, WS_POPUP,
                                 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    /* If the owner couldn't be created, still show the warning with MB_TOPMOST. */
    int result = MessageBoxW(owner, message, title, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    if (!result) *error = GetLastError();
    if (owner) DestroyWindow(owner);
    return result;
}

#endif
