/* watch.c and watch_facts.c on the Mac (app.md §5.7, §7): the notifications' rules, the folders'
 * handles on a fake file system (gone, replaced, renamed away, overflowing, re-created below a
 * missing parent), the 10 s schedule, the check's rounds and answers on fake operations (with the
 * core's Mac column for words, and its Windows column, not written yet), and the facts on a
 * scratch tree through plat_mac.c and a fake compat_os.
 *
 *   winc-watch-test SCRATCH PE     SCRATCH: an empty folder; PE: run.sh's PE fixtures */
#include "check.h"

#include "../watch.h"
#include "core.h"
#include "sig_mac.h"

#include <CommonCrypto/CommonDigest.h>

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

int checks, failures;

/* ------------------------------------------------------------------ the rules */

static void test_classify(void) {
    const char *cur = "rekordbox 7.2.19";
    int RR = WATCH_ROUND | WATCH_R1;
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_ADDED, "rekordbox 7.2.20") == RR, "a new version folder: an update");
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_REMOVED, "rekordbox 7.2.19") == RR, "the current one removed");
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_RENAMED_OLD, "rekordbox 7.2.18") == RR, "an old one renamed");
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_REMOVED, "rekordbox 7.2.19\\onnxruntime.dll") == RR, "the live DLL removed (I72)");
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_MODIFIED, "Rekordbox 7.2.19\\ONNXRUNTIME.DLL") == RR, "case-blind");
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_MODIFIED, "rekordbox 7.2.19") == 0, "the current folder's own times");
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_MODIFIED, "rekordbox 7.2.18") == 0, "another folder's times");
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_REMOVED, "rekordbox 7.2.18\\onnxruntime.dll") == RR, "another version folder's DLL");
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_ADDED, "rekordbox 7.2.19\\plugins\\x.dll") == 0, "deeper than the folder");
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_ADDED, "readme.txt") == 0 && watch_classify(WD_REKORDBOX, cur, WATCH_ADDED, "rekordbox 7.2") == 0,
          "not a version folder");
    CHECK(watch_classify(WD_REKORDBOX, "", WATCH_REMOVED, "rekordbox 7.2.18\\onnxruntime.dll") == RR, "the current one unknown: any");
    CHECK(watch_classify(WD_REKORDBOX, cur, WATCH_ADDED, "") == 0 && watch_classify(WD_REKORDBOX, cur, WATCH_ADDED, NULL) == 0, "no name");
    CHECK(watch_classify(WD_ROOT, cur, WATCH_REMOVED, "ort") == WATCH_R1, "ort\\ removed: R1 only");
    CHECK(watch_classify(WD_ROOT, cur, WATCH_ADDED, "ort") == WATCH_R1, "ort\\ made again");
    CHECK(watch_classify(WD_ROOT, cur, WATCH_REMOVED, "ORT\\onnxruntime.dll") == WATCH_R1, "ort\\'s copy removed");
    CHECK(watch_classify(WD_ROOT, cur, WATCH_ADDED, "ort\\a\\b") == 0 && watch_classify(WD_ROOT, cur, WATCH_ADDED, "ortx") == 0
              && watch_classify(WD_ROOT, cur, WATCH_ADDED, "bin\\0123456789ab\\RB Stems Plus.exe") == 0
              && watch_classify(WD_ROOT, cur, WATCH_MODIFIED, "lock") == 0,
          "the rest of the root");
    /* %APPDATA%\Pioneer with its subtree: the settings folder by names, the model folder by names
     * and writes, and the folders on the way to them */
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_ADDED, "rekordbox6\\master.db-wal") == WATCH_ROUND, "rekordbox opening: a round, no R1");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_RENAMED_NEW, "REKORDBOX6\\x") == WATCH_ROUND, "case-blind");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_MODIFIED, "rekordbox6\\master.db") == 0, "the settings folder: its writes don't count");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_ADDED, "rekordbox6\\share\\a.dat") == 0, "settings: its own names only");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_MODIFIED, "rekordbox6\\models\\demucs3_model\\hdemucs.onnx") == WATCH_ROUND, "the model written");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_RENAMED_NEW, "rekordbox6\\models\\Demucs3_Model\\hdemucs.onnx") == WATCH_ROUND,
          "the model renamed in");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_ADDED, "rekordbox6\\models\\demucs3_model\\a\\b") == 0, "the model folder: its own names only");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_ADDED, "rekordbox6\\models\\other_model\\x") == 0
              && watch_classify(WD_PIONEER, cur, WATCH_MODIFIED, "rekordbox6\\models\\x") == 0,
          "another model folder");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_REMOVED, "rekordbox6") == WATCH_ROUND
              && watch_classify(WD_PIONEER, cur, WATCH_RENAMED_OLD, "rekordbox6") == WATCH_ROUND
              && watch_classify(WD_PIONEER, cur, WATCH_ADDED, "rekordbox6\\models") == WATCH_ROUND
              && watch_classify(WD_PIONEER, cur, WATCH_REMOVED, "rekordbox6\\models\\demucs3_model") == WATCH_ROUND,
          "the settings, models\\ or model folder added, removed or renamed");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_MODIFIED, "rekordbox6") == 0
              && watch_classify(WD_PIONEER, cur, WATCH_MODIFIED, "rekordbox6\\models\\demucs3_model") == 0,
          "their own times don't count");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_ADDED, "rekordbox") == 0 && watch_classify(WD_PIONEER, cur, WATCH_ADDED, "rekordbox6x") == 0
              && watch_classify(WD_PIONEER, cur, WATCH_ADDED, "rekordbox6x\\a") == 0
              && watch_classify(WD_PIONEER, cur, WATCH_ADDED, "rekordbox\\share\\PIONEER\\USBANLZ\\a.DAT") == 0,
          "the rest of Pioneer\\");
    /* a short (8.3) name in a note's path stands for any name (review VF1) */
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_MODIFIED, "REKORD~1\\models\\demucs3_model\\x.onnx") == WATCH_ROUND
              && watch_classify(WD_PIONEER, cur, WATCH_MODIFIED, "rekordbox6\\models\\DEMUCS~1\\x.onnx") == WATCH_ROUND,
          "a model write through a short name");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_RENAMED_OLD, "rekordbox6\\models\\DEMUCS~1") == WATCH_ROUND
              && watch_classify(WD_PIONEER, cur, WATCH_REMOVED, "REKORD~1") == WATCH_ROUND,
          "a folder on the way renamed or removed by its short name");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_ADDED, "REKORD~1\\x") == WATCH_ROUND, "a name added in the settings folder by its short name");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_MODIFIED, "REKORD~1\\master.db") == 0 && watch_classify(WD_PIONEER, cur, WATCH_ADDED, "REKORD~1\\x\\y") == 0,
          "the settings folder by its short name: names only, its own");
    CHECK(watch_classify(WD_PIONEER, cur, WATCH_ADDED, "rekordbox6~x\\a") == 0, "a ~ without a digit is a long name");
    CHECK(watch_subtree(WD_REKORDBOX) && watch_subtree(WD_PIONEER) && watch_subtree(WD_ROOT), "every folder with its subtree");
    CHECK(watch_filter(WD_PIONEER) == (WATCH_F_NAMES | WATCH_F_WRITES) && watch_filter(WD_REKORDBOX) == (WATCH_F_NAMES | WATCH_F_WRITES), "filters");

    CHECK(watch_os_refusal(22621, 533320) == NULL && watch_os_refusal(26200, 533325) == NULL, "the minimum is enough");
    CHECK(watch_os_refusal(0, 533320) && watch_os_refusal(22000, 533320) && watch_os_refusal(22631, 528449) && watch_os_refusal(22631, 0),
          "below it: refused");
    char b[128];
    CHECK(watch_log_line(b, sizeof b, 2026, 11, 2, 3, 4, 5, "nothing to ask") == 36 && !strcmp(b, "2026-11-02 03:04:05  nothing to ask\n"),
          "appendLog's line: %s", b);
    CHECK(watch_stop_event_name(4242, b, sizeof b) && !strcmp(b, "Local\\io.github.rbstemsplus.watcher.stop.4242"), "the stop event (W3a)");
    CHECK(!watch_stop_event_name(1, b, 10), "too small");
    CHECK(watch_stub_path("C:\\Users\\u\\AppData\\Local\\Programs\\RB Stems Plus\\1.4.0\\rbsp-watch-arm64.exe", b, sizeof b)
              && !strcmp(b, "C:\\Users\\u\\AppData\\Local\\Programs\\RB Stems Plus\\RB Stems Plus.exe"),
          "the stub: %s", b);
    CHECK(watch_stub_path("C:\\1.4.0\\rbsp-watch-x64.exe", b, sizeof b) && !strcmp(b, "C:\\RB Stems Plus.exe"), "at a drive's root: %s", b);
    CHECK(!watch_stub_path("rbsp-watch-x64.exe", b, sizeof b), "no folder");
    compat_r1 r;
    memset(&r, 0, sizeof r);
    r.live = COMPAT_LIVE_OURS;
    CHECK(watch_r1_key("7.2.19", &r, b, sizeof b) && !strcmp(b, "r1:7.2.19 live=ours ort=0"), "R1's key: %s", b);
}

/* ------------------------------------------------------------------ the folders, on a fake file system */

typedef struct {
    char dirs[32][256];   /* the folders there */
    int ids[32];
    int n, next_id;
    char slot_path[WATCH_SLOTS][256];
    int slot_id[WATCH_SLOTS], slot_sub[WATCH_SLOTS], slot_filter[WATCH_SLOTS], opened[WATCH_SLOTS];
    int refuse;           /* open refuses this path index + 1 (EACCES) */
    int opens, closes;    /* handles opened and closed */
    char log[4096];
} fakefs;

static int fs_find(fakefs *f, const char *p) {
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->dirs[i], p)) return i;
    return -1;
}
static void fs_mk(fakefs *f, const char *p) {
    int i = fs_find(f, p);
    if (i < 0) {
        i = f->n++;
        snprintf(f->dirs[i], sizeof f->dirs[i], "%s", p);
    }
    f->ids[i] = ++f->next_id;
}
static void fs_rm(fakefs *f, const char *p) {
    int i = fs_find(f, p);
    if (i >= 0) f->dirs[i][0] = 0;
}
/* a rename of p keeps its folders' ids (an open handle follows it) */
static void fs_mv(fakefs *f, const char *p, const char *to) {
    size_t pn = strlen(p);
    for (int i = 0; i < f->n; i++)
        if (!strncmp(f->dirs[i], p, pn) && (!f->dirs[i][pn] || f->dirs[i][pn] == '\\')) {
            char t[256];
            snprintf(t, sizeof t, "%s%s", to, f->dirs[i] + pn);
            snprintf(f->dirs[i], sizeof f->dirs[i], "%s", t);
        }
}
/* NTFS's rule: a folder can't be renamed while a handle is open anywhere below it (its own
 * handle, shared for delete, doesn't stop it) */
