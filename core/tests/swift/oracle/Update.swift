// Updates. At launch, at most once a day (or at once after the install command put a newer app
// here: manifestDue), the app downloads payload.json (Payload.swift); if its
// app.version is newer, a line in the status line and "Update RB Stems Plus" in the app menu. Update RB Stems Plus does
// what the bootstrap does: download the app's zip, verify its sha256, then, once this app has quit,
// copy the new app next to it, sign it ad hoc (its watcher helper first), swap the two in one step
// (swapApps), register it with LaunchServices and open it (updateScript). The new app, once open,
// registers its watcher again for its new code (checkWatcher).
// A payload-only change needs nothing here: Install and Reinstall use the latest payload.
//
// One version for both platforms (from 1.4.0). A release's VERSION, tag and payload_version are
// one number; app.version is the version of the last release that changed the Mac app's files.
// A release that leaves them unchanged carries the previous zip and its app.version forward byte
// for byte (scripts/release.sh), so: no notice and no swap for it (app.version isn't newer than
// this app), and an app from before 1.4.0 that updates gets that zip, whose own version is
// app.version, as its check wants. The app shows the release's version once a verified
// payload.json names this very app (ShownVersion). What release.sh must write for this:
// docs/RELEASING.md, "One version for both platforms".
import AppKit

let lsregister = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"

/// Whether `version` is newer than this app's ("1.0.10" > "1.0.9"): the app version of the
/// release only, never payload_version (a carried app is no update).
func newerThanApp(_ version: String, own: String = appVersion) -> Bool { version.compare(own, options: .numeric) == .orderedDescending }

/// The notice for a verified payload.json whose app is `app` (its app.version), on an app whose
/// own version is `own`: it names the app it would install, or nil when that isn't newer.
func updateNotice(app: String, own: String = appVersion) -> String? {
    newerThanApp(app, own: own) ? "RB Stems Plus \(app) is available: choose Update RB Stems Plus in the RB Stems Plus menu." : nil
}

// MARK: the version shown (the release's)

/// The release version this app shows (About, the report, the logs), recorded next to the floor
/// with the app version it was recorded for: `shown`, the payload_version of a verified
/// payload.json whose app.version is this very app's own version `app`.
/// The rule (moves to the shared core's version text as is, with these vectors):
/// - a verified payload.json naming this app as its app moves the record to its payload_version
///   (never back: a lower one leaves it);
/// - one naming another app (a newer release this app hasn't installed yet) leaves it, so the
///   shown version doesn't step back;
/// - a record for another app version (this app was replaced since) counts as none, and is
///   cleared; with none, the app's own version is shown.
/// The same app bytes always have the same own version, so a record never names a release
/// whose app this isn't.
struct ShownVersion: Equatable {
    var shown: String
    var app: String

    /// The record's file: the shown version, then the app's own version, one per line.
    var data: Data { Data("\(shown)\n\(app)\n".utf8) }
    init(shown: String, app: String) { self.shown = shown; self.app = app }
    /// A record read back, or nil if it isn't one (both release versions, the shown one not
    /// older than the app's).
    init?(_ data: Data?) {
        guard let d = data, d.count < 64 else { return nil }
        let l = String(decoding: d, as: UTF8.self).split(separator: "\n", omittingEmptySubsequences: false)
        guard l.count == 3, l[2].isEmpty, isReleaseVersion(String(l[0])), isReleaseVersion(String(l[1])),
              !olderPayload(String(l[0]), than: String(l[1])) else { return nil }
        shown = String(l[0]); app = String(l[1])
    }
}

/// The record after this app (own version `own`) has read a verified payload.json (`seen`: its
/// payload_version and app.version; nil: none), from the record before. nil: no record.
func nextShownVersion(own: String, record: ShownVersion?, seen: (payload: String, app: String)?) -> ShownVersion? {
    guard isReleaseVersion(own) else { return nil }
    let kept = record.flatMap { r in r.app == own && isReleaseVersion(r.shown) && !olderPayload(r.shown, than: own) ? r : nil }
    guard let s = seen, s.app == own, isReleaseVersion(s.payload), !olderPayload(s.payload, than: own),
          kept.map({ olderPayload($0.shown, than: s.payload) }) ?? true else { return kept }
    return ShownVersion(shown: s.payload, app: own)
}

