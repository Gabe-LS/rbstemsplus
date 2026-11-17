/* kept.c: kept sets (a prepared playlist's entries, kept until a date) and the cache's room.
 * The file format is the bridge's (bridge/rbstems_bridge.c, "kept sets"): the bridge reads the
 * files, this helper writes them and deletes those whose date has passed. Folders, files and
 * sizes go through common/'s file layer and the cache's layout and size rule (common/entries.h). */
#ifndef RBSP_KEPT_H
#define RBSP_KEPT_H
#include <stddef.h>
#include <time.h>

#define KEPT_SETS_MAX 5                 /* the bridge keeps nothing past 5 sets */
#define KEPT_SHARE 0.75                 /* of max_gb: what kept sets may take together */

typedef struct kept kept;

/* ~/Library/Application Support/rbstemsplus/kept (the account's home from the user database, as
 * the bridge's: plat_home; Windows: LocalAppData's rbstemsplus\kept); "" when there is no home */
void kept_default_dir(char *buf, size_t n);

/* set NAME ([A-Za-z0-9_-], 1 to 64) in folder DIR (made if missing), kept until UNTIL. Sets whose
 * date has passed are deleted; with KEPT_SETS_MAX others still kept it is refused. The other sets'
 * entries are loaded (they count towards the share and are never removed to make room), and so
 * is this set's own earlier file: a run that stops leaves its entries in the set, and they count
 * towards the share only as a track holds them (kept_hold, kept_add). NAME NULL: no set
 * of this run's, the others only (none without the folder). CACHE: the folder holding the model
 * folders (for the sizes; "" for none: every size 0). NULL and *why on failure. */
kept *kept_open(const char *dir, const char *name, time_t until, const char *cache, char *why, size_t n);
void kept_close(kept *k);

/* entry MODEL/KEY (64 hex digits each) becomes this set's; its disk space (BYTES) counts towards
 * the share once, unless it is counted already (a kept entry that had left the cache when the
 * sets were read isn't, and counts when it is written again) */
void kept_add(kept *k, const char *model, const char *key, long long bytes);

/* an entry of the set's earlier file that a track of this run still holds: kept_release leaves it
 * protected. COUNT: its disk space counts now (a track the run won't reach); a track it reaches
 * counts as it goes (kept_add) */
void kept_hold(kept *k, const char *model, const char *key, int count);

/* the track holding entry KEY has changed: it may be removed to make room, unless the run finds it
 * again (a re-tagged file has the same entries). It stays in the set's file until a run finishes */
void kept_unhold(kept *k, const char *key);

/* the disk space that counting entry MODEL/KEY would add: 0 when it counts already */
long long kept_adds(const kept *k, const char *model, const char *key);

/* the set's earlier entries no track of this run holds (it left the playlist or moved, another
 * model's) may be removed to make room. They leave the set's file when a run finishes: a run that
 * stops leaves them there, as they may be a moved track's */
void kept_release(kept *k);

/* entry MODEL/KEY's disk space in CACHE (its .flac, .desc and their companions), or -1 when its
 * FLAC isn't there */
long long kept_entry_bytes(const char *cache, const char *model, const char *key);

/* whether entry KEY (64 hex digits) is in a kept set already (another set's, this set's earlier
 * file, or this run's): keeping it again adds nothing */
int kept_has(const kept *k, const char *key);

/* the disk space of every kept set's entries (this run's included) */
long long kept_bytes(const kept *k);

/* writes the set's file (renamed into place whole): this run's entries, and with ALL_DONE == 0
 * also the earlier file's (a run that stopped early keeps what it didn't redo). 0, or -1 and *why */
int kept_write(kept *k, int all_done, char *why, size_t n);

/* entries in no kept set, least recently used first, are removed from CACHE until it takes at
 * most TARGET bytes (*used: its size now, updated). K may be NULL (no set is kept). Returns the
 * number of entries removed. Only <model>/<xx>/<key>.flac|.desc and their "._" companions. */
long long make_room(const char *cache, const kept *k, long long *used, long long target);

#endif