static int fs_renamable(const fakefs *f, const char *p) {
    size_t pn = strlen(p);
    for (int s = 0; s < WATCH_SLOTS; s++)
        if (f->opened[s] && !strncmp(f->slot_path[s], p, pn) && f->slot_path[s][pn] == '\\') return 0;
    return 1;
}
static int fs_open(void *ctx, int slot, const char *path, int subtree, int filter) {
    fakefs *f = ctx;
    int i = fs_find(f, path);
    if (i < 0) return ENOENT;
    if (f->refuse == i + 1) return EACCES;
    CHECK(!f->opened[slot], "slot %d opened twice (%s)", slot, path);
    f->opened[slot] = 1;
    snprintf(f->slot_path[slot], sizeof f->slot_path[slot], "%s", path);
    f->slot_id[slot] = f->ids[i];
    f->slot_sub[slot] = subtree;
    f->slot_filter[slot] = filter;
    f->opens++;
    return 0;
}
static void fs_close(void *ctx, int slot) {
    fakefs *f = ctx;
    CHECK(f->opened[slot], "slot %d closed while closed", slot);
    f->opened[slot] = 0;
    f->closes++;
}
static int fs_same(void *ctx, int slot, const char *path) {
    fakefs *f = ctx;
    int i = fs_find(f, path);
    return i >= 0 && f->ids[i] == f->slot_id[slot];
}
static void fs_log(void *ctx, const char *line) {
    fakefs *f = ctx;
    size_t n = strlen(f->log);
    snprintf(f->log + n, sizeof f->log - n, "%s\n", line);
}

static void test_engine(void) {
    fakefs f;
    memset(&f, 0, sizeof f);
    const char *P = "C:\\Program Files", *RB = "C:\\Program Files\\rekordbox", *ROOT = "C:\\Program Files\\RB Stems Plus";
    const char *ROAM = "C:\\Users\\u\\AppData\\Roaming", *PIO = "C:\\Users\\u\\AppData\\Roaming\\Pioneer";
    fs_mk(&f, "C:\\");
    fs_mk(&f, P);
    fs_mk(&f, RB);
    fs_mk(&f, "C:\\Users\\u\\AppData");
    fs_mk(&f, ROAM);
    watch_fs ops = {&f, fs_open, fs_close, fs_same, fs_log};
    const char *paths[WD_COUNT] = {RB, PIO, ROOT};
    watch_engine e;
    watch_engine_init(&e, &ops, paths);
    int fl = watch_engine_tick(&e);
    CHECK(fl == 0, "the first look asks for nothing: %d", fl);
    CHECK(e.open[WD_REKORDBOX] && f.slot_sub[WD_REKORDBOX] && f.slot_filter[WD_REKORDBOX] == (WATCH_F_NAMES | WATCH_F_WRITES),
          "rekordbox's folder is open with its subtree");
    CHECK(!e.open[WD_ROOT] && e.open[WD_ROOT + WD_COUNT] && !strcmp(e.anchor[WD_ROOT], P) && !f.slot_sub[WD_ROOT + WD_COUNT]
              && f.slot_filter[WD_ROOT + WD_COUNT] == WATCH_F_NAMES,
          "the root is missing: its nearest ancestor is watched, names only (%s)", e.anchor[WD_ROOT]);
    CHECK(!e.open[WD_PIONEER] && !strcmp(e.anchor[WD_PIONEER], ROAM), "Pioneer\\ missing: Roaming watched (%s)", e.anchor[WD_PIONEER]);
    CHECK(strstr(f.log, "isn't there: watching") != NULL, "logged");

    /* the root appears below its anchor */
    watch_note unrel = {WATCH_ADDED, "unrelated"}, rootn = {WATCH_RENAMED_NEW, "rb stems plus"};
    CHECK(watch_engine_read(&e, WD_ROOT + WD_COUNT, WATCH_READ_OK, &unrel, 1) == 0 && !strcmp(e.anchor[WD_ROOT], P), "an unrelated name");
    fs_mk(&f, ROOT);
    fl = watch_engine_read(&e, WD_ROOT + WD_COUNT, WATCH_READ_OK, &rootn, 1);
    CHECK(fl == WATCH_R1 && e.open[WD_ROOT] && f.slot_sub[WD_ROOT] && !e.open[WD_ROOT + WD_COUNT] && !e.anchor[WD_ROOT][0],
          "the root is there (case-blind): open with its subtree, R1");
    /* Pioneer\ appears: one handle on it, with its subtree, whatever is below it */
    fs_mk(&f, PIO);
    watch_note pion = {WATCH_ADDED, "Pioneer"};
    fl = watch_engine_read(&e, WD_PIONEER + WD_COUNT, WATCH_READ_OK, &pion, 1);
    CHECK(fl == WATCH_ROUND && e.open[WD_PIONEER] && f.slot_sub[WD_PIONEER] && f.slot_filter[WD_PIONEER] == (WATCH_F_NAMES | WATCH_F_WRITES)
              && !e.open[WD_PIONEER + WD_COUNT],
          "Pioneer\\ made: open with its subtree, a round");
    watch_note below[3] = {{WATCH_ADDED, "rekordbox6"}, {WATCH_ADDED, "rekordbox6\\models"}, {WATCH_ADDED, "rekordbox6\\models\\demucs3_model"}};
    fs_mk(&f, "C:\\Users\\u\\AppData\\Roaming\\Pioneer\\rekordbox6");
    fs_mk(&f, "C:\\Users\\u\\AppData\\Roaming\\Pioneer\\rekordbox6\\models");
    fs_mk(&f, "C:\\Users\\u\\AppData\\Roaming\\Pioneer\\rekordbox6\\models\\demucs3_model");
    int opens = f.opens;
    fl = watch_engine_read(&e, WD_PIONEER, WATCH_READ_OK, below, 3);
    CHECK(fl == WATCH_ROUND && f.opens == opens, "the settings and model folders made below it: a round, no other handle");
    /* the rename that VM round 3 saw refused: no handle is held below Pioneer\ (NTFS refuses to
     * rename a folder with a handle open below it) */
    CHECK(fs_renamable(&f, PIO) && fs_renamable(&f, "C:\\Users\\u\\AppData\\Roaming\\Pioneer\\rekordbox6")
              && fs_renamable(&f, "C:\\Users\\u\\AppData\\Roaming\\Pioneer\\rekordbox6\\models"),
          "Pioneer\\, rekordbox6\\ and models\\ can be renamed while the watcher runs");
    fs_mv(&f, PIO, "C:\\Users\\u\\AppData\\Roaming\\Pioneer-old");
    fl = watch_engine_tick(&e);
    CHECK(fl == WATCH_ROUND && !e.open[WD_PIONEER] && !strcmp(e.anchor[WD_PIONEER], ROAM),
          "Pioneer\\ renamed away: the tick sees it, a round, Roaming watched (%s)", e.anchor[WD_PIONEER]);
    fs_mv(&f, "C:\\Users\\u\\AppData\\Roaming\\Pioneer-old", PIO);
    watch_note back = {WATCH_RENAMED_NEW, "Pioneer"};
    fl = watch_engine_read(&e, WD_PIONEER + WD_COUNT, WATCH_READ_OK, &back, 1);
    CHECK(fl == WATCH_ROUND && e.open[WD_PIONEER] && f.slot_id[WD_PIONEER] == f.ids[fs_find(&f, PIO)], "renamed back: watched again, a round");

    /* notes through a folder's read */
    watch_engine_set_current(&e, "C:\\Program Files\\rekordbox\\rekordbox 7.2.19");
    CHECK(!strcmp(e.current, "rekordbox 7.2.19"), "the current folder's name");
    watch_note notes[3] = {{WATCH_MODIFIED, "rekordbox 7.2.18\\x.dll"}, {WATCH_REMOVED, "rekordbox 7.2.19\\onnxruntime.dll"}, {WATCH_ADDED, "x"}};
    fl = watch_engine_read(&e, WD_REKORDBOX, WATCH_READ_OK, notes, 1);
    CHECK(fl == 0 && e.open[WD_REKORDBOX], "another version folder: nothing, the watch stays");
    fl = watch_engine_read(&e, WD_REKORDBOX, WATCH_READ_OK, notes, 3);
    CHECK(fl == (WATCH_ROUND | WATCH_R1) && e.open[WD_REKORDBOX], "the live DLL: a round with R1's check");
    watch_note ort = {WATCH_REMOVED, "ort\\onnxruntime.dll"};
    CHECK(watch_engine_read(&e, WD_ROOT, WATCH_READ_OK, &ort, 1) == WATCH_R1, "ort\\'s copy: R1");

    /* rekordbox's folder removed by its uninstaller: closed at once, its parent watched */
    fs_rm(&f, RB);
    fl = watch_engine_read(&e, WD_REKORDBOX, WATCH_READ_GONE, NULL, 0);
    CHECK(fl == (WATCH_ROUND | WATCH_R1) && !e.open[WD_REKORDBOX] && !f.opened[WD_REKORDBOX] && !strcmp(e.anchor[WD_REKORDBOX], P),
          "gone: closed, Program Files watched (%s)", e.anchor[WD_REKORDBOX]);
    CHECK(watch_engine_read(&e, WD_REKORDBOX, WATCH_READ_OK, notes, 3) == 0, "a late read of the closed slot: nothing");
    fs_mk(&f, RB);
    watch_note rbn = {WATCH_ADDED, "rekordbox"};
    fl = watch_engine_read(&e, WD_REKORDBOX + WD_COUNT, WATCH_READ_OK, &rbn, 1);
    CHECK(fl == (WATCH_ROUND | WATCH_R1) && e.open[WD_REKORDBOX] && !e.open[WD_REKORDBOX + WD_COUNT], "installed again: watched again");

    /* replaced (a new folder of that name) or renamed away while open: seen at the next read */
    fs_mk(&f, PIO);   /* a new id */
    watch_note db = {WATCH_MODIFIED, "rekordbox6\\master.db"};
    fl = watch_engine_read(&e, WD_PIONEER, WATCH_READ_OK, &db, 1);
    CHECK(fl == WATCH_ROUND && e.open[WD_PIONEER] && f.slot_id[WD_PIONEER] == f.ids[fs_find(&f, PIO)], "replaced: closed and opened anew");
    fl = watch_engine_read(&e, WD_PIONEER, WATCH_READ_OK, &db, 1);
    CHECK(fl == 0 && e.open[WD_PIONEER], "the database written: nothing");
    fs_rm(&f, PIO);
    fl = watch_engine_tick(&e);
    CHECK(fl == WATCH_ROUND && !e.open[WD_PIONEER] && !strcmp(e.anchor[WD_PIONEER], ROAM), "removed: the tick sees it, its parent watched (%d)", fl);
    fs_mk(&f, PIO);
    fl = watch_engine_tick(&e);
    CHECK(fl == WATCH_ROUND && e.open[WD_PIONEER], "back at the next tick, even with its notification missed");

    /* an overflow: closed, opened again, everything may have changed */
    int id = f.slot_id[WD_ROOT];
    fs_mk(&f, ROOT);
    fl = watch_engine_read(&e, WD_ROOT, WATCH_READ_OVERFLOW, NULL, 0);
    CHECK(fl == WATCH_R1 && e.open[WD_ROOT] && f.slot_id[WD_ROOT] != id, "overflow: R1, the handle opened anew");
    /* refused (ERROR_ACCESS_DENIED): closed, watched from its parent */
    f.refuse = fs_find(&f, ROOT) + 1;
    fl = watch_engine_read(&e, WD_ROOT, WATCH_READ_ERROR, NULL, 0);
    CHECK(fl == WATCH_R1 && !e.open[WD_ROOT] && !strcmp(e.anchor[WD_ROOT], P), "refused: its parent watched");
    f.refuse = 0;
    fl = watch_engine_read(&e, WD_ROOT + WD_COUNT, WATCH_READ_OVERFLOW, NULL, 0);
    CHECK(fl == WATCH_R1 && e.open[WD_ROOT], "the anchor's overflow: looked at again, open");

    /* an anchor that goes: one further up; with nothing above, nothing is watched (logged) */
    fs_rm(&f, ROOT);
    watch_engine_read(&e, WD_ROOT, WATCH_READ_GONE, NULL, 0);
    fs_rm(&f, P);
    watch_engine_read(&e, WD_ROOT + WD_COUNT, WATCH_READ_GONE, NULL, 0);
    CHECK(!strcmp(e.anchor[WD_ROOT], "C:\\") && e.open[WD_ROOT + WD_COUNT], "its anchor gone: the drive's root (%s)", e.anchor[WD_ROOT]);
    fs_rm(&f, "C:\\");
    watch_engine_read(&e, WD_ROOT + WD_COUNT, WATCH_READ_GONE, NULL, 0);
    CHECK(!e.open[WD_ROOT + WD_COUNT] && !e.anchor[WD_ROOT][0] && strstr(f.log, "nor any folder above it"), "nothing above: logged");
    fs_mk(&f, "C:\\");
    fs_mk(&f, P);
    fs_mk(&f, ROOT);
    fl = watch_engine_tick(&e);
    CHECK((fl & WATCH_R1) && e.open[WD_ROOT], "the tick finds it again");
    for (int s = 0; s < WATCH_SLOTS; s++) CHECK(f.opened[s] == e.open[s], "slot %d: the engine and the handles agree", s);
}

