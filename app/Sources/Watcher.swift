// Watcher mode: RB Stems Plus Watcher.app (the helper inside the app, Contents/Helpers), started
// by the LaunchAgent with --watch. No window: it waits for updates to finish and rekordbox to be
// closed, compares what the user installed with what is there, and opens the app with a prompt
// when something went missing. It never changes anything itself. Every start writes at least one
// line to watcher.log, also when it stops at once.
import AppKit
import Security

/// How many checks one watcher start runs at most: the first, then again while starts keep
/// coming in during them (watcherRounds).
let watcherMaxRounds = 3

func runWatcher() -> Never {
    let wlog = watcherLogPath
    let log = { (s: String) in appendLog(wlog, s) }
    // started from a valid app only (the app repairs or removes a LaunchAgent that isn't): that
    // same app is the one it opens
    let own = ownApp()
    guard let app = own.app else {
        log("not checking: \(Bundle.main.bundlePath) isn't a valid RB Stems Plus (\(own.problem ?? "?")); open RB Stems Plus to repair the watcher")
        exit(0)
    }
    try? FileManager.default.createDirectory(atPath: support, withIntermediateDirectories: true)
    // one watcher at a time (launchd may start it again while it waits); a start that finds the
    // lock taken leaves the again-mark for the running check
    guard takeWatcherLock(watcherLock, again: watcherAgainFile, log: log) else { exit(0) }
    var putOff = Set<String>()                           // asked in this process, answered Remind Me Later
    watcherRounds(max: watcherMaxRounds,
                  check: { watcherCheck(app: app, putOff: &putOff, log: log) },
                  release: { safeRemove(watcherLock) },
                  again: { fileType(watcherAgainFile) != nil },
                  retake: { mkdir(watcherLock, 0o700) == 0 },
                  log: log)
    exit(0)
}

/// Takes the watcher's lock (a folder, made by mkdir) or says why not. First it leaves the
/// again-mark (`again`): a check that holds the lock looks for it after releasing the lock, so a
/// start that comes in while a check is finishing is never lost (the mark before the lock: see
/// watcherRounds). A lock not touched for 30 minutes is a stopped run's: taken over. Returns
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

/// The checks of one watcher start: `check` (true: done, a change since may be looked at; false:
/// stop, e.g. the app takes over), then the lock released (`release`), then, if a start came in
/// meanwhile (`again`: its mark is there), the lock taken again (`retake`) and another check,
/// `max` checks in all. Starts leave the mark before they try the lock: one that came before the
/// release left the mark and is seen here; one after it gets the lock itself. If `retake` fails,
/// such a start holds the lock and checks. Returns how many checks ran.
@discardableResult
func watcherRounds(max: Int, check: () -> Bool, release: () -> Void, again: () -> Bool, retake: () -> Bool, log: (String) -> Void) -> Int {
    var n = 0
    while true {
        n += 1
        let goOn = check()
        release()
        guard goOn, again() else { return n }
        guard n < max else { log("changes kept coming during \(n) checks: the next start looks again"); return n }
        guard retake() else { log("something changed during the check; another start is checking it"); return n }
        log("something changed during the check: looking again")
    }
}

/// One check (runWatcher), for the valid app `app`. `putOff`: what the user answered Remind Me
/// Later to in this process, not asked again until the next start. Returns false when the
/// watcher should stop (busy, an uninstall left half-done, the app opened to reinstall).
func watcherCheck(app: String, putOff: inout Set<String>, log: (String) -> Void) -> Bool {
    // wait while an update, an install or RB Stems Plus itself is changing things (up to 10 min)
    func updating() -> Bool {
        pioneerUpdating() || appBusy()
    }
    var waited = 0
    while waited < 120 && updating() { sleep(5); waited += 1 }
    if waited > 0 { log("waited \(waited * 5) s for an update or RB Stems Plus to finish") }
    // still busy after 10 minutes: the app will say what it needs itself
    if appBusy() { log("RB Stems Plus is still busy: not asking"); return false }
    // an uninstall that was stopped (the app quit half-way): the next launch offers to finish
    // it, never to reinstall what it was removing
    if let a = markedAction(), a.id.hasPrefix("uninstall") {
        log("an uninstall didn't finish (\(a.id)): not asking"); return false
    }
    // never prompt while rekordbox is open: reinstalling needs it closed, and the user may be playing
    if rekordboxOpen() {
        log("rekordbox is open: waiting until it quits")
        while rekordboxOpen() { sleep(10); utimes(watcherLock, nil) }
        sleep(3)
    }
    // what this check sees starts here: a start before now is covered by it, one after it
    // leaves the again-mark anew
    safeRemove(watcherAgainFile)
    let c = chosen()
    log("run: \(watcherName) \(appVersion) (\(appBuild)); rekordbox \(rekordboxVersion()), engine \(engineVersion()), Demucs v4 \(stemsPlusPresent() ? "on" : "off"), Stems Cache \(stemsCachePresent() ? "on" : "off"); chosen: model=\(c.model) cache=\(c.cache)")
    // what went missing, less what the user was already told can't be reinstalled yet
    let keys = missingKeys().filter { !dismissed().contains(unsupportedKey($0)) }
    if keys.isEmpty { log("nothing to ask"); return true }
    if Set(keys).isSubset(of: putOff) { log("still missing (\(keys.joined(separator: ", "))), put off a moment ago: not asking again now"); return true }
    log("asking: \(keys.joined(separator: ", "))")
    let choice = watcherPrompt(keys)
    log("user chose: \(choice)")
    switch choice {
    case "Reinstall Now":
        // the app does the reinstall: it reads the request at launch, or on the reopen event
        // macOS sends when it is already open
        writeReinstallRequest()
        // open the app once this process has exited (and so let go of the lock): its reinstall
        // changes what the watcher watches
        afterExit(poll: "0.2", "/bin/sleep 0.5; /usr/bin/open -a \"$1\"", [app])
        return false
    case "Don't Ask Again for This Version":
        remember(keys)
    case "OK":
        // "not supported yet": nothing to do until a newer version list arrives, so don't ask
        // after every rekordbox session (the settings folder changes each time)
        remember(keys.map(unsupportedKey))
    default:
        putOff.formUnion(keys)
    }
    return true
}

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

