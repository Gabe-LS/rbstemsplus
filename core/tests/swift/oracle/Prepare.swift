// Preparing playlists: the stems of a rekordbox playlist written into Stems Cache's cache before
// a set, by the prepare helper inside the app (Contents/Helpers/rbsp-prepare, helper/prepare.c),
// and kept there for a while (a kept set, read by the bridge). This file has no window: the
// checks before preparing, the helper's runs and what they print, the kept sets. The window's
// side is PrepareView.swift, the watcher's check when rekordbox opens is in Watcher.swift.
// The decisions: at most 5 playlists, 75% of the limit, kept 10 days by default.
import Foundation

/// What a run of playlist ID adds to the helper's arguments. first: "Prepare the First N Tracks":
/// the helper reads the whole playlist, so the set keeps what it holds of the tracks past N and
/// lets go of what left the playlist.
func runArgs(_ id: String, until: String, first: Int?) -> [String] {
    ["--until", until, "--keep", id, "--playlist", id] + (first.map { ["--first", String($0)] } ?? [])
}

/// The helper's place inside the app.
let prepareHelperName = "rbsp-prepare"
func prepareHelper(_ app: String = Bundle.main.bundlePath) -> String {
    #if TEST_BUILD
    if let fake = probeFakeHelper(app) { return fake }      // the harness's scripted helper (app/Probe)
    #endif
    return app + "/Contents/Helpers/" + prepareHelperName
}
/// The kept sets: <playlist id>.kept (the bridge's) and <playlist id>.manifest (the helper's).
let keptDir = support + "/kept"
let keptSetsMax = 5
let keepDaysDefault = 10
/// rekordbox's settings file holding "Prioritize sound quality" (PartAnalysisQuality, 0) or
/// "Prioritize speed" (1): with speed rekordbox uses another separator and no stems are cached.
let rbSettingsFile = rbSettingsDir + "/rekordbox3.settings"

// MARK: - the checks before preparing (requirement 6)

/// What preparing depends on, gathered by prepareFacts() (or by the tests).
struct PrepareFacts {
    var appleSilicon = true
    var helperThere = true
    var rekordboxInstalled = true
    var rekordboxOK = true                  // its version is one Stems Cache supports
    var rekordboxVersion = ""
    var cacheInstalled = true
    var bridgeKeeps = true                  // the bridge says kept=1
    var ortThere = true                     // Pioneer's ONNX Runtime, the copy Stems Cache keeps
    var configUnreadable: String?
    var enabled = true
    /// Stems Cache's light, as the Status tab shows it (cacheLight): preparing needs it on
    var cacheLook = LightLook.on
    var cacheTip = ""
    /// PartAnalysisQuality: 0 sound quality, 1 speed, nil unknown
    var quality: Int?
    /// our model is installed (else rekordbox's own runs)
    var plusOn = false
    /// the installed model's checksum (nil: still being worked out)
    var modelSHA: String?
    var ourModels: [String] = [ourModel]
    /// the folder the stems go to now (nil: none usable), and whether it is a chosen drive's
    var cacheFolder: String?
    var chosenDrive = false
    var maxGB = 20.0
}