/* W3, W15: an anchor held while two levels are missing stays as it is on idle ticks; one
 * replaced is looked for again, silently when its path is the same */
static void test_engine_idle(void) {
    fakefs f;
    memset(&f, 0, sizeof f);
    const char *RB = "C:\\Program Files\\rekordbox", *ROOT = "C:\\Program Files\\RB Stems Plus", *ROAM = "C:\\Users\\u\\AppData\\Roaming";
    fs_mk(&f, "C:\\");
    fs_mk(&f, "C:\\Program Files");
    fs_mk(&f, RB);
    fs_mk(&f, ROOT);
    fs_mk(&f, "C:\\Users\\u\\AppData");
    fs_mk(&f, ROAM);
    watch_fs ops = {&f, fs_open, fs_close, fs_same, fs_log};
    const char *paths[WD_COUNT] = {RB, "C:\\Users\\u\\AppData\\Roaming\\Pioneer\\x", ROOT};
    watch_engine e;
    watch_engine_init(&e, &ops, paths);
    CHECK(watch_engine_tick(&e) == 0 && !strcmp(e.anchor[WD_PIONEER], ROAM), "two levels missing: watched from Roaming");
    int opens = f.opens, closes = f.closes;
    size_t logged = strlen(f.log);
    int fl = 0;
    for (int t = 0; t < 60; t++) fl |= watch_engine_tick(&e);
    CHECK(fl == 0 && f.opens == opens && f.closes == closes && strlen(f.log) == logged,
          "60 idle ticks: no open, no close, no log line (%d opens, %d closes, %zu bytes logged)", f.opens - opens, f.closes - closes,
          strlen(f.log) - logged);
    /* the anchor replaced by a folder of the same name: the tick walks again, without a log line */
    fs_mk(&f, ROAM);
    watch_engine_tick(&e);
    CHECK(e.open[WD_PIONEER + WD_COUNT] && f.slot_id[WD_PIONEER + WD_COUNT] == f.ids[fs_find(&f, ROAM)] && strlen(f.log) == logged,
          "a replaced anchor: opened anew, silently");
    /* replaced again, seen by the anchor's own read with no hit */
    fs_mk(&f, ROAM);
    watch_note unrel = {WATCH_ADDED, "Microsoft"};
    watch_engine_read(&e, WD_PIONEER + WD_COUNT, WATCH_READ_OK, &unrel, 1);
    CHECK(f.slot_id[WD_PIONEER + WD_COUNT] == f.ids[fs_find(&f, ROAM)], "seen after the anchor's read too");
    /* nothing above at all: one line, not one per tick */
    watch_engine e2;
    const char *none[WD_COUNT] = {"Q:\\a\\b", NULL, NULL};
    watch_engine_init(&e2, &ops, none);
    watch_engine_tick(&e2);
    logged = strlen(f.log);
    for (int t = 0; t < 10; t++) watch_engine_tick(&e2);
    CHECK(strlen(f.log) == logged, "nothing to watch: logged once");
}

static void test_sched(void) {
    watch_sched s;
    memset(&s, 0, sizeof s);
    int64_t w;
    CHECK(!watch_sched_poll(&s, 0, &w) && w == -1, "nothing pending");
    watch_sched_note(&s, WATCH_ROUND | WATCH_R1);
    CHECK(watch_sched_poll(&s, 1000, &w) && s.running, "the first set starts at once");
    watch_sched_note(&s, WATCH_ROUND);
    CHECK(!watch_sched_poll(&s, 1500, &w) && w == -1, "while a set runs, nothing starts");
    CHECK(watch_sched_take(&s) == (WATCH_ROUND | WATCH_R1) && watch_sched_take(&s) == 0, "taken once");
    watch_sched_note(&s, WATCH_R1);
    watch_sched_done(&s);
    CHECK(!watch_sched_poll(&s, 6000, &w) && w == 5000, "10 s after the last start: 5 s to wait (%lld)", (long long)w);
    CHECK(watch_sched_poll(&s, 11000, &w), "then it starts");
}

/* ------------------------------------------------------------------ the check, on fake operations */

typedef struct {
    watch_facts f;
    int facts_fail;
    int updating, busy, open_n, half;   /* open_n: rekordbox open for that many looks */
    int pauses, sessions;
    const watch_facts *after_pause;
    int pending[8], npending, takes;    /* what each take notes first */
    watch_sched sc;                      /* the schedule take and give_back work on */
    int given_back;
    int answer;                          /* the button pressed */
    int asks, refreshes, refreshes_at_prompt;
    char title[256], message[1024], buttons[512];
    char remembered[1024];
    int requests, opens, open_fails, request_fails;
    char request[16];
    char current[256];
    char log[16384];
} fakeops;

static int o_facts(void *c, watch_facts *f) {
    fakeops *o = c;
    if (o->facts_fail) return -1;
    *f = o->f;
    return 0;
}
static int32_t o_updating(void *c) {
    fakeops *o = c;
    return o->updating > 0 ? o->updating-- : 0;
}
static int32_t o_busy(void *c) { return ((fakeops *)c)->busy; }
static int32_t o_open(void *c) {
    fakeops *o = c;
    return o->open_n > 0 ? o->open_n-- > 0 : 0;
}
static int32_t o_half(void *c, char *id, size_t cap) {
    fakeops *o = c;
    if (o->half) snprintf(id, cap, "uninstall-cache");
    return o->half;
}
static void o_pause(void *c, int32_t s) {
    (void)s;
    fakeops *o = c;
    o->pauses++;
    if (o->after_pause) {
        o->f = *o->after_pause;
        o->after_pause = NULL;
    }
}
static void o_session(void *c) { ((fakeops *)c)->sessions++; }
/* each take: the next scripted notification noted into the schedule, then the schedule taken */
static int o_take(void *c) {
    fakeops *o = c;
    if (o->takes < o->npending) watch_sched_note(&o->sc, o->pending[o->takes++]);
    return watch_sched_take(&o->sc);
}
static void o_give_back(void *c, int flags) {
    fakeops *o = c;
    o->given_back |= flags;
    watch_sched_note(&o->sc, flags);
}
static int o_ask(void *c, const char *title, const char *message, const char *const *buttons, int n) {
    fakeops *o = c;
    o->asks++;
    o->refreshes_at_prompt = o->refreshes;
    snprintf(o->title, sizeof o->title, "%s", title);
    snprintf(o->message, sizeof o->message, "%s", message);
    o->buttons[0] = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(o->buttons);
        snprintf(o->buttons + l, sizeof o->buttons - l, "%s%s", i ? "|" : "", buttons[i]);
    }
    return o->answer;
}
static int o_remember(void *c, const char *t, size_t n) {
    fakeops *o = c;
    snprintf(o->remembered, sizeof o->remembered, "%.*s", (int)n, t);
    return 0;
}
/* the request, then the app (the Windows op holds one lock over both) */
static int o_reinstall(void *c, const char *name) {
    fakeops *o = c;
    snprintf(o->request, sizeof o->request, "%s", name);
    if (o->request_fails) return WATCH_NO_REQUEST;
    o->requests++;
    o->opens++;
    if (o->open_fails) return WATCH_NO_APP;
    return 0;
}
static int o_open_app(void *c) {
    fakeops *o = c;
    o->opens++;
    return o->open_fails ? ENOENT : 0;
}
static void o_current(void *c, const char *name) { snprintf(((fakeops *)c)->current, 256, "%s", name); }
static void o_refresh_app(void *c) { ((fakeops *)c)->refreshes++; }
static void o_log(void *c, const char *line) {
    fakeops *o = c;
    size_t n = strlen(o->log);
    snprintf(o->log + n, sizeof o->log - n, "%s\n", line);
}

