// The root scripts: Stems Cache in and out (Admin.swift passes them to bash as an argument, never
// as a file). Their text is constant. The fixed places (rekordbox's folder, the root folder, the
// lock) and AlphaTheta's team are written into it; what changes (the action, the rekordbox version
// and bundle ID the app checked, the checksums, the force flag) follows as arguments, and the
// bridge's bytes come on stdin. Nothing in the user's folder is read by root.
//
//   sudo -n /bin/bash -c <body> rbsp <action> <version> <bundle ID> [<bridge sha> <ort sha> <force>]
//
// Both scripts start with the same checks (scriptHeader), then work only relative to rekordbox's
// Contents folder, so a folder swapped in /Applications (which admin accounts can write) after the
// checks is never followed. codesign never gets a path in /Applications: it opens files again by
// their full path, so a folder renamed and replaced during a call would be followed. It works on a
// copy of rekordbox.app made from that Contents folder ("..") into the root-only staging folder,
// and the two files the re-sign changes are copied back by relative path. The folder is still
// checked to be at rekordbox's place before and after each copy and each codesign call.
//
// The saved set: Pioneer's three files the re-sign changes, in pioneer/<version>/ of the root
// folder, with SHA256SUMS written last (a set without it, from v1.0, counts only if its library is
// AlphaTheta's). Whenever rekordbox doesn't verify (a run was stopped half-way) and the set is
// complete, it is put back first. The originals are deleted only once rekordbox verifies as
// AlphaTheta's again.
//
// "# step:" comments mark the points where the tests stop or fail a run (app/Tests); they do nothing.
import Foundation

/// A root script, its arguments ($1…) and what it reads on stdin.
struct AdminScript { let body: String; let args: [String]; var input: Data? = nil }

/// The root scripts' exit codes (any other: a step failed before rekordbox was changed).
enum RootExit {
    static let noOriginals: Int32 = 3        // no complete saved set for this rekordbox: nothing changed
    static let checkFailed: Int32 = 4        // a check failed: nothing changed
    static let rekordboxOpen: Int32 = 5      // nothing changed
    static let rolledBack: Int32 = 6         // a step failed after rekordbox was changed; Pioneer's files are back
    static let alreadyRunning: Int32 = 7     // another root run holds the lock: nothing changed
    static let installerRunning: Int32 = 8   // Pioneer's installer or updater is running: nothing changed
    static let rekordboxMissing: Int32 = 9   // rekordbox's folder in /Applications is missing: nothing changed
    static let notPioneers: Int32 = 10       // rekordbox isn't as Pioneer's installer leaves it: nothing changed
    static let rollbackFailed: Int32 = 11    // a step failed after rekordbox was changed, and so did putting Pioneer's files back
}

/// rekordbox's bundle ID: root checks that rekordbox's Info.plist says it.
let rbBundleID = "com.pioneerdj.rekordboxdj"
/// rekordbox's folder in /Applications ("rekordbox 7"): Pioneer's installer creates and replaces it.
let rbFolder = (rbApp as NSString).deletingLastPathComponent
/// The root scripts' lock: a folder with the owner's pid and start time. /var/run is emptied at boot.
let rootLock = "/var/run/rbstemsplus.lock"