/// Why playlists can't be prepared now, one line each (none: they can). The same lines for
/// the Prepare tab and the watcher's check.
func prepareProblems(_ f: PrepareFacts) -> [String] {
    if !f.appleSilicon { return ["Preparing playlists needs a Mac with Apple silicon."] }
    if !f.helperThere { return ["This copy of RB Stems Plus can't prepare playlists: its helper is missing. Download RB Stems Plus again."] }
    if !f.rekordboxInstalled { return ["rekordbox 7 isn't in /Applications/rekordbox 7."] }
    if !f.cacheInstalled { return ["Install Stems Cache first: prepared stems are saved where Stems Cache saves them."] }
    var out: [String] = []
    if !f.bridgeKeeps || !f.ortThere { out.append("Reinstall Stems Cache: this installation can't keep prepared playlists.") }
    if !f.rekordboxOK { out.append("Preparing doesn't support rekordbox \(shortVersion(f.rekordboxVersion)) yet." + checksAgain) }
    if let why = f.configUnreadable { out.append("Stems Cache can't read its settings (config.ini: \(why)).") }
    else if !f.enabled { out.append("Stems Cache is turned off in config.ini (enabled=0).") }
    if f.cacheLook != .on && !f.cacheTip.isEmpty && !out.contains(f.cacheTip) { out.append(f.cacheTip) }
    if f.quality == 1 {
        out.append("rekordbox is set to Prioritize speed for STEMS: prepared stems are used only with Prioritize sound quality (rekordbox › Preferences › Extensions › STEMS).")
    }
    if let sha = f.modelSHA, f.plusOn, !f.ourModels.contains(sha) {
        out.append("The Demucs v4 model in rekordbox isn't the one RB Stems Plus installed: reinstall Demucs v4.")
    }
    if f.cacheFolder == nil && out.isEmpty { out.append("Where Stems Cache saves stems can't be used now.") }
    return out
}

/// PartAnalysisQuality in rekordbox's settings file: <VALUE name="PartAnalysisQuality" val="0"/>.
func stemsQuality(_ file: String = rbSettingsFile) -> Int? {
    guard let d = FileManager.default.contents(atPath: file), d.count < 4 << 20 else { return nil }
    let s = String(decoding: d, as: UTF8.self)
    guard let r = s.range(of: "name=\"PartAnalysisQuality\"") else { return nil }
    let rest = s[r.upperBound...].prefix(64)
    guard let v = rest.range(of: "val=\"") else { return nil }
    return Int(rest[v.upperBound...].prefix { $0.isNumber })
}

/// Whether the installed bridge keeps prepared playlists: kept=1 among the tokens before
/// pioneer= in its line ("rbstems-cache: rebuild=1 location=1 kept=1 pioneer=<sha>;").
func bridgeSaysKept(_ path: String = rbLib) -> Bool {
    guard let d = FileManager.default.contents(atPath: path) else { return false }
    return bridgeSaysKept(data: d)
}

func bridgeSaysKept(data d: Data) -> Bool {
    guard d.range(of: Data("rbstems bridge: ".utf8)) != nil, let start = d.range(of: Data("rbstems-cache: ".utf8)) else { return false }
    let rest = d[start.upperBound...].prefix(4096)
    guard let end = rest.firstIndex(of: UInt8(ascii: ";")) else { return false }
    let line = String(decoding: rest[rest.startIndex..<end], as: UTF8.self)
    return line.components(separatedBy: "pioneer=")[0].split(separator: " ").contains("kept=1")
}

/// Whether this Mac has Apple silicon (the helper is built for it only), also when this app runs
/// under Rosetta.
let appleSilicon: Bool = {
    var v: Int32 = 0, n = MemoryLayout<Int32>.size
    if sysctlbyname("hw.optional.arm64", &v, &n, nil, 0) == 0 { return v == 1 }
    return false
}()