static payload_manifest lists;

static void base(fakeops *o, watch_ops *ops) {
    memset(o, 0, sizeof *o);
    watch_ops t = {o, CORE_OS_MAC, o_facts, o_updating, o_busy, o_open, o_half, o_pause, o_session, o_take, o_give_back, o_ask,
                   o_remember, o_reinstall, o_open_app, o_current, o_log, o_refresh_app};
    *ops = t;
    watch_facts *f = &o->f;
    f->rb_state = COMPAT_RB_OK;
    snprintf(f->rekordbox_version, sizeof f->rekordbox_version, "7.2.19");
    snprintf(f->folder, sizeof f->folder, "C:\\Program Files\\rekordbox\\rekordbox 7.2.19");
    snprintf(f->watch_name, sizeof f->watch_name, "rekordbox 7.2.19");
    snprintf(f->engine, sizeof f->engine, "0002");
    f->chosen_cache = 1;
    f->r1_known = 1;
    f->r1.live = COMPAT_LIVE_MISSING;
    f->r1.offer_restore = 1;
    f->have_lists = 1;
    f->m = &lists;
    f->version = "1.4.0";
    memset(&lists, 0, sizeof lists);
    lists.has_windows = 1;
    snprintf(lists.win_rekordbox[0], sizeof lists.win_rekordbox[0], "7.2.19");
    lists.n_win_rekordbox = 1;
    snprintf(lists.win_stems_engine[0], sizeof lists.win_stems_engine[0], "0002");
    lists.n_win_stems_engine = 1;
}

static void rows_base(fakeops *o, watch_ops *ops) {
    base(o, ops);
    ops->text_os = CORE_OS_WIN;
    o->f.r1.offer_restore = 0;
    o->f.cache_present = 1;
    o->f.inv_complete = 1;
}

static void row(watch_facts *f, int i, const char *version, int state, int why, int r1) {
    inv_row_view *r = &f->rows[i];
    *r = (inv_row_view){.state = state, .why = why, .r1 = r1};
    snprintf(r->version, sizeof r->version, "%s", version);
    snprintf(r->name, sizeof r->name, "rekordbox %s", version);
    if (i >= f->nrows) f->nrows = i + 1;
}

static void test_rows_digest_refresh(void) {
    fakeops o;
    watch_ops ops;
    rows_base(&o, &ops);
    row(&o.f, 0, "7.2.19", INV_ROW_ON, WHY_NONE, 0);
    watch_state st = {0};
    watch_set s = {.st = &st};
    watch_check(&ops, &s, WATCH_REGISTRY);
    row(&o.f, 1, "7.2.18", INV_ROW_ON, WHY_NONE, 0);
    watch_check(&ops, &s, WATCH_REGISTRY);
    CHECK(o.refreshes == 2, "an older row alone changes the digest");
    CHECK(strstr(o.log, "state: rekordbox 7.2.19 On, 7.2.18 On"), "state line reports every row");
    watch_check(&ops, &s, WATCH_REGISTRY);
    CHECK(o.refreshes == 2, "the same rows don't refresh again");
}

static void test_early_compat_refresh(void) {
    fakeops o;
    watch_ops ops;
    rows_base(&o, &ops);
    row(&o.f, 0, "7.2.19", INV_ROW_ON, WHY_NONE, 0);
    row(&o.f, 1, "7.2.20", INV_ROW_UNSUPPORTED, WHY_RB_UNLISTED, 0);
    char first[INV_REFRESH_KEYS_MAX], next[INV_REFRESH_KEYS_MAX];
    CHECK(inv_refresh_keys(o.f.rows, o.f.nrows, o.f.m, "", first) && strstr(first, ":7.2.20\n"), "new unsupported version requests a refresh");
    CHECK(!inv_refresh_keys(o.f.rows, o.f.nrows, o.f.m, first, next), "same version and list are already recorded");
    row(&o.f, 2, "7.2.21", INV_ROW_UNSUPPORTED, WHY_RB_UNLISTED, 0);
    CHECK(inv_refresh_keys(o.f.rows, o.f.nrows, o.f.m, first, next) && strstr(next, ":7.2.21\n") && !strstr(next, ":7.2.20\n"),
          "a second unsupported version gets its own request");
    strcpy(lists.win_rekordbox[lists.n_win_rekordbox++], "7.2.20");
    row(&o.f, 1, "7.2.20", INV_ROW_READY, WHY_NONE, 0);
    CHECK(inv_refresh_keys(o.f.rows, o.f.nrows, o.f.m, first, next) && !strstr(next, ":7.2.20\n"), "a Ready version needs no early refresh");
    row(&o.f, 1, "7.2.20", INV_ROW_UNSUPPORTED, WHY_BUILD_UNLISTED, 0);
    CHECK(inv_refresh_keys(o.f.rows, o.f.nrows, o.f.m, first, next) && strstr(next, ":7.2.20\n"), "a changed list permits another request");
    o.f.rows[0].state = INV_ROW_READY;
    CHECK(!inv_refresh_keys(o.f.rows, o.f.nrows, o.f.m, "", next), "no early refresh without Cache on elsewhere");
}

static void test_stale_entry_no_retry(void) {
    fakeops o;
    watch_ops ops;
    rows_base(&o, &ops);
    watch_state st = {0};
    watch_set s = {.st = &st};
    o.f.rb_state = COMPAT_RB_NO_ANSWER;
    o.f.inv_complete = 0;
    watch_check(&ops, &s, WATCH_REGISTRY);
    CHECK(o.pauses == 0, "an incomplete read without a settling folder doesn't pause");
    o.f.rb_unsettled = 1;
    watch_check(&ops, &s, WATCH_REGISTRY);
    CHECK(o.pauses == 1, "a settling folder gets one retry");
}

static void test_grouped_add_prompt(void) {
    fakeops o;
    watch_ops ops;
    rows_base(&o, &ops);
    row(&o.f, 0, "7.2.19", INV_ROW_READY, WHY_NONE, 0);
    row(&o.f, 1, "7.2.18", INV_ROW_READY, WHY_NONE, 0);
    row(&o.f, 2, "7.2.17", INV_ROW_ON, WHY_NONE, 0);
    o.answer = 2;
    watch_state st = {0};
    watch_set s = {.st = &st};
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(o.asks == 1 && !strcmp(o.message, "Stems Cache isn't on in rekordbox 7.2.19, 7.2.18 yet.") &&
          !strcmp(o.buttons, "Install Now|Remind Me Later|Don't Ask Again for These Versions"),
          "one add prompt names both Ready rows: %s / %s", o.message, o.buttons);
    CHECK(!strcmp(o.remembered, "cache:7.2.18\ncache:7.2.19\n"), "only the shown keys are recorded: %s", o.remembered);
    CHECK(o.requests == 0 && o.opens == 0, "dismissal doesn't start an install");
    o.f.dismissed = o.remembered;
    o.f.dismissed_n = strlen(o.remembered);
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(o.asks == 1, "the next round asks nothing");
}

static void test_unsupported_notice_once(void) {
    fakeops o;
    watch_ops ops;
    rows_base(&o, &ops);
    row(&o.f, 0, "7.2.20", INV_ROW_UNSUPPORTED, WHY_RB_UNLISTED, 0);
    row(&o.f, 1, "7.2.19", INV_ROW_UNSUPPORTED, WHY_ORT_UNLISTED, 0);
    watch_state st = {0};
    watch_set s = {.st = &st};
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(o.asks == 1 && strstr(o.message, "7.2.20, 7.2.19") && !strncmp(o.buttons, "OK|", 3),
          "one notice shows both unsupported versions: %s", o.message);
    char remembered[sizeof o.remembered];
    snprintf(remembered, sizeof remembered, "%s", o.remembered);
    o.f.dismissed = remembered;
    o.f.dismissed_n = strlen(remembered);
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(o.asks == 1, "one unsupported notice per version and list");
    strcpy(lists.win_ort_code_sha256, "different ORT code");
    watch_sched_registry(&o.sc, 1, 0);
    watch_sched_take(&o.sc);
    watch_sched_registry(&o.sc, 1, 1);
    int flags = watch_sched_take(&o.sc);
    CHECK(flags == WATCH_REGISTRY, "the periodic check sees list changes without a registry change");
    if (flags) watch_check(&ops, &s, flags);
    CHECK(o.asks == 2 && o.requests == 0 && o.opens == 0, "a changed list allows another notice on a registry round");
}

static void test_repair_precedence(void) {
    fakeops o;
    watch_ops ops;
    rows_base(&o, &ops);
    row(&o.f, 0, "7.2.19", INV_ROW_READY, WHY_NONE, 0);
    row(&o.f, 1, "7.2.18", INV_ROW_REPAIR, WHY_R1_MISSING, 1);
    o.answer = 1;
    watch_state st = {0};
    watch_set s = {.st = &st};
    watch_check(&ops, &s, WATCH_R1);
    CHECK(o.opens == 1 && o.asks == 1 && strstr(o.message, "7.2.19") && !strstr(o.message, "7.2.18"),
          "an older repair opens the app and only the Ready row is offered: %s", o.message);
    watch_check(&ops, &s, WATCH_R1);
    CHECK(o.opens == 1 && o.asks == 1, "same repair opens once and Remind Me Later holds for this set");
}

static void test_removal_silent(void) {
    fakeops o;
    watch_ops ops;
    rows_base(&o, &ops);
    row(&o.f, 0, "7.2.19", INV_ROW_READY, WHY_NONE, 0);
    row(&o.f, 1, "7.2.18", INV_ROW_ON, WHY_NONE, 0);
    o.answer = 1;
    watch_state st = {0};
    watch_set s = {.st = &st};
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(o.asks == 1, "the Ready row was offered before removal");
    watch_state before_removal = st;
    o.f.nrows = 1;
    s = (watch_set){.st = &st};
    watch_check(&ops, &s, WATCH_ROUND | WATCH_R1);
    CHECK(o.refreshes == 2 && o.asks == 1 && o.opens == 0 && o.requests == 0,
          "removing the other row refreshes quietly even with a previous reminder");

    st = before_removal;
    s = (watch_set){.st = &st};
    row(&o.f, 1, "7.2.18", INV_ROW_REPAIR, WHY_DELISTED_BRIDGE, 0);
    watch_check(&ops, &s, WATCH_ROUND | WATCH_R1);
    CHECK(o.refreshes == 3 && o.asks == 1 && o.opens == 0 && o.requests == 0,
          "a removal leaving a disk-only cleanup row refreshes quietly");
}

