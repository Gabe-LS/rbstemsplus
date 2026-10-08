// The watcher helper (RB Stems Plus Watcher.app inside the app), its LaunchAgent's registration
// and the watcher's lock, tried in the temporary folder: the helper as scripts/build-app.sh
// makes and signs it (its own functions, run alone on a fake app whose executable is this
// program), the bootstrap's and the update's signing, the bundle checks, the decision to
// register the agent again, the again-mark and the bootstrap's process handling. Nothing here
// touches launchd, ~/Library/LaunchAgents or the user's folders.
import Foundation

/// A shell function of `file` (from "name() {" to the first line "}"), as its text, to run alone.
private func shellFunction(_ file: String, _ name: String) -> String {
    let lines = ((try? String(contentsOfFile: file, encoding: .utf8)) ?? "").components(separatedBy: "\n")
    let start = lines.firstIndex(of: name + "() {")
    let end = start.flatMap { s in lines[s...].firstIndex(of: "}") }
    check(start != nil && end != nil, "\((file as NSString).lastPathComponent) has \(name)")
    return start.flatMap { s in end.map { lines[s...$0].joined(separator: "\n") } } ?? "false"
}

/// A line "NAME=..." of `file`, as it is.
private func shellAssignment(_ file: String, _ name: String) -> String {
    let lines = ((try? String(contentsOfFile: file, encoding: .utf8)) ?? "").components(separatedBy: "\n")
    let line = lines.first { $0.hasPrefix(name + "=") }
    check(line != nil, "\((file as NSString).lastPathComponent) sets \(name)")
    return line ?? "false"
}