/// The version shown by an app whose own version is `own`, with this record.
func shownVersion(own: String, record: ShownVersion?) -> String {
    nextShownVersion(own: own, record: record, seen: nil)?.shown ?? own
}

/// The version shown now (read only: the watcher, the report, the logs, About).
func currentShownVersion(_ f: PayloadFiles = PayloadFiles(), own: String = appVersion) -> String {
    shownVersion(own: own, record: ShownVersion(FileManager.default.contents(atPath: f.shown)))
}

/// After a payload.json was read or downloaded: moves the record by the rule, or clears one left
/// by another app version. Only a verified one counts (`signedBy`: checkManifest's). Returns the
/// version shown now.
@discardableResult
func noteShownVersion(_ m: Manifest?, _ f: PayloadFiles = PayloadFiles(), own: String = appVersion) -> String {
    let data = FileManager.default.contents(atPath: f.shown), before = ShownVersion(data)
    let seen = m.flatMap { $0.signedBy == nil ? nil : (payload: $0.payloadVersion, app: $0.app.version) }
    let after = nextShownVersion(own: own, record: before, seen: seen)
    if let a = after {
        if a.data != data { writeAtomically(a.data, to: f.shown) }
    } else if let t = fileType(f.shown), t != S_IFDIR {         // that one file (or a link there), nothing else
        safeRemove(f.shown, Deletable(inside: [], roots: [], files: [f.shown], apps: []))
    }
    return after?.shown ?? own
}

/// What a payload.json just read or downloaded (verified: only those move the record) means for
/// this app: first the version shown is recorded (noteShownVersion, so About, the report and
/// the logs follow it), then the update notice, or nil (updateNotice). updateFound's work
/// without the menu item, so the tests can run it on their own files.
@discardableResult
func manifestSeen(_ m: Manifest, _ f: PayloadFiles = PayloadFiles(), own: String = appVersion) -> String? {
    noteShownVersion(m, f, own: own)
    return updateNotice(app: m.app.version, own: own)
}

/// This app's version as the log and the report write it: "1.4.0 (1.4.0)" (the version, then the
/// build), or, when the release shown is another, "1.4.1 (Mac app 1.4.0, build 1.4.0)".
func versionText(own: String = appVersion, build: String = appBuild, shown: String) -> String {
    shown == own ? "\(own) (\(build))" : "\(shown) (Mac app \(own), build \(build))"
}

/// Whether to download payload.json at launch: none saved, or it's a day old, or it's from before
/// this app (the install command put a newer app here without saving the release's payload.json;
/// its Stems Cache update would otherwise wait a day), or a feature is blocked by its version
/// lists (a newer list may allow it; `blocked` is checked last).
func manifestDue(age: TimeInterval?, savedApp: String?, app: String, blocked: () -> Bool) -> Bool {
    guard let a = age, let s = savedApp else { return true }
    return a > 86_400 || app.compare(s, options: .numeric) == .orderedDescending || blocked()
}

extension Controller {
    /// At launch: the notice from the last payload.json, then a new one in the background when
    /// manifestDue says so. A downloaded one may name a newer Stems Cache: asked at once
    /// (offerCacheUpdate), not at the next launch.
    func checkForUpdate() {
        let saved = savedManifest()
        noteShownVersion(saved)                          // also clears a record a replaced app left
        if let m = saved { updateFound(m) }
        guard manifestDue(age: manifestAge(), savedApp: saved?.app.version, app: appVersion, blocked: blockedByVersionList) else { return }
        DispatchQueue.global().async {                   // not the work queue: actions mustn't wait for it
            guard let m = fetchManifest(self.log) else { return }
            self.onMain { self.updateFound(m); self.refresh(); self.offerCacheUpdate() }
        }
    }

