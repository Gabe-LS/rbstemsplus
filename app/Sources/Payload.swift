// The payload: what the app downloads from the release, described by the release's payload.json
// (versions, file names, sha256 and size, the compatible rekordbox and STEMS Engine versions).
// The files go to ~/Library/Application Support/rbstemsplus/payload/<payload version>/
// {model.onnx,bridge.dylib}; payload.json itself is kept next to those folders as the last good
// copy, with its signature (payload.json.sig), so an install works offline once its files are here.
//
// payload.json counts only with a valid signature by a compiled-in key (Signing.swift), checked
// when it is downloaded and again every time the saved copy is read; one older than the newest
// accepted on this Mac, or than the app itself, is refused. An unsigned or invalid saved copy
// (1.0 saved one without a signature) is ignored, and the next download replaces it.
//
// A payload.json with a key given twice in any object is refused, signed or not: JSON readers
// disagree on which one counts (Manifest).
//
// Every file is checked against payload.json's sha256 (and the model's size) before it is used.
// A damaged download is fetched once more from scratch, then refused. Nothing is changed in
// rekordbox until every file an action needs is here and verified.
import Foundation
import CryptoKit

/// payload.json. Unknown keys are ignored, so a later release can add some (refusing them would
/// strand every older app the day one is added). A key twice in any object, or a key with an
/// escape or outside printable ASCII, is refused (plainJSONKeys). The keys are matched exactly,
/// as written in the file (no conversion that could make two keys one).
struct Manifest: Codable {
    struct App: Codable { var version: String; var zip: String; var sha256: String }
    struct File: Codable { var file: String; var sha256: String; var size: Int? }
    struct Compatible: Codable {
        var rekordbox: [String]; var ortVersionPrefix: String; var stemsEngine: [String]
        private enum CodingKeys: String, CodingKey { case rekordbox, ortVersionPrefix = "ort_version_prefix", stemsEngine = "stems_engine" }
    }
    var payloadVersion: String
    var app: App
    var bridge: File
    var model: File
    var compatible: Compatible
    /// The checksums of the models earlier releases installed: still ours (Models.swift).
    var previousModels: [String]?
    /// The compiled-in key that signed this payload.json ("release", "backup", "test"), or nil if
    /// it wasn't checked. Never read from the file: only checkManifest below sets it.
    fileprivate(set) var signedBy: String? = nil

    private enum CodingKeys: String, CodingKey {
        case payloadVersion = "payload_version", app, bridge, model, compatible, previousModels = "previous_models"
    }

    var dir: String { payloadRoot + "/" + payloadVersion }
    var modelPath: String { dir + "/model.onnx" }
    var bridgePath: String { dir + "/bridge.dylib" }

    /// The values that become folder names, URLs or checks must be plain: a version or file name
    /// made only of letters, digits, dots, dashes and underscores; checksums (previous_models too)
    /// of 64 hex digits.
    var valid: Bool {
        let plain: (String) -> Bool = { s in
            !s.isEmpty && s != "." && s != ".." && s.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || "._-".contains($0)) }
        }
        return [payloadVersion, app.version, app.zip, bridge.file, model.file].allSatisfy(plain)
            && ([app.sha256, bridge.sha256, model.sha256] + (previousModels ?? [])).allSatisfy(isSHA256)
            && (model.size ?? 0) > 0
    }
}

let manifestFile = payloadRoot + "/payload.json"
let manifestSignatureFile = manifestFile + ".sig"
/// The newest payload_version accepted on this Mac: an older payload.json is refused, even when
/// signed (an old release can't be served again to undo a fix). A process of this account
/// could delete this file; it still couldn't forge a signature.
let payloadFloorFile = support + "/payload-version"

/// The compatibility list used until a payload.json has been downloaded (1.0.0's).
let builtInCompatible = Manifest.Compatible(rekordbox: ["7.2.17", "7.2.18", "7.2.19"], ortVersionPrefix: "1.18.", stemsEngine: ["0002"])