/// The facts now, for the app (`app`: the bundle the helper is in). The model's checksum is the
/// background one (nil until worked out: `done` runs then). `measured`: where the stems are saved
/// comes from the app's measurement (LocationMonitor); the watcher, which doesn't measure (and
/// mustn't look at a drive while rekordbox opens), passes false and takes config.ini's chosen
/// folder as it is: its --quick check doesn't look into a cache on another drive.
func prepareFacts(app: String = Bundle.main.bundlePath, measured: Bool = true, done: @escaping () -> Void = {}) -> PrepareFacts {
    var f = PrepareFacts()
    f.appleSilicon = appleSilicon
    f.helperThere = FileManager.default.isExecutableFile(atPath: prepareHelper(app))
    f.rekordboxInstalled = rekordboxInstalled()
    f.rekordboxVersion = rekordboxVersion()
    f.rekordboxOK = rekordboxSupported(f.rekordboxVersion)
    f.cacheInstalled = stemsCachePresent()
    f.bridgeKeeps = bridgeSaysKept()
    f.ortThere = FileManager.default.fileExists(atPath: rootOrt)
    let st = readConfigState()
    f.configUnreadable = st.unreadable
    f.enabled = st.config.enabled
    f.maxGB = st.config.maxGB
    f.plusOn = stemsPlusPresent()
    let loc = LocationMonitor.shared.snapshot()
    let own = f.cacheInstalled && !f.plusOn
    f.modelSHA = currentModelSHA(done: done)
    let light = cacheLight(installed: f.cacheInstalled, plusOn: f.plusOn, chosen: chosen().cache, rekordboxModel: st.config.rekordboxModel,
                           bridgeModels: own ? bridgeCacheModels() : nil, model: own ? f.modelSHA : nil,
                           engine: engineVersion(), pausedFloor: f.enabled ? loc.pausedFloor : nil, location: loc.lightTip, drive: loc.drive)
    f.cacheLook = light.look
    f.cacheTip = light.tip
    f.quality = stemsQuality()
    if let m = savedManifest()?.model.sha256 { f.ourModels.append(m) }
    switch st.location {
    case .thisMac: f.cacheFolder = defaultCacheDir
    case .chosen(let dir, _): f.cacheFolder = !measured || loc.status.flatMap(folderInUse) == dir ? dir : nil; f.chosenDrive = true
    case .malformed: f.cacheFolder = nil
    }
    return f
}

/// The helper's arguments every run shares: the model rekordbox runs, the cache, its limit.
func prepareBaseArgs(_ f: PrepareFacts) -> [String] {
    var a = ["--model", modelDir + "/hdemucs.onnx", "--cache", f.cacheFolder ?? (f.chosenDrive ? "" : defaultCacheDir), "--max-gb", configNumber(f.maxGB),
             "--kept-dir", keptDir, "--ort", rootOrt, "--rekordbox", rbApp]
    if f.chosenDrive { a.append("--chosen") }
    return a
}

// MARK: - what the helper prints (one JSON object per line)

/// A playlist or folder of rekordbox's library (--playlists).
struct Playlist: Equatable {
    var id: String, parent: String, name: String, kind: String, tracks: Int
    var canPrepare: Bool { kind == "playlist" }
}

private func jsonLine(_ line: Substring) -> [String: Any]? {
    guard line.first == "{", let d = line.data(using: .utf8) else { return nil }
    return (try? JSONSerialization.jsonObject(with: d)) as? [String: Any]
}

private func str(_ o: [String: Any], _ k: String) -> String { (o[k] as? String) ?? (o[k] as? NSNumber)?.stringValue ?? "" }
private func num(_ o: [String: Any], _ k: String) -> Double { (o[k] as? NSNumber)?.doubleValue ?? 0 }
private func int(_ o: [String: Any], _ k: String) -> Int { (o[k] as? NSNumber)?.intValue ?? 0 }
private func bool(_ o: [String: Any], _ k: String) -> Bool { (o[k] as? NSNumber)?.boolValue ?? false }

func parsePlaylists(_ out: String) -> [Playlist] {
    out.split(separator: "\n").compactMap(jsonLine).compactMap { o in
        let id = str(o, "playlist")
        return id.isEmpty ? nil : Playlist(id: id, parent: str(o, "parent"), name: str(o, "name"), kind: str(o, "kind"), tracks: int(o, "tracks"))
    }
}

/// One track's line, from an estimate or a run.
struct TrackLine: Equatable {
    var file = "", result = "", why = "", access = "", folder = ""
    var entries = 0, written = 0, already = 0, missing = 0, refused = 0
    var seconds = 0.0, gb = 0.0
    var name: String { (file as NSString).lastPathComponent }
}

/// A prepared track's note: whether it was separated now or saved already (a count of ~11-s
/// chunks says nothing to the user), and parts that couldn't be saved (rekordbox separates them
/// when the track is loaded). Silent parts are never saved, and need not be.
func preparedNote(_ t: TrackLine) -> String {
    if t.written == 0 && t.already == 0 { return t.refused > 0 ? "couldn't be saved: rekordbox separates it when loaded" : "silent: nothing to save" }
    let s = t.written == 0 ? "already saved" : t.already > 0 ? "partly saved already, the rest separated now" : "separated now"
    return t.refused > 0 ? s + ", some parts couldn't be saved" : s
}

