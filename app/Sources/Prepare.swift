// Preparing playlists: the stems of a rekordbox playlist written into Stems Cache's cache before
// a set, by the prepare helper inside the app (Contents/Helpers/rbsp-prepare, helper/prepare.c),
// and kept there for a while (a kept set, read by the bridge). This file has no window: the
// checks before preparing, the helper's runs and what they print, the kept sets. The window's
// side is the core's (core/prepare_tab.c, drawn by app/Shell/ShellTabs.swift); the watcher's
// check when rekordbox opens is in Watcher.swift.
// The decisions: at most 5 playlists, 75% of the limit, kept 10 days by default.
import Foundation

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

private let settingsAttribute = #"[ \t\r\n]+([^ \t\r\n=/<>\"']+)[ \t\r\n]*=[ \t\r\n]*(?:"([^"<]*)"|'([^'<]*)')"#
private let settingsAttributeRegex = try! NSRegularExpression(pattern: settingsAttribute)
private let settingsValueRegex = try! NSRegularExpression(pattern: "<VALUE((?:\(settingsAttribute))*)[ \t\r\n]*/>")

/// PartAnalysisQuality in rekordbox's settings file: <VALUE name="PartAnalysisQuality" val="0"/>.
func stemsQuality(_ file: String = rbSettingsFile) -> Int? {
    guard let d = FileManager.default.contents(atPath: file), d.count < 4 << 20 else { return nil }
    let s = String(decoding: d, as: UTF8.self) as NSString
    for element in settingsValueRegex.matches(in: s as String, range: NSRange(location: 0, length: s.length)) {
        var name: String?, value: String?
        for attribute in settingsAttributeRegex.matches(in: s as String, range: element.range(at: 1)) {
            let key = s.substring(with: attribute.range(at: 1))
            let quoted = attribute.range(at: attribute.range(at: 2).location == NSNotFound ? 3 : 2)
            if key == "name", name == nil { name = s.substring(with: quoted) }
            if key == "val", value == nil { value = s.substring(with: quoted) }
        }
        guard name == "PartAnalysisQuality" else { continue }
        guard let value else { return nil }
        let digits = value.utf8.prefix { (48...57).contains($0) }
        return Int32(String(decoding: digits, as: UTF8.self)).map(Int.init)
    }
    return nil
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

// MARK: - what the helper prints (one JSON object per line)

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

/// Validate the entire file as helper/kept.c's parse does before trusting its expiry.
private func keptContents(_ data: Data) -> (until: Date, entries: Int)? {
    let header = "rbstemsplus-kept 1\nuntil ".utf8
    return data.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) in
        guard bytes.starts(with: header) else { return nil }
        let first = header.count
        var i = first
        var seconds: Int64 = 0
        while i < bytes.count, i - first < 12, (48...57).contains(bytes[i]) {
            seconds = seconds * 10 + Int64(bytes[i] - 48)
            i += 1
        }
        guard i > first, i < bytes.count, bytes[i] == 10, bytes[first] != 48 || i - first == 1 else { return nil }
        i += 1
        let rowSize = 130
        guard (bytes.count - i) % rowSize == 0 else { return nil }
        func isHex(_ byte: UInt8) -> Bool { (48...57).contains(byte) || (97...102).contains(byte) }
        for row in stride(from: i, to: bytes.count, by: rowSize) {
            guard bytes[row + 64] == 32, bytes[row + 129] == 10,
                  bytes[row..<row + 64].allSatisfy(isHex),
                  bytes[row + 65..<row + 129].allSatisfy(isHex) else { return nil }
        }
        return (Date(timeIntervalSince1970: Double(seconds)), (bytes.count - i) / rowSize)
    }
}

/// The unexpired and unreadable kept sets in `dir`, by name.
func keptSets(_ dir: String = keptDir, now: Date = Date()) -> [KeptSet] {
    let names = (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []
    var out: [KeptSet] = []
    for n in names where n.hasSuffix(".kept") && !n.hasPrefix(".") {
        let name = String(n.dropLast(5))
        guard keptNameOK(name), fileType(dir + "/" + n) == S_IFREG else { continue }
        var set = KeptSet(name: name)
        if let d = FileManager.default.contents(atPath: dir + "/" + n), d.count <= 8 << 20,
           let parsed = keptContents(d) {
            set.until = parsed.until
            set.entries = parsed.entries
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
/// the set is gone: only ENOENT counts as gone, and an unlink that fails otherwise is a failure.
@discardableResult
func stopKeeping(_ name: String, dir: String = keptDir) -> Bool {
    guard keptNameOK(name) else { return false }
    var ok = true
    for f in [dir + "/" + name + ".kept", dir + "/" + name + ".manifest"] where fileType(f) == S_IFREG {
        if unlink(f) != 0 && errno != ENOENT { ok = false }
    }
    return ok && fileLookup(dir + "/" + name + ".kept") == .missing
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

/// Runs the helper to its end (an estimate, the playlists): its output and exit status. `job` can
/// stop it. Any thread but the main one.
func runPrepareHelper(_ args: [String], app: String = Bundle.main.bundlePath, timeout: TimeInterval = 300, job: HelperJob? = nil) -> (status: Int32, out: String, err: String) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: prepareHelper(app))
    p.arguments = args
    let out = Pipe(), err = Pipe()
    p.standardOutput = out; p.standardError = err; p.standardInput = FileHandle.nullDevice
    do { try p.run() } catch { return (-1, "", "the helper can't start: \(error.localizedDescription)") }
    job?.attach(p)
    var o = Data(), e = Data()
    let g = DispatchGroup()
    g.enter(); DispatchQueue.global().async { o = out.fileHandleForReading.readDataToEndOfFile(); g.leave() }
    g.enter(); DispatchQueue.global().async { e = err.fileHandleForReading.readDataToEndOfFile(); g.leave() }
    if g.wait(timeout: .now() + timeout) == .timedOut { p.terminate(); g.wait() }
    p.waitUntilExit()
    return (p.terminationStatus, String(decoding: o, as: UTF8.self), String(decoding: e, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))
}

// MARK: - words
