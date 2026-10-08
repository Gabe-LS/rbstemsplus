// Create Report: a zip for a GitHub issue, saved on the Desktop. It holds summary.txt (versions,
// the Mac, what is installed, checksums, rekordbox's signature state), the app's, the watcher's
// and the bridge's logs, the state files, config.ini and payload.json, rekordbox's (and the app's)
// errors from macOS's log for the last 30 minutes, and up to 5 crash or hang reports of rekordbox,
// RB Stems Plus or RB Stems Plus Watcher from the last 7 days. Everything is masked (redact): the home folder becomes
// "~", the user's name "<user>", email addresses "<email>", and anything that looks like a
// password, key or token "<token>"; the logs never name tracks.
// Nothing is sent: the user attaches the zip to the issue the app opens. That issue's link carries
// the critical facts on its own (the last problem, then the summary's short lines, most important
// first), under the user's own text, so an issue is useful even without the zip.
import AppKit

extension Controller {
    @objc func createReport() {
        guard !actionRunning else { return }
        busy("Creating a report…")                       // no action: no busy marker
        work.async {
            let made = buildReport(self.log)
            self.busy(nil)
            guard let made = made else {
                _ = self.alert("Report failed", "The report couldn't be created. See the log in this window.", ["OK"]); return
            }
            self.log("report: \(redact(made.zip))")
            let b = self.alert("Report saved",
                               "It's saved as \(redact(made.zip)). To get help, click Open GitHub Issue, then drag the report into the issue.",
                               ["Open GitHub Issue", "Done"])
            guard b == "Open GitHub Issue" else { return }
            DispatchQueue.main.async {
                NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: made.zip)])   // to drag into the issue
                if let url = issueURL(title: made.title, details: made.details) { NSWorkspace.shared.open(url) }
            }
        }
    }
}