private func trackLine(_ o: [String: Any]) -> TrackLine {
    TrackLine(file: str(o, "file"), result: str(o, "result"), why: str(o, "why"), access: str(o, "access"), folder: str(o, "folder"),
              entries: int(o, "entries"), written: int(o, "written"), already: int(o, "already"), missing: int(o, "missing"),
              refused: int(o, "refused"), seconds: num(o, "seconds"), gb: num(o, "gb"))
}

/// --tracks' lines: a playlist's tracks as the library lists them, with nothing said about them yet.
func parseTrackList(_ out: String) -> [TrackLine] {
    out.split(separator: "\n").compactMap(jsonLine).compactMap { o in
        let file = str(o, "file")
        return file.isEmpty ? nil : TrackLine(file: file)
    }
}

/// The same tracks, with nothing said about them: what the last check said isn't what is true now.
func notChecked(_ rows: [TrackLine]) -> [TrackLine] { rows.map { TrackLine(file: $0.file) } }

/// `t` in its track's row, or in a new row at the end for a track the rows don't have. Its row.
/// Lines come in the rows' order: `hint`, the row after the last one, is tried first, or big
/// playlists stall the window. The paths' bytes decide (what the library holds, as Swift's == would
/// make one of two paths that differ only in Unicode form).
@discardableResult
func place(_ t: TrackLine, in rows: inout [TrackLine], hint: Int = 0) -> Int {
    let same = { (a: TrackLine) in a.file.utf8.elementsEqual(t.file.utf8) }
    if hint < rows.count, same(rows[hint]) { rows[hint] = t; return hint }
    if let i = rows.firstIndex(where: same) { rows[i] = t; return i }
    rows.append(t)
    return rows.count - 1
}

/// A row's Result column. Nothing said about the track yet: `checking` tells a check under way
/// from no check at all.
func resultText(_ t: TrackLine, checking: Bool) -> String {
    if t.result.isEmpty { return checking ? "Checking…" : "Not checked" }
    return ["ready": "Ready", "prepare": "To prepare", "skipped": "Skipped", "prepared": "Prepared", "stopped": "Stopped", "unchecked": "Not checked"][t.result] ?? t.result
}

/// An estimate's last line (prepare.c's header comment) and its tracks.
struct Estimate: Equatable {
    var tracks: [TrackLine] = []
    var refused: String?
    var ready = 0, prepare = 0, skipped = 0, unchecked = 0, asking = 0, fitFiles = 0
    var timeS = 0.0, needGB = 0.0, keptGB = 0.0, shareGB = 0.0, maxGB = 0.0, maxGBNeeded = 0.0
    var fits = true, diskOK = true
    var folders: [String] = []
    /// the tracks to prepare, and how long they last
    var toPrepare: [TrackLine] { tracks.filter { $0.result == "prepare" } }
}

func parseEstimate(_ out: String) -> Estimate? {
    var e = Estimate(), ended = false
    for o in out.split(separator: "\n").compactMap(jsonLine) {
        if let r = o["refused"] as? String { e.refused = r; return e }
        if o["file"] != nil { e.tracks.append(trackLine(o)); continue }
        guard o["estimate"] != nil else { continue }
        ended = true
        e.ready = int(o, "ready"); e.prepare = int(o, "prepare"); e.skipped = int(o, "skipped"); e.unchecked = int(o, "unchecked")
        e.asking = int(o, "asking"); e.fitFiles = int(o, "fit_files")
        e.timeS = num(o, "time_s"); e.needGB = num(o, "need_gb"); e.keptGB = num(o, "kept_gb"); e.shareGB = num(o, "share_gb")
        e.maxGB = num(o, "max_gb"); e.maxGBNeeded = num(o, "max_gb_needed")
        e.fits = bool(o, "fits"); e.diskOK = bool(o, "disk_ok")
        e.folders = (o["folders"] as? [String]) ?? []
    }
    return ended ? e : nil
}

