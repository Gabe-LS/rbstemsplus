// The root scripts (Scripts.swift), run as this account on fake rekordbox installs in the
// temporary folder. Never as root, and never on the real rekordbox or /Library: the real-path
// runs are tests/root/vm-root-tests.sh, for a throwaway VM.
//
// The test body is the release body with these replacements, each of which must match the release
// text exactly once (so a change there breaks the tests instead of quietly testing less):
// - rekordbox's folder, the root folder and the lock become paths in the temporary folder;
// - root's uid becomes this account's, so the ownership checks pass on the account's own files;
// - AlphaTheta's requirement becomes the identifier the fake rekordbox is signed with (our re-sign
//   takes the bundle ID as identifier, so it fails that requirement just as it fails AlphaTheta's);
// - rekordbox's process name becomes another one, so the runs work while rekordbox is open, and so
//   do Pioneer's installer's and updater's names and Installer.app's path, so a real process that
//   comes and goes on this Mac (macOS's own "installer") can't change a run;
// - every "# step:" comment becomes a hook that can stop (kill -9), fail or change the run there.
// The release body has none of this: nothing in it (no variable, argument or file) turns a test
// path on, which is checked below, so a release build can't run it.
import Foundation

/// The test program's helpers for tests/root/vm-root-tests.sh (true if it was one of them):
/// - "--print-script install|uninstall": a root script's release text;
/// - "--sleep <seconds>": a process to copy under another name (installer, rekordbox…);
/// - "--make-fake <folder>": a fake rekordbox install there, for a dry run of that script as
///   this account (its own tools in <folder>/parts, the bridge as parts/bridge.dylib).
func rootScriptTool(_ args: [String]) -> Bool {
    guard args.count == 2 else { return false }
    switch (args[0], args[1]) {
    case ("--print-script", "install"): print(cacheInstallBody)
    case ("--print-script", "uninstall"): print(cacheUninstallBody)
    case ("--sleep", let t): sleep(UInt32(t) ?? 30)
    case ("--make-fake", let dir):
        guard let parts = makeParts(dir + "/parts") else { print("can't compile the fake (xcrun clang)"); exit(1) }
        makeRekordbox(FakeMac(base: dir), parts)
    default: return false
    }
    return true
}

/// A fake rekordbox install, its root folder and lock, in a fresh folder.
private struct FakeMac {
    let base: String
    var rbDir: String { base + "/Applications/rekordbox 7" }
    var app: String { rbDir + "/rekordbox.app" }
    var contents: String { app + "/Contents" }
    var lib: String { contents + "/Frameworks/libonnxruntime.1.18.0.dylib" }
    var out: String { base + "/Library/rbstemsplus" }
    var ort: String { out + "/ort/libonnxruntime.1.18.0.dylib" }
    var lock: String { base + "/run/rbstemsplus.lock" }
    var trace: String { base + "/trace" }
    func set(_ v: String = "7.2.19.0342") -> String { out + "/pioneer/" + v }
}

private let three = ["MacOS/rekordbox", "_CodeSignature/CodeResources", "Frameworks/libonnxruntime.1.18.0.dylib"]
private let version = "7.2.19.0342"
private let fakeTeam = "fake.alphatheta"
/// the name the test body looks for instead of rekordbox's, so the runs work while rekordbox is open
private let fakeRekordboxProcess = "rbsp-rekordbox"
/// the names and path the test body looks for instead of Pioneer's installer's and updater's and
/// Installer.app's (process names: at most 16 characters, as macOS keeps them)
private let fakeInstallerProcess = "rbsp-installer", fakeUpdaterProcess = "rbsp-Upmgr rb"
private let fakeInstallerApp = "/rs-procs/Installer.app/Contents/MacOS/Installer"

private func mkdirs(_ p: String) { try? FileManager.default.createDirectory(atPath: p, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o755]) }
private func there(_ p: String) -> Bool { fileType(p) != nil }
private func cs(_ args: [String]) -> Int32 { runTool("/usr/bin/codesign", args).0 }

/// The test body: the release body with the replacements above. `hooks`: at a step (its name, or
/// its number counted over the whole run), run that bash code. `extra`: more bash code, after the
/// hooks (e.g. a codesign function that wraps the real one).
private func testBody(_ release: String, _ f: FakeMac, hooks: [(at: String, code: String)] = [], extra: String = "") -> String? {
    var b = release
    var hook = "__n=0\n__step() { __n=$((__n+1)); printf '%s\\n' \"$1\" >> '\(f.trace)'\n"
    for h in hooks { hook += "  if [ \"$1\" = '\(h.at)' ] || [ \"$__n\" = '\(h.at)' ]; then \(h.code); fi\n" }
    hook += "}" + (extra.isEmpty ? "" : "\n" + extra)
    let say = "say() { /usr/bin/printf '%s\\n' \"$*\" 2>/dev/null || true; }"
    for (from, to) in [("RBDIR='\(rbFolder)'", "RBDIR='\(f.rbDir)'"), ("OUT='\(rootDir)'", "OUT='\(f.out)'"),
                       ("LOCK='\(rootLock)'", "LOCK='\(f.lock)'"), ("OWNER=0:0", "OWNER=\(getuid()):\(getgid())"),
                       ("PIONEER='anchor apple generic and certificate leaf[subject.OU] = \"\(pioneerTeam)\"'", "PIONEER='identifier \"\(fakeTeam)\"'"),
                       ("if running -x rekordbox; then", "if running -x '\(fakeRekordboxProcess)'; then"),
                       ("if running -x installer || running -x 'Upmgr rekordbox' || running -f '\(appleInstallerPattern)'; then",
                        "if running -x '\(fakeInstallerProcess)' || running -x '\(fakeUpdaterProcess)' || running -f '\(fakeInstallerApp.replacingOccurrences(of: ".", with: "[.]"))'; then"),
                       (say, say + "\n" + hook)] {
        guard b.components(separatedBy: from).count == 2 else { print("the root script no longer has: \(from)"); return nil }
        b = b.replacingOccurrences(of: from, with: to)
    }
    let steps = b.components(separatedBy: "# step: ").count - 1
    guard steps > 0 else { return nil }
    b = b.replacingOccurrences(of: "# step: ", with: "__step ")
    return b
}