/// Writes the report and zips it. Returns the zip's path, and the issue's title and technical
/// details (issueDetails, masked), or nil.
func buildReport(_ log: (String) -> Void) -> (zip: String, title: String, details: String)? {
    let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HHmmss"
    let name = "RB Stems Plus report \(f.string(from: Date()))"
    // made next to the logs (a folder safeRemove may clear), zipped, then moved out
    let tmp = logDir + "/.report-\(UUID().uuidString)"
    let dir = tmp + "/" + name
    defer { safeRemove(tmp) }
    guard (try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)) != nil else { return nil }
    let facts = reportFacts()
    // the checksums this report prints itself, and payload.json's (public): never masked
    let manifestText = (try? String(contentsOfFile: manifestFile, encoding: .utf8)) ?? ""
    let keep = knownChecksums(facts.map(\.line).joined(separator: "\n") + "\n" + manifestText)
    func mask(_ s: String) -> String { redact(s, keep: keep) }
    try? mask(reportSummary(facts)).write(toFile: dir + "/summary.txt", atomically: true, encoding: .utf8)
    // the logs (their last 2 MB) and the small state files, masked
    let files = [logPath, watcherLogPath, bridgeLogPath, chosenFile, dismissedFile, requestFile, busyFile, configFile, manifestFile]
    for path in files where FileManager.default.fileExists(atPath: path) {
        try? mask(tail(path, 2_000_000)).write(toFile: dir + "/" + (path as NSString).lastPathComponent, atomically: true, encoding: .utf8)
    }
    // rekordbox's own errors from the last 30 minutes, from macOS's log: problems outside
    // RB Stems Plus (e.g. rekordbox's audio engine failing to start) show only there
    let sys = runTool("/usr/bin/log", ["show", "--last", "30m", "--style", "compact", "--predicate",
        "(process == \"rekordbox\" OR process == \"RB Stems Plus\" OR process == \"\(watcherName)\") AND (messageType == error OR messageType == fault)"])
    let sysText = sys.1.split(separator: "\n", omittingEmptySubsequences: false).suffix(3000).joined(separator: "\n")
    try? mask(sys.0 == 0 ? sysText : "log show failed (\(sys.0)): \(sysText)")
        .write(toFile: dir + "/system-log-errors.txt", atomically: true, encoding: .utf8)
    // crash and hang reports of the last 7 days for rekordbox, RB Stems Plus and its watcher (the newest 5)
    let fm = FileManager.default
    var reports: [(path: String, date: Date)] = []
    for d in [home + "/Library/Logs/DiagnosticReports", "/Library/Logs/DiagnosticReports"] {
        for f in (try? fm.contentsOfDirectory(atPath: d)) ?? [] where crashReportWanted(f) {
            let p = d + "/" + f
            if let date = (try? fm.attributesOfItem(atPath: p))?[.modificationDate] as? Date,
               date.timeIntervalSinceNow > -7 * 24 * 3600, fm.isReadableFile(atPath: p) { reports.append((p, date)) }
        }
    }
    if !reports.isEmpty { try? fm.createDirectory(atPath: dir + "/crash-reports", withIntermediateDirectories: true) }
    for r in reports.sorted(by: { $0.date > $1.date }).prefix(5) {
        guard let data = fm.contents(atPath: r.path) else { continue }
        try? mask(String(decoding: data.prefix(2_000_000), as: UTF8.self))
            .write(toFile: dir + "/crash-reports/" + (r.path as NSString).lastPathComponent, atomically: true, encoding: .utf8)
    }
    log("report: \(reports.count) crash or hang report\(reports.count == 1 ? "" : "s") from the last 7 days, \(min(reports.count, 5)) included")
    // the issue's title and details: the last problem, then the short lines, masked
    let problem = lastProblem(appLog: tail(logPath, 500_000), bridgeLog: tail(bridgeLogPath, 500_000))
    let details = issueDetails(problem: mask(problem.text), facts: facts.filter(\.link).map { mask($0.line) })
    let title = problem.title.map { "Problem report: " + mask($0) } ?? "Problem report"
    let zip = tmp + "/" + name + ".zip"
    let r = runTool("/usr/bin/ditto", ["-c", "-k", "--norsrc", "--noextattr", "--noqtn", "--keepParent", dir, zip])
    guard r.0 == 0 else { log("report: zip failed: \(r.1)"); return nil }
    // on the Desktop; if macOS (or the user) refuses that, next to the logs
    for target in [home + "/Desktop", logDir + "/reports"] {
        try? FileManager.default.createDirectory(atPath: target, withIntermediateDirectories: true)
        let dest = target + "/" + name + ".zip"
        if (try? FileManager.default.moveItem(atPath: zip, toPath: dest)) != nil { return (dest, title, details) }
        log("report: couldn't save it in \(redact(target))")
    }
    return nil
}

/// Whether a file in DiagnosticReports is a crash or hang report the report takes: rekordbox's,
/// the app's ("RB Stems Plus-2026-…ips") or the watcher's ("RB Stems Plus Watcher-2026-…ips").
func crashReportWanted(_ name: String) -> Bool {
    name.hasPrefix("rekordbox") || name.hasPrefix("RB Stems Plus") || name.hasPrefix(watcherName)
}

/// The last `bytes` of a file as text ("" if it can't be read).
func tail(_ path: String, _ bytes: UInt64) -> String {
    guard let h = FileHandle(forReadingAtPath: path) else { return "" }
    defer { h.closeFile() }
    let size = h.seekToEndOfFile()
    h.seek(toFileOffset: size > bytes ? size - bytes : 0)
    return String(decoding: h.readDataToEndOfFile(), as: UTF8.self)
}

// MARK: the facts

/// summary.txt: every fact, one per line, most important first.
func reportSummary(_ facts: [(line: String, link: Bool)]) -> String {
    (["RB Stems Plus report, \(Date())"] + facts.map(\.line)).joined(separator: "\n") + "\n"
}

