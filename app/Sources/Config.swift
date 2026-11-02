// config.ini in ~/Library/Application Support/rbstemsplus: the Stems Cache settings the bridge
// reads when rekordbox starts, written by Settings… (or by hand), and rekordbox_model, which the
// app sets when this account turns Stems Cache on or off: with it, the bridge also saves the
// stems of rekordbox's own model in this account. cache_dir and cache_id say where the stems are
// saved when that isn't this Mac (Location.swift).
//
// One grammar, the same in the bridge (read_config in bridge/rbstems_bridge.c), read as bytes:
// - the whole file: over 64 KB, a NUL byte or invalid UTF-8 anywhere make it unreadable (the
//   bridge then saves nothing; the app says so and never writes over it); missing: the defaults;
// - a UTF-8 byte order mark only at offset 0; lines end at "\n" and hold at most 1024 bytes
//   (a CR counts);
// - leading spaces, tabs and CRs are skipped; an empty line or one starting with '#' is a comment;
// - "[name]": a section; ours at the top of the file and in [cache] (the name between '[' and the
//   first ']', spaces and tabs around it ignored, case ignored); other sections are someone
//   else's; a section line over the limit leaves an unknown section;
// - key=value: the key is everything before the first '=', trimmed, case ignored; the value is
//   quoted ("..." or '...', up to the same quote) or ends at the first space, tab, CR or '#'. A
//   line over the limit keeps its key but loses its value.
// Unknown keys are ignored; a bad or out-of-range value is ignored with a log line.
import Foundation

struct Config {
    var enabled = true
    var rekordboxModel = false    // Stems Cache also saves rekordbox's own model's stems in this account
    var maxGB = 20.0          // Stems Cache's size limit
    var maxDays = 60.0        // stems unused for this long are removed
}

let maxGBRange = 0.1...100_000.0
let maxDaysRange = 1.0...36_500.0
/// The grammar's limits, the bridge's too.
let configMaxBytes = 65_536
let configLineMax = 1024

/// Where config.ini says the stems are saved.
enum ConfigLocation: Equatable {
    case thisMac
    /// cache_dir (our folder, by the path rules) and cache_id (the marker's id, as written)
    case chosen(dir: String, id: String)
    /// a location key is there but the location can't be used: no cache (never this Mac); why
    case malformed(String)
}

/// config.ini as read: its settings, the location, or why it can't be read.
struct ConfigState {
    var config = Config()
    var location = ConfigLocation.thisMac
    /// why the file can't be read (it exists): nil when it was read or isn't there
    var unreadable: String?
}

/// The file's bytes, or why not. nil bytes and nil why: it doesn't exist.
func readConfigBytes(_ file: String) -> (bytes: [UInt8]?, unreadable: String?) {
    let fd = open(file, O_RDONLY | O_CLOEXEC)
    if fd < 0 { let e = errno; return e == ENOENT ? (nil, nil) : (nil, String(cString: strerror(e))) }
    defer { close(fd) }
    var out: [UInt8] = [], buf = [UInt8](repeating: 0, count: 16384)
    while true {
        let n = read(fd, &buf, buf.count)
        if n < 0 { return (nil, String(cString: strerror(errno))) }
        if n == 0 { break }
        out += buf[0..<n]
        if out.count > configMaxBytes { return (nil, "over 64 KB") }
    }
    if out.contains(0) { return (nil, "a NUL byte") }
    if !isStrictUTF8(out) { return (nil, "not UTF-8") }
    return (out, nil)
}

/// Whether bytes are valid UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF):
/// decoding replaces anything invalid, so the round trip differs.
func isStrictUTF8(_ b: [UInt8]) -> Bool { Array(String(decoding: b, as: UTF8.self).utf8) == b }