/// A run's lines as they come.
enum RunLine: Equatable {
    case progress(file: Int, files: Int, chunk: Int, chunks: Int)
    case track(TrackLine)
    case room(removed: Int, cacheGB: Double)
    case done(files: Int, stopped: String?, keptGB: Double, error: String?)
}

func parseRunLine(_ line: Substring) -> RunLine? {
    guard let o = jsonLine(line) else { return nil }
    if o["chunk"] != nil { return .progress(file: int(o, "at"), files: int(o, "files"), chunk: int(o, "chunk"), chunks: int(o, "chunks")) }
    if o["file"] != nil { return .track(trackLine(o)) }
    if o["room"] != nil { return .room(removed: int(o, "room"), cacheGB: num(o, "cache_gb")) }
    if o["done"] != nil {
        let err = [str(o, "kept_error"), str(o, "manifest_error")].filter { !$0.isEmpty }.first
        return .done(files: int(o, "done"), stopped: o["stopped"] as? String, keptGB: num(o, "kept_gb"), error: err)
    }
    return nil
}

// MARK: - the kept sets

struct KeptSet: Equatable {
    var name: String                     // the playlist's id
    var until: Date?                     // nil: the file isn't the bridge's format
    var entries = 0
    var files = 0                        // the tracks its manifest records
}

/// A set's name, as the helper allows it (kept.c, name_ok).
func keptNameOK(_ s: String) -> Bool {
    !s.isEmpty && s.count <= 64 && s.unicodeScalars.allSatisfy { ("A"..."Z").contains($0) || ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" || $0 == "_" }
}

/// The kept sets in `dir` whose date hasn't passed (`now`), by name.
func keptSets(_ dir: String = keptDir, now: Date = Date()) -> [KeptSet] {
    let names = (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []
    var out: [KeptSet] = []
    for n in names where n.hasSuffix(".kept") && !n.hasPrefix(".") {
        let name = String(n.dropLast(5))
        guard keptNameOK(name), fileType(dir + "/" + n) == S_IFREG else { continue }
        var set = KeptSet(name: name)
        if let d = FileManager.default.contents(atPath: dir + "/" + n), d.count <= 8 << 20 {
            let lines = String(decoding: d, as: UTF8.self).split(separator: "\n", omittingEmptySubsequences: false)
            if lines.count >= 2, lines[0] == "rbstemsplus-kept 1", lines[1].hasPrefix("until "), let u = Double(lines[1].dropFirst(6)) {
                set.until = Date(timeIntervalSince1970: u)
                set.entries = lines.dropFirst(2).filter { $0.count == 129 }.count
            }
        }
        if let u = set.until, u <= now { continue }
        if let m = FileManager.default.contents(atPath: dir + "/" + name + ".manifest") {
            set.files = String(decoding: m, as: UTF8.self).split(separator: "\n").filter { $0.hasPrefix("file ") }.count
        }
        out.append(set)
    }
    return out.sorted { $0.name < $1.name }
}

/// Stops keeping a set: its file and its manifest go (its stems stay, like any others). True if
/// the set is gone.
@discardableResult
func stopKeeping(_ name: String, dir: String = keptDir) -> Bool {
    guard keptNameOK(name) else { return false }
    for f in [dir + "/" + name + ".kept", dir + "/" + name + ".manifest"] where fileType(f) == S_IFREG { unlink(f) }
    return fileType(dir + "/" + name + ".kept") == nil
}

// MARK: - running the helper

/// Lets a check that is no longer wanted be stopped, from any thread, whether its helper has started
/// yet or not. An estimate writes nothing that stopping it can leave half-done.
final class HelperJob {
    private let lock = NSLock()
    private var cancelled = false, process: Process?

    func attach(_ p: Process) {
        lock.lock(); process = p; let stop = cancelled; lock.unlock()
        if stop { p.terminate() }
    }

    func cancel() {
        lock.lock(); cancelled = true; let p = process; lock.unlock()
        if p?.isRunning == true { p?.terminate() }
    }
}

/// Runs the helper to its end (an estimate, the playlists): its output and exit status. `track`
/// gets each track's line as it comes, on a background thread; `job` can stop it. Any thread but
/// the main one.
func runPrepareHelper(_ args: [String], app: String = Bundle.main.bundlePath, timeout: TimeInterval = 300, job: HelperJob? = nil, track: ((TrackLine) -> Void)? = nil) -> (status: Int32, out: String, err: String) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: prepareHelper(app))
    p.arguments = args
    let out = Pipe(), err = Pipe()
    p.standardOutput = out; p.standardError = err; p.standardInput = FileHandle.nullDevice
    do { try p.run() } catch { return (-1, "", "the helper can't start: \(error.localizedDescription)") }
    job?.attach(p)
    var o = Data(), e = Data()
    let g = DispatchGroup()
    g.enter(); DispatchQueue.global().async {
        var from = 0                                    // what isn't a whole line yet
        while true {
            let d = out.fileHandleForReading.availableData
            if d.isEmpty { break }
            o.append(d)
            guard let track = track else { continue }
            while let nl = o[from...].firstIndex(of: UInt8(ascii: "\n")) {
                #if TEST_BUILD
                probeHelperLine(o[from..<nl])                   // the harness's P7 timing point (app/Probe)
                #endif
                if case .track(let t)? = parseRunLine(Substring(String(decoding: o[from..<nl], as: UTF8.self))) { track(t) }
                from = nl + 1
            }
        }
        g.leave()
    }
    g.enter(); DispatchQueue.global().async { e = err.fileHandleForReading.readDataToEndOfFile(); g.leave() }
    if g.wait(timeout: .now() + timeout) == .timedOut { p.terminate(); g.wait() }
    p.waitUntilExit()
    return (p.terminationStatus, String(decoding: o, as: UTF8.self), String(decoding: e, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))
}