/// What a developer needs to understand this Mac's state, one fact per line, most important
/// first: the app and its two lights, the Mac, rekordbox, what is installed and chosen, the disk,
/// the watcher and the last update check, rekordbox's signature, then the rest. `link`: also in
/// the issue's link; the long ones (full checksums, the folders' listings, rekordbox's whole
/// signature check) are only in summary.txt, in the zip.
func reportFacts() -> [(line: String, link: Bool)] {
    let fm = FileManager.default
    func sha(_ p: String) -> String { fm.fileExists(atPath: p) ? (sha256(p) ?? "unreadable") : "missing" }
    func run(_ exe: String, _ args: String...) -> String { runTool(exe, args).1.trimmingCharacters(in: .whitespacesAndNewlines) }
    let plus = stemsPlusPresent(), cache = stemsCachePresent(), c = chosen(), cfg = readConfig(), stems = savedStemsSize()
    // the lights, as the window's refresh() shows them
    let paused = cache && cfg.enabled ? cachePausedFloor() : nil
    let model = modelDir + "/hdemucs.onnx", modelSha = sha(model), bridgeModels = cache ? bridgeCacheModels() : nil
    let lights = (plusLight(installed: plus),
                  cacheLight(installed: cache, plusOn: plus, chosen: c.cache, rekordboxModel: cfg.rekordboxModel, bridgeModels: bridgeModels,
                             model: isSHA256(modelSha) ? modelSha : nil, engine: engineVersion(), pausedFloor: paused))
    let volume = cacheVolumePath(), disk = diskBytes(volume)
    let rbPid = runTool("/usr/bin/pgrep", ["-f", "rekordbox\\.app/Contents/MacOS/rekordbox"]).1.split(separator: "\n").first.flatMap { pid_t($0) }
    let verify = runTool("/usr/bin/codesign", ["--verify", "--deep", "--strict", rbApp])
    let signer = runTool("/usr/bin/codesign", ["-dv", rbApp]).1.split(separator: "\n").map(String.init)
        .filter { l in ["Identifier", "TeamIdentifier", "Signature", "Authority", "Runtime"].contains { l.hasPrefix($0 + "=") || l.hasPrefix($0 + " ") } }
    let m = savedManifest()
    var s: [(line: String, link: Bool)] = []
    func add(_ line: String, link: Bool = true) { s.append((line, link)) }
    add(appLine(version: appVersion, build: appBuild, plus: lights.0, cache: lights.1))
    add(macLine(os: ProcessInfo.processInfo.operatingSystemVersionString, model: run("/usr/sbin/sysctl", "-n", "hw.model"),
                chip: run("/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"), arch: run("/usr/bin/uname", "-m"),
                memory: ProcessInfo.processInfo.physicalMemory, language: Locale.current.identifier))
    add(rekordboxLine(version: rekordboxInstalled() ? rekordboxVersion() : nil, running: rbPid != nil,
                      rosetta: rbPid.flatMap(translated), engine: engineVersion()))
    add(installedLine(plus: plus, cache: cache, chosenModel: c.model, chosenCache: c.cache, modelSha: modelSha, modelSize: fileSize(model)))
    add(diskLine(free: freeBytes(volume), disk: disk, floor: cacheFreeFloor(disk: disk)))
    var off: Bool? = nil
    if #available(macOS 13, *) { off = launchdDisabled(runTool("/bin/launchctl", ["print-disabled", guiDomain]).1, label: agentLabel) }
    add(watcherLine(plistThere: fm.fileExists(atPath: agentPlist),
                    loaded: runTool("/bin/launchctl", ["print", "\(guiDomain)/\(agentLabel)"]).0 == 0, disabled: off,
                    payload: m?.payloadVersion, lastCheck: manifestAge()))
    add(signatureLine(valid: verify.0 == 0, verifyOutput: verify.1, signer: signer))
    // then the rest, still in the link while it fits
    add("can install: Stems Plus \(stemsPlusBlocked() ?? "yes"); Stems Cache \(stemsCacheBlocked() ?? "yes")")
    if let m = m {
        add("payload.json: payload \(m.payloadVersion), app \(m.app.version), model \(m.model.sha256.prefix(12)), bridge \(m.bridge.sha256.prefix(12))")
        add("compatible: rekordbox \(m.compatible.rekordbox.joined(separator: " ")), ONNX Runtime \(m.compatible.ortVersionPrefix)x, STEMS Engine \(m.compatible.stemsEngine.joined(separator: " "))")
    }
    add("settings: enabled=\(cfg.enabled ? 1 : 0) max_gb=\(configNumber(cfg.maxGB)) max_days=\(configNumber(cfg.maxDays)); saved stems\(stems.isEmpty ? " under 0.1 GB" : stems)")
    if cacheIsLink() { add("saved stems folder: a link, never cleared or deleted (\(cacheLinkLine))") }
    add("administrator: \(userIsAdmin ? "yes" : "no"); app at \(Bundle.main.bundlePath)")
    let saved = installedModels { _ in }.savedOriginals().map { "\($0.sha.prefix(12)) (engine \($0.engine.isEmpty ? "?" : $0.engine))" }
    add("Pioneer's models saved: \(saved.isEmpty ? "none" : saved.joined(separator: ", "))")
    add("ONNX Runtime in rekordbox: \(sha(rbLib).prefix(12)), version \(dylibVersion(rbLib) ?? "?"), Pioneer-signed \(signedByPioneer(rbLib))")
    add("ONNX Runtime copy for the bridge: \(sha(rootOrt).prefix(12)), version \(dylibVersion(rootOrt) ?? "?"), Pioneer-signed \(signedByPioneer(rootOrt))")
    // only in summary.txt
    add("model in rekordbox, full: \(modelSha)", link: false)
    add("ONNX Runtime in rekordbox, full: \(sha(rbLib)); copy for the bridge: \(sha(rootOrt))", link: false)
    add("originals folder: \(((try? fm.contentsOfDirectory(atPath: originalsDir)) ?? []).sorted().joined(separator: " "))", link: false)
    add("payload folders: \(((try? fm.contentsOfDirectory(atPath: payloadRoot)) ?? []).sorted().joined(separator: " "))", link: false)
    add("root folder: \(run("/bin/ls", "-R", rootDir).replacingOccurrences(of: "\n", with: " "))", link: false)
    add("rekordbox signature, whole: \((verify.1 + (verify.0 == 0 ? "\nvalid" : "")).trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: "\n", with: " | ")) || \(signer.joined(separator: " "))", link: false)
    return s
}

