// Updates. At launch, at most once a day, the app downloads payload.json (Payload.swift); if its
// app.version is newer, a line in the status line and "Update RB Stems Plus" in the app menu. Update RB Stems Plus does
// what the bootstrap does: download the app's zip, verify its sha256, then, once this app has quit,
// copy the new app next to it, sign it ad hoc (its watcher helper first), swap the two in one step
// (swapApps), register it with LaunchServices and open it (updateScript). The new app, once open,
// registers its watcher again for its new code (checkWatcher).
// A payload-only change needs nothing here: Install and Reinstall use the latest payload.
import AppKit

let lsregister = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"

/// Whether `version` is newer than this app's ("1.0.10" > "1.0.9").
func newerThanApp(_ version: String) -> Bool { version.compare(appVersion, options: .numeric) == .orderedDescending }

extension Controller {
    /// At launch: the notice from the last payload.json, then a new one in the background if the
    /// last is a day old, or if a feature is blocked by its version lists (a newer list may allow it).
    func checkForUpdate() {
        if let m = savedManifest() { updateFound(m) }
        guard (manifestAge() ?? .infinity) > 86_400 || blockedByVersionList() else { return }
        DispatchQueue.global().async {                   // not the work queue: actions mustn't wait for it
            guard let m = fetchManifest(self.log) else { return }
            self.onMain { self.updateFound(m); self.refresh() }
        }
    }

    /// Shows or hides the update notice and menu item for this payload.json. Main thread.
    func updateFound(_ m: Manifest) {
        let newer = newerThanApp(m.app.version)
        if newer && updateItem?.isHidden == true { log("RB Stems Plus \(m.app.version) is available") }
        updateItem?.isHidden = !newer
        notice = newer ? "RB Stems Plus \(m.app.version) is available: choose Update RB Stems Plus in the RB Stems Plus menu." : nil
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
        busy("Updating RB Stems Plus…")                  // no action marker: nothing in rekordbox changes
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