/// Parses payload.json, without checking its signature: only checkManifest uses it for a
/// payload.json that counts. Its keys must pass plainJSONKeys first.
func decodeManifest(_ data: Data) -> Manifest? {
    guard plainJSONKeys(data), let m = try? JSONDecoder().decode(Manifest.self, from: data), m.valid else { return nil }
    return m
}

/// Whether `data` is one JSON object in which no object, at any level, has the same key twice,
/// and every key is printable ASCII without escapes (so a key's bytes are its value, whatever
/// reads it). JSON readers disagree about a key given twice: JSONDecoder takes the first, plutil
/// (the bootstrap's) the last; with none, the app and the bootstrap read the same values. The
/// bootstrap's plain_json is the same check. Values are only skipped here: JSONDecoder checks them.
func plainJSONKeys(_ data: Data) -> Bool {
    let b = [UInt8](data)
    var i = 0
    func ws() { while i < b.count, b[i] == 0x20 || b[i] == 0x09 || b[i] == 0x0a || b[i] == 0x0d { i += 1 } }
    func at(_ c: UInt8) -> Bool { i < b.count && b[i] == c }
    /// A string's bytes between its quotes (escapes skipped, not decoded), or nil if malformed.
    func string() -> ArraySlice<UInt8>? {
        guard at(0x22) else { return nil }
        i += 1
        let start = i
        while i < b.count {
            switch b[i] {
            case 0x22: i += 1; return b[start..<(i - 1)]
            case 0x5c: i += 2
            case 0..<0x20: return nil
            default: i += 1
            }
        }
        return nil
    }
    func value(_ depth: Int) -> Bool {
        ws()
        guard depth < 32, i < b.count else { return false }
        switch b[i] {
        case 0x7b:                                                     // {
            i += 1; ws()
            if at(0x7d) { i += 1; return true }
            var keys = Set<ArraySlice<UInt8>>()
            while true {
                ws()
                guard let k = string(), !k.isEmpty, k.allSatisfy({ $0 >= 0x20 && $0 < 0x7f && $0 != 0x5c }),
                      keys.insert(k).inserted else { return false }
                ws()
                guard at(0x3a) else { return false }
                i += 1
                guard value(depth + 1) else { return false }
                ws()
                if at(0x2c) { i += 1; continue }
                if at(0x7d) { i += 1; return true }
                return false
            }
        case 0x5b:                                                     // [
            i += 1; ws()
            if at(0x5d) { i += 1; return true }
            while true {
                guard value(depth + 1) else { return false }
                ws()
                if at(0x2c) { i += 1; continue }
                if at(0x5d) { i += 1; return true }
                return false
            }
        case 0x22: return string() != nil
        default:                                                       // a number, true, false or null
            let start = i
            while i < b.count, "+-.0123456789Eaeflnrstu".utf8.contains(b[i]) { i += 1 }
            return i > start
        }
    }
    ws()
    guard at(0x7b), value(0) else { return false }
    ws()
    return i == b.count
}

/// Why a payload.json was refused (the app's log).
enum PayloadRefusal: Error, Equatable, CustomStringConvertible {
    case noSignature, badSignature, notValid, notSaved
    case older(String, than: String)
    var description: String {
        switch self {
        case .noSignature: return "it has no signature"
        case .badSignature: return "its signature doesn't verify with RB Stems Plus's keys"
        case .notValid: return "it isn't a valid payload description"
        case .notSaved: return "it couldn't be saved"
        case .older(let v, let f): return "it is payload \(v), older than \(f), the oldest this app accepts now"
        }
    }
}

/// Where payload.json, its signature and the newest accepted version are kept, the keys it is
/// checked with, and the oldest payload_version accepted even when none was ever saved (the
/// tests use a temporary folder, their own keys and their own minimum).
struct PayloadFiles {
    var json = manifestFile
    var signature = manifestSignatureFile
    var floor = payloadFloorFile
    var keys = trustedKeys
    var minimum = payloadMinimum
}