/// Whether process `pid` runs under Rosetta (the kernel's P_TRANSLATED flag); nil if unknown.
func translated(_ pid: pid_t) -> Bool? {
    var info = kinfo_proc()
    var size = MemoryLayout<kinfo_proc>.stride
    var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, pid]
    guard sysctl(&mib, u_int(mib.count), &info, &size, nil, 0) == 0, size > 0 else { return nil }
    return info.kp_proc.p_flag & 0x0002_0000 != 0           // P_TRANSLATED
}

/// A light as the window shows it: green, yellow or red.
func lightColour(_ look: LightLook) -> String {
    switch look { case .on: return "green"; case .limited: return "yellow"; case .off: return "red" }
}

func appLine(version: String, build: String, plus: (look: LightLook, tip: String), cache: (look: LightLook, tip: String)) -> String {
    "RB Stems Plus: \(version) (\(build)); lights: Stems Plus \(lightColour(plus.look)) (\(plus.tip)), Stems Cache \(lightColour(cache.look)) (\(cache.tip))"
}

func macLine(os: String, model: String, chip: String, arch: String, memory: UInt64, language: String) -> String {
    "macOS: \(os); Mac: \(model), \(chip.isEmpty ? "?" : chip), \(arch), \(memory / 1_073_741_824) GB memory; language: \(language)"
}

/// `rosetta`: nil when rekordbox isn't running or it can't be told.
func rekordboxLine(version: String?, running: Bool, rosetta: Bool?, engine: String) -> String {
    let how = !running ? "no" : rosetta.map { $0 ? "yes, under Rosetta" : "yes, native" } ?? "yes"
    return "rekordbox: \(version ?? "not installed"), running: \(how); STEMS Engine: \(engine.isEmpty ? "not downloaded" : engine)"
}

