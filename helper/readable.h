/* readable.c: opening a track without waiting forever (requirement 18). In ~/Documents, ~/Desktop
 * and ~/Downloads macOS asks the user before the first read ("RB Stems Plus would like to access
 * files in your Documents folder"), naming the app, and the read waits until it is answered,
 * however long. Here the stat and open run on their own thread: a read that doesn't return in
 * time is given up (the thread is left to finish on its own) and the folder is remembered, so its
 * other tracks don't wait again. */
#ifndef RBSP_READABLE_H
#define RBSP_READABLE_H
#include <stddef.h>
#include "plat_helper.h"

typedef enum { READ_OK, READ_ABSENT, READ_ASKING, READ_DENIED, READ_FAILED } read_state;

/* whether PATH can be read, waiting at most SECS seconds; *st its kind, size and time when READ_OK,
 * else why (a sentence) and *folder the protected folder concerned ("Documents", "Desktop",
 * "Downloads"; "" elsewhere) */
read_state readable(const char *path, double secs, plat_finfo *st, char *why, size_t n, const char **folder);

/* the protected folder PATH is in ("" when none), from its path alone: nothing is read */
const char *protected_folder(const char *path);

/* this account's home folder, from the user database, never $HOME ("" if there is none): plat_home */
const char *home_folder(void);

#endif
