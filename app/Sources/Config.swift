// config.ini in ~/Library/Application Support/rbstemsplus: the Stems Cache settings the bridge
// reads when rekordbox starts, written by Settings… (or by hand), and rekordbox_model, which the
// app sets when this account turns Stems Cache on or off: with it, the bridge also saves the
// stems of rekordbox's own model in this account. Read with the bridge's rules:
// key=value lines, '#' starts a comment line, keys at the top or in [cache] (other sections are
// someone else's), keys and section names ignore case, a value is quoted or ends at the first
// space or '#'. Unknown keys are ignored; a bad or out-of-range value is ignored with a log line.
import Foundation

struct Config {
    var enabled = true
    var rekordboxModel = false    // Stems Cache also saves rekordbox's own model's stems in this account
    var maxGB = 20.0          // Stems Cache's size limit
    var maxDays = 60.0        // stems unused for this long are removed
}

let maxGBRange = 0.1...100_000.0
let maxDaysRange = 1.0...36_500.0

/// The value part of a config line: quoted, or up to the first space or '#'.
private func configValue(_ s: Substring) -> String {
    let v = s.drop { $0 == " " || $0 == "\t" }
    if let q = v.first, q == "\"" || q == "'", let end = v.dropFirst().firstIndex(of: q) { return String(v[v.index(after: v.startIndex)..<end]) }
    return String(v.prefix { !" \t\r#".contains($0) })
}

/// Whether a line is a key of ours, and which: (key, value) in lower case, or nil.
private func configLines(_ text: String) -> [(line: String, key: String?, value: String)] {
    var ours = true
    return text.components(separatedBy: "\n").map { line in
        var s = Substring(line)
        if s.hasPrefix("\u{FEFF}") { s = s.dropFirst() }
        s = s.drop { $0 == " " || $0 == "\t" }
        if s.hasPrefix("#") || s.isEmpty { return (line, nil, "") }
        if s.hasPrefix("[") {
            let name = s.dropFirst().prefix { $0 != "]" }.trimmingCharacters(in: .whitespaces)
            ours = name.lowercased() == "cache"
            return (line, nil, "")
        }
        guard ours, let eq = s.firstIndex(of: "=") else { return (line, nil, "") }
        let key = s[..<eq].trimmingCharacters(in: .whitespaces).lowercased()
        return (line, key, configValue(s[s.index(after: eq)...]))
    }
}

/// Reads config.ini (the defaults if it doesn't exist). `log` gets a line per ignored value.
func readConfig(_ log: (String) -> Void = { _ in }, file: String = configFile) -> Config {
    var c = Config()
    let text = (try? String(contentsOfFile: file, encoding: .utf8)) ?? ""
    for l in configLines(text) {
        switch l.key {
        case "enabled", "rekordbox_model":
            let on: Bool
            switch l.value.lowercased() {
            case "1", "yes", "on", "true": on = true
            case "0", "no", "off", "false": on = false
            default: log("config.ini: ignored \(l.key!)=\(l.value) (allowed 1 or 0)"); continue
            }
            if l.key == "enabled" { c.enabled = on } else { c.rekordboxModel = on }
        case "max_gb", "max_days":
            let gb = l.key == "max_gb", range = gb ? maxGBRange : maxDaysRange
            if let x = Double(l.value), range.contains(x) { if gb { c.maxGB = x } else { c.maxDays = x } }
            else { log("config.ini: ignored \(l.key!)=\(l.value) (allowed \(configNumber(range.lowerBound)) to \(configNumber(range.upperBound)))") }
        default: break
        }
    }
    return c
}

/// Whether config.ini sets this key (one of ours, at the top or in [cache]), whatever its value.
func configSets(_ key: String, file: String = configFile) -> Bool {
    configLines((try? String(contentsOfFile: file, encoding: .utf8)) ?? "").contains { $0.key == key }
}

/// A number as config.ini and the Settings sheet show it: "20", "0.5".
func configNumber(_ x: Double) -> String { x == x.rounded() ? String(Int(x)) : String(x) }

/// Writes max_gb and max_days into config.ini, keeping every other line as it is (writeConfigValues).
@discardableResult
func writeConfig(maxGB: Double, maxDays: Double) -> Bool {
    writeConfigValues([("max_gb", configNumber(maxGB)), ("max_days", configNumber(maxDays))])
}

/// Turns saving rekordbox's own model's stems on or off for this account (rekordbox_model).
@discardableResult
func writeRekordboxModel(_ on: Bool) -> Bool { writeConfigValues([("rekordbox_model", on ? "1" : "0")]) }

/// Writes these keys into config.ini, keeping every other line (comments, other keys and
/// sections) as it is: each key is replaced where it is (every line of it), or added at the top.
@discardableResult
func writeConfigValues(_ values: [(key: String, value: String)], file: String = configFile) -> Bool {
    let text = (try? String(contentsOfFile: file, encoding: .utf8))
        ?? "# RB Stems Plus settings (RB Stems Plus › Settings…), read by Stems Cache when rekordbox starts.\n"
    var lines: [String] = [], done = Set<String>()
    for l in configLines(text) {
        if let k = l.key, let v = values.first(where: { $0.key == k }) { lines.append("\(k)=\(v.value)"); done.insert(k) }
        else { lines.append(l.line) }
    }
    // missing keys go before the first [section], so they are top-level keys
    let at = lines.firstIndex { $0.trimmingCharacters(in: .whitespaces).hasPrefix("[") } ?? lines.count
    let add = values.filter { !done.contains($0.key) }.map { "\($0.key)=\($0.value)" }
    let insertAt = at == lines.count && lines.last == "" ? lines.count - 1 : at       // before a final newline
    lines.insert(contentsOf: add, at: insertAt)
    var out = lines.joined(separator: "\n")
    if !out.hasSuffix("\n") { out += "\n" }
    try? FileManager.default.createDirectory(atPath: (file as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
    return (try? out.write(toFile: file, atomically: true, encoding: .utf8)) != nil
}