func installedLine(plus: Bool, cache: Bool, chosenModel: Bool, chosenCache: Bool, modelSha: String, modelSize: Int?) -> String {
    func yn(_ b: Bool) -> String { b ? "yes" : "no" }
    return "installed: Stems Plus \(yn(plus)), Stems Cache \(yn(cache)); chosen: Stems Plus \(yn(chosenModel)), Stems Cache \(yn(chosenCache)); "
        + "model in rekordbox: \(modelSha.prefix(12))\(modelSize.map { " (\($0) bytes)" } ?? "")"
}

func diskLine(free: Int64?, disk: Int64?, floor: Int64) -> String {
    func gb(_ b: Int64?) -> String { b.map { String(format: "%.1f GB", Double($0) / 1e9) } ?? "unknown" }
    return "disk: \(gb(free)) free of \(gb(disk)); Stems Cache's floor: \(cacheFloorGB(floor)) GB\(free.map { $0 < floor } == true ? ", under it: saving paused" : "")"
}

/// `disabled`: nil where it can't be told (before macOS 13). `lastCheck`: how long ago payload.json
/// was last downloaded.
func watcherLine(plistThere: Bool, loaded: Bool, disabled: Bool?, payload: String?, lastCheck: TimeInterval?) -> String {
    let w = !plistThere ? "not installed" : disabled == true ? "installed, turned off in Login Items" : loaded ? "installed, loaded" : "installed, not loaded"
    let ago: String
    if let t = lastCheck { ago = t < 3600 ? "\(Int(t / 60)) min ago" : t < 48 * 3600 ? "\(Int(t / 3600)) h ago" : "\(Int(t / 86400)) days ago" }
    else { ago = "never" }
    return "watcher: \(w); payload: \(payload ?? "none"); last update check: \(ago)"
}

/// codesign's verdict on rekordbox, short: valid or not (with codesign's first line), and the team.
func signatureLine(valid: Bool, verifyOutput: String, signer: [String]) -> String {
    let team = signer.first { $0.hasPrefix("TeamIdentifier=") }.map { String($0.dropFirst(15)) } ?? "none"
    let why = verifyOutput.split(separator: "\n").first.map { ": " + String($0.prefix(100)) } ?? ""
    return "rekordbox signature: \(valid ? "valid" : "invalid" + why); team \(team)"
}

// MARK: the last problem

/// How far back the issue's "Last problem" looks.
let lastProblemWindow: TimeInterval = 3 * 3600