/// One line of config.ini by the grammar.
struct ConfigLine {
    var text: String                 // as it is in the file, without "\n" (the writer keeps it)
    var section = false              // a [section] line
    var ours = false                 // in our scope (the line's own scope, for a key line)
    var key: String?                 // a key line's key, lower case, whatever the scope
    var value: String?               // nil: no value (over-long line)
    var overLong = false
}

private func isBlank(_ c: UInt8) -> Bool { c == 0x20 || c == 0x09 || c == 0x0D }

private func trimmed(_ s: ArraySlice<UInt8>, cr: Bool = true) -> ArraySlice<UInt8> {
    var s = s
    while let c = s.first, c == 0x20 || c == 0x09 || (cr && c == 0x0D) { s = s.dropFirst() }
    while let c = s.last, c == 0x20 || c == 0x09 || (cr && c == 0x0D) { s = s.dropLast() }
    return s
}

private func asciiLower(_ s: ArraySlice<UInt8>) -> String {
    String(decoding: s.map { $0 >= 0x41 && $0 <= 0x5A ? $0 + 32 : $0 }, as: UTF8.self)
}

/// The value part of a key line: quoted, or up to the first space, tab, CR or '#'.
private func configValue(_ s: ArraySlice<UInt8>) -> String {
    let v = s.drop { $0 == 0x20 || $0 == 0x09 }
    if let q = v.first, q == 0x22 || q == 0x27, let end = v.dropFirst().firstIndex(of: q) {
        return String(decoding: v[v.index(after: v.startIndex)..<end], as: UTF8.self)
    }
    return String(decoding: v.prefix { !($0 == 0x20 || $0 == 0x09 || $0 == 0x0D || $0 == 0x23) }, as: UTF8.self)
}

/// The lines of a readable file, and whether a section line was over the limit.
func configLines(_ bytes: [UInt8]) -> (lines: [ConfigLine], overlongSection: Bool) {
    var ours = true, overlongSection = false, out: [ConfigLine] = []
    var all = bytes.split(separator: 0x0A, omittingEmptySubsequences: false)
    if bytes.isEmpty { all = [] }
    for (i, raw) in all.enumerated() {
        var line = ConfigLine(text: String(decoding: raw, as: UTF8.self))
        var s = raw
        if i == 0 && s.starts(with: [0xEF, 0xBB, 0xBF]) { s = s.dropFirst(3) }     // a BOM, at offset 0 only
        line.overLong = s.count > configLineMax
        s = s.drop(while: isBlank)
        if s.isEmpty || s.first == 0x23 { out.append(line); continue }                // a comment
        if s.first == 0x5B {                                                         // [section]
            line.section = true
            if line.overLong { ours = false; overlongSection = true }
            else if let close = s.firstIndex(of: 0x5D) { ours = asciiLower(trimmed(s[s.index(after: s.startIndex)..<close], cr: false)) == "cache" }
            else { ours = false }
            out.append(line); continue
        }
        line.ours = ours
        guard let eq = s.firstIndex(of: 0x3D) else { out.append(line); continue }
        line.key = asciiLower(trimmed(s[s.startIndex..<eq]))
        if !line.overLong { line.value = configValue(s[s.index(after: eq)...]) }
        out.append(line)
    }
    return (out, overlongSection)
}