/// A prepared playlist's check (`args`: an estimate of set `name`): the playlist as it is now in
/// rekordbox's library, so a track added since it was prepared counts; the set's own files when
/// the playlist can't be read (deleted, or the library busy). `run` runs the helper.
func checkSet(_ name: String, _ args: [String], run: ([String]) -> (status: Int32, out: String, err: String)) -> (status: Int32, out: String, err: String) {
    let r = run(args + ["--playlist", name])
    return parseEstimate(r.out) != nil ? r : run(args)
}

/// A preparing run: the helper started on a playlist, its lines handed to `line` on the main
/// thread as they come, `ended` once it has exited (its status, and its error output). Pause
/// stops the process (SIGSTOP), Resume lets it go on, Stop asks it to stop after its current
/// chunk (SIGTERM): it writes the kept set first.
final class PrepareRun {
    let process = Process()
    private(set) var paused = false
    private var buffer = Data()
    let playlist: String

    init(playlist: String) { self.playlist = playlist }

    func start(_ args: [String], app: String = Bundle.main.bundlePath, line: @escaping (RunLine) -> Void, ended: @escaping (Int32, String) -> Void) -> Bool {
        process.executableURL = URL(fileURLWithPath: prepareHelper(app))
        process.arguments = args
        let out = Pipe(), err = Pipe()
        process.standardOutput = out; process.standardError = err; process.standardInput = FileHandle.nullDevice
        var errData = Data()
        let errDone = DispatchSemaphore(value: 0)
        DispatchQueue.global().async { errData = err.fileHandleForReading.readDataToEndOfFile(); errDone.signal() }
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            guard let self = self, !d.isEmpty else { h.readabilityHandler = nil; return }
            self.buffer.append(d)
            while let nl = self.buffer.firstIndex(of: UInt8(ascii: "\n")) {
                let l = String(decoding: self.buffer[self.buffer.startIndex..<nl], as: UTF8.self)
                self.buffer.removeSubrange(self.buffer.startIndex...nl)
                if let r = parseRunLine(Substring(l)) { DispatchQueue.main.async { line(r) } }
            }
        }
        process.terminationHandler = { p in
            out.fileHandleForReading.readabilityHandler = nil
            let rest = out.fileHandleForReading.readDataToEndOfFile()
            errDone.wait()
            let tail = String(decoding: rest, as: UTF8.self).split(separator: "\n").compactMap(parseRunLine)
            let e = String(decoding: errData, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            DispatchQueue.main.async { tail.forEach(line); ended(p.terminationStatus, e) }
        }
        do { try process.run() } catch { return false }
        return true
    }