static void test_registry_update_refreshes_open_app(void) {
    fakeops o;
    watch_ops ops;
    base(&o, &ops);
    ops.text_os = CORE_OS_WIN;
    o.f.r1.offer_restore = 0;
    o.f.r1.live = COMPAT_LIVE_OURS;
    o.f.cache_present = 1;
    watch_state state = {0};
    watch_set set = {.st = &state};
    CHECK(watch_check(&ops, &set, WATCH_ROUND) == 1 && o.asks == 0 && o.refreshes == 1, "first verified observation refreshes an open app");

    /* The folder was seen first, with the old installation still current. The registry change
     * arrives later, and the first scan after it still can't use the new EXE. */
    CHECK(watch_check(&ops, &set, WATCH_ROUND) == 1 && o.refreshes == 1, "unchanged old version after directory notice");
    watch_facts updated = o.f;
    snprintf(updated.rekordbox_version, sizeof updated.rekordbox_version, "7.2.20");
    snprintf(updated.watch_name, sizeof updated.watch_name, "rekordbox 7.2.20");
    snprintf(updated.folder, sizeof updated.folder, "C:\\Program Files\\rekordbox\\rekordbox 7.2.20");
    row(&updated, 0, "7.2.20", INV_ROW_UNSUPPORTED, WHY_RB_UNLISTED, 0);
    updated.cache_present = 0;
    updated.r1.live = COMPAT_LIVE_PIONEER;
    o.f.rb_unsettled = 1;
    o.after_pause = &updated;
    o.answer = -1;
    watch_sched_registry(&o.sc, 1, 0);
    CHECK(watch_sched_take(&o.sc) == WATCH_REGISTRY, "a new Uninstall generation schedules a watcher round");
    watch_sched_registry(&o.sc, 2, 0);
    int64_t wait;
    if (watch_sched_poll(&o.sc, 0, &wait)) watch_check(&ops, &set, 0);
    CHECK(o.pauses == 1 && o.asks == 1 && strstr(o.message, "7.2.20") &&
          o.refreshes == 2 && o.refreshes_at_prompt == 2 && !o.opens && !o.requests,
          "registry-only notice retries settling and quietly refreshes the app before the unsupported prompt");
    CHECK(watch_check(&ops, &set, WATCH_REGISTRY) == 1 && o.refreshes == 2 && o.asks == 1,
          "an unrelated registry change doesn't refresh or repeat the prompt");
    CHECK(watch_classify(WD_REKORDBOX, "rekordbox 7.2.19", WATCH_ADDED, "rekordbox 7.2.20\\rekordbox.exe") == (WATCH_ROUND | WATCH_R1),
          "a late EXE copy in a different folder schedules a check");
}