/// The tools the fakes are made of, compiled once: rekordbox, two builds of Pioneer's library, a
/// library for the legitimate link in Frameworks, the bridge (with its marker text) and a process
/// that sleeps (named installer, Upmgr rekordbox…).
private struct Parts { let dir: String; var exe: String { dir + "/rekordbox" }; var lib1: String { dir + "/lib1.dylib" }
    var lib2: String { dir + "/lib2.dylib" }; var tf: String { dir + "/tf.dylib" }; var bridge: String { dir + "/bridge.dylib" }
    var sleeper: String { dir + "/sleeper" }; var ents: String { dir + "/ents.plist" } }

private func compile(_ src: String, _ out: String, dylib: Bool) -> Bool {
    FileManager.default.createFile(atPath: out + ".c", contents: Data(src.utf8))
    return runTool("/usr/bin/xcrun", ["clang"] + (dylib ? ["-dynamiclib"] : []) + ["-o", out, out + ".c"]).0 == 0
        && cs(["-f", "-s", "-", out]) == 0
}

private func makeParts(_ dir: String) -> Parts? {
    let p = Parts(dir: dir)
    mkdirs(dir)
    guard compile("int main(void) { return 0; }\n", p.exe, dylib: false),
          compile("const char *v = \"Pioneer's ONNX Runtime, build 1\"; int f(void) { return 1; }\n", p.lib1, dylib: true),
          compile("const char *v = \"Pioneer's ONNX Runtime, build 2\"; int f(void) { return 2; }\n", p.lib2, dylib: true),
          compile("int tf(void) { return 3; }\n", p.tf, dylib: true),
          compile("const char *v = \"rbstems bridge: test\"; int f(void) { return 4; }\n", p.bridge, dylib: true),
          compile("#include <stdlib.h>\n#include <unistd.h>\nint main(int c, char **v) { sleep(c > 1 ? atoi(v[1]) : 30); return 0; }\n", p.sleeper, dylib: false)
    else { return nil }
    try? (["com.apple.security.cs.disable-library-validation": true] as NSDictionary).write(to: URL(fileURLWithPath: p.ents))
    return p
}

/// rekordbox as Pioneer's installer leaves it, signed by the fake AlphaTheta.
private func makeRekordbox(_ f: FakeMac, _ p: Parts, lib: String? = nil, version v: String = version, id: String = rbBundleID) {
    let fm = FileManager.default
    try? fm.removeItem(atPath: f.app)
    for d in [f.contents + "/MacOS", f.contents + "/Frameworks", f.base + "/Library", f.base + "/run"] { mkdirs(d) }
    try! fm.copyItem(atPath: p.exe, toPath: f.contents + "/MacOS/rekordbox")
    try! fm.copyItem(atPath: lib ?? p.lib1, toPath: f.lib)
    try! fm.copyItem(atPath: p.tf, toPath: f.contents + "/Frameworks/libtensorflow_cc.2.16.1.dylib")
    symlink("libtensorflow_cc.2.16.1.dylib", f.contents + "/Frameworks/libtensorflow_cc.2.dylib")   // Pioneer's own link
    try! (["CFBundleIdentifier": id, "CFBundleShortVersionString": v, "CFBundleExecutable": "rekordbox"] as NSDictionary)
        .write(to: URL(fileURLWithPath: f.contents + "/Info.plist"))
    _ = cs(["-f", "-s", "-", "-i", fakeTeam, f.lib])
    _ = cs(["-f", "-s", "-", "-i", "fake.tensorflow", f.contents + "/Frameworks/libtensorflow_cc.2.16.1.dylib"])
    _ = cs(["-f", "-s", "-", "-i", fakeTeam, "--entitlements", p.ents, f.app])
}

private func pioneers(_ app: String) -> Bool { cs(["--verify", "--strict", "-R=identifier \"\(fakeTeam)\"", app]) == 0 }
private func ours(_ f: FakeMac) -> Bool {
    (FileManager.default.contents(atPath: f.lib)?.range(of: Data("rbstems bridge: ".utf8)) != nil)
        && cs(["--verify", "--deep", "--strict", f.app]) == 0
}
/// The saved set has its checksums, and they match its files.
private func setComplete(_ f: FakeMac) -> Bool {
    guard let sums = try? String(contentsOfFile: f.set() + "/SHA256SUMS", encoding: .utf8) else { return false }
    let want = three.map { "\(sha256(f.set() + "/" + $0) ?? "-")  \($0)" }.joined(separator: "\n") + "\n"
    return sums == want
}
private func liveShas(_ f: FakeMac) -> [String] { three.map { sha256(f.contents + "/" + $0) ?? "-" } }
private func leftovers(_ f: FakeMac) -> [String] {
    let temps: [String] = three.flatMap { (f: String) -> [String] in [f + ".rbsp-restore", f + ".rbsp-new"] } + ["Frameworks/.bridge.new", "Frameworks/.rbsp-probe"]
    var found = (temps.map { f.contents + "/" + $0 } + [f.lock]).filter(there)
    found += ((try? FileManager.default.contentsOfDirectory(atPath: f.out)) ?? []).filter { $0.hasPrefix("stage.") && $0.count == 12 }   // mktemp's stage.XXXXXX
    return found.map { $0.replacingOccurrences(of: f.base, with: "") }
}

