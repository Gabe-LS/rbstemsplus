// What the app may delete, and whether it is the real app. Every deletion goes through
// safeRemove, which allows only exact places (Deletable). Deleting the app, the watcher's
// LaunchAgent and updates need a valid bundle (ownBundle). Shared by the app and the watcher.
import Foundation

// MARK: - paths

/// A path's components, or nil unless it is absolute and plain: no empty, "." or ".." component
/// (a trailing "/" would make a link's target the thing removed). Paths are compared by these,
/// never after standardizing (which resolves "..", and links in some cases).
func pathComponents(_ path: String) -> [String]? {
    guard path.hasPrefix("/") else { return nil }
    let c = path.split(separator: "/", omittingEmptySubsequences: false).dropFirst().map(String.init)
    guard !c.isEmpty, !c.contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }) else { return nil }
    return c
}

/// What is at a path itself (lstat: a link is a link), or nil if nothing.
func fileType(_ path: String) -> mode_t? {
    var st = stat()
    return lstat(path, &st) == 0 ? st.st_mode & S_IFMT : nil
}

/// The bundle ID of an app folder: a real folder (not a link) and its Contents/Info.plist.
func bundleIDOf(_ app: String) -> String? {
    guard fileType(app) == S_IFDIR else { return nil }
    return NSDictionary(contentsOfFile: app + "/Contents/Info.plist")?["CFBundleIdentifier"] as? String
}

// MARK: - the gate

/// The places the app may delete. Nothing else, whatever a caller asks.
struct Deletable {
    var inside: [String]        // anything inside these folders
    var roots: [String]         // these folders themselves, only when asked for (whole)
    var files: [String]         // exactly these
    var apps: [String]          // exactly these, and only if they are an app with our bundle ID
}

let deletable = Deletable(inside: [support, cacheDir, logDir, payloadRoot, updateDir],
                          roots: [support, cacheDir, logDir],
                          files: [agentPlist, modelDir + "/.hdemucs.new", modelDir + "/.hdemucs.restore"],
                          apps: [updateStagingApp, updateOldApp, bootstrapNewApp, bootstrapOldApp])

/// Where a refused deletion is reported: the app's log (the tests collect them instead).
var refusedRemoval: (String) -> Void = { appendLog(logPath, "refused to delete \($0)") }

/// Whether `path` may be deleted (decides only; nothing is touched). `whole`: the roots
/// themselves too (Uninstall RB Stems Plus completely, Clear saved stems).
/// - A folder inside which things may go must itself be a real folder (lstat: users do link
///   ~/Library/Caches, so its parents aren't checked), and so must every folder between it and
///   the path: a link there would lead outside.
/// - The last component may be a link: safeRemove removes the link, never what it points to.
func mayRemove(_ path: String, whole: Bool = false, _ d: Deletable = deletable) -> Bool {
    guard let c = pathComponents(path) else { return false }
    for root in d.inside {
        guard let r = pathComponents(root), c.count > r.count, Array(c.prefix(r.count)) == r else { continue }
        var p = root
        for name in [""] + Array(c[r.count..<(c.count - 1)]) {
            if !name.isEmpty { p += "/" + name }
            guard let t = fileType(p) else { return true }            // not there: nothing to delete
            guard t == S_IFDIR else { return false }
        }
        return true
    }
    if whole, d.roots.contains(where: { pathComponents($0) == c }) { return fileType(path).map { $0 == S_IFDIR } ?? true }
    if d.files.contains(where: { pathComponents($0) == c }) { return true }
    if d.apps.contains(where: { pathComponents($0) == c }) { return fileType(path) == nil || bundleIDOf(path) == bundleID }
    return false
}

/// The only way the app deletes anything. Returns whether nothing is left at `path`.
@discardableResult
func safeRemove(_ path: String, whole: Bool = false, _ d: Deletable = deletable) -> Bool {
    guard mayRemove(path, whole: whole, d) else { refusedRemoval(path); return false }
    guard let t = fileType(path) else { return true }
    // removefile walks without following links; a link or a file is unlinked itself
    if t == S_IFDIR { return removefile(path, nil, removefile_flags_t(REMOVEFILE_RECURSIVE)) == 0 }
    return unlink(path) == 0
}