static void test_check(void) {
    fakeops o;
    watch_ops ops;
    watch_state st;
    watch_set s;

    /* I72: the live DLL deleted, our app closed: the app is opened (its own offer), once */
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    memset(&s, 0, sizeof s);
    s.st = &st;
    o.open_n = 2;
    int r = watch_check(&ops, &s, WATCH_ROUND | WATCH_R1);
    CHECK(r == 0 && o.opens == 1 && o.asks == 0 && o.requests == 0, "R1: RB Stems Plus opened, nothing asked (%d %d %d)", r, o.opens, o.asks);
    CHECK(o.pauses >= 2 && o.sessions >= 1 && strstr(o.log, "rekordbox is open: waiting until it quits"), "settled first: rekordbox closed");
    CHECK(strstr(o.log, "rekordbox 7.2.19's onnxruntime.dll is missing: opening RB Stems Plus to put Pioneer's file back") != NULL,
          "logged: %s", o.log);
    CHECK(strstr(o.log, "run: RB Stems Plus Watcher 1.4.0; rekordbox 7.2.19, engine 0002, Demucs v4 off, Stems Cache off; chosen: model=false cache=true")
              != NULL,
          "the run line");
    CHECK(!strcmp(o.current, "rekordbox 7.2.19"), "the folders' filter told the current folder");
    /* the same state again: not opened again; while R1 holds, Stems Cache's question isn't asked
     * (W7: one offer, the app's); Demucs v4's is */
    o.answer = 1;   /* Remind Me Later */
    r = watch_check(&ops, &s, WATCH_R1);
    CHECK(r == 1 && o.opens == 1 && o.asks == 0, "R1 again: not opened twice; no reinstall question (%d asks)", o.asks);
    CHECK(strstr(o.log, "cache:7.2.19 isn't asked while Pioneer's file should be put back\nnothing to ask\n") != NULL, "logged: %s", o.log);
    o.f.chosen_model = 1;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 1 && o.asks == 1 && !strcmp(o.title, "rekordbox put its own stems model back")
              && !strcmp(o.buttons, "Reinstall Now|Remind Me Later|Don't Ask Again for This Version")
              && !strcmp(o.message, "Demucs v4 is off. Reinstall it to separate tracks with Demucs v4 instead of rekordbox's own model."),
          "the opening round: Demucs v4's question only: %s / %s / %s", o.title, o.message, o.buttons);
    CHECK(strstr(o.log, "asking: model:0002\nuser chose: Remind Me Later\n") != NULL, "asked and answered, logged");
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 1 && o.asks == 1 && strstr(o.log, "still missing (model:0002), put off a moment ago: not asking again now"), "put off for this set");
    /* another state of the live DLL: opened again */
    o.f.r1.live = COMPAT_LIVE_FOREIGN;
    r = watch_check(&ops, &s, WATCH_R1);
    CHECK(r == 0 && o.opens == 2 && strstr(o.log, "neither Pioneer's nor RB Stems Plus's"), "a new R1 state: opened again");

    /* W11: the app couldn't be opened: not remembered, tried again */
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    memset(&s, 0, sizeof s);
    s.st = &st;
    o.open_fails = 1;
    r = watch_check(&ops, &s, WATCH_R1);
    CHECK(r == 1 && o.opens == 1 && strstr(o.log, "couldn't open RB Stems Plus"), "not opened: logged, the round goes on");
    o.open_fails = 0;
    r = watch_check(&ops, &s, WATCH_R1);
    CHECK(r == 0 && o.opens == 2, "the next R1 round tries again");
    /* W16: "Don't Ask Again for This Version" in the app's R1 offer: not opened for that version */
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    memset(&s, 0, sizeof s);
    s.st = &st;
    o.f.dismissed = "a\nr1:7.2.19\n";
    o.f.dismissed_n = strlen(o.f.dismissed);
    r = watch_check(&ops, &s, WATCH_R1);
    CHECK(r == 1 && o.opens == 0 && strstr(o.log, "not opening RB Stems Plus (Don't Ask Again for this version)"), "dismissed for 7.2.19");
    snprintf(o.f.rekordbox_version, sizeof o.f.rekordbox_version, "7.2.20");
    r = watch_check(&ops, &s, WATCH_R1);
    CHECK(r == 0 && o.opens == 1, "another version: opened");
    /* W12: the remembered openings never fill up: the oldest go */
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    memset(&s, 0, sizeof s);
    s.st = &st;
    for (int i = 0; i < 60; i++) {
        snprintf(o.f.rekordbox_version, sizeof o.f.rekordbox_version, "7.2.%d", i);
        watch_check(&ops, &s, WATCH_R1);
    }
    CHECK(o.opens == 60 && strlen(st.r1_opened) < sizeof st.r1_opened && strstr(st.r1_opened, "r1:7.2.59 "),
          "60 states: each opened once, the newest kept (%d)", o.opens);
    snprintf(o.f.rekordbox_version, sizeof o.f.rekordbox_version, "7.2.59");
    watch_check(&ops, &s, WATCH_R1);
    CHECK(o.opens == 60, "the newest still remembered");
    /* W6: an unlisted bridge without a list, with ort\'s copy looked at and missing: R1 */
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    memset(&s, 0, sizeof s);
    s.st = &st;
    o.f.r1.live = COMPAT_LIVE_OURS_UNLISTED;
    o.f.r1.ort_looked = 1;
    o.f.have_lists = 0;
    o.f.m = NULL;
    r = watch_check(&ops, &s, WATCH_R1);
    CHECK(r == 0 && o.opens == 1, "ort\\'s copy missing: rekordbox can't start, R1 opens the app");

    /* the opening round (settings folder): no R1; with nothing to put back, the question */
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    memset(&s, 0, sizeof s);
    s.st = &st;
    o.f.r1.offer_restore = 0;
    o.answer = 0;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 0 && o.opens == 1 && o.requests == 1 && o.asks == 1, "opening round: Reinstall Now: the request, then the app");
    CHECK(!strcmp(o.title, "rekordbox was updated") && !strcmp(o.message, "Stems Cache is off. Reinstall it to reuse the stems it has saved."),
          "the core's dialog: %s / %s", o.title, o.message);
    /* W9: Reinstall Now's two failures, each logged */
    o.request_fails = 1;
    memset(&s, 0, sizeof s);
    s.st = &st;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 0 && o.requests == 1 && strstr(o.log, "couldn't write the reinstall request"), "the request not written: logged");
    o.request_fails = 0;
    o.open_fails = 1;
    memset(&s, 0, sizeof s);
    s.st = &st;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 0 && o.requests == 2 && strstr(o.log, "couldn't open RB Stems Plus"), "written, the app not started: logged");
    /* while R1 holds, an opening round neither opens the app nor asks about Stems Cache */
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    memset(&s, 0, sizeof s);
    s.st = &st;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 1 && o.opens == 0 && o.asks == 0 && o.requests == 0, "opening round while R1 holds: nothing");
    /* R1 for an unlisted bridge without a verified list: nothing */
    base(&o, &ops);
    memset(&s, 0, sizeof s);
    memset(&st, 0, sizeof st);
    s.st = &st;
    o.f.r1.live = COMPAT_LIVE_OURS_UNLISTED;
    o.f.cache_present = 1;
    o.f.have_lists = 0;
    o.f.m = NULL;
    r = watch_check(&ops, &s, WATCH_R1);
    CHECK(r == 1 && o.opens == 0 && strstr(o.log, "nothing to ask"), "unlisted against no list: no R1, nothing missing");
    /* R1 and a notification taken after settling */
    base(&o, &ops);
    memset(&s, 0, sizeof s);
    memset(&st, 0, sizeof st);
    s.st = &st;
    o.pending[0] = WATCH_R1;
    o.npending = 1;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 0 && o.opens == 1, "R1 from a notification taken once settled");

    /* Don't Ask Again: dismissed gets the key; the next set skips it */
    base(&o, &ops);
    memset(&s, 0, sizeof s);
    memset(&st, 0, sizeof st);
    s.st = &st;
    o.f.chosen_model = 1;
    o.f.r1.offer_restore = 0;
    o.f.dismissed = "a\nzz\n";
    o.f.dismissed_n = 5;
    o.answer = 2;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 1 && !strcmp(o.remembered, "a\ncache:7.2.19\nmodel:0002\nzz\n"), "remembered: %s", o.remembered);
    CHECK(!strcmp(o.message, "Demucs v4 and Stems Cache are off. Reinstall them to separate tracks with Demucs v4 and reuse the stems Stems Cache has saved."),
          "both off: %s", o.message);
    o.f.dismissed = "cache:7.2.19\nmodel:0002\n";
    o.f.dismissed_n = strlen(o.f.dismissed);
    o.asks = 0;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 1 && o.asks == 0 && strstr(o.log, "nothing to ask"), "dismissed: nothing to ask");

    /* not supported yet: the OK form, the unsupported key remembered */
    base(&o, &ops);
    memset(&s, 0, sizeof s);
    memset(&st, 0, sizeof st);
    s.st = &st;
    o.f.r1.offer_restore = 0;
    snprintf(o.f.rekordbox_version, sizeof o.f.rekordbox_version, "7.2.20");
    o.answer = 0;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 1 && !strcmp(o.buttons, "OK|Don't Ask Again for This Version") && o.requests == 0, "blocked: %s", o.buttons);
    CHECK(!strcmp(o.message,
                  "Stems Cache is off. It doesn't support rekordbox 7.2.20 yet. RB Stems Plus checks again each time you open it, and asks you to reinstall once it can."),
          "blocked message: %s", o.message);
    CHECK(!strcmp(o.remembered, "cache:7.2.20 unsupported with 7.2.19,0002,\n"), "the unsupported key: %s", o.remembered);
    o.f.dismissed = "cache:7.2.20 unsupported with 7.2.19,0002,\n";
    o.f.dismissed_n = strlen(o.f.dismissed);
    o.asks = 0;
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(o.asks == 0 && strstr(o.log, "nothing to ask"), "told once: not asked again against the same list");
    snprintf(lists.win_rekordbox[1], sizeof lists.win_rekordbox[1], "7.2.20");
    lists.n_win_rekordbox = 2;
    o.answer = 1;
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(o.asks == 1 && !strcmp(o.buttons, "Reinstall Now|Remind Me Later|Don't Ask Again for This Version"), "a newer list: asked to reinstall");
    o.asks = 0;
    o.answer = -1;   /* closed */
    memset(&s, 0, sizeof s);
    s.st = &st;
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(o.asks == 1 && strstr(o.log, "user chose: \n"), "closed: logged, put off");
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(o.asks == 1, "and put off for the set");

    /* the Windows column (W-core's, as the exe asks): the question in its words */
    base(&o, &ops);
    memset(&s, 0, sizeof s);
    memset(&st, 0, sizeof st);
    s.st = &st;
    ops.text_os = CORE_OS_WIN;
    o.f.r1.offer_restore = 0;
    o.f.chosen_model = 1;
    o.f.dismissed = "cache:7.2.19\n";
    o.f.dismissed_n = strlen(o.f.dismissed);
    o.answer = 0;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 0 && o.asks == 1 && o.requests == 1 && o.opens == 1, "Windows: asked, Reinstall Now (%d asks)", o.asks);
    CHECK(!strcmp(o.title, "rekordbox put its own stems model back") && !strcmp(o.buttons, "Reinstall Now|Remind Me Later|Don't Ask Again for This Version")
              && !strcmp(o.message, "Demucs v4 is off. Reinstall it to separate tracks with Demucs v4 instead of rekordbox's own model."),
          "the Windows column's words: %s / %s / %s", o.title, o.message, o.buttons);
    CHECK(strstr(o.log, "asking: model:0002\nuser chose: Reinstall Now\n") != NULL && !strcmp(o.request, "model"),
          "the model offer writes a model-only request (got %s)", o.request);
    /* the blocked form in the Windows column */
    base(&o, &ops);
    memset(&s, 0, sizeof s);
    s.st = &st;
    ops.text_os = CORE_OS_WIN;
    o.f.r1.offer_restore = 0;
    snprintf(o.f.rekordbox_version, sizeof o.f.rekordbox_version, "7.2.20");
    row(&o.f, 0, "7.2.20", INV_ROW_UNSUPPORTED, WHY_RB_UNLISTED, 0);
    o.answer = 1;
    r = watch_check(&ops, &s, WATCH_ROUND);
    CHECK(r == 1 && !strcmp(o.title, "rekordbox was updated") && !strcmp(o.buttons, "OK|Don't Ask Again for This Version")
              && !strcmp(o.message, "Stems Cache doesn't support rekordbox 7.2.20 yet. RB Stems Plus asks you to add it once it can."),
          "Windows, blocked: %s / %s / %s", o.title, o.message, o.buttons);

    /* a text not written in its column: not asked, never in another column's words */
    base(&o, &ops);
    memset(&s, 0, sizeof s);
    memset(&st, 0, sizeof st);
    s.st = &st;
    ops.text_os = CORE_OS_WIN;
    o.f.r1.offer_restore = 0;
    o.f.inv_complete = 1;
    row(&o.f, 0, "7.2.19", INV_ROW_READY, WHY_NONE, 0);
    watch_test_unwritten_text = T_W_REMIND_LATER;
    r = watch_check(&ops, &s, WATCH_ROUND);
    watch_test_unwritten_text = -1;
    CHECK(r == 1 && o.asks == 0 && strstr(o.log, "the Stems Cache question's texts aren't written for Windows yet: not asking"),
          "a button not written: not asked");
    base(&o, &ops);
    memset(&s, 0, sizeof s);
    s.st = &st;
    ops.text_os = CORE_OS_WIN;
    o.f.r1.offer_restore = 0;
    o.f.inv_complete = 1;
    row(&o.f, 0, "7.2.19", INV_ROW_READY, WHY_NONE, 0);
    watch_test_unwritten_text = T_W_TITLE_UPDATED;
    r = watch_check(&ops, &s, WATCH_ROUND);
    watch_test_unwritten_text = -1;
    CHECK(r == 1 && o.asks == 0 && strstr(o.log, "aren't written for Windows yet: not asking"), "the title not written: not asked");

    /* no verified list, rekordbox not verified or not there, facts unreadable */
    base(&o, &ops);
    memset(&s, 0, sizeof s);
    s.st = &st;
    o.f.r1.offer_restore = 0;
    o.f.have_lists = 0;
    o.f.m = NULL;
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(o.asks == 0 && strstr(o.log, "no verified payload.json says what can be reinstalled"), "no lists: not asked");
    base(&o, &ops);
    o.f.rb_state = COMPAT_RB_UNVERIFIED;
    watch_check(&ops, &s, WATCH_ROUND | WATCH_R1);
    CHECK(o.asks == 0 && o.opens == 0 && strstr(o.log, "couldn't verify rekordbox 7.2.19: not asking"), "unverified");
    base(&o, &ops);
    o.f.rb_state = COMPAT_RB_NOT_INSTALLED;
    watch_check(&ops, &s, WATCH_ROUND | WATCH_R1);
    CHECK(o.asks == 0 && o.opens == 0 && strstr(o.log, "rekordbox isn't installed: nothing to ask"), "not installed");
    base(&o, &ops);
    o.facts_fail = 1;
    CHECK(watch_check(&ops, &s, WATCH_ROUND) == 1 && o.asks == 0, "facts unreadable: nothing");

    /* settle: an update, then busy, then a half-done uninstall */
    base(&o, &ops);
    o.updating = 3;
    o.f.r1.offer_restore = 0;
    o.answer = 1;
    watch_check(&ops, &s, WATCH_ROUND);
    CHECK(strstr(o.log, "an update is running: waiting until it ends\nwaited 15 s for an update or RB Stems Plus to finish") != NULL, "update: waited");
    base(&o, &ops);
    o.busy = 1;
    CHECK(watch_check(&ops, &s, WATCH_ROUND) == 0 && strstr(o.log, "RB Stems Plus is still busy: not asking") && o.pauses == 120, "busy: 10 minutes");
    base(&o, &ops);
    o.half = 1;
    CHECK(watch_check(&ops, &s, WATCH_ROUND) == 0 && strstr(o.log, "an uninstall didn't finish (uninstall-cache): not asking"), "half-done");

    /* rounds: changes during a check give another, at most 3 */
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    o.f.r1.offer_restore = 0;
    o.answer = 1;
    o.pending[0] = 0;          /* round 1, after settling */
    o.pending[1] = WATCH_ROUND; /* again? yes */
    o.pending[2] = 0;          /* round 2, after settling */
    o.pending[3] = 0;          /* again? no */
    o.npending = 4;
    CHECK(watch_rounds(&ops, &st) == 2 && strstr(o.log, "something changed during the check: looking again"), "two rounds");
    CHECK(o.asks == 1 && strstr(o.log, "put off a moment ago"), "put off within the set");
    o.takes = 0;
    o.npending = 0;
    o.asks = 0;
    watch_rounds(&ops, &st);
    CHECK(o.asks == 1, "a new set asks again (the Mac's next start)");
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    o.f.r1.offer_restore = 0;
    o.answer = 1;
    for (int i = 0; i < 8; i++) o.pending[i] = WATCH_ROUND;
    o.npending = 8;
    CHECK(watch_rounds(&ops, &st) == 3 && strstr(o.log, "changes kept coming during 3 checks: the next start looks again"), "at most 3");

    /* W1: a notification in round 3 isn't lost: given back, a new set 10 s later */
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    o.f.r1.offer_restore = 0;
    o.answer = 1;
    int64_t w;
    watch_sched_note(&o.sc, WATCH_ROUND);
    CHECK(watch_sched_poll(&o.sc, 0, &w), "the set starts");
    o.pending[0] = 0;              /* round 1, settled */
    o.pending[1] = WATCH_ROUND;    /* again: round 2 */
    o.pending[2] = 0;
    o.pending[3] = WATCH_ROUND;    /* again: round 3 */
    o.pending[4] = 0;
    o.pending[5] = WATCH_R1;       /* during round 3: no fourth round */
    o.npending = 6;
    CHECK(watch_rounds(&ops, &st) == 3 && o.given_back == WATCH_R1, "the last round's notification given back (%d)", o.given_back);
    watch_sched_done(&o.sc);
    CHECK(!watch_sched_poll(&o.sc, 5000, &w) && w == 5000 && watch_sched_poll(&o.sc, 10000, &w), "a new set 10 s later");
    CHECK(watch_sched_take(&o.sc) == WATCH_R1, "with R1's flag");

    /* W2: a failed settle drops what came in: one set in 60 s, not one every 10 s */
    base(&o, &ops);
    memset(&st, 0, sizeof st);
    o.half = 1;
    watch_sched_note(&o.sc, WATCH_ROUND | WATCH_R1);
    int sets = 0;
    for (int64_t t = 0; t <= 60000; t += 1000)
        if (watch_sched_poll(&o.sc, t, &w)) {
            sets++;
            watch_rounds(&ops, &st);
            watch_sched_done(&o.sc);
        }
    CHECK(sets == 1 && o.sc.pending == 0, "a half-done uninstall: one set (%d)", sets);
    const char *l = strstr(o.log, "an uninstall didn't finish");
    CHECK(l && !strstr(l + 1, "an uninstall didn't finish"), "logged once");
}