func watcherTests() {
    let fm = FileManager.default
    let repo = fm.currentDirectoryPath
    let buildApp = repo + "/scripts/build-app.sh", bootstrap = repo + "/scripts/bootstrap.sh"
    let base = tmp + "/watcher"
    func mk(_ p: String) { try! fm.createDirectory(atPath: p, withIntermediateDirectories: true) }
    func plist(_ p: String) -> NSDictionary? { NSDictionary(contentsOfFile: p) }
    func verifies(_ app: String) -> Bool { runTool("/usr/bin/codesign", ["--verify", "--deep", "--strict", app]).0 == 0 }
    func signInfo(_ code: String) -> String { runTool("/usr/bin/codesign", ["-dv", code]).1 }
    let makeHelper = shellFunction(buildApp, "make_helper"), signApp = shellFunction(buildApp, "sign_app")

    /// A fake app at `app` as build-app.sh assembles it before its helper: app/Info.plist (its
    /// version set), this program as the executable, an icon file.
    func fakeBuiltApp(_ app: String, version: String = "9.8.7") {
        mk(app + "/Contents/MacOS"); mk(app + "/Contents/Resources")
        try? fm.removeItem(atPath: app + "/Contents/Info.plist")
        try! fm.copyItem(atPath: repo + "/app/Info.plist", toPath: app + "/Contents/Info.plist")
        runTool("/usr/libexec/PlistBuddy", ["-c", "Set :CFBundleShortVersionString \(version)", "-c", "Set :CFBundleVersion \(version)", app + "/Contents/Info.plist"])
        try? fm.copyItem(atPath: me, toPath: app + "/Contents/MacOS/RB Stems Plus")
        fm.createFile(atPath: app + "/Contents/Resources/RBStemsPlus.icns", contents: Data("icon".utf8))
    }
    /// build-app.sh's make_helper, then its sign_app, on `app`: true if both worked.
    func buildHelper(_ app: String) -> Bool {
        let r = runTool("/bin/bash", ["-c", "set -euo pipefail\nroot=\"$1\"\n\(makeHelper)\n\(signApp)\nmake_helper \"$2\"\nsign_app \"$2\"", "rbsp", repo, app])
        if r.0 != 0 { print("  (build-app.sh's functions said: \(r.1))") }
        return r.0 == 0
    }

    // MARK: the helper, as build-app.sh makes and signs it

    let app = base + "/Apps/RB Stems Plus.app"
    fakeBuiltApp(app)
    check(buildHelper(app), "build: the helper is made and the app signed")
    let helper = watcherHelper(app), hexe = watcherExecutable(app)
    check(helper == app + "/Contents/Helpers/RB Stems Plus Watcher.app" && hexe == helper + "/Contents/MacOS/RB Stems Plus Watcher",
          "the helper's place: Contents/Helpers/RB Stems Plus Watcher.app, its executable named after it")
    let info = plist(helper + "/Contents/Info.plist"), mainInfo = plist(app + "/Contents/Info.plist")
    check(info?["CFBundleIdentifier"] as? String == watcherBundleID && watcherBundleID == "io.github.rbstemsplus.watcher", "helper: the watcher's bundle ID")
    check(info?["CFBundleName"] as? String == "RB Stems Plus Watcher" && info?["CFBundleDisplayName"] as? String == "RB Stems Plus Watcher"
          && info?["CFBundleExecutable"] as? String == watcherName, "helper: named RB Stems Plus Watcher")
    check(info?["LSUIElement"] as? Bool == true, "helper: no Dock icon (LSUIElement)")
    check(info?["CFBundleShortVersionString"] as? String == "9.8.7" && info?["CFBundleVersion"] as? String == "9.8.7"
          && info?["LSMinimumSystemVersion"] as? String == mainInfo?["LSMinimumSystemVersion"] as? String,
          "helper: the app's version and minimum macOS")
    check(info?["CFBundleIconFile"] as? String == "RBStemsPlus" && fileType(helper + "/Contents/Resources/RBStemsPlus.icns") == S_IFREG, "helper: the app's icon")
    var st = stat()
    check(lstat(hexe, &st) == 0 && st.st_mode & S_IFMT == S_IFREG && st.st_nlink == 1 && access(hexe, X_OK) == 0,
          "helper: its executable is a real file of its own (not a link, not a hard link)")
    let odd = runTool("/usr/bin/find", [app, "!", "-type", "f", "!", "-type", "d", "-print"]).1
    check(odd.isEmpty, "build: no link or special file in the app (the bootstrap and the update refuse them)")
    check(verifies(app), "build: codesign --verify --deep --strict passes on the app")
    let hs = signInfo(helper), asg = signInfo(app)
    check(hs.contains("Identifier=io.github.rbstemsplus.watcher") && hs.contains("(adhoc,runtime)"), "build: the helper is signed ad hoc with the hardened runtime, as the watcher")
    check(asg.contains("Identifier=io.github.rbstemsplus.app") && asg.contains("(adhoc,runtime)"), "build: the app is signed ad hoc with the hardened runtime")
    check(runTool("/usr/bin/cmp", ["-s", app + "/Contents/MacOS/RB Stems Plus", hexe]).0 != 0, "build: the helper's executable carries its own signature")
    check(runTool("/bin/bash", ["-n", buildApp]).0 == 0 && ((try? String(contentsOfFile: buildApp, encoding: .utf8)) ?? "").contains("codesign --verify --deep --strict \"$app\""),
          "build-app.sh verifies the whole app, helper included")
    check(((try? String(contentsOfFile: buildApp, encoding: .utf8)) ?? "").contains("Contents/Helpers/RB Stems Plus Watcher.app")
          && watcherHelperPath == "Contents/Helpers/RB Stems Plus Watcher.app", "build-app.sh and the app agree on the helper's place")

    // signing again on the user's Mac: the bootstrap (helper first), the update, and 1.0's update (the app only)
    let signBootstrap = shellFunction(bootstrap, "sign_app")
    let helperVar = shellAssignment(bootstrap, "WATCHER_HELPER")
    for (name, body) in [("bootstrap", "\(helperVar)\n\(signBootstrap)\nsign_app \"$1\""),
                         ("1.0's update (the app only)", "/usr/bin/codesign -f -s - --options runtime \"$1\"")] {
        let copy = base + "/Signed-\(name.prefix(9)).app".replacingOccurrences(of: " ", with: "")
        try? fm.removeItem(atPath: copy); try! fm.copyItem(atPath: app, toPath: copy)
        check(runTool("/bin/bash", ["-c", body, "rbsp", copy]).0 == 0 && verifies(copy), "\(name): signs the app so that it verifies --deep --strict")
    }

    // the app's own update (updateScript): a download with the helper is signed inside out and verifies
    let u = updateCase()
    mk(u.new + "/Contents/Helpers"); try! fm.copyItem(atPath: helper, toPath: watcherHelper(u.new))
    runTool("/usr/bin/codesign", ["--remove-signature", watcherExecutable(u.new)])
    let line = runUpdate(u)
    check(line.hasSuffix("update to 2: updated") && verifies(u.app) && signInfo(watcherHelper(u.app)).contains("(adhoc,runtime)"),
          "update: the new app's helper is signed first, the app verifies --deep --strict (\(line))")

    // MARK: the bundle checks: the helper only inside a valid app, at its place

    check(helperProblem(helper, executable: hexe) == nil, "helper: valid inside a valid app")
    check(appOfHelper(helper) == app && appOfHelper(app) == nil && appOfHelper(base + "/Contents/Helpers/RB Stems Plus Watcher.app") == base,
          "helper: its app is the folder around Contents/Helpers")
    check(ownApp(bundle: helper, executable: hexe) == (app, nil), "run from the helper: its app is the app around it")
    check(ownApp(bundle: app, executable: app + "/Contents/MacOS/RB Stems Plus") == (app, nil), "run from the app: the app itself")
    check(ownApp(bundle: helper, executable: app + "/Contents/MacOS/RB Stems Plus").app == nil, "run from the helper: the executable must be the helper's")
    check(helperProblem(app, executable: hexe) != nil, "helper: refuses the app itself")
    // a helper elsewhere: not in an app, in an app with another bundle ID, in a translocated app, or reached through a link
    let loose = base + "/Loose/RB Stems Plus Watcher.app"
    mk(loose + "/Contents/MacOS"); try! fm.copyItem(atPath: helper + "/Contents/Info.plist", toPath: loose + "/Contents/Info.plist")
    check(helperProblem(loose, executable: loose + "/Contents/MacOS/" + watcherName) != nil, "helper: refuses one outside an app")
    let other = base + "/Other.app"
    try! fm.copyItem(atPath: app, toPath: other)
    runTool("/usr/libexec/PlistBuddy", ["-c", "Set :CFBundleIdentifier com.example.other", other + "/Contents/Info.plist"])
    check(helperProblem(watcherHelper(other), executable: watcherExecutable(other)) != nil, "helper: refuses one inside an app with another bundle ID")
    let translocated = base + "/AppTranslocation/1/d/RB Stems Plus.app"
    mk((translocated as NSString).deletingLastPathComponent); try! fm.copyItem(atPath: app, toPath: translocated)
    check(helperProblem(watcherHelper(translocated), executable: watcherExecutable(translocated)) != nil, "helper: refuses one inside a translocated app")
    let linked = base + "/Linked.app"
    mk(linked + "/Contents"); try! fm.copyItem(atPath: app + "/Contents/Info.plist", toPath: linked + "/Contents/Info.plist")
    symlink(app + "/Contents/Helpers", linked + "/Contents/Helpers")
    check(helperProblem(watcherHelper(linked), executable: watcherExecutable(linked)) != nil, "helper: refuses a Helpers folder that is a link")
    let wrongID = base + "/WrongID.app"
    try! fm.copyItem(atPath: app, toPath: wrongID)
    runTool("/usr/libexec/PlistBuddy", ["-c", "Set :CFBundleIdentifier \(bundleID)", watcherHelper(wrongID) + "/Contents/Info.plist"])
    check(helperProblem(watcherHelper(wrongID), executable: watcherExecutable(wrongID)) != nil, "helper: refuses one without the watcher's bundle ID")
    check(ownApp(bundle: watcherHelper(wrongID), executable: watcherExecutable(wrongID)).app == nil, "a helper carrying the app's bundle ID isn't taken for the app")
    check(bundleProblem(watcherHelper(wrongID), executable: watcherExecutable(wrongID)) == "inside another app", "the app is never one nested inside another app")
    check(helperProblem(helper, executable: base + "/RB Stems Plus Watcher") != nil, "helper: refuses an executable outside it")

    // MARK: the LaunchAgent: what it starts, and registering it again when the code changed

    let legacy = app + "/Contents/MacOS/RB Stems Plus"                 // 1.0's agent started the app itself
    check(startsValidCopy(hexe) && startsValidCopy(legacy), "agent: the helper's executable and 1.0's (the app's) start a valid copy")
    check(!startsValidCopy(watcherExecutable(translocated)) && !startsValidCopy(watcherExecutable(other)) && !startsValidCopy(watcherExecutable(base + "/Missing.app")),
          "agent: a translocated, foreign or missing app doesn't")
    check(agentFix(target: legacy, own: hexe) == .rewrite, "agent: 1.0's agent (starting the app itself) is rewritten to the helper")
    check(agentFix(target: hexe, own: nil) == .keep && agentFix(target: legacy, own: nil) == .keep,
          "agent: from an invalid copy, one starting a valid helper or a valid 1.0 app is kept")
    check(agentFix(target: watcherExecutable(other), own: nil) == .remove, "agent: from an invalid copy, one starting a foreign app's helper is removed")
    let id1 = watcherIdentity(helper)
    check(id1?.hasPrefix("cdhash ") == true && id1?.count == 7 + 40, "the helper's code identity is its cdhash (\(id1 ?? "none"))")
    // a new version: another Info.plist, signed again (as an update or the install command does)
    runTool("/usr/libexec/PlistBuddy", ["-c", "Set :CFBundleVersion 9.8.8", helper + "/Contents/Info.plist"])
    runTool("/usr/bin/codesign", ["-f", "-s", "-", "--options", "runtime", helper])
    let id2 = watcherIdentity(helper)
    check(id2 != nil && id2 != id1, "the identity changes when the helper is signed again with other contents")
    let unsigned = base + "/Unsigned.app"
    mk(unsigned + "/Contents/MacOS"); fm.createFile(atPath: unsigned + "/Contents/MacOS/" + watcherName, contents: Data("plain".utf8))
    check(watcherIdentity(unsigned) == "sha256 " + sha256(data: Data("plain".utf8)), "unsigned: the identity is its executable's SHA-256")
    check(watcherIdentity(base + "/Nothing.app") == nil, "nothing there: no identity")
    let rec1 = registrationRecord(exe: hexe, identity: id1 ?? "a"), rec2 = registrationRecord(exe: hexe, identity: id2 ?? "b")
    check(agentDecision(target: hexe, own: hexe, recorded: rec1, current: rec1) == .keep, "registration: the same code as registered: kept")
    check(agentDecision(target: hexe, own: hexe, recorded: rec1, current: rec2) == .rewrite, "registration: the code changed (update, install command): registered again")
    check(agentDecision(target: hexe, own: hexe, recorded: nil, current: rec2) == .rewrite, "registration: no record (registered by 1.0): registered again")
    check(agentDecision(target: hexe, own: hexe, recorded: registrationRecord(exe: base + "/elsewhere", identity: id1 ?? "a"), current: rec1) == .rewrite,
          "registration: recorded for another place: registered again")
    check(agentDecision(target: hexe, own: hexe, recorded: rec1, current: nil) == .keep, "registration: the current code unknown: kept, nothing to compare")
    check(agentDecision(target: legacy, own: hexe, recorded: rec1, current: rec1) == .rewrite, "registration: 1.0's agent: rewritten whatever the record says")
    check(agentDecision(target: hexe, own: nil, recorded: nil, current: rec1) == .keep, "registration: an invalid copy never registers again")
    check(agentDecision(target: watcherExecutable(other), own: nil, recorded: nil, current: nil) == .remove, "registration: an invalid copy still removes a foreign agent")
    check(watcherRegisteredFile.hasPrefix(support + "/") && mayRemove(watcherRegisteredFile) && mayRemove(watcherAgainFile),
          "the registration record and the again-mark are in the support folder, which the app may clear")
    check(agentLabel == "io.github.rbstemsplus.watcher" && agentPlist == home + "/Library/LaunchAgents/io.github.rbstemsplus.watcher.plist",
          "the LaunchAgent's label and plist are unchanged (the troubleshooting page's commands use them)")

    // MARK: the watcher's lock: every start that doesn't check says so, and leaves the again-mark

    let wl = base + "/lock", lockDir = wl + "/watcher.lock", againFile = wl + "/watcher.again"
    mk(wl)
    let gate = Deletable(inside: [wl], roots: [], files: [], apps: [])
    var lines: [String] = []
    let logLine = { (s: String) in lines.append(s) }
    check(takeWatcherLock(lockDir, again: againFile, gate: gate, log: logLine) && fileType(lockDir) == S_IFDIR && lines.isEmpty,
          "lock: a free lock is taken, silently")
    check(fileType(againFile) == S_IFREG, "lock: every start leaves the again-mark first")
    try? fm.removeItem(atPath: againFile)
    check(!takeWatcherLock(lockDir, again: againFile, gate: gate, log: logLine) && lines == ["another check is running: it will look again"],
          "lock: held by a running check: one log line, \"another check is running: it will look again\"")
    check(fileType(againFile) == S_IFREG, "lock: held: the again-mark is left for the running check")
    lines = []
    check(!takeWatcherLock(lockDir, again: againFile, gate: gate, log: logLine) && lines.isEmpty,
          "lock: more starts while the mark is still there add no log line (launchd starts it every 10 s while rekordbox is open)")
    var old = [timeval(tv_sec: Int(Date().timeIntervalSince1970) - 3600, tv_usec: 0), timeval(tv_sec: Int(Date().timeIntervalSince1970) - 3600, tv_usec: 0)]
    utimes(lockDir, &old); lines = []
    check(takeWatcherLock(lockDir, again: againFile, gate: gate, log: logLine) && lines.count == 1 && lines[0].contains("60 minutes old"),
          "lock: one not touched for an hour (a stopped run) is taken over, and the log says so")
    try? fm.removeItem(atPath: lockDir)
    symlink(base + "/outside-victim", againFile + ".link")
    check(writeAtomically(Data(), to: againFile + ".link") && fileType(againFile + ".link") == S_IFREG && fileType(base + "/outside-victim") == nil,
          "lock: the again-mark replaces a planted link, never writes through it")

    // MARK: the checks of one start: again while starts came in, never a change lost

    /// watcherRounds with `checks` (each: what the check returns, and whether a start comes in
    /// during it); `retakeOK`: whether the lock can be taken again. Returns the checks run, the
    /// releases and the log.
    func rounds(_ checks: [(goOn: Bool, startDuring: Bool)], max: Int = watcherMaxRounds, retakeOK: Bool = true) -> (n: Int, released: Int, log: [String]) {
        var mark = false, i = 0, released = 0, log: [String] = []
        let n = watcherRounds(max: max,
                              check: { mark = false; let c = checks[Swift.min(i, checks.count - 1)]; i += 1; if c.startDuring { mark = true }; return c.goOn },
                              release: { released += 1 }, again: { mark }, retake: { retakeOK }, log: { log.append($0) })
        return (n, released, log)
    }
    var r = rounds([(true, false)])
    check(r.n == 1 && r.released == 1 && r.log.isEmpty, "rounds: nothing came in: one check, the lock released")
    r = rounds([(true, true), (true, false)])
    check(r.n == 2 && r.released == 2 && r.log == ["something changed during the check: looking again"], "rounds: a start came in during the check: one more check")
    r = rounds([(true, true)])
    check(r.n == watcherMaxRounds && r.released == watcherMaxRounds && r.log.last?.contains("changes kept coming") == true,
          "rounds: starts keep coming: at most \(watcherMaxRounds) checks, then the next start looks again")
    r = rounds([(false, true)])
    check(r.n == 1 && r.released == 1, "rounds: a check that stops (busy, the app opened to reinstall): no more checks")
    r = rounds([(true, true)], retakeOK: false)
    check(r.n == 1 && r.released == 1 && r.log.first?.contains("another start is checking it") == true, "rounds: another start took the lock meanwhile: it checks")
    // the race, with the real lock and mark: a start while the check finishes is seen
    try? fm.removeItem(atPath: againFile); try? fm.removeItem(atPath: lockDir); lines = []
    check(takeWatcherLock(lockDir, again: againFile, gate: gate, log: logLine), "race: the first start takes the lock")
    var startedDuring = 0, checksRun = 0
    let n = watcherRounds(max: watcherMaxRounds,
                          check: {
                              checksRun += 1
                              try? fm.removeItem(atPath: againFile)                       // what the check sees starts here
                              if checksRun == 1 { startedDuring += takeWatcherLock(lockDir, again: againFile, gate: gate, log: logLine) ? 0 : 1 }
                              return true
                          },
                          release: { safeRemove(lockDir, whole: false, gate) },
                          again: { fileType(againFile) != nil },
                          retake: { mkdir(lockDir, 0o700) == 0 },
                          log: logLine)
    check(startedDuring == 1 && n == 2 && fileType(lockDir) == nil && fileType(againFile) == nil,
          "race: a start during the check is refused, logged, and checked by the running watcher; the lock is free after")
    check(lines.first == "another check is running: it will look again", "race: the refused start wrote its line")

    // MARK: the install command quits the app, never the watcher (by its bundle ID or 1.0's --watch)

    let appPids = shellFunction(bootstrap, "app_pids")
    let stubs = """
        pgrep() { printf '%s\\n' 101 102 103 104; }
        lsappinfo() { case "$4" in 101|102) echo '"CFBundleIdentifier"="io.github.rbstemsplus.app"' ;; 103) echo '"CFBundleIdentifier"="io.github.rbstemsplus.watcher"' ;; *) echo '"CFBundleIdentifier"="com.example.other"' ;; esac; }
        ps() { case "$4" in 102) echo "/Applications/RB Stems Plus.app/Contents/MacOS/RB Stems Plus --watch" ;; *) echo "/Applications/RB Stems Plus.app/Contents/MacOS/RB Stems Plus" ;; esac; }
        """
    let vars = ["APP_NAME", "BUNDLE_ID", "WATCHER_ID"].map { shellAssignment(bootstrap, $0) }.joined(separator: "\n")
    let pids = runTool("/bin/bash", ["-c", "\(vars)\n\(stubs)\n\(appPids)\napp_pids"])
    check(pids.0 == 0 && pids.1.trimmingCharacters(in: .whitespacesAndNewlines) == "101",
          "bootstrap: quits only the app, not the watcher helper (its bundle ID), not 1.0's watcher (--watch), not another app (\(pids.1.debugDescription))")
    check(shellAssignment(bootstrap, "WATCHER_ID") == "WATCHER_ID=\"\(watcherBundleID)\"" && shellAssignment(bootstrap, "WATCHER_HELPER") == "WATCHER_HELPER=\"\(watcherHelperPath)\"",
          "bootstrap and the app agree on the watcher's bundle ID and place")

    // MARK: Create Report takes the watcher's crash reports too

    check(crashReportWanted("RB Stems Plus Watcher-2026-10-07-101010.ips") && crashReportWanted("RB Stems Plus-2026-10-07-101010.ips")
          && crashReportWanted("rekordbox-2026-10-07-101010.ips") && !crashReportWanted("bash-2026-10-07-101010.ips"),
          "report: crash reports of rekordbox, the app and RB Stems Plus Watcher, nothing else")
}