/// Reads config.ini by the grammar. `log` gets a line per ignored value.
func readConfigState(_ log: (String) -> Void = { _ in }, file: String = configFile) -> ConfigState {
    var st = ConfigState()
    let r = readConfigBytes(file)
    if let why = r.unreadable { st.unreadable = why; st.location = .malformed("config.ini can't be read (\(why))"); return st }
    guard let bytes = r.bytes else { return st }
    let parsed = configLines(bytes)
    var dirs: [String?] = [], ids: [String?] = [], anyLocationKey = false
    for l in parsed.lines {
        guard let key = l.key else { continue }
        if key == "cache_dir" || key == "cache_id" { anyLocationKey = true }
        guard l.ours else { continue }
        guard let value = l.value else {
            if key == "cache_dir" { dirs.append(nil) } else if key == "cache_id" { ids.append(nil) }
            else if ["enabled", "rekordbox_model", "max_gb", "max_days"].contains(key) { log("config.ini: ignored \(key) (a line over \(configLineMax) bytes)") }
            continue
        }
        switch key {
        case "enabled", "rekordbox_model":
            let on: Bool
            switch value.lowercased() {
            case "1", "yes", "on", "true": on = true
            case "0", "no", "off", "false": on = false
            default: log("config.ini: ignored \(key)=\(value) (allowed 1 or 0)"); continue
            }
            if key == "enabled" { st.config.enabled = on } else { st.config.rekordboxModel = on }
        case "max_gb", "max_days":
            let gb = key == "max_gb", range = gb ? maxGBRange : maxDaysRange
            if let x = Double(value), range.contains(x) { if gb { st.config.maxGB = x } else { st.config.maxDays = x } }
            else { log("config.ini: ignored \(key)=\(value) (allowed \(configNumber(range.lowerBound)) to \(configNumber(range.upperBound)))") }
        case "cache_dir": dirs.append(value)
        case "cache_id": ids.append(value)
        default: break
        }
    }
    st.location = configLocation(dirs: dirs, ids: ids, overlongSection: parsed.overlongSection, anyLocationKey: anyLocationKey)
    if case .malformed(let why) = st.location { log("config.ini: where stems are saved is set wrongly (\(why)): Stems Cache saves nothing") }
    return st
}

/// The location from the location lines in our scope (nil: an over-long line). A section line
/// over the limit with a location key anywhere in the file: wrong (plan 3.1.4).
func configLocation(dirs: [String?], ids: [String?], overlongSection: Bool, anyLocationKey: Bool) -> ConfigLocation {
    // a section line over the limit leaves the scope of what follows unknown: with a location
    // key anywhere, the location can't be trusted
    if overlongSection && anyLocationKey { return .malformed("a section line over \(configLineMax) bytes") }
    if dirs.isEmpty && ids.isEmpty { return .thisMac }
    if dirs.isEmpty { return .malformed("cache_id without cache_dir") }
    if ids.isEmpty { return .malformed("cache_dir without cache_id") }
    if dirs.contains(nil) || ids.contains(nil) { return .malformed("a line over \(configLineMax) bytes") }
    let d = dirs.compactMap { $0 }, i = ids.compactMap { $0 }
    if Set(d).count > 1 { return .malformed("cache_dir set twice, differently") }
    if Set(i.map { $0.lowercased() }).count > 1 { return .malformed("cache_id set twice, differently") }
    if let why = cacheDirProblem(d[0]) { return .malformed("cache_dir: \(why)") }
    if !isUUID(i[0]) { return .malformed("cache_id isn't an id") }
    return .chosen(dir: d[0], id: i[0])
}

/// Reads config.ini's settings (the defaults if it doesn't exist or can't be read).
func readConfig(_ log: (String) -> Void = { _ in }, file: String = configFile) -> Config {
    readConfigState(log, file: file).config
}

/// Whether config.ini sets this key (one of ours, at the top or in [cache]), whatever its value.
func configSets(_ key: String, file: String = configFile) -> Bool {
    guard let b = readConfigBytes(file).bytes else { return false }
    return configLines(b).lines.contains { $0.ours && $0.key == key }
}

/// A number as config.ini and the Settings sheet show it: "20", "0.5".
func configNumber(_ x: Double) -> String { x == x.rounded() ? String(Int(x)) : String(x) }

/// Writes max_gb and max_days into config.ini, keeping every other line as it is (writeConfigValues).
@discardableResult
func writeConfig(maxGB: Double, maxDays: Double, file: String = configFile) -> Bool {
    writeConfigValues([("max_gb", configNumber(maxGB)), ("max_days", configNumber(maxDays))], file: file)
}