/// The oldest payload_version this app accepts, also when no payload.json was ever accepted or
/// the record of the newest one is gone: its own version (compiledVersion, from VERSION, written
/// in by scripts/build-app.sh), as each release's payload_version is its VERSION. A signed older
/// release can't be served to it to undo a fix. scripts/bootstrap.sh has the same floor.
let payloadMinimum = compiledVersion

/// The newest payload_version accepted, or nil if none was (or the file isn't one).
func payloadFloor(_ f: PayloadFiles = PayloadFiles()) -> String? {
    guard let d = FileManager.default.contents(atPath: f.floor) else { return nil }
    let v = String(decoding: d, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
    return !v.isEmpty && v.count < 64 && v.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || "._-".contains($0)) }) ? v : nil
}

/// Whether payload version `a` is older than `b` ("1.0.9" < "1.0.10").
func olderPayload(_ a: String, than b: String) -> Bool { a.compare(b, options: .numeric) == .orderedAscending }

/// The oldest payload_version accepted now: the newest one accepted on this Mac, or the app's
/// minimum if that is newer (or none was accepted).
func acceptedFloor(_ f: PayloadFiles = PayloadFiles()) -> String? {
    let minimum = f.minimum.isEmpty ? nil : f.minimum
    guard let saved = payloadFloor(f) else { return minimum }
    guard let m = minimum else { return saved }
    return olderPayload(saved, than: m) ? m : saved
}

/// payload.json's bytes, checked: signed by one of `keys`, valid, and not older than `floor`.
/// The only way to a Manifest that counts (signedBy set).
func checkManifest(_ data: Data, signature: Data?, keys: [TrustedKey], floor: String?) -> Result<Manifest, PayloadRefusal> {
    guard let sig = signature, !sig.isEmpty else { return .failure(.noSignature) }
    guard let who = signer(of: data, signature: sig, keys: keys) else { return .failure(.badSignature) }
    guard var m = decodeManifest(data) else { return .failure(.notValid) }
    if let f = floor, olderPayload(m.payloadVersion, than: f) { return .failure(.older(m.payloadVersion, than: f)) }
    m.signedBy = who
    return .success(m)
}

/// Where a saved payload.json that was ignored is reported: the app's log, once per reason and
/// process (it is read often). The tests collect them instead.
var payloadRefused: (String) -> Void = { s in
    refusedLock.lock(); defer { refusedLock.unlock() }
    guard s != lastRefused else { return }
    lastRefused = s
    appendLog(logPath, s)
}
private let refusedLock = NSLock()
private var lastRefused = ""

/// The last good payload.json, checked again now: nil if none was ever downloaded, or if the
/// saved copy has no valid signature or is older than the newest accepted (it is then ignored
/// until the next download replaces it).
func savedManifest(_ f: PayloadFiles = PayloadFiles()) -> Manifest? {
    guard let data = FileManager.default.contents(atPath: f.json) else { return nil }
    switch checkManifest(data, signature: FileManager.default.contents(atPath: f.signature), keys: f.keys, floor: acceptedFloor(f)) {
    case .success(let m): return m
    case .failure(let why): payloadRefused("saved payload.json ignored: \(why)"); return nil
    }
}

/// How long ago payload.json was last downloaded (its file is replaced at each download).
func manifestAge() -> TimeInterval? {
    (try? FileManager.default.attributesOfItem(atPath: manifestFile)[.modificationDate] as? Date).map { -$0.timeIntervalSinceNow }
}

/// Checks a downloaded payload.json and its signature and, if they count, saves both as the last
/// good copy and raises the newest accepted version. Anything refused changes nothing.
func acceptManifest(_ data: Data, signature: Data, _ f: PayloadFiles = PayloadFiles()) -> Result<Manifest, PayloadRefusal> {
    let floor = payloadFloor(f)
    let r = checkManifest(data, signature: signature, keys: f.keys, floor: acceptedFloor(f))
    guard case .success(let m) = r else { return r }
    // the signature first: if this stops between the two, the saved pair doesn't verify and is
    // ignored until the next download
    guard writeAtomically(signature, to: f.signature), writeAtomically(data, to: f.json) else { return .failure(.notSaved) }
    if floor.map({ olderPayload($0, than: m.payloadVersion) }) ?? true {
        writeAtomically(Data((m.payloadVersion + "\n").utf8), to: f.floor)
    }
    return r
}