/// The last problem in the logs, for the issue: up to 5 problems of app.log (lines saying
/// something failed, was refused or couldn't be done; an admin run's FAILED line, with its name),
/// then the newest 3 ERROR lines of bridge.log. Only lines of the lastProblemWindow before `now`,
/// each with its date and time; a line without a readable date is skipped. The same problem
/// several times in a row is one line, "(3 times, last at HH:mm)". A problem the logs show fixed
/// later is left out: an admin run that later succeeded under the same name, "Stems Plus not
/// installed" before "Stems Plus: installed" or "already in place", "Stems Cache not installed"
/// before a successful install or reinstall run, the bridge's loading errors before "bridge
/// loaded", and its model reading errors before "Demucs session opened".
/// `text` is "" and `title` nil when there is no problem; else `title` says what failed, in at
/// most 60 characters. Not masked: the caller masks both.
func lastProblem(appLog: String, bridgeLog: String, now: Date = Date()) -> (title: String?, text: String) {
    let f = DateFormatter()
    f.locale = Locale(identifier: "en_US_POSIX"); f.dateFormat = "yyyy-MM-dd HH:mm:ss"
    let hm = DateFormatter()
    hm.locale = Locale(identifier: "en_US_POSIX"); hm.dateFormat = "HH:mm"
    struct Line { var stamp: String; var date: Date; var message: String }
    struct Problem { var line: Line; var title: String }
    /// The log's lines in the window, oldest first.
    func recent(_ log: String) -> [Line] {
        log.split(separator: "\n").compactMap { l in
            let line = String(l)
            guard line.count > 19, let d = f.date(from: String(line.prefix(19))),
                  d <= now.addingTimeInterval(60), now.timeIntervalSince(d) <= lastProblemWindow else { return nil }
            return Line(stamp: String(line.prefix(19)), date: d, message: line.dropFirst(19).trimmingCharacters(in: .whitespaces))
        }
    }
    func cut(_ s: String, _ n: Int) -> String { s.count > n ? String(s.prefix(n - 1)) + "…" : s }
    let failed = try! NSRegularExpression(pattern: "\\b(failed|refused|couldn't|could not|cannot|error)\\b", options: .caseInsensitive)
    let adminStart = try! NSRegularExpression(pattern: "^admin run \\((.+)\\), attempt \\d+$")

    // app.log: the admin runs (their name, and which succeeded), and the problems
    let app = recent(appLog)
    var okRun: [Int: String] = [:]                      // line index → the name of an admin run that succeeded there
    var found: [(index: Int, problem: Problem, fixed: (Int) -> Bool)] = []
    var run: String?
    for (i, l) in app.enumerated() {
        let m = l.message
        if let match = adminStart.firstMatch(in: m, range: NSRange(m.startIndex..., in: m)), let r = Range(match.range(at: 1), in: m) {
            run = String(m[r]); continue
        }
        if let name = run, m.hasPrefix("ok:") { okRun[i] = name; run = nil; continue }
        if let name = run, m.hasPrefix("FAILED") {
            var line = l; line.message = "admin run (\(name)) " + m
            found.append((i, Problem(line: line, title: "admin run (\(name)) failed"), { j in okRun[j] == name }))
            run = nil; continue
        }
        let plusMissing = m.hasPrefix("Stems Plus not installed"), cacheMissing = m.hasPrefix("Stems Cache not installed")
        guard !m.hasPrefix("report:"), plusMissing || cacheMissing
                || failed.firstMatch(in: m, range: NSRange(m.startIndex..., in: m)) != nil else { continue }
        let fixed: (Int) -> Bool
        if plusMissing { fixed = { j in app[j].message.hasPrefix("Stems Plus: installed") || app[j].message.hasPrefix("Stems Plus: already in place") } }
        else if cacheMissing { fixed = { j in okRun[j] == "install" || okRun[j] == "reinstall" } }
        else { fixed = { _ in false } }
        found.append((i, Problem(line: l, title: m), fixed))
    }
    // (okRun is complete only now: a problem is fixed by a line after it)
    let appProblems = found.filter { p in !app.indices.contains { j in j > p.index && p.fixed(j) } }.map(\.problem)

    // bridge.log: its ERROR lines, unless a later load or session shows them fixed
    let bridge = recent(bridgeLog)
    var bridgeProblems: [Problem] = []
    for (i, l) in bridge.enumerated() where l.message.hasPrefix("ERROR") {
        let m = l.message
        let fix: String? = m.hasPrefix("ERROR real library") || m.hasPrefix("ERROR no real ONNX Runtime") || m.hasPrefix("ERROR the home folder")
            ? "bridge loaded" : m.hasPrefix("ERROR cannot read") ? "Demucs session opened" : nil
        if let fix = fix, bridge[(i + 1)...].contains(where: { $0.message.hasPrefix(fix) }) { continue }
        bridgeProblems.append(Problem(line: l, title: "Stems Cache: " + m.dropFirst(5).trimmingCharacters(in: .whitespaces)))
    }

    /// The same message in a row: one line, with how many times and the last time.
    func collapse(_ ps: [Problem]) -> [(first: Problem, count: Int, last: Line)] {
        var out: [(first: Problem, count: Int, last: Line)] = []
        for p in ps {
            if let prev = out.last, prev.first.line.message == p.line.message { out[out.count - 1] = (prev.first, prev.count + 1, p.line) }
            else { out.append((p, 1, p.line)) }
        }
        return out
    }
    func render(_ g: (first: Problem, count: Int, last: Line)) -> String {
        cut("\(g.first.line.stamp)  \(g.first.line.message)", 200) + (g.count > 1 ? " (\(g.count) times, last at \(hm.string(from: g.last.date)))" : "")
    }
    let appGroups = Array(collapse(appProblems).suffix(5)), bridgeGroups = Array(collapse(bridgeProblems).suffix(3))
    var lines = appGroups.map(render)
    if !bridgeGroups.isEmpty { lines += ["bridge.log:"] + bridgeGroups.map(render) }
    guard let title = appGroups.last?.first.title ?? bridgeGroups.last?.first.title else { return (nil, "") }
    return (cut(title, 60), "Last problem:\n" + lines.joined(separator: "\n"))
}