    var running: Bool { process.isRunning }
    func pause() { guard process.isRunning, !paused else { return }; kill(process.processIdentifier, SIGSTOP); paused = true }
    func resume() { guard process.isRunning, paused else { return }; kill(process.processIdentifier, SIGCONT); paused = false }
    func stop() {
        guard process.isRunning else { return }
        kill(process.processIdentifier, SIGTERM)
        if paused { kill(process.processIdentifier, SIGCONT); paused = false }
    }
}

// MARK: - words

/// A line for the logs, which never hold track names or paths (README, Privacy): every path in it
/// becomes <path>.
func noPaths(_ s: String) -> String {
    s.replacingOccurrences(of: #"(~|/Volumes|/Users|/private|/tmp|/var|/Applications|/Library)(/[^\n:;,)]*)?"#, with: "<path>", options: .regularExpression)
}

/// "1 h 05 min", "12 min", "under a minute"
func durationText(_ s: Double) -> String {
    if s < 60 { return "under a minute" }
    let m = Int((s / 60).rounded())
    return m < 60 ? "\(m) min" : String(format: "%d h %02d min", m / 60, m % 60)
}

/// "850 MB", "2.4 GB"
func sizeText(_ gb: Double) -> String { gb < 1 ? "\(max(1, Int((gb * 1024).rounded()))) MB" : String(format: "%.1f GB", gb) }

/// What an estimate says, for the Prepare tab: the tracks, then the space and time.
func estimateSummary(_ e: Estimate) -> String {
    var parts: [String] = []
    if e.ready > 0 { parts.append("\(e.ready) \(e.ready == 1 ? "track" : "tracks") ready") }
    if e.prepare > 0 {
        let music = e.toPrepare.reduce(0) { $0 + $1.seconds }
        parts.append("\(e.prepare) to prepare (\(durationText(music)) of music)")
    }
    if e.skipped > 0 { parts.append("\(e.skipped) skipped") }
    var s = parts.isEmpty ? "No tracks." : parts.joined(separator: ", ") + "."
    if e.prepare > 0 { s += " About \(sizeText(e.needGB)) and \(durationText(e.timeS))." }
    s += " Prepared playlists will use \(sizeText(e.keptGB + e.needGB)) of the \(sizeText(e.shareGB)) they may use (75% of Stems Cache's \(configNumber(e.maxGB)) GB limit)."
    return s
}

/// The "doesn't fit" sheet's choices: prepare the tracks that fit, raise the limit (only when
/// the disk has room for it), or cancel. `freeGB`: the disk's free space above its floor.
func doesntFitChoices(_ e: Estimate, freeGB: Double) -> (message: String, buttons: [String], raiseTo: Double?) {
    let need = (e.maxGBNeeded * 10).rounded(.up) / 10
    var buttons: [String] = []
    if e.fitFiles > 0 { buttons.append("Prepare the First \(e.fitFiles) \(e.fitFiles == 1 ? "Track" : "Tracks")") }
    let raise = need > e.maxGB && (need - e.maxGB) <= freeGB ? need : nil
    if let r = raise { buttons.append("Raise the Limit to \(configNumber(r)) GB") }
    buttons.append("Cancel")
    let message = "Prepared playlists may use 75% of Stems Cache's \(configNumber(e.maxGB)) GB limit (\(sizeText(e.shareGB))), the rest stays for the tracks rekordbox separates during a set. This playlist would take them to \(sizeText(e.keptGB + e.needGB))."
        + (raise == nil ? " The disk doesn't have room for a higher limit." : "")
    return (message, buttons, raise)
}