/* ------------------------------------------------------------------ the facts, on a scratch tree */

static void spit(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "can't write %s\n", path); abort(); }
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}
static void spits(const char *path, const char *s) { spit(path, s, strlen(s)); }

typedef struct {
    compat_uninstall e[2];
    char signed_exe[1200];
} fakeos;
static int fo_entries(void *ctx, compat_uninstall *out, int max, int *incomplete) {
    fakeos *f = ctx;
    *incomplete = 0;
    int n = f->e[1].key[0] ? 2 : 1;
    if (max < n) return 0;
    memcpy(out, f->e, (size_t)n * sizeof *out);
    return n;
}
static int fo_pin(void *ctx, const char *path, plat_file pf) {
    fakeos *f = ctx;
    (void)pf;
    return !strcmp(path, f->signed_exe) ? COMPAT_PIN_OK : COMPAT_PIN_NO;
}
static int fo_procs(void *ctx, compat_proc *out, int max) {
    (void)ctx, (void)out, (void)max;
    return 0;
}
static uint16_t fo_native(void *ctx) {
    (void)ctx;
    return COMPAT_MACHINE_ARM64;
}
static int64_t fo_start(void *ctx, int32_t pid) {
    (void)ctx;
    return pid == 123 ? 456 : -1;
}
static char flogb[4096];
static void fo_log(void *ctx, const char *line) {
    (void)ctx;
    size_t n = strlen(flogb);
    snprintf(flogb + n, sizeof flogb - n, "%s\n", line);
}

static void sha_hex(const void *d, size_t n, char out[65]) {
    unsigned char h[32];
    CC_SHA256(d, (CC_LONG)n, h);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}

/* a payload.json naming Windows app version app and bridge sha256 bsha, signed with k, into dir
 * (with the Mac part) */
static void signed_pair(const test_key *k, const char *dir, const char *pv, const char *app, const char *bsha) {
    char j[4096], p[1300];
    snprintf(j, sizeof j,
             "{\"payload_version\": \"%s\", %s"
             "\"model\": {\"file\": \"m.onnx\", \"sha256\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\", \"size\": 9}, "
             "\"windows\": {\"app\": {\"version\": \"%s\", \"zip\": \"w.zip\", "
             "\"sha256\": \"1111111111111111111111111111111111111111111111111111111111111111\", "
             "\"admin_sha256\": \"2222222222222222222222222222222222222222222222222222222222222222\", "
             "\"stub_sha256\": {\"x64\": \"3333333333333333333333333333333333333333333333333333333333333333\", "
             "\"arm64\": \"4444444444444444444444444444444444444444444444444444444444444444\"}}, "
             "\"bridge\": {\"file\": \"b.dll\", \"sha256\": \"%s\"}, "
             "\"compatible\": {\"rekordbox\": [\"7.2.19\"], \"ort_sha256\": [], \"stems_engine\": [\"0002\"]}}}",
             pv,
             "\"app\": {\"version\": \"1.0.0\", \"zip\": \"a.zip\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
             "\"bridge\": {\"file\": \"b.dylib\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}, "
             "\"compatible\": {\"rekordbox\": [], \"ort_version_prefix\": \"1.18.\", \"stems_engine\": []}, ",
             app, bsha);
    unsigned char s[SIG_DER_MAX];
    size_t sn = test_sign(k, j, strlen(j), s);
    CHECK(sn > 0, "signed");
    snprintf(p, sizeof p, "%s/payload.json.sig", dir);
    spit(p, s, sn);
    snprintf(p, sizeof p, "%s/payload.json", dir);
    spits(p, j);
}

/* a file's whole text, "" when there is none */
static const char *slurp(const char *path, char *b, size_t cap) {
    FILE *in = fopen(path, "rb");
    size_t got = in ? fread(b, 1, cap - 1, in) : 0;
    if (in) fclose(in);
    b[got] = 0;
    return b;
}

/* N10 (review WCM3): dismissed's one writer, the watcher's reinstall question and the app's R1
 * "Don't Ask Again for This Version": the file as it is now and the new lines together, sorted,
 * no repeats; a line the other process wrote meanwhile is kept; a file that isn't UTF-8 counts as
 * none; one that can't be read, or lines that aren't UTF-8, leave it as it is */
static void test_dismissed(const char *scratch) {
    char sup[1024], p[1100], b[512], key[64];
    snprintf(sup, sizeof sup, "%s/dismissed-support", scratch);
    CHECK(mkdir(sup, 0700) == 0, "a support folder");
    snprintf(p, sizeof p, "%s/dismissed", sup);
    CHECK(watch_r1_dismissed_key("7.2.19", key, sizeof key) == 9 && !strcmp(key, "r1:7.2.19"), "R1's dismissed line: %s", key);
    CHECK(!dismissed_has(sup, "r1:7.2.19"), "no file: not dismissed");
    CHECK(dismissed_add(sup, key, strlen(key)) == 0 && !strcmp(slurp(p, b, sizeof b), "r1:7.2.19\n"), "no file: the line alone: %s", b);
    CHECK(dismissed_has(sup, "r1:7.2.19") && !dismissed_has(sup, "r1:7.2.20") && !dismissed_has(sup, "r1:7.2"), "dismissed for 7.2.19 only");
    /* the watcher's lines kept, sorted; the same line again isn't repeated */
    spits(p, "model:0002\ncache:7.2.19\n");
    CHECK(dismissed_add(sup, key, strlen(key)) == 0 && !strcmp(slurp(p, b, sizeof b), "cache:7.2.19\nmodel:0002\nr1:7.2.19\n"), "kept: %s", b);
    CHECK(dismissed_add(sup, "r1:7.2.19\n", 10) == 0 && !strcmp(slurp(p, b, sizeof b), "cache:7.2.19\nmodel:0002\nr1:7.2.19\n"), "no repeat: %s", b);
    /* the race the lock's read closes: the watcher's whole text, from its read before the app's
     * line, keeps that line */
    spits(p, "a\nzz\n");
    CHECK(dismissed_add(sup, key, strlen(key)) == 0, "the app's line");
    const char *watcher = "a\ncache:7.2.19\nmodel:0002\nzz\n";
    CHECK(dismissed_add(sup, watcher, strlen(watcher)) == 0 && !strcmp(slurp(p, b, sizeof b), "a\ncache:7.2.19\nmodel:0002\nr1:7.2.19\nzz\n"),
          "the watcher's answer after it: both kept: %s", b);
    CHECK(dismissed_has(sup, "r1:7.2.19") && dismissed_has(sup, "model:0002"), "both read back");
    /* not UTF-8: none, as the readers count it */
    spits(p, "\xff\xfe\n");
    CHECK(!dismissed_has(sup, "r1:7.2.19"), "not UTF-8: nothing dismissed");
    CHECK(dismissed_add(sup, key, strlen(key)) == 0 && !strcmp(slurp(p, b, sizeof b), "r1:7.2.19\n"), "not UTF-8: replaced: %s", b);
    CHECK(dismissed_add(sup, "r1:\xff", 4) != 0 && !strcmp(slurp(p, b, sizeof b), "r1:7.2.19\n"), "lines not UTF-8: refused, the file as it was");
    /* at the readers' bound: a new line is refused, a repeat still writes */
    char full[64 * 1024];
    for (int i = 0; i < 6553; i++) snprintf(full + 10 * i, sizeof full - 10 * i, "a:%07d\n", i);
    memcpy(full + sizeof full - 6, "zzzzz\n", 6);
    spit(p, full, sizeof full);
    CHECK(dismissed_add(sup, key, strlen(key)) == E2BIG, "at 64 KiB: a new line is refused");
    CHECK(dismissed_has(sup, "a:0000000") && !dismissed_has(sup, "r1:7.2.19"), "at 64 KiB: the old line is still readable, the new line absent");
    CHECK(dismissed_add(sup, "a:0000001", 9) == 0, "at 64 KiB: a repeat still writes");
    spits(p, "r1:7.2.19\n");
    /* a file that can't be read is never overwritten blind; no support folder: an error */
    CHECK(unlink(p) == 0 && mkdir(p, 0700) == 0, "dismissed made a folder");
    struct stat st;
    CHECK(dismissed_add(sup, key, strlen(key)) != 0 && stat(p, &st) == 0 && S_ISDIR(st.st_mode) && !dismissed_has(sup, "r1:7.2.19"), "unreadable: left as it is");
    snprintf(p, sizeof p, "%s/nowhere", scratch);
    CHECK(dismissed_add(p, key, strlen(key)) != 0 && !dismissed_has(p, "r1:7.2.19"), "no support folder: an error");
    CHECK(dismissed_add(NULL, key, 1) != 0 && dismissed_add(sup, NULL, 0) != 0 && !dismissed_has(NULL, key) && !dismissed_has(sup, NULL), "NULLs refused");
}