// MARK: masking

/// Replaces the home folder with "~" and the user's account and full names with "<user>", and
/// masks email addresses ("<email>"), private keys, the values of URL query parameters and of
/// settings named like a password, key, token, secret or session, bearer tokens, JWTs, AWS,
/// GitHub, Slack and API keys, and any other run of 32 or more characters that looks random
/// ("<token>"). Checksums in `keep` (64 or 40 hex digits: the ones the report prints itself, and
/// payload.json's, needed for support) stay readable.
func redact(_ s: String, keep: Set<String> = []) -> String {
    var out = maskSecrets(s.replacingOccurrences(of: home, with: "~"), keep: keep)
    for name in [NSFullUserName(), NSUserName()] where name.count >= 2 {
        let pattern = "\\b" + NSRegularExpression.escapedPattern(for: name) + "\\b"
        out = out.replacingOccurrences(of: pattern, with: "<user>", options: [.regularExpression, .caseInsensitive])
    }
    return out
}

/// redact's masks, in order: each later one sees the earlier ones' placeholders, never the
/// values they replaced.
private let secretMasks: [(NSRegularExpression, String)] = [
    // a PEM private key, whole
    ("-----BEGIN [A-Z ]*PRIVATE KEY-----[\\s\\S]*?(-----END [A-Z ]*PRIVATE KEY-----|\\z)", "<private key>"),
    ("[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)*\\.[A-Za-z]{2,}", "<email>"),
    // a URL's query parameter named like a secret (X-Amz-Signature, access_token, sessionid, …)
    ("([?&;][^=&\\s#\"'<>]*(token|key|auth|session|sig|secret|passw|pwd|credential|code)[^=&\\s#\"'<>]*=)[^&\\s#\"'<>]+", "$1<token>"),
    // "Bearer …", "Basic …"
    ("\\b(Bearer|Basic)\\s+[A-Za-z0-9._~+/=-]{8,}", "$1 <token>"),
    // name=value or name: value, for names like password, api_key, client_secret, session_id
    ("\\b([A-Za-z0-9_-]*(password|passwd|pwd|secret|token|api[_-]?key|access[_-]?key|private[_-]?key|session[_-]?id|sessionid|cookie|authorization)[A-Za-z0-9_-]*\"?\\s*[:=]\\s*\"?)(?!<token>|Bearer\\b|Basic\\b)[^\\s\"',;&]+",
     "$1<token>"),
    ("\\beyJ[A-Za-z0-9_-]{4,}\\.[A-Za-z0-9_-]{4,}\\.[A-Za-z0-9_-]*", "<token>"),                // a JWT
    ("\\b(AKIA|ASIA)[A-Z0-9]{16}\\b", "<token>"),                                             // an AWS access key ID
    ("\\b(gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})\\b", "<token>"),            // GitHub
    ("\\bxox[abposr]-[A-Za-z0-9-]{10,}", "<token>"),                                          // Slack
    ("\\bsk-[A-Za-z0-9_-]{20,}", "<token>"),                                                  // API keys "sk-…"
].map { (try! NSRegularExpression(pattern: $0.0, options: .caseInsensitive), $0.1) }

/// A run of 32 or more characters of hex, base64 or base64url (with "=" padding).
private let longRun = try! NSRegularExpression(pattern: "(?<![A-Za-z0-9_+=-])[A-Za-z0-9_+-]{32,}={0,2}(?![A-Za-z0-9_+=-])")