/// Writes a file through a new temporary file and a rename: a link at `path` is replaced,
/// never written through.
@discardableResult
func writeAtomically(_ data: Data, to path: String) -> Bool {
    let tmp = (path as NSString).deletingLastPathComponent + "/.\((path as NSString).lastPathComponent).\(UUID().uuidString).tmp"
    let fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o644)
    guard fd >= 0 else { return false }
    let written = data.withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
    let ok = written == data.count && fsync(fd) == 0
    close(fd)
    guard ok, rename(tmp, path) == 0 else { unlink(tmp); return false }     // tmp: ours, made just now (O_EXCL)
    return true
}

// MARK: - this process

/// Why this process mustn't run, or nil. Checked first, before anything creates a folder.
/// As root, "home" would be root's and a deletion could reach anything. Without a plain home
/// folder every path above would be wrong. A folder of ours that is a link or a file would
/// send what the app writes elsewhere.
func startupProblem(uid: uid_t = getuid(), euid: uid_t = geteuid(), home: String = home,
                    folders: [String] = [support, cacheDir, logDir]) -> String? {
    if uid == 0 || euid == 0 { return "RB Stems Plus can't run as root. Open it from your own account." }
    var isDir: ObjCBool = false
    guard let c = pathComponents(home), c.count >= 2, FileManager.default.fileExists(atPath: home, isDirectory: &isDir), isDir.boolValue else {
        return "Your account's home folder can't be found."
    }
    for f in folders where fileType(f).map({ $0 != S_IFDIR }) ?? false {
        return "\(f.replacingOccurrences(of: home, with: "~")) isn't a folder. RB Stems Plus uses only a real folder there: move it to the Trash, then open RB Stems Plus again."
    }
    return nil
}

/// Why `bundle` isn't an app this process may delete, register as the watcher or replace with
/// an update, or nil if it is. `executable`: the running executable's path.
func bundleProblem(_ bundle: String, executable: String?) -> String? {
    if let why = appFolderProblem(bundle) { return why }
    guard let exe = executable, (exe as NSString).deletingLastPathComponent == bundle + "/Contents/MacOS" else { return "not running from the app" }
    return nil
}

/// Why `bundle` isn't RB Stems Plus's app folder, whatever runs from it, or nil.
func appFolderProblem(_ bundle: String) -> String? {
    guard let c = pathComponents(bundle), bundle.hasSuffix(".app") else { return "not in an app" }
    // the app is never inside another app (its helper is, and is checked as the helper)
    if c.dropLast().contains(where: { $0.hasSuffix(".app") }) { return "inside another app" }
    if bundle.contains("/AppTranslocation/") { return "opened where it was downloaded (translocated)" }
    guard fileType(bundle) == S_IFDIR else { return "the app is a link" }
    guard bundleIDOf(bundle) == bundleID else { return "not RB Stems Plus's bundle ID" }
    return nil
}

/// The app a watcher helper at `helper` belongs to: the app folder it sits in at
/// watcherHelperPath, or nil if it isn't at that place.
func appOfHelper(_ helper: String) -> String? {
    guard let h = pathComponents(helper), let rel = pathComponents("/" + watcherHelperPath),
          h.count > rel.count, Array(h.suffix(rel.count)) == rel else { return nil }
    return "/" + h.dropLast(rel.count).joined(separator: "/")
}

/// Why `helper` isn't the watcher helper of a valid RB Stems Plus, or nil if it is: exactly at
/// watcherHelperPath in a valid app folder (appFolderProblem), every folder from the app down to
/// it a real folder (not a link), the watcher's bundle ID, `executable` (the running
/// executable's path) inside its Contents/MacOS.
func helperProblem(_ helper: String, executable: String?) -> String? {
    guard let app = appOfHelper(helper) else { return "not in RB Stems Plus's \(watcherHelperPath)" }
    if let why = appFolderProblem(app) { return why }
    var p = app
    for name in pathComponents("/" + watcherHelperPath) ?? [] {
        p += "/" + name
        guard fileType(p) == S_IFDIR else { return "\(watcherHelperPath) isn't a real folder" }
    }
    guard bundleIDOf(helper) == watcherBundleID else { return "not the watcher's bundle ID" }
    guard let exe = executable, (exe as NSString).deletingLastPathComponent == helper + "/Contents/MacOS" else { return "not running from the watcher" }
    return nil
}