/// The start of both scripts, in this order:
/// - its own PATH (macOS's sudo keeps the caller's) and no HOME; a closed output (the app quit)
///   never stops it: SIGPIPE is ignored and every message goes through say, which ignores
///   errors (other programs' messages are collected by x and cs and passed to say). say is
///   /usr/bin/printf, not bash's: bash keeps what its printf couldn't write and writes it later
///   into the next command substitution or file, which would corrupt the checks and SHA256SUMS;
/// - one exit trap (finish): the rollback when rekordbox was changed and a step failed, then the
///   staging folder and the lock;
/// - the lock, then Pioneer's installer or updater, then rekordbox open (by process name: a
///   pattern would also match this script's own command line; Installer.app by its path, as
///   Sparkle's updaters also run a process named Installer);
/// - rekordbox's folder (missing: its own exit code; remove-root-folder needs it missing);
/// - cd into Contents, then: Contents, rekordbox.app and rekordbox 7, Frameworks, MacOS and
///   _CodeSignature are real folders owned by root that nobody else can write, and the three
///   files and Info.plist are such regular files (the rest of Frameworks has Pioneer's own links);
/// - the bundle ID and version, which must match the arguments; the version is digits and dots;
/// - the root folder (a real folder of root's alone; created if missing, and an empty one is
///   removed again when an uninstall ends);
/// - leftovers of a stopped run, by exact name (also the install's probe, .rbsp-probe);
/// - the staging folder in the root folder, for the copies of rekordbox.app codesign checks and
///   signs (fresh, cs). On rekordbox's disk (normally) a copy is a clone: quick, and takes no space.
private func scriptHeader(actions: String) -> String {
    #"""
    export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C; unset HOME CDPATH; IFS=$' \t\n'
    trap '' PIPE
    set -eu
    say() { /usr/bin/printf '%s\n' "$*" 2>/dev/null || true; }
    RBDIR='\#(rbFolder)'; APP="$RBDIR/rekordbox.app"
    OUT='\#(rootDir)'; LOCK='\#(rootLock)'
    OWNER=0:0
    PIONEER='anchor apple generic and certificate leaf[subject.OU] = "\#(pioneerTeam)"'
    LIB=Frameworks/libonnxruntime.1.18.0.dylib
    THREE="MacOS/rekordbox _CodeSignature/CodeResources $LIB"
    ACTION="${1-}"; VERSION="${2-}"; BUNDLE="${3-}"
    LOCKED=0; HERE=; S=; RB=; CLONE=; P=; CHANGING=0; FINISHING=0
    case "$ACTION" in \#(actions)) ;; *) say "unknown action \"$ACTION\": nothing changed"; exit 4;; esac
    [ "$(id -u)" = "${OWNER%%:*}" ] || { say "not running as root: nothing changed"; exit 4; }

    # another program's messages, passed on through say
    x() { local out rc=0; out="$("$@" 2>&1)" || rc=$?; [ -z "$out" ] || say "$out"; return $rc; }
    sha() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1; }
    # the folder checked below is still the one at rekordbox's place; if not, the run stops
    # (while finishing, the caller is told instead)
    same_place() { [ "$(stat -f %d:%i . 2>/dev/null)" = "$HERE" ] && [ "$(stat -f %d:%i "$APP/Contents" 2>/dev/null)" = "$HERE" ]; }
    still_there() {
      same_place && return 0
      say "rekordbox's folder was moved or replaced during the run"; [ "$FINISHING" = 1 ] && return 1; exit 10
    }
    # RB: a fresh copy of rekordbox.app, made from the Contents folder this script is in. ".." is
    # followed by the kernel from that folder, wherever it was moved, and rekordbox.app and its
    # folders are root's alone (checked below), so the copy is what is in rekordbox. A copy that
    # can't be made stops the run (a rollback if rekordbox was changed).
    fresh() {
      [ -n "$S" ] && [ -n "$RB" ] || exit 4
      still_there || return 1
      if ! { rm -rf "$RB" && x cp ${CLONE:+-c} -R .. "$RB"; }; then
        say "couldn't copy rekordbox to check it (is the disk full?)"
        [ "$FINISHING" = 1 ] && return 1; [ "$CHANGING" = 1 ] && exit 1; exit 4
      fi
      still_there
    }
    # codesign, only ever on the copy (never a path in /Applications)
    cs() {
      local out rc=0
      still_there || return 1
      out="$(codesign "$@" "$RB" 2>&1)" || rc=$?
      [ -z "$out" ] || say "$out"
      still_there || return 1
      return $rc
    }
    has_bridge() { grep -a -q 'rbstems bridge: ' "$RB/Contents/$LIB" 2>/dev/null; }
    is_pioneers() { fresh && cs --verify --strict -R="$PIONEER"; }
    is_ours() { fresh && has_bridge && cs --verify --deep --strict; }
    # "<sha256>  <file>" for the three files under $1
    sums() { local f; for f in $THREE; do printf '%s  %s\n' "$(sha "$1/$f")" "$f"; done; }
    # the saved set is whole: its checksums match, or (v1.0, no checksums) its library is AlphaTheta's
    set_complete() {
      local f
      for f in $THREE; do [ -f "$P/$f" ] && [ ! -L "$P/$f" ] || return 1; done
      if [ -f "$P/SHA256SUMS" ]; then [ "$(sums "$P")" = "$(cat "$P/SHA256SUMS")" ]
      else codesign --verify -R="$PIONEER" "$P/$LIB" >/dev/null 2>&1; fi
    }
    # Pioneer's three files back from the saved set: the executable, CodeResources, the library
    # last (while the bridge is in, a later run sees that rekordbox needs its files back)
    restore_set() {
      local f
      for f in $THREE; do
        rm -f "$f.rbsp-restore" 2>/dev/null || true
        if ! { x cp -p "$P/$f" "$f.rbsp-restore" && x mv -f "$f.rbsp-restore" "$f"; }; then
          rm -f "$f.rbsp-restore" 2>/dev/null || true; return 1
        fi
        # step: restored-file
      done
    }
    # a file the re-sign changed, from the signed copy into rekordbox by its relative path: a new
    # file next to it, then renamed over it (as codesign itself does)
    put() {
      rm -f "$1.rbsp-new" 2>/dev/null || true
      if ! { x cp "$RB/Contents/$1" "$1.rbsp-new" && x mv -f "$1.rbsp-new" "$1"; }; then
        rm -f "$1.rbsp-new" 2>/dev/null || true; return 1
      fi
    }
    rollback() {
      rm -f Frameworks/.bridge.new 2>/dev/null || true
      restore_set || say "putting Pioneer's files back failed"
      if is_pioneers; then say "rolled back: rekordbox has Pioneer's original files again"; return 6; fi
      if is_ours; then say "Pioneer's files couldn't be put back: rekordbox still has Stems Cache"; return 4; fi
      say "ROLLBACK FAILED: reinstall rekordbox from Pioneer's installer"; return 11
    }
    # the only exit trap: the rollback if rekordbox was changed and a step failed, then the cleanup
    finish() {
      local rc=$?
      set +e; FINISHING=1; trap '' INT TERM HUP
      if [ "$CHANGING" = 1 ] && [ "$rc" != 0 ]; then
        say "failed (exit $rc) after rekordbox was changed"; rollback; rc=$?
      fi
      if [ -n "$HERE" ]; then rm -f Frameworks/.bridge.new Frameworks/.rbsp-probe MacOS/rekordbox.rbsp-new _CodeSignature/CodeResources.rbsp-new 2>/dev/null; fi
      if [ -n "$S" ]; then rm -rf "$S" 2>/dev/null; fi
      # an uninstall leaves no empty root folder (one may have been made for the staging folder)
      if [ "$LOCKED" = 1 ] && [ "$ACTION" != install ] && [ -d "$OUT" ] && [ ! -L "$OUT" ]; then rmdir "$OUT" 2>/dev/null; fi
      if [ "$LOCKED" = 1 ] && [ ! -L "$LOCK" ] && [ "$(head -n 1 "$LOCK/owner" 2>/dev/null)" = "$$" ]; then rm -rf "$LOCK"; fi
      exit "$rc"
    }
    trap finish EXIT
    trap 'exit 130' INT; trap 'exit 143' TERM; trap 'exit 129' HUP

    # the lock: one root run at a time. A lock whose process is gone (by pid and start time) is
    # from a stopped run: it is moved aside before it is removed, so only one run takes it over.
    started() { ps -o lstart= -p "$1" 2>/dev/null || true; }
    lock_live() {
      local pid start
      pid="$(sed -n 1p "$LOCK/owner" 2>/dev/null || true)"; start="$(sed -n 2p "$LOCK/owner" 2>/dev/null || true)"
      case "$pid" in
        ''|*[!0-9]*)                   # being taken right now, or a run stopped before writing it
          [ $(( $(date +%s) - $(stat -f %m "$LOCK" 2>/dev/null || printf 0) )) -lt 10 ];;
        *) kill -0 "$pid" 2>/dev/null && { [ -z "$start" ] || [ "$(started "$pid")" = "$start" ]; };;
      esac
    }
    for try in 1 2 3; do
      if mkdir -m 700 "$LOCK" 2>/dev/null; then
        LOCKED=1
        (set -C; printf '%s\n%s\n' "$$" "$(started $$)" > "$LOCK/owner")
        break
      fi
      if [ -L "$LOCK" ] || [ ! -d "$LOCK" ] || lock_live; then say "another change to rekordbox is running: nothing changed"; exit 7; fi
      old="$(cat "$LOCK/owner" 2>/dev/null || true)"
      say "removing the lock of a stopped run"
      if mv "$LOCK" "$LOCK.stale.$$" 2>/dev/null; then
        if [ "$(cat "$LOCK.stale.$$/owner" 2>/dev/null || true)" = "$old" ]; then rm -rf "$LOCK.stale.$$"
        else
          [ -e "$LOCK" ] || mv "$LOCK.stale.$$" "$LOCK" 2>/dev/null || true
          say "another change to rekordbox is running: nothing changed"; exit 7
        fi
      fi
    done
    [ "$LOCKED" = 1 ] || { say "another change to rekordbox is running: nothing changed"; exit 7; }
    # step: locked

    running() {
      local rc=0; pgrep "$@" >/dev/null 2>&1 || rc=$?
      [ "$rc" -le 1 ] || { say "couldn't list the running programs: nothing changed"; exit 4; }
      [ "$rc" = 0 ]
    }
    if running -x installer || running -x 'Upmgr rekordbox' || running -f '\#(appleInstallerPattern)'; then
      say "Pioneer's installer or updater is running: nothing changed"; exit 8
    fi
    if running -x rekordbox; then say "rekordbox is open: nothing changed"; exit 5; fi

    # rekordbox's folder missing: nothing to change; only the root folder can go (when the app asks)
    if [ ! -e "$RBDIR" ] && [ ! -L "$RBDIR" ]; then
      if [ "$ACTION" = remove-root-folder ]; then
        if [ -L "$OUT" ] || [ -f "$OUT" ]; then x rm -f "$OUT"; elif [ -d "$OUT" ]; then x rm -rf "$OUT"; fi
        say "rekordbox isn't installed: removed $OUT"; exit 0
      fi
      say "rekordbox isn't installed ($RBDIR is missing): nothing changed"; exit 9
    fi
    if [ "$ACTION" = remove-root-folder ]; then say "rekordbox is installed: $OUT stays"; exit 4; fi

    # rekordbox as Pioneer's installer leaves it: real folders and files, root's alone
    bad() { say "$1: reinstall rekordbox with Pioneer's installer. Nothing changed"; exit 10; }
    rootonly() {
      [ "$(stat -f %u "$1")" = "${OWNER%%:*}" ] || bad "$2 isn't owned by root"
      [ $(( 8#$(stat -f %Lp "$1") & 8#022 )) = 0 ] || bad "$2 can be changed by other accounts"
    }
    rootdir() { [ -d "$1" ] && [ ! -L "$1" ] || bad "$2 isn't a folder"; rootonly "$1" "$2"; }
    for d in "$RBDIR" "$APP" "$APP/Contents"; do [ -d "$d" ] && [ ! -L "$d" ] || bad "$d isn't a folder"; done
    cd -P "$APP/Contents" 2>/dev/null || bad "can't open $APP/Contents"
    HERE="$(stat -f %d:%i .)"
    [ "$(stat -f %d:%i "$APP/Contents")" = "$HERE" ] && [ "$(stat -f %d:%i "$APP")" = "$(stat -f %d:%i ..)" ] \
      && [ "$(stat -f %d:%i "$RBDIR")" = "$(stat -f %d:%i ../..)" ] || bad "rekordbox's folder changed while it was checked"
    rootdir ../.. "$RBDIR"; rootdir .. rekordbox.app; rootdir . Contents
    for d in Frameworks MacOS _CodeSignature; do rootdir "$d" "$d"; done
    for f in $THREE Info.plist; do [ -f "$f" ] && [ ! -L "$f" ] || bad "$f isn't a file"; rootonly "$f" "$f"; done
    ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' Info.plist 2>/dev/null)" || ID=
    V="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' Info.plist 2>/dev/null)" || V=
    [ "$ID" = "$BUNDLE" ] || bad "the app in $RBDIR is \"$ID\", not rekordbox"
    VERSION_RE='^[0-9]+(\.[0-9]+)*$'
    [[ "$V" =~ $VERSION_RE ]] || bad "rekordbox's version \"$V\" isn't a version number"
    [ "$V" = "$VERSION" ] || { say "rekordbox is $V now, not $VERSION as checked: nothing changed"; exit 4; }
    P="$OUT/pioneer/$V"

    # the root folder: root's alone (made here if missing: the staging folder goes in it)
    if [ ! -e "$OUT" ] && [ ! -L "$OUT" ]; then x mkdir -m 755 "$OUT"; fi
    for d in "$OUT" "$OUT/pioneer" "$P" "$P/Frameworks" "$P/MacOS" "$P/_CodeSignature" "$OUT/ort"; do
      if [ -L "$d" ] || { [ -e "$d" ] && [ ! -d "$d" ]; }; then say "$d isn't a folder: nothing changed"; exit 4; fi
    done
    if [ -d "$OUT" ]; then
      [ "$(stat -f %u "$OUT")" = "${OWNER%%:*}" ] && [ $(( 8#$(stat -f %Lp "$OUT") & 8#022 )) = 0 ] \
        || { say "$OUT isn't root's alone: nothing changed"; exit 4; }
    fi

    # leftovers of a stopped run (in rekordbox: a write App Management may refuse until allowed)
    for f in $THREE; do for g in "$f.rbsp-restore" "$f.rbsp-new"; do if [ -e "$g" ] || [ -L "$g" ]; then x rm -rf "$g"; fi; done; done
    for f in Frameworks/.bridge.new Frameworks/.rbsp-probe MacOS/rekordbox.cstemp; do if [ -e "$f" ] || [ -L "$f" ]; then x rm -rf "$f"; fi; done
    x rm -rf "$OUT"/stage.??????
    for f in $THREE SHA256SUMS; do x rm -rf "$P/$f.part"; done
    umask 022
    # the staging folder (root's alone), with the copies of rekordbox.app for codesign: clones on
    # rekordbox's disk, full copies (slower, and taking space) on another
    S="$(mktemp -d "$OUT/stage.XXXXXX")"; RB="$S/rekordbox.app"
    if [ "$(stat -f %d .)" = "$(stat -f %d "$S")" ]; then CLONE=1
    else say "the root folder isn't on rekordbox's disk: rekordbox is copied in full for each check (slower)"; fi
    say "rekordbox $V checked, running as $(id -un)"
    """#
}

/// Stems Cache in: save Pioneer's three files once per rekordbox version (again when Pioneer
/// re-shipped the same version), copy Pioneer's library to the root-owned ort/ folder (where the
/// bridge loads it), swap in the bridge, re-sign.
/// - bridge, bridgeSha: the verified payload's bridge (its bytes, sent on stdin) and
///   payload.json's checksum for it. Root reads at most 32 MB and checks its own copy.
/// - ortSha: the checksum of Pioneer's library the app checked (in rekordbox, or the ort/ copy
///   when the bridge is already in).
/// - force: swap the bridge in again even if one is there (Reinstall).
/// - version: the rekordbox version the app checked; root stops if rekordbox has another.
/// Before any write, rekordbox must verify as AlphaTheta's or as our complete seal (or, after a
/// stopped run, get Pioneer's saved files back first). Every file is checked in a root-only
/// staging folder, so it can't be swapped after the check. The re-sign happens on a copy of
/// rekordbox.app there; then the bridge, the executable and CodeResources (the files the re-sign
/// changed) go into rekordbox by relative path, and a fresh copy must verify. Once rekordbox is
/// touched, any failure (e.g. a full disk) puts Pioneer's three files back.
func cacheInstallScript(bridge: Data, bridgeSha: String, ortSha: String, force: Bool, version: String = rekordboxVersion()) -> AdminScript {
    AdminScript(body: cacheInstallBody, args: ["install", version, rbBundleID, bridgeSha, ortSha, force ? "1" : "0"], input: bridge)
}

let cacheInstallBody = scriptHeader(actions: "install") + "\n" + #"""
    # step: checked
    # the first write into rekordbox, before the slow work (copies, saving, signing): App
    # Management refuses it with "Operation not permitted" until the user allows RB Stems Plus,
    # so they are asked at once, and a Try Again doesn't repeat that work. An empty file, removed
    # again (finish and the next run's leftover cleanup remove it too). Only the install probes:
    # an uninstall of a rekordbox that is already Pioneer's writes nothing into it.
    if ! { x touch Frameworks/.rbsp-probe && x rm -f Frameworks/.rbsp-probe; }; then
      rm -f Frameworks/.rbsp-probe 2>/dev/null || true
      say "couldn't write into rekordbox: nothing changed"; exit 4
    fi
    BRIDGE_SHA="${4-}"; ORT_SHA="${5-}"; FORCE="${6-}"
    SHA_RE='^[0-9a-f]{64}$'
    [[ "$BRIDGE_SHA" =~ $SHA_RE ]] && [[ "$ORT_SHA" =~ $SHA_RE ]] || { say "a checksum argument isn't a checksum: nothing changed"; exit 4; }
    ORT="$OUT/ort/libonnxruntime.1.18.0.dylib"
    head -c 33554433 > "$S/bridge.dylib"
    [ "$(stat -f %z "$S/bridge.dylib")" -le 33554432 ] || { say "the bridge is over 32 MB: nothing changed"; exit 4; }
    [ "$(sha "$S/bridge.dylib")" = "$BRIDGE_SHA" ] || { say "the bridge's checksum doesn't match payload.json: nothing changed"; exit 4; }
    # step: staged

    if is_pioneers; then STATE=pioneer
    elif is_ours; then STATE=ours
    elif set_complete; then
      say "rekordbox doesn't verify (a run was stopped half-way): putting Pioneer's files back first"
      CHANGING=1
      restore_set || exit 1
      is_pioneers || exit 1
      CHANGING=0; STATE=pioneer
      # step: repaired
    else bad "rekordbox isn't signed by AlphaTheta, nor complete with the bridge, and no saved copy of its files is complete"
    fi

    if [ "$STATE" = ours ] && [ "$FORCE" != 1 ]; then
      set_complete || { say "no complete saved copy of rekordbox $V's files: nothing changed"; exit 3; }
      say "Stems Cache is already in place"; exit 0
    fi
    if [ "$STATE" = pioneer ]; then
      # Pioneer's three files: saved once per version, again if Pioneer re-shipped this version.
      # The set's library goes last and its checksums after it; one sync at the end.
      LIVE="$(sums .)"
      if [ -f "$P/SHA256SUMS" ] && [ "$(cat "$P/SHA256SUMS")" = "$LIVE" ] && set_complete; then
        say "Pioneer's files of rekordbox $V are saved already"
      else
        say "saving Pioneer's files of rekordbox $V"
        x mkdir -p "$P/Frameworks" "$P/MacOS" "$P/_CodeSignature"
        x rm -f "$P/SHA256SUMS" "$P/$LIB"
        for f in $THREE; do
          x cp -p "$f" "$P/$f.part"; x mv -f "$P/$f.part" "$P/$f"
          # step: saved-file
        done
        [ "$(sums "$P")" = "$LIVE" ] || { say "the saved copy differs from rekordbox's files: nothing changed"; exit 4; }
        x codesign --verify -R="$PIONEER" "$P/$LIB" || { say "the saved library isn't signed by AlphaTheta: nothing changed"; exit 4; }
        printf '%s\n' "$LIVE" > "$P/SHA256SUMS.part"; x mv -f "$P/SHA256SUMS.part" "$P/SHA256SUMS"
        sync
        # step: saved
      fi
      x cp "$LIB" "$S/ort.dylib"
      [ "$(sha "$S/ort.dylib")" = "$ORT_SHA" ] || { say "rekordbox's ONNX Runtime changed after it was checked: nothing changed"; exit 4; }
      x codesign --verify -R="$PIONEER" "$S/ort.dylib" || { say "rekordbox's ONNX Runtime isn't signed by AlphaTheta: nothing changed"; exit 4; }
    else
      set_complete || { say "no complete saved copy of rekordbox $V's files: nothing changed"; exit 3; }
      [ "$(sha "$ORT")" = "$ORT_SHA" ] || { say "Pioneer's ONNX Runtime copy changed after it was checked: nothing changed"; exit 4; }
      x codesign --verify -R="$PIONEER" "$ORT" || { say "Pioneer's ONNX Runtime copy isn't signed by AlphaTheta: nothing changed"; exit 4; }
    fi
    cs -d --entitlements "$S/ents.plist" --xml && [ -s "$S/ents.plist" ] || { say "couldn't read rekordbox's entitlements: nothing changed"; exit 4; }
    if [ "$STATE" = pioneer ]; then
      x mkdir -p "$OUT/ort"; x mv -f "$S/ort.dylib" "$ORT"
      # step: ort-copied
    fi
    # only root may change what the bridge loads and what uninstalling puts back; the bridge
    # refuses a library others can change
    x chown -R "$OWNER" "$OUT"; x chmod -R go-w "$OUT"

    # the re-sign, on the copy of rekordbox (the last one checked above): the bridge in, then
    # codesign, which rewrites the executable and CodeResources. rekordbox itself is unchanged.
    x cp "$S/bridge.dylib" "$RB/Contents/$LIB"
    cs -f -s - --options runtime --entitlements "$S/ents.plist" || { say "couldn't sign the copy of rekordbox: nothing changed"; exit 4; }
    cs --verify --deep --strict || { say "the signed copy of rekordbox doesn't verify: nothing changed"; exit 4; }
    # step: signed-copy

    # the bridge next to its place (App Management allowed it at the start; a full disk may still
    # refuse it); rekordbox is unchanged so far, and a partial copy is removed by finish
    x cp "$S/bridge.dylib" Frameworks/.bridge.new || { say "couldn't write into rekordbox: nothing changed"; exit 4; }
    # step: bridge-copied
    # from here on rekordbox changes: if anything fails, Pioneer's three files go back (finish)
    CHANGING=1
    x mv -f Frameworks/.bridge.new "$LIB"
    # step: swapped
    put MacOS/rekordbox
    put _CodeSignature/CodeResources
    # step: signed
    [ "$(sums .)" = "$(sums "$RB/Contents")" ] || { say "rekordbox's files aren't the signed copy's"; exit 1; }
    is_ours || { say "rekordbox doesn't verify after the re-sign"; exit 1; }
    CHANGING=0
    sync
    # step: installed
    # the originals of earlier rekordbox versions are never used again (uninstall restores this one's)
    for d in "$OUT/pioneer"/*; do if [ "$d" != "$P" ] && [ -e "$d" ]; then x rm -rf "$d"; fi; done
    say "Stems Cache installed"
    """#

/// Stems Cache out: Pioneer's three files back from the saved set, then (once rekordbox verifies
/// as AlphaTheta's) the root folder removed. If rekordbox already verifies as AlphaTheta's (a
/// rekordbox update replaced the bridge), only the root folder is removed. If it verifies as
/// neither (a stopped run), the saved set is put back the same way. If putting the files back
/// fails, finish tries once more, and the saved set stays.
func cacheUninstallScript(version: String = rekordboxVersion()) -> AdminScript {
    AdminScript(body: cacheUninstallBody, args: ["uninstall", version, rbBundleID])
}

/// The root folder alone, for when rekordbox is gone (a run exited with RootExit.rekordboxMissing)
/// and the user agreed: root checks itself that rekordbox's folder is still missing, and removes
/// nothing else. Pioneer's installer brings rekordbox back whole.
func removeRootFolderScript() -> AdminScript {
    AdminScript(body: cacheUninstallBody, args: ["remove-root-folder", "", ""])
}

let cacheUninstallBody = scriptHeader(actions: "uninstall|remove-root-folder") + "\n" + #"""
    if is_pioneers; then
      # step: pioneers
      if [ -e "$OUT" ]; then x rm -rf "$OUT"; fi
      say "rekordbox is signed by AlphaTheta (the bridge isn't in it): removed the saved files"; exit 0
    fi
    set_complete || { say "no complete saved copy of rekordbox $V's files: nothing changed (reinstall rekordbox from Pioneer's installer)"; exit 3; }
    if is_ours; then say "putting Pioneer's files back"
    else say "rekordbox doesn't verify (a run was stopped half-way): putting Pioneer's files back"; fi
    # step: restoring
    # the first write into rekordbox, before anything changes (App Management refuses it with
    # "Operation not permitted" until the user allows RB Stems Plus): a temporary name, removed
    # again. Not earlier: when rekordbox is already Pioneer's, nothing is written into it.
    first="${THREE%% *}.rbsp-restore"
    x cp -p "$P/${THREE%% *}" "$first" || { rm -f "$first" 2>/dev/null || true; say "couldn't write into rekordbox: nothing changed"; exit 4; }
    rm -f "$first"
    CHANGING=1
    restore_set || exit 1
    # step: restored
    fresh && cs --verify --deep --strict -R="$PIONEER" || { say "rekordbox doesn't verify as AlphaTheta's after putting its files back"; exit 1; }
    CHANGING=0
    sync
    # step: verified
    x rm -rf "$OUT"
    say "Stems Cache removed: rekordbox is signed by AlphaTheta again"
    """#
