// Compatibility gating: whether each feature can be installed on this Mac. The lists come from
// payload.json's "compatible" block (the built-in 1.0.0 list until one is downloaded).
// - Stems Plus: the STEMS Engine version (demucs3_ver.txt) is listed.
// - Stems Cache: rekordbox is installed, the user is an administrator, the rekordbox version is
//   listed, and Pioneer's ONNX Runtime is 1.18.x and validly signed by AlphaTheta. It works with
//   either stems model, so it doesn't need Stems Plus (nor a STEMS Engine yet: the light says
//   when it starts saving).
// Each reason is one line for the status line, naming the feature. Uninstalling is never gated.
import Foundation
import Security

func compatibleList() -> Manifest.Compatible { savedManifest()?.compatible ?? builtInCompatible }

/// Whether a version is listed: rekordbox's "7.2.19.0342" matches "7.2.19".
func listed(_ v: String, in list: [String]) -> Bool { !v.isEmpty && list.contains { v == $0 || v.hasPrefix($0 + ".") } }

/// "7.2.19.0342" → "7.2.19", for messages.
func shortVersion(_ v: String) -> String { v.split(separator: ".").prefix(3).joined(separator: ".") }

/// The end of every "not supported yet" reason. Opening the app downloads payload.json again
/// when a feature is off only for this reason (Update.swift), and a new list may allow it.
let checksAgain = " RB Stems Plus checks again each time you open it."

/// Whether a feature is off only because a version isn't listed yet.
func blockedByVersionList() -> Bool { [stemsPlusBlocked(), stemsCacheBlocked()].contains { $0?.hasSuffix(checksAgain) == true } }

func engineSupported(_ engine: String) -> Bool { compatibleList().stemsEngine.contains(engine) }
func rekordboxSupported(_ version: String) -> Bool { listed(version, in: compatibleList().rekordbox) }

/// Whether this user is an administrator (sudo refuses standard users, even with the right
/// password). Asked once per run.
let userIsAdmin: Bool = {
    let r = runTool("/usr/sbin/dseditgroup", ["-o", "checkmember", "-m", NSUserName(), "admin"])
    if r.0 == 0 { return true }
    if r.0 == 67 { return false }                       // "is NOT a member"
    // directory services didn't answer: the groups this process runs with
    return runTool("/usr/bin/id", ["-Gn"]).1.split(separator: " ").contains("admin")
}()

/// Why Stems Plus can't be installed on this Mac, or nil if it can.
func stemsPlusBlocked() -> String? {
    if !rekordboxInstalled() { return "rekordbox 7 isn't installed." }
    let engine = engineVersion()
    if engine.isEmpty { return "Open rekordbox and turn on STEMS once, so it downloads its STEMS Engine." }
    if !engineSupported(engine) { return "Stems Plus doesn't support STEMS Engine \(engine) yet." + checksAgain }
    return nil
}

/// Why Stems Cache can't be installed from a standard account (sudo refuses it).
let needsAdminLine = "Stems Cache needs an administrator account."

/// On a standard account with something of Stems Cache on this Mac (`cacheTraces`), why its
/// uninstall buttons (and, if this account chose it, Reinstall) are off; nil on an administrator
/// account, or with nothing to remove (needsAdminLine then says why it can't be installed).
func standardAccountLine(admin: Bool, cacheTraces: Bool, cacheChosen: Bool) -> String? {
    guard !admin, cacheTraces else { return nil }
    return cacheChosen ? "Reinstalling or removing Stems Cache needs an administrator account."
                       : "Removing Stems Cache needs an administrator account."
}

/// Why Stems Cache can't be installed on this Mac, or nil if it can.
func stemsCacheBlocked() -> String? {
    if !userIsAdmin { return needsAdminLine }
    if !rekordboxInstalled() { return "rekordbox 7 isn't installed." }
    let v = rekordboxVersion()
    if !rekordboxSupported(v) { return "Stems Cache doesn't support rekordbox \(shortVersion(v)) yet." + checksAgain }
    // Pioneer's library: in rekordbox, or (bridge already in) the root-owned copy the bridge loads
    let lib = stemsCachePresent() ? rootOrt : rbLib
    // the prototype's bridge (developer's Mac): its originals aren't where this app looks
    if stemsCachePresent() && !FileManager.default.fileExists(atPath: rootOrt) && FileManager.default.fileExists(atPath: prototypeRootDir) {
        return "Stems Cache from a test version is in rekordbox: reinstall rekordbox first."
    }
    if !FileManager.default.fileExists(atPath: lib) { return "Stems Cache can't find rekordbox's ONNX Runtime: reinstall rekordbox." }
    if !signedByPioneer(lib) { return "rekordbox's ONNX Runtime isn't the original: reinstall rekordbox." }
    let ort = dylibVersion(lib) ?? "unknown"
    if !ort.hasPrefix(compatibleList().ortVersionPrefix) {
        return "Stems Cache doesn't support ONNX Runtime \(ort) yet." + checksAgain
    }
    return nil
}