static void test_facts(const char *scratch, const char *pedir) {
    char rbdir[1024], v19[1100], root[1024], ortdir[1100], models[1024], support[1024], p[1300];
    snprintf(rbdir, sizeof rbdir, "%s/rekordbox", scratch);
    snprintf(v19, sizeof v19, "%s/rekordbox 7.2.19", rbdir);
    snprintf(root, sizeof root, "%s/RB Stems Plus", scratch);
    snprintf(ortdir, sizeof ortdir, "%s/ort", root);
    snprintf(models, sizeof models, "%s/demucs3_model", scratch);
    snprintf(support, sizeof support, "%s/rbstemsplus", scratch);
    mkdir(rbdir, 0755);
    mkdir(v19, 0755);
    mkdir(root, 0755);
    mkdir(ortdir, 0755);
    mkdir(models, 0755);
    mkdir(support, 0755);
    snprintf(p, sizeof p, "%s/rekordbox-x64.exe", pedir);
    FILE *in = fopen(p, "rb");
    CHECK(in != NULL, "the PE fixture %s", p);
    if (!in) return;
    static unsigned char exe[1 << 20];
    size_t exe_n = fread(exe, 1, sizeof exe, in);
    fclose(in);

    fakeos fo;
    memset(&fo, 0, sizeof fo);
    snprintf(fo.signed_exe, sizeof fo.signed_exe, "%s/rekordbox.exe", v19);
    spit(fo.signed_exe, exe, exe_n);
    snprintf(fo.e[0].key, sizeof fo.e[0].key, "Pioneer rekordbox 7.2.19");
    snprintf(fo.e[0].display_name, sizeof fo.e[0].display_name, "rekordbox 7.2.19 64bit");
    snprintf(fo.e[0].display_version, sizeof fo.e[0].display_version, "7.2.19.0342");
    snprintf(fo.e[0].install_location, sizeof fo.e[0].install_location, "%s", v19);
    compat_os os = {fo_entries, fo_pin, fo_procs, fo_native, &fo};
    watch_env env = {&os, rbdir, root, models, support, NULL, 0, "1.4.0", fo_start, fo_log, NULL};
    watch_reader rd;
    memset(&rd, 0, sizeof rd);
    watch_facts f;
    compat_memo_clear();

    CHECK(watch_read_facts(&env, &rd, &f) == 0, "facts read");
    CHECK(f.rb_state == COMPAT_RB_OK && !strcmp(f.rekordbox_version, "7.2.19") && !strcmp(f.folder, v19) && !strcmp(f.watch_name, "rekordbox 7.2.19"),
          "the current rekordbox: %d %s %s", f.rb_state, f.rekordbox_version, f.watch_name);
    CHECK(f.r1_known && f.r1.live == COMPAT_LIVE_MISSING && f.r1.offer_restore && !f.cache_present, "the live DLL missing: R1's offer");
    CHECK(!f.chosen_model && !f.chosen_cache && !f.dismissed && !f.engine[0] && !f.plus_present && !f.have_lists && f.m == NULL,
          "nothing else there");

    fo.e[1] = fo.e[0];
    snprintf(fo.e[1].key, sizeof fo.e[1].key, "Pioneer rekordbox 7.2.20");
    snprintf(fo.e[1].display_version, sizeof fo.e[1].display_version, "7.2.20.0123");
    CHECK((size_t)snprintf(fo.e[1].install_location, sizeof fo.e[1].install_location, "%s/rekordbox 7.2.20", rbdir)
          < sizeof fo.e[1].install_location, "fixture path fits");
    CHECK(watch_read_facts(&env, &rd, &f) == 0 && !f.rb_unsettled && f.inv_complete && f.nrows == 1,
          "a stale entry has no row and never triggers settling");
    CHECK(mkdir(fo.e[1].install_location, 0755) == 0, "the new folder appears");
    CHECK(watch_read_facts(&env, &rd, &f) == 0 && f.rb_unsettled && !f.inv_complete && f.nrows == 2,
          "a real folder awaiting its EXE has a row and triggers settling");
    rmdir(fo.e[1].install_location);
    fo.e[1].key[0] = 0;

    snprintf(p, sizeof p, "%s/onnxruntime.dll", v19);
    spits(p, "MZ... rbstems bridge: 1.5.0 ...");
    snprintf(p, sizeof p, "%s/installed", support);
    spits(p, "model=1\ncache=1\n");
    snprintf(p, sizeof p, "%s/dismissed", support);
    spits(p, "model:0001\n");
    snprintf(p, sizeof p, "%s/demucs3_ver.txt", models);
    spits(p, " 0002\r\n");
    snprintf(p, sizeof p, "%s/hdemucs.onnx", models);
    spits(p, "");
    CHECK(truncate(p, WATCH_OUR_MODEL_SIZE) == 0, "a sparse model of our size");
    CHECK(watch_read_facts(&env, &rd, &f) == 0, "facts read again");
    CHECK(f.r1.live == COMPAT_LIVE_OURS_UNLISTED && f.cache_present && f.r1.offer_restore && f.r1.ort_looked && !f.r1.ort_copy,
          "the bridge's text: Stems Cache on (unlisted: no list); ort\\'s copy looked at, missing");
    CHECK(f.chosen_model && f.chosen_cache && f.dismissed && !strcmp(f.dismissed, "model:0001\n") && f.dismissed_n == 11, "installed, dismissed");
    /* N10: the app's R1 Don't Ask Again, through the one writer, reaches the watcher's reader */
    CHECK(dismissed_add(support, "r1:7.2.19", 9) == 0 && watch_read_facts(&env, &rd, &f) == 0 && f.dismissed
              && !strcmp(f.dismissed, "model:0001\nr1:7.2.19\n"),
          "R1 dismissed for 7.2.19, the watcher's line kept: %s", f.dismissed ? f.dismissed : "(none)");
    CHECK(!strcmp(f.engine, "0002") && f.plus_present, "the engine trimmed, Demucs v4 by its size");
    snprintf(p, sizeof p, "%s/hdemucs.onnx", models);
    CHECK(truncate(p, 1000) == 0 && watch_read_facts(&env, &rd, &f) == 0 && !f.plus_present, "rekordbox's own model: not ours");

    /* the busy marker */
    CHECK(!watch_app_busy(&env), "no marker: not busy");
    snprintf(p, sizeof p, "%s/busy", support);
    spits(p, "123\n456\nuninstall-cache\nUninstalling Stems Cache\n");
    char id[64] = "";
    CHECK(watch_app_busy(&env) && watch_half_uninstalled(&env, id, sizeof id) && !strcmp(id, "uninstall-cache"), "busy, an uninstall marked");
    spits(p, "123\n999\ninstall-cache\nInstalling\n");
    CHECK(!watch_app_busy(&env) && !watch_half_uninstalled(&env, id, sizeof id), "another start time: not busy; not an uninstall");

    /* the support folder's files, replaced whole */
    CHECK(watch_write_support(&env, "request", "reinstall\n", 10) == 0, "the request written");
    snprintf(p, sizeof p, "%s/request", support);
    char buf[64] = "";
    FILE *rq = fopen(p, "rb");
    size_t got = rq ? fread(buf, 1, sizeof buf - 1, rq) : 0;
    if (rq) fclose(rq);
    buf[got] = 0;
    CHECK(!strcmp(buf, "reinstall\n"), "its text: %s", buf);
    snprintf(p, sizeof p, "%s/nowhere", scratch);
    watch_env bad = env;
    bad.support = p;
    CHECK(watch_write_support(&bad, "request", "x", 1) != 0 && strstr(flogb, "couldn't write request"), "no support folder: an error, logged");

    /* W5: the lists and our bridges from signed payloads; installed-release\ read as W3a reads it */
    test_key rel;
    int fixture_ready = test_key_new(&rel) == 0;
    CHECK(fixture_ready, "the signed payload fixture initializes");
    if (!fixture_ready) {
        watch_reader_free(&rd);
        return;
    }
    sig_pubkey keys[1] = {{"test", {0}}};
    memcpy(keys[0].xy, rel.xy, 64);
    env.keys = keys;
    env.nkeys = 1;
    const char *bridge = "MZ... rbstems bridge: 1.5.0 ...";
    char bsha[65];
    sha_hex(bridge, strlen(bridge), bsha);
    char inst[1200], prev[1300], pay[1200];
    snprintf(inst, sizeof inst, "%s/installed-release", support);
    snprintf(prev, sizeof prev, "%s/prev", inst);
    snprintf(pay, sizeof pay, "%s/payload", support);
    mkdir(inst, 0755);
    mkdir(prev, 0755);
    mkdir(pay, 0755);
    signed_pair(&rel, inst, "1.4.0", "1.4.0", bsha);
    snprintf(p, sizeof p, "%s/payload-version", support);
    spits(p, "1.5.0\n");   /* a newer payload accepted, not installed yet */
    snprintf(p, sizeof p, "%s/onnxruntime.dll", ortdir);
    spits(p, "Pioneer's ORT");   /* ort\'s copy there: nothing to put back */
    snprintf(p, sizeof p, "%s/payload-version", support);
    CHECK(watch_read_facts(&env, &rd, &f) == 0 && f.have_lists && f.m == &rd.installed && f.r1.live == COMPAT_LIVE_OURS && !f.r1.offer_restore,
          "a raised floor: installed-release still counts, the bridge is ours (%d)", f.r1.live);
    signed_pair(&rel, inst, "1.5.2", "1.4.0", bsha);
    signed_pair(&rel, pay, "1.5.3", "1.4.0", bsha);
    spits(p, "1.5.3\n");
    CHECK(watch_read_facts(&env, &rd, &f) == 0 && f.m == &rd.saved && !strcmp(rd.installed.payload_version, "1.5.2")
              && !strcmp(rd.saved.payload_version, "1.5.3"),
          "a record carrying an older app: accepted, and payload\\ read");
    signed_pair(&rel, pay, "1.5.4-beta.1", "1.4.0", bsha);
    spits(p, "1.5.3\n");
    CHECK(watch_read_facts(&env, &rd, &f) == 0 && f.m == &rd.installed, "a suffixed payload\\: ignored (no beta channel)");
    signed_pair(&rel, pay, "1.5.3", "1.4.0", bsha);
    signed_pair(&rel, inst, "1.5.0", "1.3.0", bsha);
    snprintf(p, sizeof p, "%s/payload.json", pay);
    unlink(p);
    CHECK(watch_read_facts(&env, &rd, &f) == 0 && !f.have_lists && f.r1.live == COMPAT_LIVE_OURS_UNLISTED, "another app's record: ignored");
    signed_pair(&rel, inst, "1.5.1-beta.1", "1.4.0", bsha);
    CHECK(watch_read_facts(&env, &rd, &f) == 0 && !f.have_lists && f.m != &rd.installed, "a suffixed installed-release\\: ignored");
    signed_pair(&rel, prev, "1.4.0", "1.4.0", bsha);
    snprintf(p, sizeof p, "%s/payload.json.sig", inst);
    unlink(p);
    CHECK(watch_read_facts(&env, &rd, &f) == 0 && f.have_lists && f.m == &rd.installed && f.r1.live == COMPAT_LIVE_OURS,
          "a half-written record: prev\\ counts");
    test_key_free(&rel);
    watch_reader_free(&rd);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s SCRATCH PE\n", argv[0]);
        return 2;
    }
    test_classify();
    test_engine();
    test_engine_idle();
    test_sched();
    test_rows_digest_refresh();
    test_early_compat_refresh();
    test_stale_entry_no_retry();
    test_grouped_add_prompt();
    test_unsupported_notice_once();
    test_repair_precedence();
    test_removal_silent();
    test_registry_update_refreshes_open_app();
    test_check();
    test_dismissed(argv[1]);
    test_facts(argv[1], argv[2]);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