func rootScriptTests() {
    let fm = FileManager.default

    // MARK: the scripts themselves
    for (name, body) in [("install", cacheInstallBody), ("uninstall", cacheUninstallBody)] {
        check(runTool("/bin/bash", ["-n", "-c", body]).0 == 0, "root \(name) script: parses")
        check(!body.contains("echo"), "root \(name) script: no echo, every message goes through say")
        check(!body.contains("__") && !body.contains("TEST"), "root \(name) script: no test hook in the release text")
    }
    check(cacheInstallBody.contains("x touch Frameworks/.rbsp-probe") && !cacheUninstallBody.contains("touch")
          && [cacheInstallBody, cacheUninstallBody].allSatisfy { $0.contains("Frameworks/.bridge.new Frameworks/.rbsp-probe MacOS/rekordbox.cstemp") },
          "only the install probes a write into rekordbox; both remove a leftover probe")
    let i1 = cacheInstallScript(bridge: Data("x".utf8), bridgeSha: String(repeating: "a", count: 64), ortSha: String(repeating: "b", count: 64), force: true, version: "1.2.3")
    let i2 = cacheInstallScript(bridge: Data(), bridgeSha: "c", ortSha: "d", force: false, version: "9")
    check(i1.body == i2.body && cacheUninstallScript(version: "1").body == cacheUninstallScript(version: "2").body
          && removeRootFolderScript().body == cacheUninstallBody, "root scripts: constant text, whatever the values")
    check(i1.args == ["install", "1.2.3", rbBundleID, String(repeating: "a", count: 64), String(repeating: "b", count: 64), "1"] && i1.input == Data("x".utf8),
          "install: action, version, bundle ID, checksums, force; the bridge on stdin")
    check(cacheUninstallScript(version: "1").args == ["uninstall", "1", rbBundleID] && removeRootFolderScript().args == ["remove-root-folder", "", ""],
          "uninstall and remove-root-folder arguments")
    // a run's exit code reaches the caller, with the root script's own reason (e.g. for "Uninstall stopped")
    check(Controller.RootOutcome.done.code == nil && Controller.RootOutcome.done.problem == nil, "a run that succeeded has no code and no problem")
    check(Controller.RootOutcome.failed(RootExit.noOriginals).code == RootExit.noOriginals
          && Controller.RootOutcome.failed(RootExit.noOriginals).problem?.title == "Reinstall rekordbox", "a failed run's code and its reason (no saved originals)")
    check(Controller.RootOutcome.rekordboxMissing.code == RootExit.rekordboxMissing
          && Controller.RootOutcome.rekordboxMissing.problem?.title == "rekordbox missing", "rekordbox missing: its code and its reason")
    check(Controller.RootOutcome.failed(Controller.cancelled).problem == nil && Controller.RootOutcome.failed(RootExit.checkFailed).problem == nil,
          "cancelled, or a reason only in the log: no alert text")
    for c in [RootExit.alreadyRunning, RootExit.installerRunning, RootExit.notPioneers, RootExit.rollbackFailed] {
        check(Controller.rootProblem(c) != nil, "exit \(c) has its own reason for the user")
    }
    check(![rbFolder, rootDir, rootLock, pioneerTeam].contains { $0.contains("'") || $0.contains("\n") }, "the fixed paths fit in single quotes")
    check(rbApp == rbFolder + "/rekordbox.app" && rbLib == rbApp + "/Contents/Frameworks/libonnxruntime.1.18.0.dylib"
          && rootOrt == rootDir + "/ort/libonnxruntime.1.18.0.dylib", "the scripts' paths match Paths.swift")
    // the version pattern, as the script has it
    if let reLine = cacheInstallBody.split(separator: "\n").first(where: { $0.hasPrefix("VERSION_RE=") }) {
        func ok(_ v: String) -> Bool { runTool("/bin/bash", ["-c", reLine + "\n[[ \"$1\" =~ $VERSION_RE ]]", "rbsp", v]).0 == 0 }
        for v in ["", "7.2.19/", "..", ".", "7..2", "7.2.19 ", " 7.2.19", "7.2.19\n", ".7", "7.", "/7", "7/../8", "-1", "7.2.19x", "7.2.19.0342/.."] {
            check(!ok(v), "version \(v.debugDescription) is refused")
        }
        for v in ["7.2.19.0342", "7", "7.2"] { check(ok(v), "version \(v.debugDescription) is accepted") }
    } else { check(false, "the install script has VERSION_RE") }

    // MARK: runs on fake rekordbox installs (once: "make test" runs the hardened-runtime copy with --skip-root-runs)
    if CommandLine.arguments.contains("--skip-root-runs") { print("skip the root scripts' runs (--skip-root-runs)"); return }
    guard let parts = makeParts(tmp + "/rs-parts") else { check(false, "compile the fake rekordbox (xcrun clang)"); return }
    let bridge = fm.contents(atPath: parts.bridge)!, bridgeSha = sha256(data: bridge)
    func run(_ f: FakeMac, _ s: AdminScript, hooks: [(at: String, code: String)] = []) -> (Int32, String) {
        try? fm.removeItem(atPath: f.trace)
        guard let body = testBody(s.body, f, hooks: hooks) else { return (-100, "no test body") }
        return runTool("/bin/bash", ["-c", body, "rbsp"] + s.args, input: s.input)
    }
    func install(_ f: FakeMac, force: Bool = false, v: String = version, hooks: [(at: String, code: String)] = [], data: Data? = nil, sha: String? = nil) -> (Int32, String) {
        // as the app does: the ort/ copy's checksum when the bridge is in rekordbox, else rekordbox's library's
        let bridgeIn = fm.contents(atPath: f.lib)?.range(of: Data("rbstems bridge: ".utf8)) != nil
        let ortSha = sha256(bridgeIn ? f.ort : f.lib) ?? String(repeating: "0", count: 64)
        return run(f, cacheInstallScript(bridge: data ?? bridge, bridgeSha: sha ?? bridgeSha, ortSha: ortSha, force: force, version: v), hooks: hooks)
    }
    func uninstall(_ f: FakeMac, hooks: [(at: String, code: String)] = []) -> (Int32, String) { run(f, cacheUninstallScript(version: version), hooks: hooks) }
    var n = 0
    // a fresh fake: a copy of one made once, as Pioneer's installer leaves it or with Stems Cache in
    var templates: [Bool: String] = [:]
    func fresh(installed: Bool = false, lib: String? = nil) -> FakeMac {
        n += 1
        let f = FakeMac(base: tmp + "/rs-\(n)")
        if lib != nil { makeRekordbox(f, parts, lib: lib); if installed { _ = install(f) }; return f }
        if templates[installed] == nil {
            let t = FakeMac(base: tmp + "/rs-template-\(installed)")
            makeRekordbox(t, parts)
            if installed { _ = install(t) }
            templates[installed] = t.base
        }
        try! fm.copyItem(atPath: templates[installed]!, toPath: f.base)
        return f
    }
    func steps(_ f: FakeMac) -> [String] { ((try? String(contentsOfFile: f.trace, encoding: .utf8)) ?? "").split(separator: "\n").map(String.init) }
    func short(_ r: (Int32, String)) -> String { "exit \(r.0): \(r.1.split(separator: "\n").last ?? "")" }
    /// every name in a folder, and every file's checksum (links not followed)
    func tree(_ dir: String) -> String {
        runTool("/bin/bash", ["-c", "cd \"$1\" && find . -print | LC_ALL=C sort && find . -type f -exec shasum -a 256 {} + | LC_ALL=C sort", "rbsp", dir]).1
    }

    // install and uninstall
    var f = fresh()
    let pioneerShas = liveShas(f)
    let before = Set(tree(f.app).split(separator: "\n"))
    var r = install(f)
    let changed = Set(tree(f.app).split(separator: "\n")).symmetricDifference(before)
    check(!changed.isEmpty && changed.allSatisfy { l in three.contains { l.hasSuffix("./Contents/" + $0) } },
          "install: rekordbox changes in exactly its three files, the rest is as it was (\(changed.count) lines differ)")
    check(r.0 == 0 && ours(f) && setComplete(f) && liveShas(f)[0] != pioneerShas[0], "install: the bridge in, re-signed, the set saved (\(short(r)))")
    check(sha256(f.ort) == pioneerShas[2] && cs(["--verify", "-R=identifier \"\(fakeTeam)\"", f.ort]) == 0, "install: Pioneer's library copied to ort/")
    check(three.map { sha256(f.set() + "/" + $0) } == pioneerShas, "install: the saved set is Pioneer's three files")
    check(fileType(f.contents + "/Frameworks/libtensorflow_cc.2.dylib") == S_IFLNK, "install: Pioneer's own link in Frameworks is fine and stays")
    check(leftovers(f).isEmpty, "install: no leftovers, the lock is released (\(leftovers(f)))")
    r = install(f)
    check(r.0 == 0 && r.1.contains("already in place") && ours(f), "install again: already in place (\(short(r)))")
    r = install(f, force: true)
    check(r.0 == 0 && ours(f) && setComplete(f), "reinstall (force): the bridge in again (\(short(r)))")
    r = uninstall(f)
    check(r.0 == 0 && pioneers(f.app) && liveShas(f) == pioneerShas && !there(f.out), "uninstall: Pioneer's files back, the root folder removed (\(short(r)))")
    check(leftovers(f).isEmpty, "uninstall: no leftovers, the lock is released")
    r = uninstall(f)
    check(r.0 == 0 && pioneers(f.app) && !there(f.out), "uninstall with nothing installed: fine, and no root folder is left (\(short(r)))")

    // the bridge on stdin
    f = fresh()
    r = install(f, sha: String(repeating: "0", count: 64))
    check(r.0 == 4 && r.1.contains("checksum doesn't match") && pioneers(f.app), "install: a bridge with another checksum is refused (\(short(r)))")
    r = install(f, data: Data(count: 32 << 20 + 1), sha: sha256(data: Data(count: 32 << 20 + 1)))
    check(r.0 == 4 && r.1.contains("over 32 MB") && pioneers(f.app), "install: a bridge over 32 MB is refused (\(short(r)))")
    r = install(f, sha: "")
    check(r.0 == 4 && pioneers(f.app), "install: an empty checksum is refused (\(short(r)))")
    check(leftovers(f).isEmpty, "refusals leave no staging folder and no lock")

    // what the app checked must still be so
    f = fresh()
    r = install(f, v: "7.2.18")
    check(r.0 == 4 && pioneers(f.app) && !there(f.out), "install: another version than checked: nothing changed (\(short(r)))")
    for (v, label) in [("..", "\"..\""), ("", "empty"), ("7.2.19/", "\"7.2.19/\""), ("7..2", "\"7..2\""), ("7.2.19 ", "\"7.2.19 \""), ("../../x", "\"../../x\"")] {
        f = fresh(); makeRekordbox(f, parts, version: v)
        r = install(f, v: v)
        check(r.0 == 10 && !there(f.out), "install: rekordbox version \(label) is refused (\(short(r)))")
    }
    f = fresh(); makeRekordbox(f, parts, id: "com.example.other")
    r = install(f)
    check(r.0 == 10 && !there(f.out), "install: another app's bundle ID is refused (\(short(r)))")
    r = uninstall(f)
    check(r.0 == 10, "uninstall: another app's bundle ID is refused (\(short(r)))")

    // symlinks and writable folders where Pioneer's installer leaves real, root-only ones
    for (what, path) in [("rekordbox 7", "/.."), ("rekordbox.app", ""), ("Contents", "/Contents"), ("Frameworks", "/Contents/Frameworks"),
                         ("MacOS", "/Contents/MacOS"), ("_CodeSignature", "/Contents/_CodeSignature"),
                         ("library", "/Contents/Frameworks/libonnxruntime.1.18.0.dylib"), ("executable", "/Contents/MacOS/rekordbox"),
                         ("CodeResources", "/Contents/_CodeSignature/CodeResources"), ("Info.plist", "/Contents/Info.plist")] {
        for act in ["install", "uninstall"] {
            f = fresh(installed: act == "uninstall")
            let target = (f.app + path as NSString).standardizingPath, real = target + ".real"
            try! fm.moveItem(atPath: target, toPath: real); symlink(real, target)
            r = act == "install" ? install(f) : uninstall(f)
            check(r.0 == 10, "\(act): \(what) as a link is refused (\(short(r)))")
            check(there(real) && (act == "install" ? !there(f.set()) : setComplete(f)), "\(act): nothing removed or saved behind the \(what) link")
        }
    }
    f = fresh()
    chmod(f.contents + "/Frameworks", 0o775)
    r = install(f)
    check(r.0 == 10 && pioneers(f.app), "install: a Frameworks folder others can write is refused (\(short(r)))")
    f = fresh()
    chmod(f.lib, 0o777)
    r = install(f)
    check(r.0 == 10, "install: a library others can write is refused (\(short(r)))")

    // rekordbox missing; removing the root folder
    f = fresh(installed: true)
    mkdirs(f.base + "/moved/Applications"); try! fm.moveItem(atPath: f.rbDir, toPath: FakeMac(base: f.base + "/moved").rbDir)
    r = install(f)
    check(r.0 == RootExit.rekordboxMissing, "install: rekordbox missing has its own exit code (\(short(r)))")
    r = uninstall(f)
    check(r.0 == RootExit.rekordboxMissing && there(f.set()), "uninstall: rekordbox missing: nothing removed (\(short(r)))")
    r = run(f, removeRootFolderScript())
    check(r.0 == 0 && !there(f.out) && ours(FakeMac(base: f.base + "/moved")), 
          "remove-root-folder: with rekordbox missing, only the root folder goes (\(short(r)))")
    f = fresh(installed: true)
    r = run(f, removeRootFolderScript())
    check(r.0 == 4 && setComplete(f) && ours(f), "remove-root-folder: refused while rekordbox is there (\(short(r)))")
    f = fresh(installed: true)
    try! fm.moveItem(atPath: f.app, toPath: f.base + "/rekordbox.app")
    r = uninstall(f)
    check(r.0 == 10 && setComplete(f), "uninstall: rekordbox 7 without rekordbox.app: reinstall rekordbox, the set stays (\(short(r)))")

    // leftovers of a stopped run are removed by exact name
    f = fresh()
    for l in three.map({ f.contents + "/" + $0 + ".rbsp-restore" }) + [f.contents + "/Frameworks/.bridge.new", f.contents + "/Frameworks/.rbsp-probe", f.contents + "/MacOS/rekordbox.cstemp"] {
        fm.createFile(atPath: l, contents: Data("partial".utf8))
    }
    mkdirs(f.out + "/stage.AbC123"); mkdirs(f.set() + "/MacOS"); fm.createFile(atPath: f.set() + "/MacOS/rekordbox.part", contents: Data())
    mkdirs(f.out + "/stage.toolong1")
    r = install(f)
    check(r.0 == 0 && ours(f) && setComplete(f) && leftovers(f).isEmpty && !there(f.contents + "/MacOS/rekordbox.cstemp") && !there(f.set() + "/MacOS/rekordbox.part"),
          "leftovers are removed, then the install works (\(short(r)))")
    check(there(f.out + "/stage.toolong1"), "a folder that only starts like a leftover stays")

    // the lock
    func sleeper(_ name: String, _ seconds: String = "30") -> Process {
        let path = tmp + (name.hasPrefix("/") ? name : "/rs-procs/" + name)
        mkdirs((path as NSString).deletingLastPathComponent); try? fm.removeItem(atPath: path); try! fm.copyItem(atPath: parts.sleeper, toPath: path)
        let p = child(path, [seconds]); try! p.run(); usleep(200_000); return p
    }
    func lockBy(_ f: FakeMac, pid: Int32, start: String? = nil) {
        mkdirs(f.lock)
        if let start = start { fm.createFile(atPath: f.lock + "/owner", contents: Data("\(pid)\n\(start)\n".utf8)) }
        else { runTool("/bin/bash", ["-c", "printf '%s\\n%s\\n' \"$1\" \"$(ps -o lstart= -p \"$1\")\" > \"$2\"", "rbsp", String(pid), f.lock + "/owner"]) }
    }
    let live = sleeper("holder")
    f = fresh(); lockBy(f, pid: live.processIdentifier)
    r = install(f)
    check(r.0 == RootExit.alreadyRunning && pioneers(f.app) && there(f.lock + "/owner"), "lock held by a running process: refused, the lock stays (\(short(r)))")
    lockBy(f, pid: live.processIdentifier, start: "Thu Jan  1 00:00:00 1970")
    r = install(f)
    check(r.0 == 0 && ours(f) && !there(f.lock), "lock whose pid now belongs to another process: taken over (\(short(r)))")
    live.terminate(); live.waitUntilExit()
    f = fresh(); lockBy(f, pid: live.processIdentifier)
    r = install(f)
    check(r.0 == 0 && ours(f) && !there(f.lock), "lock of a process that is gone: taken over, then released (\(short(r)))")
    f = fresh(); mkdirs(f.lock)
    r = install(f)
    check(r.0 == RootExit.alreadyRunning, "lock being taken right now (no owner yet): refused (\(short(r)))")
    runTool("/usr/bin/touch", ["-t", "202001010000", f.lock])
    r = install(f)
    check(r.0 == 0 && !there(f.lock), "lock without owner from a run stopped long ago: taken over (\(short(r)))")
    f = fresh(); mkdirs(f.base + "/run"); symlink(f.base + "/elsewhere", f.lock)
    r = install(f)
    check(r.0 == RootExit.alreadyRunning && fileType(f.lock) == S_IFLNK, "a link at the lock's place: refused, left alone (\(short(r)))")

    // rekordbox, Pioneer's installer or its updater running
    let open = sleeper(fakeRekordboxProcess)
    f = fresh()
    r = install(f)
    check(r.0 == RootExit.rekordboxOpen && pioneers(f.app) && !there(f.lock), "rekordbox open: refused (\(short(r)))")
    open.terminate(); open.waitUntilExit()
    // (under test names: the release names are checked in the VM, tests/root/vm-root-tests.sh)
    for name in [fakeInstallerProcess, fakeUpdaterProcess, fakeInstallerApp] {
        let p = sleeper(name)
        f = fresh()
        r = install(f)
        check(r.0 == RootExit.installerRunning && pioneers(f.app) && !there(f.lock), "Pioneer's installer or updater (as \(name)) running: refused (\(short(r)))")
        p.terminate(); p.waitUntilExit()
    }
    let other = sleeper("/rs-procs/Other.app/Contents/MacOS/Installer")
    f = fresh()
    r = install(f)
    check(r.0 == 0 && ours(f), "another app's process named Installer doesn't stop it (\(short(r)))")
    other.terminate(); other.waitUntilExit()

    // the install's first write into rekordbox refused (as App Management does, "Operation not
    // permitted"), right after the checks: nothing changed, its own exit code, and none of the slow
    // work done (no copy of rekordbox, nothing saved, no codesign call) for a Try Again to repeat
    f = fresh()
    r = install(f, hooks: [("checked", "chflags uchg Frameworks")])
    _ = runTool("/usr/bin/chflags", ["nouchg", f.contents + "/Frameworks"])
    check(r.0 == RootExit.checkFailed && r.1.contains("Operation not permitted") && r.1.contains("couldn't write into rekordbox: nothing changed")
          && pioneers(f.app) && liveShas(f) == pioneerShas && leftovers(f).isEmpty, "install: the first write refused: nothing changed (\(short(r)))")
    check(steps(f) == ["locked", "checked"] && ((try? fm.contentsOfDirectory(atPath: f.out)) ?? []).isEmpty,
          "install: the first write refused before anything was copied, saved or signed (\(steps(f)), root folder: \((try? fm.contentsOfDirectory(atPath: f.out)) ?? []))")
    // an uninstall of a rekordbox that is already Pioneer's (only the root folder goes) writes
    // nothing into it, so App Management can't refuse it: it works with rekordbox's folders locked
    for withRootFolder in [true, false] {
        f = fresh(installed: withRootFolder)
        if withRootFolder { _ = uninstall(f, hooks: [("verified", "exit 0")]) }     // Pioneer's files back, the root folder kept
        let locked = ["Frameworks", "MacOS", "_CodeSignature", "."].map { f.contents + "/" + $0 }
        let appBefore = tree(f.app)
        _ = runTool("/usr/bin/chflags", ["uchg"] + locked)
        r = uninstall(f)
        _ = runTool("/usr/bin/chflags", ["nouchg"] + locked)
        check(r.0 == 0 && !r.1.contains("Operation not permitted") && pioneers(f.app) && tree(f.app) == appBefore && !there(f.out) && leftovers(f).isEmpty,
              "uninstall, rekordbox already Pioneer's\(withRootFolder ? "" : ", no root folder"): nothing written into rekordbox, works with its folders locked (\(short(r)))")
    }
    // the uninstall's first write refused, after its checks, before rekordbox changes: nothing changed, not a rollback
    f = fresh(installed: true)
    let oursShas = liveShas(f)
    r = uninstall(f, hooks: [("restoring", "chflags uchg MacOS")])
    _ = runTool("/usr/bin/chflags", ["nouchg", f.contents + "/MacOS"])
    check(r.0 == RootExit.checkFailed && r.1.contains("Operation not permitted") && r.1.contains("couldn't write into rekordbox: nothing changed")
          && ours(f) && liveShas(f) == oursShas && setComplete(f) && leftovers(f).isEmpty, "uninstall: the first write refused: nothing changed (\(short(r)))")
    // a later write of the install refused (e.g. a full disk) before rekordbox changes: still nothing changed, not a rollback
    f = fresh()
    r = install(f, hooks: [("signed-copy", "chflags uchg Frameworks")])
    _ = runTool("/usr/bin/chflags", ["nouchg", f.contents + "/Frameworks"])
    check(r.0 == RootExit.checkFailed && r.1.contains("Operation not permitted") && pioneers(f.app) && liveShas(f) == pioneerShas
          && leftovers(f).isEmpty, "install: the bridge's write refused: nothing changed (\(short(r)))")

    // a step fails after rekordbox was changed: Pioneer's files go back
    for at in ["swapped", "signed"] {
        f = fresh()
        r = install(f, hooks: [(at, "say 'test: failing here'; exit 1")])
        check(r.0 == RootExit.rolledBack && pioneers(f.app) && liveShas(f) == pioneerShas && leftovers(f).isEmpty,
              "install fails after \(at): rolled back (\(short(r)))")
    }
    f = fresh(installed: true)
    r = uninstall(f, hooks: [("restored", "say 'test: failing here'; exit 1")])
    check(r.0 == RootExit.rolledBack && pioneers(f.app) && setComplete(f), "uninstall fails after the restore: tried again, the set stays (\(short(r)))")
    // the output closed half-way (the app quit): the rollback still runs to the end
    f = fresh()
    if let body = testBody(cacheInstallBody, f, hooks: [("swapped", "sleep 1"), ("signed", "say 'test: failing here'; say more; exit 1")]) {
        let s = cacheInstallScript(bridge: bridge, bridgeSha: bridgeSha, ortSha: sha256(f.lib)!, force: false, version: version)
        let rc = tmp + "/rs-epipe-rc"
        runTool("/bin/bash", ["-c", "/bin/bash -c \"$1\" \"${@:3}\" | /usr/bin/head -c 1 >/dev/null; printf '%s' \"${PIPESTATUS[0]}\" > \"$2\"", "rbsp", body, rc, "rbsp"] + s.args, input: s.input)
        let code = (try? String(contentsOfFile: rc, encoding: .utf8)) ?? "?"
        check(code == "6" && pioneers(f.app) && liveShas(f) == pioneerShas && leftovers(f).isEmpty && steps(f).filter { $0 == "restored-file" }.count == 3,
              "output closed mid-run (EPIPE): the rollback finishes (exit \(code))")
    }
    // rekordbox's folder moved or replaced during the run: codesign never follows it
    f = fresh()
    r = install(f, hooks: [("swapped", "mv \"$RBDIR\" \"$RBDIR.moved\"")])
    check(r.0 == RootExit.rollbackFailed && pioneers(f.rbDir + ".moved/rekordbox.app") && !there(f.rbDir),
          "rekordbox moved during the install: its files are put back where it went, and the user is told to reinstall (\(short(r)))")
    f = fresh()
    let decoy = FakeMac(base: tmp + "/rs-decoy")
    makeRekordbox(decoy, parts)
    let decoyShas = liveShas(decoy)
    r = install(f, hooks: [("swapped", "mv \"$RBDIR\" \"$RBDIR.moved\"; cp -R '\(decoy.rbDir)' \"$RBDIR\"")])
    check(r.0 == RootExit.rollbackFailed && liveShas(FakeMac(base: f.base)) == decoyShas && pioneers(f.rbDir + ".moved/rekordbox.app"),
          "another rekordbox swapped in during the install: never signed or changed (\(short(r)))")

    // rekordbox's folder renamed and replaced WHILE codesign runs (an admin account can, in
    // /Applications): codesign only ever gets the copy in the root folder, so nothing outside is
    // written, Pioneer's originals stay, and the run stops. Swapped in: a tree whose MacOS,
    // _CodeSignature and Frameworks are links to a folder outside (what root would overwrite if
    // codesign followed the path), or a rekordbox signed by the fake AlphaTheta (what would fool
    // the check before the uninstall removes the saved files).
    let outside = tmp + "/rs-swap-outside"
    let linked = FakeMac(base: tmp + "/rs-swap-linked")
    makeRekordbox(linked, parts)
    mkdirs(outside)
    for d in ["MacOS", "_CodeSignature", "Frameworks"] {
        try! fm.moveItem(atPath: linked.contents + "/" + d, toPath: outside + "/" + d); symlink(outside + "/" + d, linked.contents + "/" + d)
    }
    let outside0 = tree(outside)
    let signedDecoy = FakeMac(base: tmp + "/rs-swap-signed")
    makeRekordbox(signedDecoy, parts)
    /// codesign, wrapped: counts its calls and records its last argument (the path); at call `k`
    /// it renames rekordbox's folder and copies `swapIn` to its place while the real codesign runs.
    func wrapper(_ f: FakeMac, k: Int, swapIn: String) -> String {
        """
        codesign() {
          local n p
          n=$(( $(cat '\(f.base)/cs-count' 2>/dev/null || printf 0) + 1 )); printf '%s\\n' "$n" > '\(f.base)/cs-count'
          printf '%s\\n' "${@: -1}" >> '\(f.base)/cs-paths'
          if [ "$n" = '\(k)' ]; then
            command codesign "$@" & p=$!
            mv "$RBDIR" "$RBDIR.moved" && cp -R '\(swapIn)' "$RBDIR"
            wait "$p"; return
          fi
          command codesign "$@"
        }
        """
    }
    func codesignPaths(_ f: FakeMac) -> [String] { ((try? String(contentsOfFile: f.base + "/cs-paths", encoding: .utf8)) ?? "").split(separator: "\n").map(String.init) }
    for act in ["install", "uninstall"] {
        // how many codesign calls a whole run makes, and that none gets a path outside the root folder
        f = fresh(installed: act == "uninstall")
        let s = act == "install" ? cacheInstallScript(bridge: bridge, bridgeSha: bridgeSha, ortSha: sha256(f.lib)!, force: false, version: version) : cacheUninstallScript(version: version)
        guard let body0 = testBody(s.body, f, extra: wrapper(f, k: 0, swapIn: "")) else { check(false, "the codesign wrapper fits"); break }
        r = runTool("/bin/bash", ["-c", body0, "rbsp"] + s.args, input: s.input)
        let calls = codesignPaths(f)
        check(r.0 == 0 && calls.count >= 3 && calls.allSatisfy({ $0.hasPrefix(f.out + "/") }),
              "\(act): codesign gets only paths in the root folder (\(calls.count) calls: \(Set(calls.map { $0.replacingOccurrences(of: f.base, with: "") }).sorted()))")
        for k in 1...max(1, calls.count) {
            for (what, swapIn) in [("links to a folder outside", linked.rbDir), ("a rekordbox signed by AlphaTheta", signedDecoy.rbDir)] {
                f = fresh(installed: act == "uninstall")
                let swapped0 = tree(swapIn)
                let s = act == "install" ? cacheInstallScript(bridge: bridge, bridgeSha: bridgeSha, ortSha: sha256(f.lib)!, force: false, version: version) : cacheUninstallScript(version: version)
                guard let body = testBody(s.body, f, extra: wrapper(f, k: k, swapIn: swapIn)) else { check(false, "the codesign wrapper fits"); continue }
                r = runTool("/bin/bash", ["-c", body, "rbsp"] + s.args, input: s.input)
                let real = f.rbDir + ".moved/rekordbox.app"
                check((r.0 == 10 || r.0 == RootExit.rollbackFailed) && tree(outside) == outside0 && tree(f.rbDir) == swapped0
                      && (pioneers(real) || setComplete(f)) && codesignPaths(f).allSatisfy({ $0.hasPrefix(f.out + "/") }),
                      "\(act): swapped for \(what) during codesign call \(k): stopped, nothing outside written, the originals kept (\(short(r)))")
            }
        }
    }

    // the saved set
    f = fresh(installed: true)
    try! fm.removeItem(atPath: f.set() + "/SHA256SUMS")
    r = uninstall(f)
    check(r.0 == 0 && pioneers(f.app) && !there(f.out), "uninstall: a v1.0 set (no SHA256SUMS) with AlphaTheta's library is used (\(short(r)))")
    f = fresh(installed: true)
    try! fm.removeItem(atPath: f.set() + "/SHA256SUMS"); try! fm.removeItem(atPath: f.set() + "/" + three[2])
    try! fm.copyItem(atPath: parts.bridge, toPath: f.set() + "/" + three[2])
    r = uninstall(f)
    check(r.0 == RootExit.noOriginals && ours(f) && there(f.set()), "uninstall: a v1.0 set whose library isn't AlphaTheta's: refused, nothing changed (\(short(r)))")
    f = fresh(installed: true)
    fm.createFile(atPath: f.set() + "/" + three[1], contents: Data("changed".utf8))
    r = uninstall(f)
    check(r.0 == RootExit.noOriginals && ours(f), "uninstall: a set that doesn't match its checksums: refused (\(short(r)))")
    f = fresh(installed: true)
    try! fm.removeItem(atPath: f.out)
    r = uninstall(f)
    check(r.0 == RootExit.noOriginals && ours(f) && !there(f.out), "uninstall: no saved set: refused, rekordbox left working, no empty root folder left (\(short(r)))")
    // Pioneer re-shipped the same version: the set is replaced
    f = fresh(installed: true)
    _ = uninstall(f, hooks: [("verified", "exit 0")])     // Pioneer's files back, the set kept
    makeRekordbox(f, parts, lib: parts.lib2)
    let reshipped = liveShas(f)
    check(pioneers(f.app) && there(f.set()) && sha256(f.set() + "/" + three[2]) == pioneerShas[2] && reshipped[2] != pioneerShas[2], "(an older set of this version is there)")
    r = install(f)
    check(r.0 == 0 && ours(f) && setComplete(f) && three.map({ sha256(f.set() + "/" + $0)! }) == reshipped && sha256(f.ort) == reshipped[2],
          "install: Pioneer re-shipped this version: the set and the ort/ copy are replaced (\(short(r)))")
    // an earlier version's set goes once this one's is in
    f = fresh()
    mkdirs(f.out + "/pioneer/7.2.18/MacOS")
    r = install(f)
    check(r.0 == 0 && !there(f.out + "/pioneer/7.2.18") && setComplete(f), "install: an earlier version's set is removed (\(short(r)))")

    // repair: a stopped run left rekordbox half-changed
    f = fresh(installed: true)
    try! fm.removeItem(atPath: f.contents + "/" + three[0]); try! fm.copyItem(atPath: f.set() + "/" + three[0], toPath: f.contents + "/" + three[0])
    check(!pioneers(f.app) && !ours(f), "(rekordbox half-restored)")
    r = uninstall(f)
    check(r.0 == 0 && pioneers(f.app) && !there(f.out), "uninstall repairs a half-restored rekordbox (\(short(r)))")
    f = fresh(installed: true)
    try! fm.removeItem(atPath: f.lib); try! fm.copyItem(atPath: f.set() + "/" + three[2], toPath: f.lib)   // v1.0's order: the library first
    r = uninstall(f)
    check(r.0 == 0 && pioneers(f.app) && !there(f.out), "uninstall repairs rekordbox when the bridge is gone but it doesn't verify (\(short(r)))")
    f = fresh(installed: true)
    try! fm.removeItem(atPath: f.contents + "/" + three[1]); try! fm.copyItem(atPath: f.set() + "/" + three[1], toPath: f.contents + "/" + three[1])
    r = install(f, force: true)
    check(r.0 == 0 && ours(f) && setComplete(f), "install repairs a half-changed rekordbox first (\(short(r)))")
    f = fresh()
    fm.createFile(atPath: f.contents + "/" + three[1], contents: Data("broken".utf8))
    r = install(f)
    check(r.0 == RootExit.notPioneers && !there(f.set()), "install: a broken rekordbox without a saved set: reinstall rekordbox (\(short(r)))")

    // kill -9 after every step, then the same action again repairs and finishes
    for act in ["install", "uninstall"] {
        var k = 1
        while k < 60 {
            f = fresh(installed: act == "uninstall")
            r = act == "install" ? install(f, hooks: [(String(k), "kill -9 $$")]) : uninstall(f, hooks: [(String(k), "kill -9 $$")])
            let trace = steps(f)
            if r.0 != 9 { check(r.0 == 0 && trace.count < k, "\(act): the sweep reached the end after \(k - 1) steps (\(short(r)))"); break }
            let at = trace.last ?? "?"
            check(pioneers(f.app) || setComplete(f), "\(act) killed at step \(k) (\(at)): rekordbox is AlphaTheta's, or its saved set is complete")
            r = act == "install" ? install(f) : uninstall(f)
            let done = act == "install" ? ours(f) && setComplete(f) : pioneers(f.app) && !there(f.out)
            check(r.0 == 0 && done && leftovers(f).isEmpty, "\(act) killed at step \(k) (\(at)), run again: finished (\(short(r)))")
            k += 1
        }
    }
    // killed during the rollback
    f = fresh()
    r = install(f, hooks: [("signed", "exit 1")])
    let rollbackSteps = steps(f).count
    f = fresh()
    r = install(f, hooks: [("signed", "exit 1"), (String(rollbackSteps - 1), "kill -9 $$")])
    check(r.0 == 9 && setComplete(f), "install killed during its rollback (\(steps(f).last ?? "?"))")
    r = install(f)
    check(r.0 == 0 && ours(f) && setComplete(f) && leftovers(f).isEmpty, "then install again: repaired and installed (\(short(r)))")
}