    /// Shows or hides the update notice and menu item for this payload.json, after recording the
    /// version shown (manifestSeen). Every path that reads or downloads payload.json comes here:
    /// the update check and Install/Reinstall. Main thread.
    func updateFound(_ m: Manifest) {
        notice = manifestSeen(m)
        let newer = notice != nil
        if newer && updateItem?.isHidden == true { log("RB Stems Plus \(m.app.version) is available") }
        updateItem?.isHidden = !newer
    }

    @objc func updateApp() {
        guard !actionRunning, let m = savedManifest(), newerThanApp(m.app.version) else { return }
        // only a valid app is replaced (Safety.swift): not a translocated or bare copy
        guard let me = ownBundle() else {
            _ = alert("Not in Applications", "Move RB Stems Plus to the Applications folder, open it from there, then choose Update RB Stems Plus again.", ["OK"]); return
        }
        guard FileManager.default.isWritableFile(atPath: (me as NSString).deletingLastPathComponent) else {
            _ = alert("Administrator needed", "Only an administrator can update RB Stems Plus on this Mac.", ["OK"]); return
        }
        guard confirm("Update RB Stems Plus to \(m.app.version)", "Update", "Replaces this version with RB Stems Plus \(m.app.version).") else { return }
        // the update quits the app: a playlist being prepared is stopped first (its set written),
        // before anything is downloaded, so Cancel leaves everything as it was
        guard confirmStopPreparing("Update") else { return }
        busyInFront("Updating RB Stems Plus…")                  // no action marker: nothing in rekordbox changes
        work.async {
            safeRemove(updateDir)
            let zip = updateDir + "/" + m.app.zip, unpacked = updateDir + "/new"
            let got = fetchAsset(m.app.zip, to: zip, sha: m.app.sha256, size: nil,
                                 what: "RB Stems Plus \(m.app.version)", status: self.setStatus, log: self.log)
            let unzip = got == .ok ? runTool("/usr/bin/ditto", ["-x", "-k", zip, unpacked]) : (1, "")
            let newApp = unpacked + "/RB Stems Plus.app"
            let info = NSDictionary(contentsOfFile: newApp + "/Contents/Info.plist")
            guard got == .ok, unzip.0 == 0,
                  info?["CFBundleIdentifier"] as? String == bundleID,
                  info?["CFBundleShortVersionString"] as? String == m.app.version else {
                self.log("update: \(got == .ok ? "the zip isn't the expected app (\(unzip.1))" : "download failed")")
                safeRemove(updateDir)
                switch got {
                case .offline: _ = self.alert("Download failed", "Nothing was changed. Check your internet connection and try again.", ["OK"])
                case .noSpace: _ = self.alert("Low disk space", "The disk is full. Nothing was changed. Free up some space and try again.", ["OK"])
                default: _ = self.alert("Download failed", "The update didn't download correctly. Nothing was changed. Try again later.", ["OK"])
                }
                self.busy(nil); return
            }
            self.log("update: \(appVersion) → \(m.app.version), replacing the app after it quits")
            afterExit(poll: "0.2", Controller.updateScript,
                      [me, newApp, updateDir, logPath, m.app.version, bundleID, busyFile, lsregister, "/usr/bin/open"])
            ownQuit = true                              // this quit waits for nothing
            DispatchQueue.main.async { NSApp.terminate(nil) }
        }
    }

    /// Replaces the app once it has quit, as the bootstrap does. The new app is copied next to the
    /// app (its folder, normally /Applications) and signed there, inside out (its watcher helper
    /// first, then the app: 1.0's script signs only the app, which keeps the helper's signature
    /// from the build), then the two are exchanged in one step by the new app's own executable
    /// (--swap, swapApps): there is never a moment
    /// without an app, however the script is stopped. The old app, now at the staging name, is
    /// then deleted. Only our own apps are touched: the app, the download and anything at the
    /// staging name must be real app folders (not links) with our bundle ID, and the download
    /// may hold only files and folders. Whether the update happened is decided by what is at the
    /// app's place afterwards (the staged copy's folder), not by the exchange's exit code.
    /// Meanwhile the busy marker holds this script's pid and start time (State.swift): the watcher
    /// and the install command leave it alone. If anything fails, the old app stays, opens again
    /// and says so (checkUpdateResult); the log says why. The log is never written through a link.
    /// $1 the app, $2 the new app (unpacked), $3 the update folder, $4 the app's log, $5 the new
    /// version, $6 our bundle ID, $7 the busy marker, $8 lsregister, $9 open (the tests: true).
    /// "# step:" comments mark where the tests change a run; they do nothing.
    static let updateScript = #"""
        {
        APP="$1"; NEW="$2"; DIR="$3"; LOG="$4"; V="$5"; ID="$6"; BUSY="$7"; LSREG="$8"; OPEN="$9"
        D="${APP%/*}"; STAGE="$D/\#(updateStagingName)"
        R=failed; WHY=; K=
        \#(startedFunction)
        /usr/bin/printf '%s\n%s\n' "$$" "$(started $$)" > "$BUSY.$$" && /bin/mv -f "$BUSY.$$" "$BUSY"
        there() { [ -e "$1" ] || [ -L "$1" ]; }
        plain() {
          case "$1" in /*.app) ;; *) return 1 ;; esac
          case "$1" in */AppTranslocation/*|*/../*|*/./*|*//*) return 1 ;; esac
        }
        ours() { [ -d "$1" ] && [ ! -L "$1" ] && [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$1/Contents/Info.plist" 2>/dev/null)" = "$ID" ]; }
        contained() {
          bad="$(/usr/bin/find "$1" ! -type f ! -type d -print 2>&1)" && [ -z "$bad" ]
        }
        free_name() { ! there "$1" || { ours "$1" && /bin/rm -rf "$1" && ! there "$1"; }; }
        exe() {
          E="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$1/Contents/Info.plist" 2>/dev/null)"
          case "$E" in ""|*/*|.|..) return 1 ;; esac
          [ -f "$1/Contents/MacOS/$E" ] && [ -x "$1/Contents/MacOS/$E" ]
        }
        if ! plain "$APP" || ! ours "$APP"; then WHY="$APP isn't RB Stems Plus"; K=app
        elif ! plain "$NEW" || ! ours "$NEW" || ! contained "$NEW"; then WHY="the download isn't RB Stems Plus, or contains a link or special file"; K=download
        elif ! free_name "$STAGE"; then WHY="something else is at $STAGE"; K=staging
        elif ! /usr/bin/ditto "$NEW" "$STAGE"; then WHY="couldn't copy the new app next to the app"; K=copy; [ -L "$STAGE" ] || /bin/rm -rf "$STAGE"
        elif ! ours "$STAGE" || ! contained "$STAGE" || ! exe "$STAGE"; then WHY="the copy isn't RB Stems Plus"; K=copy-check; free_name "$STAGE"
        elif [ -d "$STAGE/\#(watcherHelperPath)" ] && ! /usr/bin/codesign -f -s - --options runtime "$STAGE/\#(watcherHelperPath)"; then WHY="couldn't sign the new app's watcher"; K=sign; free_name "$STAGE"
        elif ! /usr/bin/codesign -f -s - --options runtime "$STAGE"; then WHY="couldn't sign the new app"; K=sign; free_name "$STAGE"
        fi
        if [ -z "$WHY" ]; then
          # step: staged
          NEWID="$(/usr/bin/stat -f '%d:%i' "$STAGE")"
          OUT="$("$STAGE/Contents/MacOS/$E" --swap "$STAGE" "$APP" 2>&1)"
          if [ -n "$NEWID" ] && [ ! -L "$APP" ] && [ "$(/usr/bin/stat -f '%d:%i' "$APP" 2>/dev/null)" = "$NEWID" ]; then
            R=updated
            # step: swapped
            free_name "$STAGE" || WHY="the old app stays at $STAGE"
          else WHY="couldn't exchange the new app with the app${OUT:+ ($OUT)}"; K=swap; free_name "$STAGE"; fi
        fi
        if [ "$R" = updated ]; then
          "$LSREG" -f "$APP"; /usr/bin/touch "$APP"; /bin/rm -rf "$DIR"
        else
          /bin/mkdir -p "$DIR"; echo "failed${K:+ $K}" > "$DIR/result"
        fi
        [ -L "$LOG" ] || echo "$(/bin/date '+%Y-%m-%d %H:%M:%S')  update to $V: $R${WHY:+ ($WHY)}" >> "$LOG"
        [ "$(/usr/bin/head -n 1 "$BUSY" 2>/dev/null)" = "$$" ] && /bin/rm -f "$BUSY"
        "$OPEN" "$APP"
        } >/dev/null 2>&1
        """#

    /// A shell function: `started <pid>` prints when that process started, in seconds since 1970,
    /// as processStartTime does (State.swift), or nothing if there is no such process. In UTC and
    /// the C locale, so neither a time zone change nor the language changes it.
    static let startedFunction = #"""
        started() { set -- $(TZ=UTC0 LC_ALL=C /bin/ps -p "$1" -o lstart= 2>/dev/null); [ $# -eq 5 ] && TZ=UTC0 LC_ALL=C /bin/date -j -f '%a %b %e %T %Y' "$*" +%s 2>/dev/null; }
        """#

    /// At launch: what an update the last run started left behind. A failed one left its
    /// reason in update/result ("failed <key>", updateScript).
    func checkUpdateResult() {
        let result = FileManager.default.contents(atPath: updateDir + "/result").map { String(decoding: $0, as: UTF8.self) }
        safeRemove(updateDir)
        guard let r = result else { return }
        log("the update couldn't replace the app (\(r.trimmingCharacters(in: .whitespacesAndNewlines)))")
        _ = alert("Update failed", updateFailedMessage(updateFailureReason(r)), ["OK"])
    }
}

/// The reason a failed update left in update/result ("failed <key>", written by updateScript),
/// as one short sentence; nil for anything else (an older app's plain "failed", or a file that
/// isn't one of ours).
func updateFailureReason(_ result: String) -> String? {
    let words = result.split(whereSeparator: { $0.isWhitespace })
    guard words.count == 2, words[0] == "failed" else { return nil }
    switch words[1] {
    case "app": return "RB Stems Plus was moved or changed while it was updating."
    case "download": return "The download wasn't a valid copy of RB Stems Plus."
    case "staging": return "Something else is in the way next to RB Stems Plus (\(updateStagingName))."
    case "copy": return "The new version couldn't be copied next to RB Stems Plus."
    case "copy-check": return "The copy of the new version didn't check out."
    case "sign": return "The new version couldn't be signed."
    case "swap": return "The new version couldn't take this one's place."
    default: return nil
    }
}

/// Said at launch after an update that didn't replace the app (the old one is still there): the
/// `reason` if known, then what to do.
func updateFailedMessage(_ reason: String?) -> String {
    "RB Stems Plus wasn't changed." + (reason.map { " " + $0 } ?? "")
        + " If macOS said RB Stems Plus was prevented from modifying apps, allow it, then choose Update RB Stems Plus again. Otherwise try again later."
}

/// The update's exchange ("RB Stems Plus --swap <staged> <app>", run by updateScript from the
/// staged new app): swaps the two app folders in one step (renamex_np with RENAME_SWAP), so the
/// app's place is never empty. Both must be plain absolute paths ending in .app, in the same
/// folder, not translocated, real folders (not links) with our bundle ID. Returns why it
/// refused or failed, or nil once they are swapped.
func swapApps(_ staged: String, _ app: String) -> String? {
    guard let s = pathComponents(staged), let a = pathComponents(app), s != a else { return "not two different plain paths" }
    guard s.dropLast() == a.dropLast() else { return "not in the same folder" }
    for p in [staged, app] {
        guard p.hasSuffix(".app"), !p.contains("/AppTranslocation/") else { return "\(p) isn't an app" }
        guard fileType(p) == S_IFDIR, bundleIDOf(p) == bundleID else { return "\(p) isn't RB Stems Plus" }
    }
    guard renamex_np(staged, app, UInt32(RENAME_SWAP)) == 0 else { return String(cString: strerror(errno)) }
    return nil
}