/// What downloading payload.json gave: a payload.json that counts, no connection (or no room to
/// save it), or one that couldn't be verified (unsigned, a bad signature, invalid, or older).
enum ManifestFetch { case ok(Manifest), offline, unverified }

/// The alert for .unverified, under the title "Download failed".
let unverifiedDownloadMessage = "The download couldn't be verified. Nothing was changed. Try again later."

/// Downloads payload.json and payload.json.sig and keeps them as the last good copy if they
/// count. Otherwise the last good copy stays, and the log says why.
func fetchManifestChecked(_ log: (String) -> Void) -> ManifestFetch {
    try? FileManager.default.createDirectory(atPath: payloadRoot, withIntermediateDirectories: true)
    let id = UUID().uuidString                       // the update check may run alongside an install
    let part = payloadRoot + "/.payload.json.\(id).part", sigPart = payloadRoot + "/.payload.json.sig.\(id).part"
    defer { safeRemove(part); safeRemove(sigPart) }
    for (name, dest) in [("payload.json", part), ("payload.json.sig", sigPart)] {
        let r = runTool("/usr/bin/curl", curlBase + ["-fsSL", "--connect-timeout", "15", "--max-time", "60", "-o", dest, downloadBase + name])
        guard r.0 == 0 else {
            log("\(name): couldn't download (curl \(r.0)) \(r.1)")
            // payload.json there but no signature published (curl 22: an HTTP error)
            return name == "payload.json.sig" && r.0 == 22 ? .unverified : .offline
        }
    }
    let data = FileManager.default.contents(atPath: part) ?? Data(), sig = FileManager.default.contents(atPath: sigPart) ?? Data()
    switch acceptManifest(data, signature: sig) {
    case .success(let m):
        log("payload.json: payload \(m.payloadVersion), app \(m.app.version), rekordbox \(m.compatible.rekordbox.joined(separator: "/")), engine \(m.compatible.stemsEngine.joined(separator: "/")), signed with the \(m.signedBy ?? "?") key")
        return .ok(m)
    case .failure(let why):
        log("payload.json: REFUSED, \(why): nothing changed, the last good copy stays")
        return why == .notSaved ? .offline : .unverified
    }
}

/// fetchManifestChecked's payload.json, or nil (offline or not verified).
func fetchManifest(_ log: (String) -> Void) -> Manifest? {
    if case .ok(let m) = fetchManifestChecked(log) { return m }
    return nil
}

// MARK: - checksums

/// A file's SHA-256 in hex, read in 1 MB pieces (the model is 300 MB). nil if unreadable.
func sha256(_ path: String) -> String? {
    guard let h = FileHandle(forReadingAtPath: path) else { return nil }
    defer { try? h.close() }
    var hasher = SHA256()
    while true {
        let more: Bool = autoreleasepool {
            let d = h.readData(ofLength: 1 << 20)
            hasher.update(data: d)
            return !d.isEmpty
        }
        if !more { break }
    }
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
}

func sha256(data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }

func fileSize(_ path: String) -> Int? { (try? FileManager.default.attributesOfItem(atPath: path))?[.size] as? Int }

/// Whether a file has this size (when given) and this SHA-256.
func verified(_ path: String, sha: String, size: Int?) -> Bool {
    guard let s = fileSize(path), size == nil || s == size else { return false }
    return sha256(path) == sha
}

// MARK: - downloads

/// The first curl arguments, always: -q ignores ~/.curlrc (which could turn off certificate
/// checks or add a proxy), and only HTTPS is allowed, also after redirects (Paths.swift).
let curlBase = ["-q", "--proto", curlProtocols, "--proto-redir", curlProtocols]