/// A prompt key the user was told can't be reinstalled yet, tied to the version list it was
/// judged against: once a payload.json lists more versions, the watcher asks again.
func unsupportedKey(_ key: String) -> String {
    let l = compatibleList()
    return key + " unsupported with " + (l.rekordbox + l.stemsEngine + [l.ortVersionPrefix]).joined(separator: ",")
}

/// The watcher's own dialog, in the app's design (no app window): inline buttons, the action
/// on the right. Returns the button pressed.
func watcherPrompt(_ keys: [String]) -> String {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)        // a dialog only: no Dock icon, no menu bar
    let engine = engineVersion(), rbVersion = rekordboxVersion()
    let d = watcherDialog(modelOff: keys.contains { $0.hasPrefix("model:") }, cacheOff: keys.contains { $0.hasPrefix("cache:") },
                          engine: engine, rekordbox: shortVersion(rbVersion),
                          engineOK: engineSupported(engine), rekordboxOK: rekordboxSupported(rbVersion),
                          plusWorks: chosen().model && stemsPlusPresent())
    return Sheet(title: d.title, message: d.message, buttons: d.buttons, inline: true).runAlone()
}

/// What the watcher says when Demucs v4 (`modelOff`) and/or Stems Cache (`cacheOff`) went
/// missing, judged against the last payload.json's version lists (`engineOK`, `rekordboxOK`).
/// A feature is blocked when its version isn't listed (each on its own: Stems Cache works with
/// either model). When nothing that went missing can be reinstalled, it says
/// so instead of offering a reinstall that would refuse ("OK": ask again once a newer list
/// arrives). Otherwise it offers the reinstall, naming a feature that stays off.
func watcherDialog(modelOff: Bool, cacheOff: Bool, engine: String, rekordbox: String,
                   engineOK: Bool, rekordboxOK: Bool, plusWorks: Bool) -> (title: String, message: String, buttons: [String]) {
    let modelBlocked = modelOff && !engineOK, cacheBlocked = cacheOff && !rekordboxOK
    let both = modelOff && cacheOff
    let rekordboxLine = "doesn't support rekordbox \(rekordbox) yet"
    if (!modelOff || modelBlocked) && (!cacheOff || cacheBlocked) {
        // one feature off: "It isn't available for …" or "It doesn't support …"; both: each by name
        let why: String
        if !both { why = "It " + (modelOff ? "isn't available for STEMS Engine \(engine) yet." : rekordboxLine + ".") }
        else { why = "Demucs v4 isn't available for STEMS Engine \(engine) yet, and Stems Cache \(rekordboxLine)." }
        let off = both ? "Demucs v4 and Stems Cache are off." : modelOff ? "Demucs v4 is off." : "Stems Cache is off."
        return (cacheOff ? "rekordbox was updated" : "rekordbox installed a new STEMS Engine",
                "\(off) \(why) RB Stems Plus checks again each time you open it, and asks you to reinstall once it can.",
                ["OK", "Don't Ask Again for This Version"])
    }
    let buttons = ["Reinstall Now", "Remind Me Later", "Don't Ask Again for This Version"]
    if both {
        let message = cacheBlocked
            ? "Demucs v4 and Stems Cache are off. Reinstall Demucs v4 to separate tracks with it. Stems Cache \(rekordboxLine), so it stays off for now."
            : modelBlocked
            ? "Demucs v4 and Stems Cache are off. Reinstall Stems Cache to reuse the stems it has saved. Demucs v4 isn't available for STEMS Engine \(engine) yet, so it stays off for now."
            : "Demucs v4 and Stems Cache are off. Reinstall them to separate tracks with Demucs v4 and reuse the stems Stems Cache has saved."
        return ("rekordbox was updated", message, buttons)
    }
    if cacheOff {
        return ("rekordbox was updated", "Stems Cache is off\(plusWorks ? " (Demucs v4 still works)" : ""). Reinstall it to reuse the stems it has saved.", buttons)
    }
    return ("rekordbox put its own stems model back", "Demucs v4 is off. Reinstall it to separate tracks with Demucs v4 instead of rekordbox's own model.", buttons)
}