/// redact's masking alone (no home folder or user name).
func maskSecrets(_ s: String, keep: Set<String> = []) -> String {
    var out = s
    for (re, template) in secretMasks {
        out = re.stringByReplacingMatches(in: out, range: NSRange(out.startIndex..., in: out), withTemplate: template)
    }
    var result = "", last = out.startIndex
    for m in longRun.matches(in: out, range: NSRange(out.startIndex..., in: out)) {
        guard let r = Range(m.range, in: out), looksRandom(String(out[r]), keep: keep) else { continue }
        result += out[last..<r.lowerBound] + "<token>"
        last = r.upperBound
    }
    return result + out[last...]
}

/// Hex with digits and letters (a key, or a checksum not in `keep`), or a mix of upper and lower
/// case and digits without a word in it (names of code in crash reports have words: 8 lower case
/// letters in a row).
private func looksRandom(_ run: String, keep: Set<String>) -> Bool {
    let hexDigits = Set("0123456789abcdefABCDEF")
    if run.allSatisfy({ hexDigits.contains($0) }) {
        return run.contains(where: \.isNumber) && run.contains(where: \.isLetter) && !keep.contains(run.lowercased())
    }
    guard run.contains(where: \.isNumber), run.contains(where: \.isUppercase), run.contains(where: \.isLowercase) else { return false }
    var lower = 0
    for ch in run {
        lower = ch.isLowercase ? lower + 1 : 0
        if lower >= 8 { return false }
    }
    return true
}

/// The 64- and 40-hex-digit checksums (and commits) in `s`, lower case: what redact keeps.
func knownChecksums(_ s: String) -> Set<String> {
    let re = try! NSRegularExpression(pattern: "(?<![0-9A-Fa-f])([0-9A-Fa-f]{64}|[0-9A-Fa-f]{40})(?![0-9A-Fa-f])")
    return Set(re.matches(in: s, range: NSRange(s.startIndex..., in: s)).compactMap { Range($0.range, in: s).map { s[$0].lowercased() } })
}

// MARK: the issue

/// The longest link the app opens: browsers and GitHub take about 8 KB.
let issueURLLimit = 7000

/// `s` in at most `bytes` bytes of UTF-8, ending in "…" when cut.
func capBytes(_ s: String, _ bytes: Int) -> String {
    guard s.utf8.count > bytes else { return s }
    var out = ""
    for ch in s {
        if out.utf8.count + ch.utf8.count > bytes - 3 { break }
        out.append(ch)
    }
    return out + "…"
}

/// The technical details in the issue's link: the last problem (at most 800 bytes), a blank line,
/// then the facts, one line each (at most 300 bytes). With these limits the last problem and the
/// first four facts always fit in issueURLLimit, even when every character takes 9 in the link.
func issueDetails(problem: String, facts: [String]) -> String {
    (problem.isEmpty ? "" : capBytes(problem, 800) + "\n\n")
        + facts.map { capBytes($0.replacingOccurrences(of: "\n", with: " "), 300) }.joined(separator: "\n")
}

/// The issue's text: the user's own words first, the technical details at the bottom.
func issueBody(_ details: String) -> String {
    """
    ### What happened?



    ### Technical details
    Please also drag in the report zip that RB Stems Plus just showed in Finder (it has the full logs).

    ```
    \(details)
    ```
    """
}

/// A new GitHub issue with the title and the technical details filled in (the user attaches the
/// zip). Whole lines are left out from the end of the details until the link is at most
/// issueURLLimit long.
func issueURL(title: String, details: String) -> URL? {
    func url(_ details: String) -> URL? {
        var u = URLComponents(string: issuesURL)
        u?.queryItems = [URLQueryItem(name: "title", value: capBytes(title, 120)), URLQueryItem(name: "body", value: issueBody(details))]
        // a "+" would arrive as a space
        let query = u?.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")
        u?.percentEncodedQuery = query
        return u?.url
    }
    var lines = details.components(separatedBy: "\n")
    while let u = url(lines.joined(separator: "\n")) {
        if u.absoluteString.utf8.count <= issueURLLimit || lines.isEmpty { return u }
        lines.removeLast()
    }
    return nil
}