/// noSpace: curl couldn't write the file (exit 23), almost always a full disk.
enum Fetched { case ok, offline, damaged, noSpace }

/// Makes sure `dest` holds the release asset `name` with this sha256 (and size): kept if it is
/// already there, reused from an older payload folder, or downloaded and verified. A partial
/// download left by an earlier try is resumed. `status` gets the line to show ("Downloading the
/// Demucs v4 model… 42%"); `what` names the file for the user.
func fetchAsset(_ name: String, to dest: String, sha: String, size: Int?, what: String,
                status: (String) -> Void, log: (String) -> Void) -> Fetched {
    let fm = FileManager.default
    let dir = (dest as NSString).deletingLastPathComponent
    try? fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
    status("Checking \(what)…")
    if verified(dest, sha: sha, size: size) { return .ok }
    safeRemove(dest)
    // the same file in another payload folder (e.g. a payload update that kept the model)
    for other in (try? fm.contentsOfDirectory(atPath: payloadRoot)) ?? [] where payloadRoot + "/" + other != dir {
        let candidate = payloadRoot + "/" + other + "/" + (dest as NSString).lastPathComponent
        guard verified(candidate, sha: sha, size: size) else { continue }
        if (try? fm.linkItem(atPath: candidate, toPath: dest)) != nil || (try? fm.copyItem(atPath: candidate, toPath: dest)) != nil {
            log("\(name): reused from payload \(other)"); return .ok
        }
    }
    let part = dest + ".part"
    for attempt in 1...2 {
        let have = fileSize(part) ?? 0
        let complete = attempt == 1 && size != nil && have == size!          // finished by an earlier try
        let resume = attempt == 1 && have > 0 && !complete && (size == nil || have < size!)
        if !complete && !resume { safeRemove(part) }
        if !complete {
            status("Downloading \(what)…")
            // give up on a stalled connection (under 1 KB/s for a minute), not on a slow one
            let p = child("/usr/bin/curl", curlBase + ["-fL", "-sS", "--connect-timeout", "15", "--retry", "2", "--speed-limit", "1024", "--speed-time", "60",
                                                       "-o", part] + (resume ? ["-C", "-"] : []) + [downloadBase + name])
            let err = Pipe(); p.standardError = err; p.standardOutput = FileHandle.nullDevice; p.standardInput = FileHandle.nullDevice
            do { try p.run() } catch { log("\(name): curl didn't start: \(error)"); return .offline }
            while p.isRunning {
                usleep(300_000)
                if let s = size, s > 0 { status("Downloading \(what)… \(min(99, (fileSize(part) ?? 0) * 100 / s))%") }
            }
            p.waitUntilExit()
            let msg = String(decoding: err.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            if p.terminationStatus != 0 {
                log("\(name): download failed (curl \(p.terminationStatus)\(resume ? ", resuming" : "")) \(msg)")
                if p.terminationStatus == 23 { return .noSpace }  // write error: the disk is full
                if resume { continue }                          // e.g. the server refused to resume: from scratch
                return .offline                                 // a partial file stays for the next try
            }
        }
        status("Checking \(what)…")
        if verified(part, sha: sha, size: size), rename(part, dest) == 0 { log("\(name): downloaded and verified"); return .ok }
        log("\(name): checksum or size didn't match (\(fileSize(part) ?? 0) bytes): \(attempt == 1 ? "downloading again" : "refused")")
        safeRemove(part)
    }
    return .damaged
}

/// Deletes the folders of other payload versions, once this one's files are here and verified.
func prunePayloads(keep m: Manifest) {
    let fm = FileManager.default
    for other in (try? fm.contentsOfDirectory(atPath: payloadRoot)) ?? [] where other != m.payloadVersion {
        var isDir: ObjCBool = false
        if fm.fileExists(atPath: payloadRoot + "/" + other, isDirectory: &isDir), isDir.boolValue {
            safeRemove(payloadRoot + "/" + other)
        }
    }
}