/// The app this process belongs to, if it is valid, or why not. The app itself
/// (bundleProblem), or, run from the watcher helper, the app around it (helperProblem). Checked
/// each time: the app can be moved while it runs.
func ownApp(bundle: String = Bundle.main.bundlePath, executable: String? = Bundle.main.executablePath) -> (app: String?, problem: String?) {
    if bundleIDOf(bundle) == watcherBundleID {
        if let why = helperProblem(bundle, executable: executable) { return (nil, why) }
        return (appOfHelper(bundle), nil)
    }
    if let why = bundleProblem(bundle, executable: executable) { return (nil, why) }
    return (bundle, nil)
}

/// The app's bundle (ownApp) if it is valid, else nil.
func ownBundle() -> String? { ownApp().app }

/// Whether this account owns `path` itself (lstat). On a Mac with several accounts, an app
/// installed from another account is theirs: Uninstall RB Stems Plus completely leaves it.
func ownedByThisAccount(_ path: String, uid: uid_t = getuid()) -> Bool {
    var st = stat()
    return lstat(path, &st) == 0 && st.st_uid == uid
}

/// The status line's request when ownBundle is nil.
let moveToApplications = "Move RB Stems Plus to the Applications folder, then open it from there."

/// The watcher helper inside the app `bundle`.
func watcherHelper(_ bundle: String) -> String { bundle + "/" + watcherHelperPath }

/// The executable the LaunchAgent starts: the watcher helper's, inside the valid app `bundle`.
func watcherExecutable(_ bundle: String) -> String { watcherHelper(bundle) + "/Contents/MacOS/" + watcherName }

/// The watcher executable of this valid app, if its helper is valid too (helperProblem), else nil.
func ownWatcher() -> String? {
    guard let app = ownBundle() else { return nil }
    let exe = watcherExecutable(app)
    return helperProblem(watcherHelper(app), executable: exe) == nil && fileType(exe) == S_IFREG ? exe : nil
}

/// Whether `target` (a LaunchAgent's ProgramArguments[0]) starts a valid copy of the app: the
/// watcher helper's executable in a valid app, or (1.0's agents) the app's own executable.
func startsValidCopy(_ target: String) -> Bool {
    guard fileType(target) == S_IFREG else { return false }
    let bundle = ((target as NSString).deletingLastPathComponent as NSString).deletingLastPathComponent as NSString
    let b = bundle.deletingLastPathComponent
    return helperProblem(b, executable: target) == nil || bundleProblem(b, executable: target) == nil
}

/// What to do with a LaunchAgent whose ProgramArguments[0] is `target`, from this copy of the
/// app (`own`: watcherExecutable of a valid bundle, or nil): keep it if it starts us; else
/// rewrite it to start us (also 1.0's agents, which start the app's own executable), or, from a
/// copy that isn't a valid app, remove it unless it starts another valid copy.
enum AgentFix { case keep, rewrite, remove }
func agentFix(target: String?, own: String?) -> AgentFix {
    if let own = own { return target == own ? .keep : .rewrite }
    guard let t = target else { return .remove }
    return startsValidCopy(t) ? .keep : .remove
}

/// What the app does with the LaunchAgent at launch (agentFix), and an agent that starts us but
/// was registered with other code (`recorded`, the watcherRegisteredFile's text, vs `current`,
/// registrationRecord of what is there now) registered again: after an update or the install
/// command replaced the app, launchd's first start of the new code was killed (Code Signature
/// Invalid, a launch constraint violation) until the agent was registered again. Unknown
/// current code (nil): kept, there is nothing to compare.
func agentDecision(target: String?, own: String?, recorded: String?, current: String?) -> AgentFix {
    let fix = agentFix(target: target, own: own)
    guard fix == .keep, own != nil, let now = current else { return fix }
    return recorded == now ? .keep : .rewrite
}

/// The watcherRegisteredFile's text for the watcher `exe` with code `identity`.
func registrationRecord(exe: String, identity: String) -> String { exe + "\n" + identity + "\n" }

/// Deletes the app once this process has exited (run by afterExit: $1 the app, $2 our bundle
/// ID). It checks again what it deletes: an absolute plain path ending in .app, not
/// translocated, a real folder (not a link) with our bundle ID.
let selfDeleteScript = """
    case "$1" in /*.app) ;; *) exit 1 ;; esac
    case "$1" in */AppTranslocation/*|*/../*|*/./*|*//*) exit 1 ;; esac
    [ -d "$1" ] && [ ! -L "$1" ] || exit 1
    [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$1/Contents/Info.plist" 2>/dev/null)" = "$2" ] || exit 1
    /bin/rm -rf "$1"
    """