/// Turns saving rekordbox's own model's stems on or off for this account (rekordbox_model).
@discardableResult
func writeRekordboxModel(_ on: Bool) -> Bool { writeConfigValues([("rekordbox_model", on ? "1" : "0")]) }

/// Where the stems are saved: our folder `dir` (always double-quoted: the path rules keep '"'
/// out of it) and the marker's `id`; nil dir: this Mac (both keys removed).
@discardableResult
func writeCacheLocation(dir: String?, id: String?, file: String = configFile) -> Bool {
    // read back: the write counts only if both readers now see what was meant (Change… commits
    // on it and then deletes this Mac's stems)
    guard let dir = dir, let id = id else {
        return writeConfigValues([], remove: ["cache_dir", "cache_id"], file: file) && readConfigState(file: file).location == .thisMac
    }
    guard cacheDirProblem(dir) == nil, isUUID(id) else { return false }
    return writeConfigValues([("cache_dir", "\"" + dir + "\""), ("cache_id", id)], file: file)
        && readConfigState(file: file).location == .chosen(dir: dir, id: id)
}

/// Writes these keys into config.ini, keeping every other line (comments, other keys and
/// sections) as it is: each key is replaced where it is (every line of it, in our scope), or added
/// at the top; the keys in `remove` lose every line of theirs in our scope. Never over a file that
/// exists but can't be read: false, nothing written. A byte order mark stays at offset 0 (keys
/// added before the first line would otherwise push it onto a line, where it is part of the text).
/// Writing the location over a file with a section line over the limit (both readers then call
/// any location wrong): that line becomes "[unreadable-section]", whose keys are someone else's
/// as before, and location keys outside our scope go, so the location written is the one read.
@discardableResult
func writeConfigValues(_ values: [(key: String, value: String)], remove: [String] = [], file: String = configFile) -> Bool {
    let r = readConfigBytes(file)
    if r.unreadable != nil { return false }
    let locationKeys: Set<String> = ["cache_dir", "cache_id"]
    let touchesLocation = values.contains { locationKeys.contains($0.key) } || remove.contains { locationKeys.contains($0) }
    var lines: [String] = [], done = Set<String>(), firstSection: Int?, bom = false
    if let b = r.bytes {
        let parsed = configLines(b)
        for (i, l) in parsed.lines.enumerated() {
            var text = l.text
            if i == 0, text.unicodeScalars.first == "\u{FEFF}" { text = String(text.unicodeScalars.dropFirst()); bom = true }
            if l.section && firstSection == nil { firstSection = lines.count }
            if touchesLocation && parsed.overlongSection {
                if l.section && l.overLong { lines.append("[unreadable-section]"); continue }
                if !l.ours, let k = l.key, locationKeys.contains(k) { continue }
            }
            if l.ours, let k = l.key, remove.contains(k) { continue }
            if l.ours, let k = l.key, let v = values.first(where: { $0.key == k }) { lines.append("\(k)=\(v.value)"); done.insert(k) }
            else { lines.append(text) }
        }
    } else {
        lines = ["# RB Stems Plus settings (RB Stems Plus › Settings…), read by Stems Cache when rekordbox starts.", ""]
    }
    // missing keys go before the first [section], so they are top-level keys
    let at = firstSection ?? lines.count
    let add = values.filter { !done.contains($0.key) }.map { "\($0.key)=\($0.value)" }
    let insertAt = at == lines.count && lines.last == "" ? lines.count - 1 : at       // before a final newline
    lines.insert(contentsOf: add, at: insertAt)
    var out = (bom ? "\u{FEFF}" : "") + lines.joined(separator: "\n")
    if !out.hasSuffix("\n") { out += "\n" }
    try? FileManager.default.createDirectory(atPath: (file as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
    return (try? out.write(toFile: file, atomically: true, encoding: .utf8)) != nil
}
