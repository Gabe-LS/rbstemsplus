// What is on this Mac right now: rekordbox, its versions, and whether our two features are in
// place. Quick checks (no checksums), shared by the app and the watcher.
import Foundation

/// Our model is recognised by its size (ours and Pioneer's differ): the 1.0.0 export's, or the one
/// the last payload.json names. For the status lights and prompts only: whatever saves, replaces
/// or puts back a model decides by checksum (Models.swift).
func stemsPlusPresent() -> Bool {
    guard let size = fileSize(modelDir + "/hdemucs.onnx") else { return false }
    return size == ourModelSize || size == savedManifest()?.model.size
}

/// The bridge is recognised by its own message text (a 280 KB read, no extra process).
func stemsCachePresent() -> Bool {
    guard let d = FileManager.default.contents(atPath: rbLib) else { return false }
    return d.range(of: Data("rbstems bridge: ".utf8)) != nil
}

/// Which of rekordbox's own models the installed bridge saves stems for, by checksum, from the
/// line it carries ("rbstems-cache: rebuild=1 pioneer=<sha>,<sha>;"). nil without a bridge, or for
/// a bridge without the line (1.0's, which saves only the Stems Plus model's stems).
func bridgeCacheModels(_ path: String = rbLib) -> [String]? {
    guard let d = FileManager.default.contents(atPath: path) else { return nil }
    return bridgeCacheModels(data: d)
}

func bridgeCacheModels(data d: Data) -> [String]? {
    guard d.range(of: Data("rbstems bridge: ".utf8)) != nil, let start = d.range(of: Data("rbstems-cache: ".utf8)) else { return nil }
    let rest = d[start.upperBound...].prefix(4096)
    guard let end = rest.firstIndex(of: UInt8(ascii: ";")) else { return nil }
    let line = String(decoding: rest[rest.startIndex..<end], as: UTF8.self)
    guard let p = line.range(of: "pioneer=") else { return [] }
    return line[p.upperBound...].split(separator: ",").map(String.init).filter(isSHA256)
}

/// The checksum of rekordbox's model as it is now, if it was worked out already (else nil, and it
/// is worked out in the background, then `done` runs on the main thread). Remembered per path,
/// size and modification time: the model is about 300 MB, too slow to hash on the main thread.
func currentModelSHA(_ path: String = modelDir + "/hdemucs.onnx", done: @escaping () -> Void) -> String? {
    let a = try? FileManager.default.attributesOfItem(atPath: path)
    guard let size = a?[.size] as? Int else { return nil }
    let key = "\(path) \(size) \((a?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0)"
    modelSHALock.lock(); let known = modelSHAMemo[key], working = modelSHAWorking.contains(key)
    if known == nil && !working { modelSHAWorking.insert(key) }
    modelSHALock.unlock()
    if let k = known { return k }
    if !working {
        DispatchQueue.global(qos: .utility).async {
            let s = sha256(path)
            modelSHALock.lock(); if let s = s { modelSHAMemo[key] = s }; modelSHAWorking.remove(key); modelSHALock.unlock()
            if s != nil { DispatchQueue.main.async(execute: done) }
        }
    }
    return nil
}

private var modelSHAMemo: [String: String] = [:], modelSHAWorking = Set<String>()
private let modelSHALock = NSLock()

/// Whether anything of Stems Cache is on this Mac: the bridge in rekordbox, or the root folder
/// (Pioneer's originals and library copy), which stays behind when a rekordbox update removed the
/// bridge. Uninstalling removes either.
func cacheTraces() -> Bool { stemsCachePresent() || FileManager.default.fileExists(atPath: rootDir) }

/// pgrep run directly, not through bash -c: the wrapper's own command line would contain the
/// pattern and match itself.
func running(_ pattern: String) -> Bool { runTool("/usr/bin/pgrep", ["-f", pattern]).0 == 0 }
/// a process by its exact name, however it was started (e.g. "installer" from a PATH lookup)
func runningName(_ name: String) -> Bool { runTool("/usr/bin/pgrep", ["-x", name]).0 == 0 }
func rekordboxOpen() -> Bool { running("rekordbox\\.app/Contents/MacOS/rekordbox") }
/// Apple's Installer, by its path: other apps' updaters run a process named Installer too
/// (Sparkle's Installer.xpc, which e.g. WhatsApp keeps running), and must not block anything.
/// The root scripts check the same pattern (Scripts.swift).
let appleInstallerPattern = "^/System/Library/CoreServices/Installer[.]app/"
/// Pioneer's installer or rekordbox's updater is running (macOS's Installer runs Pioneer's
/// package): rekordbox is about to change under us. The app's actions and the watcher wait for it.
func pioneerUpdating() -> Bool {
    runningName("installer") || runningName("Upmgr rekordbox") || running(appleInstallerPattern)
}
func rekordboxInstalled() -> Bool { FileManager.default.fileExists(atPath: rbApp + "/Contents/Info.plist") }
/// rekordbox's full version, e.g. "7.2.19.0342" ("" if not installed).
func rekordboxVersion() -> String {
    (NSDictionary(contentsOfFile: rbApp + "/Contents/Info.plist")?["CFBundleShortVersionString"] as? String) ?? ""
}
/// The STEMS Engine version rekordbox downloaded, e.g. "0002" ("" if not downloaded yet).
func engineVersion(in dir: String = modelDir) -> String {
    ((try? String(contentsOfFile: dir + "/demucs3_ver.txt", encoding: .utf8)) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
}
