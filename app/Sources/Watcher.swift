// Watcher mode's platform code: RB Stems Plus Watcher.app (the helper inside the app,
// Contents/Helpers), started by the LaunchAgent with --watch. The watcher over the core
// (app/Shell/WatcherShell.swift) decides: its rounds, waiting for updates to finish and rekordbox
// to be closed, what went missing, the question and its answers, and the prepared playlists' check
// as rekordbox opens (core/flows.c's watcher flow). Here: the watcher's lock, the check's files and
// the pattern of a run it skips, and the helper's code identity the LaunchAgent is registered with.
import AppKit
import Security

/// Takes the watcher's lock (a folder, made by mkdir) or says why not. First it leaves the
/// again-mark (`again`): a check that holds the lock looks for it after releasing the lock, so a
/// start that comes in while a check is finishing is never lost (the mark before the lock: see
/// the core's watcher_rounds). A lock not touched for 30 minutes is a stopped run's: taken over. Returns
/// whether this process holds it. A refusal writes one log line, only if no start has left the
/// mark since the running check last read it: while rekordbox is open, launchd may start the
/// watcher every 10 s, and the log gets one line per check, not one per start.
func takeWatcherLock(_ lock: String, again: String, gate: Deletable = deletable, log: (String) -> Void) -> Bool {
    let marked = fileType(again) != nil
    if !writeAtomically(Data(), to: again) { log("couldn't leave the again-mark (\(again))") }
    if mkdir(lock, 0o700) == 0 { return true }
    let age = (try? FileManager.default.attributesOfItem(atPath: lock)[.modificationDate] as? Date).map { -$0.timeIntervalSinceNow } ?? 0
    if age < 1800 {
        if !marked { log("another check is running: it will look again") }
        return false
    }
    log("a check's lock is \(Int(age / 60)) minutes old (a stopped run): taking it over")
    safeRemove(lock, whole: false, gate)
    if mkdir(lock, 0o700) == 0 { return true }
    if !marked { log("another check is running: it will look again") }
    return false
}

/// The marker of the rekordbox session whose prepared playlists were checked (its pid; the core's
/// watcher flow decides, WATCH_KEPT reads and writes it).
let keptCheckedFile = support + "/kept.checked"
/// What the last notifications covered: each set's tracks that weren't ready (JSON).
let keptNoticedFile = support + "/kept.noticed"

/// A regular expression (pgrep -f) for the helper preparing set `name` (runArgs: a check has no --until).
func setRunPattern(_ name: String) -> String { "rbsp-prepare .*--until [^ ]+ --keep \(name)( |$)" }

/// The identity of the code at `bundle` (the watcher helper): its code directory hash (cdhash),
/// as macOS checks it when launchd starts it, or, unsigned, the SHA-256 of its executable. Nil
/// if neither can be read.
func watcherIdentity(_ bundle: String) -> String? {
    var code: SecStaticCode?
    if SecStaticCodeCreateWithPath(URL(fileURLWithPath: bundle) as CFURL, [], &code) == errSecSuccess, let c = code {
        var info: CFDictionary?
        if SecCodeCopySigningInformation(c, [], &info) == errSecSuccess,
           let hash = (info as? [String: Any])?[kSecCodeInfoUnique as String] as? Data {
            return "cdhash " + hash.map { String(format: "%02x", $0) }.joined()
        }
    }
    return sha256(bundle + "/Contents/MacOS/" + watcherName).map { "sha256 " + $0 }
}