// MARK: - Pioneer's ONNX Runtime

private var signedCache: [String: Bool] = [:]           // "path size mtime" → result
private let signedLock = NSLock()

/// Whether a file has a valid signature (every architecture) from an Apple-issued Developer ID
/// certificate of AlphaTheta's team. Remembered per file version (about 60 ms for ORT).
func signedByPioneer(_ path: String) -> Bool {
    let a = try? FileManager.default.attributesOfItem(atPath: path)
    let key = "\(path) \(a?[.size] ?? 0) \((a?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0)"
    signedLock.lock(); let known = signedCache[key]; signedLock.unlock()
    if let k = known { return k }
    var ok = false
    var code: SecStaticCode?, req: SecRequirement?
    let requirement = "anchor apple generic and certificate leaf[subject.OU] = \"\(pioneerTeam)\""
    if SecStaticCodeCreateWithPath(URL(fileURLWithPath: path) as CFURL, [], &code) == errSecSuccess, let code = code,
       SecRequirementCreateWithString(requirement as CFString, [], &req) == errSecSuccess {
        ok = SecStaticCodeCheckValidity(code, SecCSFlags(rawValue: kSecCSCheckAllArchitectures), req) == errSecSuccess
    }
    signedLock.lock(); signedCache[key] = ok; signedLock.unlock()
    return ok
}

/// The version a library declares in its Mach-O header (LC_ID_DYLIB's current version, e.g.
/// "1.18.0"), the same in every architecture; nil if unreadable. Read directly: otool is only an
/// Xcode stub on a user's Mac.
func dylibVersion(_ path: String) -> String? {
    guard let h = FileHandle(forReadingAtPath: path) else { return nil }
    defer { try? h.close() }
    func read(_ offset: UInt64, _ count: Int) -> [UInt8]? {
        guard (try? h.seek(toOffset: offset)) != nil else { return nil }
        let d = h.readData(ofLength: count)
        return d.count == count ? [UInt8](d) : nil
    }
    func be32(_ b: [UInt8], _ o: Int) -> UInt64 { b[o..<o + 4].reduce(0) { $0 << 8 | UInt64($1) } }
    func le32(_ b: [UInt8], _ o: Int) -> UInt64 { b[o..<o + 4].reversed().reduce(0) { $0 << 8 | UInt64($1) } }
    guard let head = read(0, 8) else { return nil }
    var slices: [UInt64] = [0]
    let magic = be32(head, 0)
    if magic == 0xcafebabe || magic == 0xcafebabf {     // universal: 32- or 64-bit offsets
        let n = Int(be32(head, 4)), size = magic == 0xcafebabe ? 20 : 32
        guard n > 0, n < 16, let table = read(8, n * size) else { return nil }
        slices = (0..<n).map { i in
            magic == 0xcafebabe ? be32(table, i * size + 8) : be32(table, i * size + 8) << 32 | be32(table, i * size + 12)
        }
    }
    var versions = Set<String>()
    for offset in slices {
        guard let mh = read(offset, 32), le32(mh, 0) == 0xfeedfacf else { return nil }   // 64-bit Mach-O
        let ncmds = Int(le32(mh, 16)), sizeofcmds = Int(le32(mh, 20))
        guard sizeofcmds < 1 << 20, let cmds = read(offset + 32, sizeofcmds) else { return nil }
        var p = 0, found = false
        for _ in 0..<ncmds where p + 20 <= cmds.count {
            let cmd = le32(cmds, p), cmdsize = Int(le32(cmds, p + 4))
            if cmd == 0xd {                             // LC_ID_DYLIB: name offset, timestamp, current version
                let v = le32(cmds, p + 16)
                versions.insert("\(v >> 16).\((v >> 8) & 0xff).\(v & 0xff)"); found = true; break
            }
            guard cmdsize > 0 else { break }
            p += cmdsize
        }
        if !found { return nil }
    }
    return versions.count == 1 ? versions.first : nil
}
