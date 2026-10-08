// The app's safety checks, tried with cases built to break them. Built from the app's own
// sources (all but main.swift) by "make test"; nothing here touches the user's files: every
// deletion happens in a temporary folder.
import Foundation

// "--swap <staged> <app>": the update's exchange, as the app's main.swift runs it (the fake apps
// of the update tests carry this program as their executable)
if CommandLine.arguments.count == 4 && CommandLine.arguments[1] == "--swap" {
    if let why = swapApps(CommandLine.arguments[2], CommandLine.arguments[3]) { FileHandle.standardError.write(Data((why + "\n").utf8)); exit(1) }
    exit(0)
}
// "--print-env": a child of the tests, to show the environment the process helper gives
if CommandLine.arguments.dropFirst().first == "--print-env" {
    for (k, v) in ProcessInfo.processInfo.environment.sorted(by: { $0.key < $1.key }) { print("\(k)=\(v)") }
    exit(0)
}
// "--home": the home folder the app uses, then Foundation's
if CommandLine.arguments.dropFirst().first == "--home" {
    print(home); print(FileManager.default.homeDirectoryForCurrentUser.path)
    exit(0)
}
// "--print-script", "--sleep", "--make-fake": helpers for tests/root/vm-root-tests.sh (RootScripts.swift)
if rootScriptTool(Array(CommandLine.arguments.dropFirst())) { exit(0) }

// "--hold-lock <path> <seconds>": another copy of the app holding the lock ("held" once it has it)
// "--try-lock <path> <wait>": a new copy trying to take it: prints held, busy or failed
let mode = Array(CommandLine.arguments.dropFirst())
if mode.count == 3 && (mode[0] == "--hold-lock" || mode[0] == "--try-lock") {
    let r = takeLock(mode[1], wait: mode[0] == "--try-lock" ? Double(mode[2])! : 0)
    switch r { case .held: print("held"); case .busy: print("busy"); case .failed: print("failed") }
    fflush(stdout)
    if mode[0] == "--hold-lock", case .held = r { usleep(UInt32(Double(mode[2])! * 1_000_000)) }
    exit(0)
}

// refused deletions are collected here, never written to the user's log
var refused: [String] = []
refusedRemoval = { refused.append($0) }

var failures = 0
func check(_ ok: Bool, _ name: String) {
    print("\(ok ? "ok  " : "FAIL") \(name)")
    if !ok { failures += 1 }
}

let me = Bundle.main.executablePath!
let tmp = NSTemporaryDirectory() + "rbsp-test-\(getpid())"
try! FileManager.default.createDirectory(atPath: tmp, withIntermediateDirectories: true)

// MARK: the environment allowlist

let allowed: Set<String> = ["PATH", "HOME", "TMPDIR", "LANG"]
for (k, v) in ["BASH_ENV": tmp + "/evil.sh", "ENV": tmp + "/evil.sh", "PERL5LIB": tmp, "PERL5OPT": "-Mevil",
               "SSL_CERT_FILE": tmp + "/ca.pem", "CURL_CA_BUNDLE": tmp + "/ca.pem", "HTTPS_PROXY": "http://127.0.0.1:9",
               "https_proxy": "http://127.0.0.1:9", "ALL_PROXY": "http://127.0.0.1:9", "DYLD_LIBRARY_PATH": tmp,
               "DYLD_FRAMEWORK_PATH": tmp, "CFFIXED_USER_HOME": "/", "HOME": "/", "TMPDIR": "/", "PATH": tmp + ":/usr/bin:/bin"] {
    setenv(k, v, 1)
}
FileManager.default.createFile(atPath: tmp + "/evil.sh", contents: Data("touch \"\(tmp)/bash-env-ran\"\n".utf8))

func envOf(_ r: (Int32, String)) -> [String: String] {
    var e: [String: String] = [:]
    for line in r.1.split(separator: "\n") { if let eq = line.firstIndex(of: "=") { e[String(line[..<eq])] = String(line[line.index(after: eq)...]) } }
    return e
}
for (name, exe, args) in [("/usr/bin/env", "/usr/bin/env", [String]()), ("an unprotected child", me, ["--print-env"])] {
    var e = envOf(runTool(exe, args))
    e["__CF_USER_TEXT_ENCODING"] = nil                  // set by CoreFoundation inside the child itself
    check(!e.isEmpty && Set(e.keys).isSubset(of: allowed), "environment of \(name) is only \(allowed.sorted()): got \(e.keys.sorted())")
    check(e["PATH"] == "/usr/bin:/bin:/usr/sbin:/sbin", "PATH of \(name) is the fixed one")
    check(e["HOME"] == String(cString: getpwuid(getuid())!.pointee.pw_dir), "HOME of \(name) is the account's, not HOME=/")
    check(e["TMPDIR"] != "/", "TMPDIR of \(name) is not the inherited one")
}
runTool("/bin/bash", ["-c", "true"])
check(!FileManager.default.fileExists(atPath: tmp + "/bash-env-ran"), "BASH_ENV doesn't reach bash")

// MARK: values reach shell bodies as arguments, never as code

let tricky = ["it's", "'\u{301}; touch \(tmp)/injected #", "\u{301}'$(touch \(tmp)/injected)'", "`touch \(tmp)/injected`",
              "a\"b\\c", "x\ny", "-n", "$HOME", "*", "e\u{301}'", "'\u{20DD}'"]
for t in tricky {
    for sh in ["/bin/sh", "/bin/bash"] {
        let r = runTool(sh, ["-c", "printf '%s' \"$1\"", "rbsp", t])
        check(r.0 == 0 && r.1 == t, "\(sh) gets \(t.debugDescription) as one argument")
    }
}
check(!FileManager.default.fileExists(atPath: tmp + "/injected"), "no tricky value ran as code")

// MARK: the root scripts (RootScripts.swift): their values as arguments, the bridge on stdin,
// run as this account on fake rekordbox installs in the temporary folder

rootScriptTests()

// MARK: signed releases (SignedReleases.swift): payload.json's signature, the bootstrap's check,
// build.sh's refusal of an uncommitted tree

signedReleaseTests()
reportTests()
publicReleaseTests()

// MARK: stdin and copying

let big = Data(repeating: 0x5a, count: 5 << 20)
check(runTool("/usr/bin/wc", ["-c"], input: big).1 == String(big.count), "5 MB on stdin arrive whole")
check(runTool("/usr/bin/true", [], input: big).0 == 0, "a program that doesn't read its stdin is no error")
check(runTool("/bin/cat", []).1 == "", "without input, stdin is empty")
check(sha256(data: Data("abc".utf8)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of \"abc\"")

let victim = tmp + "/victim", link = tmp + "/link", src = tmp + "/src"
FileManager.default.createFile(atPath: victim, contents: Data("keep".utf8))
FileManager.default.createFile(atPath: src, contents: Data("new".utf8))
symlink(victim, link)
check(copyFile(src, link) != nil && (try? String(contentsOfFile: victim, encoding: .utf8)) == "keep", "copyFile doesn't write through a link")
check(copyFile(src, tmp + "/copy") == nil && (try? String(contentsOfFile: tmp + "/copy", encoding: .utf8)) == "new", "copyFile copies")
check(copyFile(src, tmp + "/copy") != nil, "copyFile doesn't replace a file")

// MARK: the home folder

let accountDir = String(cString: getpwuid(getuid())!.pointee.pw_dir)
check(home == accountDir, "home is the account's (\(home))")
let fake = Process()
fake.executableURL = URL(fileURLWithPath: me); fake.arguments = ["--home"]
fake.environment = ["CFFIXED_USER_HOME": "/", "HOME": "/"]
let fakeOut = Pipe(); fake.standardOutput = fakeOut
try! fake.run(); fake.waitUntilExit()
let homes = String(decoding: fakeOut.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self).split(separator: "\n").map(String.init)
check(homes.first == accountDir, "with CFFIXED_USER_HOME=/ and HOME=/, home is still the account's")
check(homes.count == 2 && homes[1] == "/", "(Foundation's home there is \(homes.last ?? "?"), which the app doesn't use)")

// MARK: starting

let folder = tmp + "/start", fileThere = folder + "/file", linkThere = folder + "/link"
try! FileManager.default.createDirectory(atPath: folder + "/real", withIntermediateDirectories: true)
FileManager.default.createFile(atPath: fileThere, contents: Data())
symlink(folder + "/real", linkThere)
check(startupProblem(folders: []) == nil, "this account can start")
check(startupProblem(uid: 0, folders: []) != nil, "refuses to run as root")
check(startupProblem(euid: 0, folders: []) != nil, "refuses to run set-uid root")
for h in ["/", "", "relative/home", folder + "/../start", folder + "/missing", fileThere, "/Users"] {
    check(startupProblem(home: h, folders: []) != nil, "refuses home \(h.debugDescription)")
}
check(startupProblem(folders: [folder + "/real", folder + "/missing"]) == nil, "a real or missing folder of ours is fine")
check(startupProblem(folders: [linkThere]) != nil, "refuses a folder of ours that is a link")
check(startupProblem(folders: [fileThere]) != nil, "refuses a folder of ours that is a file")

// MARK: the gate, on a layout in the temporary folder

let g = tmp + "/gate", outside = tmp + "/outside"
let fm = FileManager.default
func mk(_ p: String) { try! fm.createDirectory(atPath: p, withIntermediateDirectories: true) }
func touch(_ p: String) { fm.createFile(atPath: p, contents: Data("x".utf8)) }
func exists(_ p: String) -> Bool { fileType(p) != nil }
func plistApp(_ p: String, id: String) {
    mk(p + "/Contents/MacOS")
    try! (["CFBundleIdentifier": id, "CFBundleExecutable": "RB Stems Plus"] as NSDictionary).write(to: URL(fileURLWithPath: p + "/Contents/Info.plist"))
    touch(p + "/Contents/MacOS/RB Stems Plus")
}
mk(g + "/support/sub"); mk(g + "/logs"); mk(outside + "/dir"); touch(outside + "/file"); touch(outside + "/dir/keep")
symlink(outside, g + "/cache")                                  // a root that is a link
symlink(outside, g + "/support/linkdir")                        // a link to a folder inside a root
symlink(outside + "/file", g + "/support/linkfile")
let gd = Deletable(inside: [g + "/support", g + "/cache", g + "/logs"], roots: [g + "/support", g + "/cache", g + "/logs"],
                   files: [g + "/agent.plist"], apps: [g + "/.RB Stems Plus.update.app", g + "/.RB Stems Plus.old.app"])
func gone(_ p: String, whole: Bool = false) -> Bool { safeRemove(p, whole: whole, gd) && !exists(p) }
func kept(_ p: String, whole: Bool = false) -> Bool { let n = refused.count; return !safeRemove(p, whole: whole, gd) && refused.count == n + 1 }

touch(g + "/support/a"); check(gone(g + "/support/a"), "removes a file inside a root")
mk(g + "/support/d"); symlink(outside + "/dir", g + "/support/d/inner")
check(gone(g + "/support/d") && exists(outside + "/dir/keep"), "removes a folder, not what a link inside points to")
check(gone(g + "/support/linkfile") && exists(outside + "/file"), "removes a final link as a link")
check(kept(g + "/support/linkdir/file") && exists(outside + "/file"), "refuses a path through a linked folder")
check(kept(g + "/support/linkdir/dir") && exists(outside + "/dir/keep"), "refuses a folder through a linked folder")
check(kept(g + "/cache/file") && exists(outside + "/file"), "refuses a path inside a root that is a link")
check(kept(g + "/cache", whole: true) && exists(outside + "/file"), "refuses a root that is a link")
for p in [g + "/support/../outside", g + "/support/sub/..", g + "/support/./sub", g + "/support//sub", g + "/support/sub/",
          "support/sub", "", "/", g, g + "/support/..", tmp, outside + "/file"] {
    check(kept(p) && kept(p, whole: true), "refuses \(p.replacingOccurrences(of: tmp, with: "<tmp>").debugDescription)")
}
check(kept(g + "/support"), "refuses a root itself unless asked for")
check(kept(g + "/elsewhere/support/x"), "refuses a path outside every rule that only ends like one")
check(safeRemove(g + "/support/never/there", whole: false, gd), "a path that isn't there is gone")
touch(g + "/agent.plist"); touch(g + "/agent.plist.bak")
check(gone(g + "/agent.plist"), "removes an exact file")
check(kept(g + "/agent.plist.bak"), "refuses a file next to an exact one")
plistApp(g + "/.RB Stems Plus.update.app", id: bundleID)
check(gone(g + "/.RB Stems Plus.update.app"), "removes a staging app with our bundle ID")
plistApp(g + "/.RB Stems Plus.old.app", id: "com.example.other")
check(kept(g + "/.RB Stems Plus.old.app") && exists(g + "/.RB Stems Plus.old.app/Contents"), "refuses a staging name with another bundle ID")
plistApp(outside + "/Ours.app", id: bundleID)
try! fm.removeItem(atPath: g + "/.RB Stems Plus.old.app"); symlink(outside + "/Ours.app", g + "/.RB Stems Plus.old.app")
check(kept(g + "/.RB Stems Plus.old.app") && exists(outside + "/Ours.app/Contents/Info.plist"), "refuses a staging name that links to our app")
check(gone(g + "/logs", whole: true), "removes a root when asked for")

// the real places: decisions only, nothing is removed
for p in ["/", home, home + "/", home + "/Library", home + "/Library/Caches", home + "/Library/Application Support",
          "/Applications", "/Applications/RB Stems Plus.app", rootDir, support + "/..", support + "/../rbstemsplus",
          cacheDir + "/../..", modelDir, modelDir + "/hdemucs.onnx", agentPlist + "/", "/Library/LaunchAgents/\(agentLabel).plist"] {
    check(!mayRemove(p) && !mayRemove(p, whole: true), "never \(p.replacingOccurrences(of: home, with: "~"))")
}
check(!mayRemove(support) && !mayRemove(cacheDir), "never the roots without asking")
check(mayRemove(payloadRoot + "/0.0.0") && mayRemove(modelDir + "/.hdemucs.new"), "our own paths are allowed")
check(!deletable.inside.contains(home) && !deletable.roots.contains(home) && deletable.inside.allSatisfy { $0.hasPrefix(home + "/Library/") },
      "every root is inside ~/Library")

// MARK: the app bundle

let b = tmp + "/bundles"
let good = b + "/RB Stems Plus.app", exe = good + "/Contents/MacOS/RB Stems Plus"
plistApp(good, id: bundleID)
check(bundleProblem(good, executable: exe) == nil, "a valid app")
plistApp(b + "/Renamed.app", id: bundleID)
check(bundleProblem(b + "/Renamed.app", executable: b + "/Renamed.app/Contents/MacOS/RB Stems Plus") == nil, "a renamed app is still ours")
plistApp(b + "/RB Stems Plus", id: bundleID)
check(bundleProblem(b + "/RB Stems Plus", executable: b + "/RB Stems Plus/Contents/MacOS/RB Stems Plus") != nil, "refuses a folder not named .app")
plistApp(b + "/Other.app", id: "com.example.other")
check(bundleProblem(b + "/Other.app", executable: b + "/Other.app/Contents/MacOS/RB Stems Plus") != nil, "refuses another bundle ID")
symlink(good, b + "/Link.app")
check(bundleProblem(b + "/Link.app", executable: b + "/Link.app/Contents/MacOS/RB Stems Plus") != nil, "refuses an app that is a link")
let translocated = b + "/AppTranslocation/1234/d/RB Stems Plus.app"
plistApp(translocated, id: bundleID)
check(bundleProblem(translocated, executable: translocated + "/Contents/MacOS/RB Stems Plus") != nil, "refuses a translocated app")
check(bundleProblem(b, executable: b + "/RB Stems Plus") != nil, "refuses a bare executable's folder")
check(bundleProblem(good, executable: b + "/RB Stems Plus") != nil, "refuses an executable outside the app")
check(bundleProblem(b + "/x/../RB Stems Plus.app", executable: b + "/x/../RB Stems Plus.app/Contents/MacOS/RB Stems Plus") != nil, "refuses .. in the app's path")
check(ownBundle() == nil, "this test binary isn't a valid app")

// the LaunchAgent: kept, rewritten or removed
check(agentFix(target: exe, own: exe) == .keep, "agent: starts this app: kept")
check(agentFix(target: b + "/Renamed.app/Contents/MacOS/RB Stems Plus", own: exe) == .rewrite, "agent: starts another copy: rewritten")
check(agentFix(target: nil, own: exe) == .rewrite, "agent: starts nothing: rewritten")
check(agentFix(target: exe, own: nil) == .keep, "agent: from an invalid copy, one starting a valid app is kept")
check(agentFix(target: translocated + "/Contents/MacOS/RB Stems Plus", own: nil) == .remove, "agent: from an invalid copy, one starting a translocated app is removed")
check(agentFix(target: "/nowhere/RB Stems Plus.app/Contents/MacOS/RB Stems Plus", own: nil) == .remove, "agent: one starting a missing app is removed")

// writing the plist replaces a link, never writes through it
let plistPath = tmp + "/agent.plist"
touch(outside + "/victim"); symlink(outside + "/victim", plistPath)
check(writeAtomically(Data("new".utf8), to: plistPath) && fileType(plistPath) == S_IFREG
      && (try? String(contentsOfFile: outside + "/victim", encoding: .utf8)) == "x", "the plist write replaces a planted link")

// the self-delete, after its wait for a process that has exited
let done = Process(); done.executableURL = URL(fileURLWithPath: "/usr/bin/true"); try! done.run(); done.waitUntilExit()
func selfDelete(_ app: String) -> Bool {
    runTool("/bin/sh", ["-c", afterExitWait + selfDeleteScript, "rbsp", String(done.processIdentifier), "0.1", app, bundleID])
    return !exists(app)
}
check(!selfDelete(b + "/Link.app") && exists(good + "/Contents/Info.plist"), "self-delete: not a link, nor what it points to")
check(!selfDelete(b + "/Other.app"), "self-delete: not another bundle ID")
check(!selfDelete(b + "/RB Stems Plus"), "self-delete: not a folder without .app")
mk(b + "/x")
check(!selfDelete(b + "/x/../RB Stems Plus.app") && exists(good), "self-delete: not a path with ..")
check(!selfDelete(translocated), "self-delete: not a translocated app")
check(selfDelete(b + "/Renamed.app"), "self-delete: a valid app with our bundle ID")

// MARK: the update: staged next to the app, only our own apps replaced (in the temporary folder,
// with true for lsregister and open: nothing is registered or opened)

/// An app folder of version `v` (Contents/Resources/version), its executable a copy of `exe`
/// (true, or this program, which does the update's --swap as the app does), so codesign signs
/// it as it signs ours.
func fakeApp(_ p: String, id: String = bundleID, v: String, exe: String = "/usr/bin/true") {
    mk(p + "/Contents/MacOS"); mk(p + "/Contents/Resources")
    try! (["CFBundleIdentifier": id, "CFBundleExecutable": "RB Stems Plus"] as NSDictionary).write(to: URL(fileURLWithPath: p + "/Contents/Info.plist"))
    try? fm.copyItem(atPath: exe, toPath: p + "/Contents/MacOS/RB Stems Plus")
    try! v.write(toFile: p + "/Contents/Resources/version", atomically: true, encoding: .utf8)
}
func versionOf(_ app: String) -> String? { try? String(contentsOfFile: app + "/Contents/Resources/version", encoding: .utf8) }
/// One update's places: the app in a folder of its own (not /Applications), the download
/// unpacked in the update folder, the log and the busy marker.
struct UpdateCase {
    let base: String
    var apps: String { base + "/Apps" }; var app: String { apps + "/RB Stems Plus.app" }
    var dir: String { base + "/update" }; var new: String { dir + "/new/RB Stems Plus.app" }
    var log: String { base + "/app.log" }; var busy: String { base + "/busy" }
    var stage: String { apps + "/" + updateStagingName }; var old: String { apps + "/" + updateOldName }
}
var updateN = 0
/// A fresh case: our app at version 1, the download at version 2 (with `newID`), whose
/// executable is this program (it does the exchange).
func updateCase(newID: String = bundleID) -> UpdateCase {
    updateN += 1
    let u = UpdateCase(base: tmp + "/update-\(updateN)")
    fakeApp(u.app, v: "1"); fakeApp(u.new, id: newID, v: "2", exe: me)
    return u
}
/// Runs the update script after its wait, as the app does; `hooks` replace its "# step:"
/// comments. Returns its log line.
func runUpdate(_ u: UpdateCase, hooks: [String: String] = [:]) -> String {
    var body = Controller.updateScript
    for (step, code) in hooks { body = body.replacingOccurrences(of: "# step: \(step)", with: code) }
    runTool("/bin/sh", ["-c", afterExitWait + body, "rbsp", String(done.processIdentifier), "0.1",
                        u.app, u.new, u.dir, u.log, "2", bundleID, u.busy, "/usr/bin/true", "/usr/bin/true"])
    return ((try? String(contentsOfFile: u.log, encoding: .utf8)) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
}
/// The update didn't happen: the app says so at its next launch, no staged copy is left.
func notUpdated(_ u: UpdateCase, _ line: String) -> Bool { exists(u.dir + "/result") && !exists(u.stage) && line.contains("update to 2: failed") }
/// The reason the failed update left for the app's next launch.
func resultOf(_ u: UpdateCase) -> String { ((try? String(contentsOfFile: u.dir + "/result", encoding: .utf8)) ?? "").trimmingCharacters(in: .whitespacesAndNewlines) }

check(runTool("/bin/sh", ["-n", "-c", Controller.updateScript]).0 == 0, "update script: parses")
check(updateFailedMessage(nil).hasPrefix("RB Stems Plus wasn't changed. If macOS said") && !updateFailedMessage(nil).contains("install command"),
      "a failed update doesn't send users to the install command")
check(updateFailedMessage(updateFailureReason("failed sign\n")) == "RB Stems Plus wasn't changed. The new version couldn't be signed. If macOS said RB Stems Plus was prevented from modifying apps, allow it, then choose Update RB Stems Plus again. Otherwise try again later.",
      "a failed update says why in one sentence, then the next step")
check(["app", "download", "staging", "copy", "copy-check", "sign", "swap"].allSatisfy { k in
          updateFailureReason("failed \(k)").map { $0.hasSuffix(".") && $0.first?.isUppercase == true } == true },
      "every reason the update script gives has a sentence")
check(updateFailureReason("failed\n") == nil && updateFailureReason("failed rm -rf") == nil && updateFailureReason("") == nil
      && updateFailureReason("failed sign extra") == nil, "an older or unknown result: no reason, only the next step")
check(Controller.updateScript.components(separatedBy: "# step: ").count == 3, "update script: its two steps are marked")
check(Controller.updateScript.components(separatedBy: "/bin/mv").count == 2 && Controller.updateScript.contains("/bin/mv -f \"$BUSY.$$\" \"$BUSY\""),
      "update script: no app is moved aside (its one mv writes the busy marker): the two apps are exchanged in one step")
check(updateStagingApp == (Bundle.main.bundlePath as NSString).deletingLastPathComponent + "/.RB Stems Plus.update.app"
      && updateOldApp == (Bundle.main.bundlePath as NSString).deletingLastPathComponent + "/.RB Stems Plus.old.app", "the staging names are in the app's own folder")
var u = updateCase()
let probe = u.base + "/staged-next-to-the-app"
var line = runUpdate(u, hooks: ["staged": #"""
    [ -d "$STAGE" ] && [ "${STAGE%/*}" = "${APP%/*}" ] && [ "$(/usr/bin/stat -f %d "$STAGE")" = "$(/usr/bin/stat -f %d "$APP")" ] \
      && [ "$(/usr/bin/head -n 1 "$BUSY")" = "$$" ] && [ -n "$(started $$)" ] && [ "$(/usr/bin/sed -n 2p "$BUSY")" = "$(started $$)" ] \
      && /usr/bin/touch '\#(probe)'
    """#])
check(versionOf(u.app) == "2" && !exists(u.stage) && !exists(u.old) && !exists(u.dir) && !exists(u.busy) && line.hasSuffix("update to 2: updated"),
      "update: the new app in place, nothing staged or old left, the update folder and the busy marker gone (\(line))")
check(exists(probe), "update: staged in the app's own folder on its disk, the busy marker holding the script's pid and start time")
check(runTool("/usr/bin/codesign", ["-dv", u.app]).1.contains("(adhoc,runtime)"), "update: the new app is signed ad hoc with the hardened runtime")

u = updateCase(newID: "com.example.other"); line = runUpdate(u)
check(versionOf(u.app) == "1" && notUpdated(u, line) && resultOf(u) == "failed download", "update: a download with another bundle ID is refused (\(line))")
u = updateCase()
mk(u.base + "/elsewhere"); try! fm.moveItem(atPath: u.new, toPath: u.base + "/elsewhere/RB Stems Plus.app"); symlink(u.base + "/elsewhere/RB Stems Plus.app", u.new)
line = runUpdate(u)
check(versionOf(u.app) == "1" && notUpdated(u, line), "update: a download that is a link is refused")
for target in ["/etc", "..", "../../../../outside", "A/../../..", "../Resources", "a/.."] {
    u = updateCase(); symlink(target, u.new + "/Contents/Resources/link"); line = runUpdate(u)
    check(versionOf(u.app) == "1" && notUpdated(u, line), "update: a link inside the download to \(target.debugDescription) is refused")
}
u = updateCase()
mk(u.new + "/Contents/Resources/A"); touch(u.new + "/Contents/Resources/A/x")
symlink("A", u.new + "/Contents/Resources/Current"); symlink("Current/x", u.new + "/Contents/Resources/x")
line = runUpdate(u)
check(versionOf(u.app) == "1" && notUpdated(u, line), "update: any link inside the download is refused, as by the install command (\(line))")
u = updateCase(); mkfifo(u.new + "/Contents/Resources/fifo", 0o600); line = runUpdate(u)
check(versionOf(u.app) == "1" && notUpdated(u, line), "update: a special file inside the download is refused")

u = updateCase(); try! fm.removeItem(atPath: u.app); line = runUpdate(u)
check(!exists(u.app) && notUpdated(u, line) && resultOf(u) == "failed app", "update: the app gone meanwhile: nothing is put in its place")
u = updateCase(); try! fm.removeItem(atPath: u.app); fakeApp(u.app, id: "com.example.other", v: "other"); line = runUpdate(u)
check(versionOf(u.app) == "other" && notUpdated(u, line), "update: another app at the app's place is left alone")
u = updateCase()
mk(u.base + "/real"); try! fm.moveItem(atPath: u.app, toPath: u.base + "/real/RB Stems Plus.app"); symlink(u.base + "/real/RB Stems Plus.app", u.app)
line = runUpdate(u)
check(fileType(u.app) == S_IFLNK && versionOf(u.base + "/real/RB Stems Plus.app") == "1" && notUpdated(u, line), "update: an app that is a link is refused, nor what it points to changed")

u = updateCase(); fakeApp(u.stage, id: "com.example.other", v: "x"); line = runUpdate(u)
check(versionOf(u.app) == "1" && versionOf(u.stage) == "x" && resultOf(u) == "failed staging", "update: another app at a staging name is left alone, no update")
u = updateCase(); fakeApp(u.base + "/ext/Ours.app", v: "ext"); try? fm.removeItem(atPath: u.stage); symlink(u.base + "/ext/Ours.app", u.stage); line = runUpdate(u)
check(versionOf(u.app) == "1" && fileType(u.stage) == S_IFLNK && versionOf(u.base + "/ext/Ours.app") == "ext" && exists(u.dir + "/result") && line.contains("update to 2: failed"),
      "update: a link at the staging name is left alone, nor what it points to")
u = updateCase(); fakeApp(u.stage, v: "left"); fakeApp(u.old, v: "left"); line = runUpdate(u)
check(versionOf(u.app) == "2" && !exists(u.stage) && versionOf(u.old) == "left",
      "update: our own leftover at the staging name is removed, then the update goes ahead (1.0's old-app name is the app's to clean up)")

// the exchange: one step, so the app's place is never empty, however the script is stopped
u = updateCase(); line = runUpdate(u, hooks: ["staged": #"/bin/kill -9 $$"#])
check(versionOf(u.app) == "1" && versionOf(u.stage) == "2", "update: killed before the exchange: the old app is in place (the staged copy a leftover)")
u = updateCase(); line = runUpdate(u, hooks: ["swapped": #"/bin/kill -9 $$"#])
check(versionOf(u.app) == "2" && versionOf(u.stage) == "1", "update: killed right after the exchange: the new app is in place (the old one a leftover)")
u = updateCase(); line = runUpdate(u, hooks: ["staged": #"/bin/rm -rf "$APP""#])
check(!exists(u.app) && notUpdated(u, line), "update: the app gone before the exchange: nothing is put in its place")
u = updateCase(); line = runUpdate(u, hooks: ["staged": #"/bin/rm -rf "$APP"; /bin/mkdir -p "$APP/Contents"; /usr/bin/printf x > "$APP/Contents/other""#])
check(exists(u.app + "/Contents/other") && !exists(u.app + "/" + updateStagingName) && notUpdated(u, line),
      "update: something else at the app's place before the exchange: left alone, nothing goes inside it")
u = updateCase(); line = runUpdate(u, hooks: ["staged": #"E=missing"#])
check(versionOf(u.app) == "1" && notUpdated(u, line) && line.contains("couldn't exchange") && resultOf(u) == "failed swap", "update: the exchange can't run: the old app stays (\(line))")
u = updateCase(); line = runUpdate(u, hooks: ["staged": #"E=fake; /usr/bin/printf '#!/bin/sh\nexit 0\n' > "$STAGE/Contents/MacOS/fake"; /bin/chmod +x "$STAGE/Contents/MacOS/fake""#])
check(versionOf(u.app) == "1" && notUpdated(u, line), "update: an exchange that says it worked but didn't: decided by what is at the app's place")

// the update's log: never written through a link
u = updateCase(); touch(u.base + "/victim"); symlink(u.base + "/victim", u.log); _ = runUpdate(u)
check(versionOf(u.app) == "2" && fileType(u.log) == S_IFLNK && (try? String(contentsOfFile: u.base + "/victim", encoding: .utf8)) == "x",
      "update: a link at the log's place is left alone, nothing written through it")

// the exchange itself (swapApps, the app's --swap), on fake apps
let sw = tmp + "/swap"
func swapCase() -> (String, String) {
    try? fm.removeItem(atPath: sw); fakeApp(sw + "/A/" + updateStagingName, v: "new"); fakeApp(sw + "/A/RB Stems Plus.app", v: "old")
    return (sw + "/A/" + updateStagingName, sw + "/A/RB Stems Plus.app")
}
var (st, ap) = swapCase()
check(swapApps(st, ap) == nil && versionOf(ap) == "new" && versionOf(st) == "old", "swap: the two apps are exchanged")
(st, ap) = swapCase()
check(runTool(me, ["--swap", st, ap]).0 == 0 && versionOf(ap) == "new", "swap: as --swap, exit 0 once exchanged")
(st, ap) = swapCase(); fakeApp(sw + "/B/RB Stems Plus.app", v: "b")
check(swapApps(st, sw + "/B/RB Stems Plus.app") != nil && versionOf(st) == "new" && versionOf(sw + "/B/RB Stems Plus.app") == "b", "swap: refuses apps in different folders")
(st, ap) = swapCase(); fakeApp(sw + "/A/Other.app", id: "com.example.other", v: "other")
check(swapApps(st, sw + "/A/Other.app") != nil && versionOf(sw + "/A/Other.app") == "other", "swap: refuses another bundle ID")
check(runTool(me, ["--swap", st, sw + "/A/Other.app"]).0 == 1 && versionOf(st) == "new", "swap: as --swap, exit 1 when refused")
(st, ap) = swapCase(); fakeApp(sw + "/ext/Ours.app", v: "ext"); symlink(sw + "/ext/Ours.app", sw + "/A/Link.app")
check(swapApps(st, sw + "/A/Link.app") != nil && fileType(sw + "/A/Link.app") == S_IFLNK && versionOf(sw + "/ext/Ours.app") == "ext", "swap: refuses an app that is a link")
check(swapApps(st, sw + "/A/Missing.app") != nil && versionOf(st) == "new", "swap: refuses a missing app")
mk(sw + "/A/Plain"); try? fm.copyItem(atPath: ap + "/Contents", toPath: sw + "/A/Plain/Contents")
check(swapApps(st, sw + "/A/Plain") != nil && versionOf(ap) == "old", "swap: refuses a folder not named .app")
check(swapApps(st, st) != nil && swapApps(st, sw + "/A/x/../RB Stems Plus.app") != nil && versionOf(ap) == "old", "swap: refuses the same app twice, and ..")

// MARK: our models and Pioneer's (rekordbox's model folder and the originals, in the temporary folder)

let md = tmp + "/models", rbm = md + "/rb", orig = md + "/originals", rbModel = rbm + "/hdemucs.onnx"
mk(rbm); mk(orig)
let oursData = Data("our model".utf8), dataA = Data("Pioneer's model for engine 0002".utf8), dataB = Data("Pioneer's model for engine 0003".utf8)
let oursSha = sha256(data: oursData), shaA = sha256(data: dataA), shaB = sha256(data: dataB)
let ourSource = md + "/ours.onnx"
fm.createFile(atPath: ourSource, contents: oursData)
let mgate = Deletable(inside: [orig], roots: [], files: [rbm + "/.hdemucs.new", rbm + "/.hdemucs.restore"], apps: [])
var mlog: [String] = []
func mm() -> Models { Models(dir: rbm, originals: orig, ours: [oursSha], gate: mgate, log: { mlog.append($0) }) }
func put(_ d: Data, _ p: String) { try? fm.removeItem(atPath: p); fm.createFile(atPath: p, contents: d) }
func setEngine(_ e: String) { try! e.write(toFile: rbm + "/demucs3_ver.txt", atomically: true, encoding: .utf8) }
func live() -> String? { fileType(rbModel) == nil ? nil : sha256(rbModel) }
func clearOriginals() { try? fm.removeItem(atPath: orig); mk(orig) }
func noTemps() -> Bool { !exists(rbm + "/.hdemucs.new") && !exists(rbm + "/.hdemucs.restore") }

// what is ours: compiled in, plus payload.json's model and previous_models; nothing else
let payloadJSON = { (prev: String) in Data("""
    {"payload_version": "1.1.0", "app": {"version": "1.1.0", "zip": "a.zip", "sha256": "\(shaA)"},
     "bridge": {"file": "b.dylib", "sha256": "\(shaA)"}, "model": {"file": "m.onnx", "sha256": "\(shaB)", "size": 9}\(prev),
     "compatible": {"rekordbox": ["7.2.19"], "ort_version_prefix": "1.18.", "stems_engine": ["0002"]}}
    """.utf8) }
check(ourModelChecksums(nil) == [ourModel], "ours without a payload.json: the compiled-in model only")
// (a signed payload.json's model and previous_models: SignedReleases.swift)
check(decodeManifest(payloadJSON("")) != nil, "a payload.json without previous_models is still valid")
check(decodeManifest(payloadJSON(", \"previous_models\": [\"../x\"]")) == nil, "a payload.json with a bad previous_models entry is refused")

// our model offered as Pioneer's: never saved
put(oursData, rbModel); setEngine("0002")
check(!mm().saveOriginal(oursSha).ok && ((try? fm.contentsOfDirectory(atPath: orig)) ?? []).isEmpty, "our model is never saved as Pioneer's")
check(mm().installOurs(from: ourSource, sha: oursSha).ok && live() == oursSha && noTemps()
      && ((try? fm.contentsOfDirectory(atPath: orig)) ?? []).isEmpty, "installing over our own model saves nothing")

// installing over Pioneer's: saved first, by checksum, with its engine version
put(dataA, rbModel)
check(mm().installOurs(from: ourSource, sha: oursSha).ok && live() == oursSha && noTemps(), "install over Pioneer's model")
check(sha256(orig + "/\(shaA).onnx") == shaA && (try? String(contentsOfFile: orig + "/\(shaA).engine", encoding: .utf8)) == "0002\n",
      "Pioneer's model is saved as <sha>.onnx with its engine version")
let inodeA = (try? fm.attributesOfItem(atPath: orig + "/\(shaA).onnx"))?[.systemFileNumber] as? Int
put(dataA, rbModel)
check(mm().saveOriginal(shaA).ok && (try? fm.attributesOfItem(atPath: orig + "/\(shaA).onnx"))?[.systemFileNumber] as? Int == inodeA,
      "a saved original is never replaced")
check(!mm().saveOriginal(shaB).ok && !exists(orig + "/\(shaB).onnx") && !exists(orig + "/\(shaB).engine"),
      "a model whose checksum isn't the one taken isn't saved, nor its engine version")
put(Data("damaged".utf8), orig + "/\(shaB).onnx"); put(dataB, rbModel); setEngine("0003")
check(mm().saveOriginal(shaB).ok && sha256(orig + "/\(shaB).onnx") == shaB && exists(orig + "/\(shaB).onnx.damaged"),
      "a damaged saved copy is set aside, not used")

// which one goes back: the current engine's, else the highest known, else one without a version
put(Data("half".utf8), orig + "/\(String(repeating: "c", count: 64)).onnx.part")
put(oursData, orig + "/\(oursSha).onnx")
setEngine("0003"); check(mm().savedOriginals().map(\.sha) == [shaB, shaA], "engine 0003: its own original first; no .part, never ours")
setEngine("0002"); check(mm().savedOriginals().first?.sha == shaA, "engine 0002: its own original first")
setEngine("0009"); check(mm().savedOriginals().first?.sha == shaB, "an unknown engine: the highest known version first")
setEngine("0002"); put(oursData, rbModel)
check(mm().restore().ok && live() == shaA && noTemps(), "restore puts back the current engine's original")
put(dataB, rbModel)
check(mm().restore().ok && live() == shaB, "restore leaves a model that isn't ours alone")

// restore when rekordbox's model is missing (its folder is there), and not without the folder
try? fm.removeItem(atPath: rbModel); setEngine("0003")
check(mm().restore().ok && live() == shaB, "restore when rekordbox's model is missing")
let noFolder = Models(dir: md + "/missing", originals: orig, ours: [oursSha], gate: mgate)
check(noFolder.restore().ok && !exists(md + "/missing"), "no model folder: nothing put back, nothing created")

// restore failing midway: the copy doesn't verify
clearOriginals(); put(oursData, rbModel); setEngine("0002")
put(Data("not what its name says".utf8), orig + "/\(shaA).onnx"); put(Data("0002\n".utf8), orig + "/\(shaA).engine")
var r = mm().restore()
check(!r.ok && live() == oursSha && noTemps() && exists(orig + "/\(shaA).onnx"), "a restore that doesn't verify fails, leaves no temp, deletes nothing (\(r.message))")
// ... and when the copy can't be made
put(dataA, orig + "/\(shaA).onnx"); chmod(rbm, 0o555)
r = mm().restore()
chmod(rbm, 0o755)
check(!r.ok && live() == oursSha && noTemps(), "a restore that can't copy fails and changes nothing (\(r.message))")
// ... and with nothing saved, our model stays and the caller is told
clearOriginals()
r = mm().restore()
check(!r.ok && live() == oursSha && r.message.contains("no saved copy"), "no saved original: restore fails, our model stays")
try? fm.removeItem(atPath: rbModel)
check(mm().restore().ok && live() == nil, "no saved original and no model: nothing to put back")

// install refuses to replace a model it couldn't save
clearOriginals(); put(dataB, rbModel); chmod(orig, 0o555)
r = mm().installOurs(from: ourSource, sha: oursSha)
chmod(orig, 0o755)
check(!r.ok && live() == shaB && noTemps(), "install refuses to replace a model whose original isn't saved (\(r.message))")
check(!mm().installOurs(from: ourSource, sha: shaA).ok && live() == shaB && noTemps(), "install refuses a copy that doesn't match its checksum")

// 1.0's layout: Pioneer's moves in, ours is set aside and never used
clearOriginals(); put(oursData, rbModel); setEngine("0002"); put(dataA, orig + "/hdemucs.onnx")
check(!mm().migrateLegacy() && sha256(orig + "/\(shaA).onnx") == shaA && !exists(orig + "/hdemucs.onnx")
      && (try? String(contentsOfFile: orig + "/\(shaA).engine", encoding: .utf8)) == "0002\n", "1.0's saved Pioneer model moves to the new layout")
clearOriginals(); put(oursData, orig + "/hdemucs.onnx")
check(mm().migrateLegacy() && exists(orig + "/ours-\(oursSha).onnx") && !exists(orig + "/hdemucs.onnx"), "1.0's saved model that is ours is set aside, and the user is told")
check(mm().savedOriginals().isEmpty && !mm().restore().ok && live() == oursSha, "a set-aside model of ours is never put back")
check(!mm().migrateLegacy(), "the migration runs once")

// MARK: what stays after "Remove RB Stems Plus anyway"

put(oursData, rbModel)
check(modelLeftBehind(mm()) == "rekordbox still has the Demucs v4 model.", "anyway: says rekordbox keeps our model")
try? fm.removeItem(atPath: rbModel)
check(modelLeftBehind(mm()) == "rekordbox has no stems model.", "anyway: says rekordbox has no model")

// MARK: a model RB Stems Plus doesn't know is Pioneer's only if nothing says otherwise (it may be
// a newer Demucs v4 model whose payload.json is missing)

let dataX = Data("a model nobody named".utf8), shaX = sha256(data: dataX)
clearOriginals(); setEngine("0002"); put(dataA, rbModel)
check(mm().installOurs(from: ourSource, sha: oursSha).ok && mm().verifiedOriginals() == [shaA], "unknown model: (Pioneer's saved for engine 0002)")
put(dataX, rbModel)
r = mm().restore()
check(!r.ok && r.message == unknownModelMessage && live() == shaX && mm().verifiedOriginals() == [shaA] && noTemps(),
      "unknown model, same engine as the saved original: restore fails, nothing changes (\(r.message))")
check(modelLeftBehind(mm()) == "rekordbox's stems model isn't one RB Stems Plus knows.", "anyway: says rekordbox's model isn't known")
r = mm().installOurs(from: ourSource, sha: oursSha)
check(!r.ok && live() == shaX && !exists(orig + "/\(shaX).onnx") && noTemps(), "unknown model, same engine: install neither saves it as Pioneer's nor replaces it")
setEngine("0003")
check(mm().restore().ok && live() == shaX, "unknown model with a new STEMS Engine: taken for Pioneer's")
check(mm().installOurs(from: ourSource, sha: oursSha).ok && mm().savedIsGood(shaX) && live() == oursSha, "unknown model with a new STEMS Engine: install saves it as Pioneer's")
put(dataB, rbModel)
check(!mm().restore().ok && live() == shaB, "another unknown model for an engine whose original is saved: refused")
put(dataA, rbModel); setEngine("0009")
check(mm().restore().ok && live() == shaA, "a saved original of Pioneer's, whatever the engine: in place")
try? fm.removeItem(atPath: orig + "/\(shaA).engine"); put(dataB, rbModel)
check(!mm().restore().ok && live() == shaB, "a saved original whose engine isn't known may be this engine's: refused")
try? fm.removeItem(atPath: rbm + "/demucs3_ver.txt")
check(!mm().restore().ok && live() == shaB, "rekordbox's engine version not known: refused")
clearOriginals(); setEngine("0002")
check(mm().restore().ok && live() == shaB, "nothing saved: taken for Pioneer's (there's no saved copy to protect)")

// rekordbox's model changing between the checks and the rename: left as it is
clearOriginals(); put(dataA, rbModel)
check(mm().installOurs(from: ourSource, sha: oursSha).ok, "changed meanwhile: (our model in place, Pioneer's saved)")
var changing = mm(); changing.willReplace = { put(dataB, rbModel) }
r = changing.restore()
check(!r.ok && live() == shaB && noTemps() && r.message.contains("changed meanwhile"), "restore: rekordbox's model changed just before the rename: left as it is (\(r.message))")
put(dataA, rbModel); r = changing.installOurs(from: ourSource, sha: oursSha)
check(!r.ok && live() == shaB && noTemps(), "install: rekordbox's model changed just before the rename: left as it is (\(r.message))")

// MARK: "Remove RB Stems Plus anyway": never for a full disk, never deleting a saved original that verifies

var asked: [(title: String, text: String, buttons: [String])] = []
func anyway(_ r: Outcome, answer: String) -> AnywayChoice {
    asked = []
    return removeAnywayChoice(r, mm(), ask: { asked.append(($0, $1, $2)); return answer }, log: { _ in })
}
clearOriginals(); put(dataA, rbModel); setEngine("0002")
_ = mm().installOurs(from: ourSource, sha: oursSha)
let full = Outcome(ok: false, message: "There isn't enough free space to put rekordbox's own stems model back.", noSpace: true)
check(anyway(full, answer: "OK") == .stop && asked.count == 1 && asked[0].title == "Uninstall stopped" && asked[0].buttons == ["OK"]
      && asked[0].text == "There isn't enough free space to put rekordbox's own stems model back. Nothing was deleted. Free up some space and try again.",
      "anyway: a full disk says to free some space, and offers nothing")
let lasting = Outcome(ok: false, message: unknownModelMessage)
check(anyway(lasting, answer: "Cancel") == .stop && asked.count == 1 && asked[0].buttons == ["Cancel", removeAnywayButton, "Open Help"]
      && asked[0].text == unknownModelMessage + " Nothing was deleted.\n\nRemove RB Stems Plus Anyway deletes only its own files and keeps its saved copy of rekordbox's own stems model. To get rekordbox's own stems model back, click Open Help.",
      "anyway: offered with Cancel first, saying the saved copy stays; cancelled: nothing")
check(anyway(lasting, answer: "Open Help") == .help(url: removeAnywayHelpURL) && modelHelpURL(mm()) == removeAnywayHelpURL,
      "anyway: Open Help stops, nothing deleted, and opens how to put the saved copy back")
if case .remove(let about, let keep) = anyway(lasting, answer: removeAnywayButton) {
    check(keep && about.contains("The saved copy of rekordbox's own stems model stays in \(orig).") && about.hasPrefix("rekordbox still has the Demucs v4 model."),
          "anyway: a saved original that verifies is kept, and the user told where (\(about))")
} else { check(false, "anyway: a saved original that verifies is kept, and the user told where") }
put(Data("damaged".utf8), orig + "/\(shaA).onnx")
if case .remove(let about, let keep) = anyway(Outcome(ok: false, message: "The saved copy of rekordbox's own stems model is damaged."), answer: removeAnywayButton) {
    check(!keep && !asked[0].text.contains("keeps") && about == "rekordbox still has the Demucs v4 model. To get rekordbox's own stems model back, click Open Help.",
          "anyway: with no saved original that verifies, everything goes")
} else { check(false, "anyway: with no saved original that verifies, everything goes") }
check(anyway(Outcome(ok: false, message: "x"), answer: "Open Help") == .help(url: noSavedModelHelpURL) && modelHelpURL(mm()) == noSavedModelHelpURL,
      "anyway: with no saved original that verifies, Open Help shows how to do without one")

// the complete uninstall's confirmation: "exactly how it was" only when that holds
check(uninstallEverythingMessage(othersCache: false, noSavedModel: false, stems: " (1.2 GB)")
      == "rekordbox goes back to exactly how it was. Your music and rekordbox library aren't touched.\n\nRB Stems Plus will be deleted, including the stems it saved (1.2 GB).",
      "uninstall confirmation: the approved text when rekordbox can go back exactly")
for (others, noSaved) in [(true, false), (false, true), (true, true)] {
    let m = uninstallEverythingMessage(othersCache: others, noSavedModel: noSaved, stems: "")
    check(!m.contains("exactly") && m.contains("another account") == others && m.contains("no saved copy") == noSaved
          && m.hasSuffix("RB Stems Plus will be deleted, including the stems it saved."),
          "uninstall confirmation: no promise when \(others ? "Stems Cache is another account's" : "")\(others && noSaved ? " and " : "")\(noSaved ? "there's no saved model" : "")")
}
check(uninstallEverythingMessage(othersCache: true, noSavedModel: true, stems: "").hasPrefix(
        "There's no saved copy of rekordbox's own stems model to put back. Stems Cache was installed from another account on this Mac. Your music")
      && uninstallEverythingMessage(othersCache: true, noSavedModel: false, stems: "").hasPrefix(
        "rekordbox gets its own stems model back. Stems Cache was installed from another account on this Mac. Your music"),
      "uninstall confirmation: the unusual cases state the fact only")

// deleting this account's folders, with or without the originals
let own = tmp + "/own", osup = own + "/support", oorig = osup + "/originals", ocache = own + "/cache", ologs = own + "/logs"
let ogate = Deletable(inside: [osup, ocache, ologs], roots: [osup, ocache, ologs], files: [], apps: [])
func ownLayout() {
    try? fm.removeItem(atPath: own); mk(oorig); mk(osup + "/payload/1.1.0"); mk(ocache + "/ab"); mk(ologs)
    for f in [osup + "/installed", osup + "/busy", oorig + "/\(shaA).onnx", oorig + "/\(shaA).engine", ocache + "/ab/x.flac", ologs + "/app.log"] { touch(f) }
}
func removeOwn(keep: Bool) -> Bool { removeOwnFolders(keepOriginals: keep, support: osup, originals: oorig, others: [ocache, ologs], ogate) }
ownLayout()
check(removeOwn(keep: true) && (try? fm.contentsOfDirectory(atPath: osup)) == ["originals"] && exists(oorig + "/\(shaA).onnx")
      && exists(oorig + "/\(shaA).engine") && !exists(ocache) && !exists(ologs), "anyway: the originals folder stays, everything else of ours goes")
ownLayout()
check(removeOwn(keep: false) && !exists(osup) && !exists(ocache) && !exists(ologs), "uninstall: all of this account's folders go")
ownLayout(); try? fm.removeItem(atPath: ocache); touch(outside + "/stem"); symlink(outside, ocache)
check(!removeOwn(keep: false) && fileType(ocache) == S_IFLNK && exists(outside + "/stem") && !exists(osup) && !exists(ologs),
      "a saved stems folder that is a link: refused, nor what it points to; the rest goes")

// the final alert: what stays
check(ownFilesRemovedText("rekordbox is back to how it was.", app: .deleted, othersCache: false, cacheLink: false) == "rekordbox is back to how it was.",
      "final: nothing to add")
check(ownFilesRemovedText("A.", app: .otherAccount, othersCache: false, cacheLink: false) == "A. RB Stems Plus stays in the Applications folder: it belongs to another account on this Mac."
      && ownFilesRemovedText("A.", app: .cantDelete, othersCache: false, cacheLink: false) == "A. RB Stems Plus couldn't delete itself: drag it to the Trash.",
      "final: says when the app stays")
check(ownFilesRemovedText("A.", app: .deleted, othersCache: true, cacheLink: false) == "A. Other accounts on this Mac that use RB Stems Plus need to paste the install command again.",
      "final: the app deleted while Stems Cache stays for another account: says they need the install command")
check(!ownFilesRemovedText("A.", app: .otherAccount, othersCache: true, cacheLink: false).contains("Other accounts on this Mac that use")
      && !ownFilesRemovedText("A.", app: .cantDelete, othersCache: true, cacheLink: false).contains("Other accounts on this Mac that use"),
      "final: the app not deleted: nothing about other accounts' install")
check(ownFilesRemovedText("A.", app: .deleted, othersCache: false, cacheLink: true) == "A. " + cacheLinkLine, "final: says the saved stems folder is a link")

// MARK: the app is deleted only by the account that owns it

touch(tmp + "/owned")
check(ownedByThisAccount(tmp + "/owned"), "an app this account owns may be deleted")
check(!ownedByThisAccount(tmp + "/owned", uid: getuid() + 1), "an app another account owns is left")
check(!ownedByThisAccount(tmp + "/nothing"), "nothing there: not ours to delete")

// MARK: removing Stems Cache once rekordbox is gone: the root folder only if the user agrees

/// removeCache with these answers; returns its result and the steps it took, in order.
func cacheRemoval(folder: Bool, root: Bool = true, run: Controller.RootOutcome = .done, agree: Bool = true,
                  removed: Controller.RootOutcome = .done) -> (CacheRemoval, [String]) {
    var steps: [String] = []
    let r = removeCache(CacheRemovalSteps(
        rekordboxFolder: { folder }, rootFolder: { root },
        uninstall: { steps.append("uninstall"); return run },
        askRootFolder: { steps.append("ask"); return agree },
        removeRootFolder: { steps.append("remove-root-folder"); return removed },
        log: { _ in }))
    return (r, steps)
}
var rc = cacheRemoval(folder: true)
check(rc.0 == .removed && rc.1 == ["uninstall"], "rekordbox there: the uninstall alone, nothing asked")
rc = cacheRemoval(folder: true, run: .failed(RootExit.notPioneers))
check(rc.0 == .failed(RootExit.notPioneers) && rc.1 == ["uninstall"], "rekordbox's folder without rekordbox.app (exit 10): failed, the root folder never offered")
rc = cacheRemoval(folder: true, run: .failed(Controller.cancelled))
check(rc.0 == .failed(Controller.cancelled) && rc.1 == ["uninstall"], "password cancelled: failed, nothing asked")
rc = cacheRemoval(folder: true, run: .rekordboxMissing)
check(rc.0 == .removed && rc.1 == ["uninstall", "ask", "remove-root-folder"], "rekordbox gone during the run: asked, then the root folder removed")
rc = cacheRemoval(folder: false)
check(rc.0 == .removed && rc.1 == ["ask", "remove-root-folder"], "rekordbox's folder missing: no uninstall run (one password), asked, removed")
rc = cacheRemoval(folder: false, agree: false)
check(rc.0 == .keptRootFolder && rc.1 == ["ask"], "the user cancels: the root folder stays, nothing run")
rc = cacheRemoval(folder: false, removed: .failed(RootExit.alreadyRunning))
check(rc.0 == .failed(RootExit.alreadyRunning) && rc.1 == ["ask", "remove-root-folder"], "removing the root folder fails: failed, with the script's code")
rc = cacheRemoval(folder: true, run: .failed(RootExit.installerRunning))
check(cacheFailureReason(rc.0) == "Wait until rekordbox's installer or updater has finished, then try again.",
      "Uninstall stopped says the root script's own reason (\(cacheFailureReason(rc.0)))")
check(cacheFailureReason(.failed(RootExit.rolledBack)) == "rekordbox has its original files again. See the log in this window."
      && cacheFailureReason(.failed(Controller.cancelled)) == "See the log in this window." && cacheFailureReason(.removed) == "See the log in this window.",
      "Uninstall stopped: otherwise the log")
check(rootFolderCleanupMessage.contains("If you moved it, click Cancel and put it back first."), "the root-folder question mentions a moved rekordbox")
check(cacheRemoval(folder: true, run: .failed(Controller.cancelled)).0.userStopped
      && !CacheRemoval.failed(RootExit.alreadyRunning).userStopped && !CacheRemoval.failed(Controller.notRun).userStopped
      && !CacheRemoval.removed.userStopped && !CacheRemoval.keptRootFolder.userStopped,
      "Uninstall stopped: no alert after a cancel, as everywhere")

// MARK: the password sheet asks until the password is right or the user cancels

/// askUntilAccepted with these answers (nil: Cancel) and "right" as the password; returns the
/// result and, per sheet shown, whether it said "Wrong password".
func passwords(_ answers: [String?]) -> (Bool, [Bool]) {
    var a = answers, shown: [Bool] = []
    let ok = askUntilAccepted(ask: { again in shown.append(again); return a.isEmpty ? nil : a.removeFirst() },
                              accepted: { $0 == "right" }, wrong: {})
    return (ok, shown)
}
check(passwords(["right"]) == (true, [false]), "password: right the first time")
check(passwords(["a", "b", "c", "d", "right"]) == (true, [false, true, true, true, true]),
      "password: no limit on tries; each sheet after a wrong one says Wrong password")
check(passwords(["a", nil]) == (false, [false, true]), "password: Cancel stops, after any number of tries")
check(keptRootFolderMessage.contains("nothing else was deleted"), "keeping the copy of rekordbox's files says the complete uninstall stopped")

// MARK: what a root run says: only the Stems Cache install takes long; a reinstall is named so

check(Controller.rootRunTexts("install") == ("Installing Stems Cache (this takes about a minute)…", "Install failed")
      && Controller.rootRunTexts("reinstall") == ("Reinstalling Stems Cache (this takes about a minute)…", "Reinstall failed"),
      "root run: install and reinstall say they take about a minute, and fail as themselves")
check(["uninstall", "remove-root-folder"].allSatisfy { !Controller.rootRunTexts($0).status.contains("minute") && Controller.rootRunTexts($0).failedTitle == "Uninstall failed" },
      "root run: uninstalls don't claim to take long")

// MARK: the confirmations say what happens, never mentioning the password

let floorLine = " Stems Cache will stop saving stems while your disk has less than 26 GB free."
check(installNote("Install Demucs v4", reinstall: false, paused: 50_000_000_000)
      == "Replaces rekordbox's stems model with Demucs v4. rekordbox's own model is kept, so it can be put back."
      && installNote("Install Stems Cache", reinstall: false, paused: nil) == "Saves the stems rekordbox separates, with either stems model, so they load much faster the next time."
      && installNote("Install Demucs v4 + Stems Cache", reinstall: false, paused: nil)
         == "Replaces rekordbox's stems model with Demucs v4, and saves the stems it separates so they load much faster the next time."
      && installNote("Install Stems Cache", reinstall: false, paused: 25_600_000_000)
         == "Saves the stems rekordbox separates, with either stems model, so they load much faster the next time." + floorLine,
      "install: what the command does, and the free-space floor only for Stems Cache")
check(installNote("Reinstall Demucs v4", reinstall: true, paused: 25_600_000_000) == "Installs the latest version of Demucs v4 again."
      && installNote("Reinstall Demucs v4 + Stems Cache", reinstall: true, paused: nil) == "Installs the latest version of Demucs v4 and Stems Cache again."
      && installNote("Reinstall Stems Cache", reinstall: true, paused: nil) == "Installs the latest version of Stems Cache again."
      && installNote("Reinstall Demucs v4 + Stems Cache", reinstall: true, paused: 25_600_000_000)
         == "Installs the latest version of Demucs v4 and Stems Cache again." + floorLine,
      "reinstall: names what the button names, and the free-space floor as Install does")
check(uninstallNote("Uninstall Demucs v4") == "Puts rekordbox's own stems model back."
      && uninstallNote("Uninstall Stems Cache") == "Stops saving stems and puts rekordbox's original files back. The stems already saved are kept.",
      "uninstall: what the command does, and the saved stems kept")
check(([installNote("Install Demucs v4 + Stems Cache", reinstall: false, paused: 50_000_000_000),
        installNote("Reinstall Demucs v4 + Stems Cache", reinstall: true, paused: 50_000_000_000),
        uninstallNote("Uninstall Demucs v4"), uninstallNote("Uninstall Stems Cache"), uninstallEverythingMessage(othersCache: true, noSavedModel: true, stems: "")]
       + ["install", "reinstall", "uninstall", "remove-root-folder"].map { Controller.rootRunTexts($0).status })
      .allSatisfy { !$0.lowercased().contains("password") }, "no confirmation or status line mentions the password")
check([installNote("Install Demucs v4 + Stems Cache", reinstall: false, paused: nil), installNote("Reinstall Stems Cache", reinstall: true, paused: nil),
       uninstallEverythingMessage(othersCache: true, noSavedModel: true, stems: "")]
      .allSatisfy { !$0.contains("permission") && !$0.contains("ask") && !$0.contains("quit") },
      "confirmations say what the command does, not side events (permission prompts, questions, quitting)")

// MARK: the help pages

check(troubleshootingURL == "https://github.com/Gabe-LS/rbstemsplus/blob/main/docs/troubleshooting.md"
      && noSavedModelHelpURL.hasPrefix(troubleshootingURL + "#") && removeAnywayHelpURL.hasPrefix(troubleshootingURL + "#"),
      "Help › RB Stems Plus Troubleshooting opens the troubleshooting page; its sections are on it")

// MARK: a standard account: why the Stems Cache buttons are off

check(standardAccountLine(admin: true, cacheTraces: true, cacheChosen: true) == nil, "standard account line: none for an administrator")
check(standardAccountLine(admin: false, cacheTraces: false, cacheChosen: false) == nil, "standard account line: none with nothing of Stems Cache to remove")
check(standardAccountLine(admin: false, cacheTraces: true, cacheChosen: false) == "Removing Stems Cache needs an administrator account.",
      "standard account line: Stems Cache from another account, its uninstall buttons off: says why")
check(standardAccountLine(admin: false, cacheTraces: true, cacheChosen: true) == "Reinstalling or removing Stems Cache needs an administrator account.",
      "standard account line: this account's Stems Cache: Reinstall and the uninstalls are off: says why")

// MARK: the watcher's Reinstall Now: only what went missing

func parts(_ p: (model: Bool, cache: Bool)) -> [Bool] { [p.model, p.cache] }
check(parts(missingParts(chosen: (true, true), plusPresent: false, cachePresent: true)) == [true, false]
      && reinstallLabel((true, false)) == "Reinstall Demucs v4",
      "Reinstall Now after Demucs v4 is off: Demucs v4 alone (no password), the intact Stems Cache left alone")
check(parts(missingParts(chosen: (true, true), plusPresent: true, cachePresent: false)) == [false, true]
      && reinstallLabel((false, true)) == "Reinstall Stems Cache", "Reinstall Now after a rekordbox update: Stems Cache alone")
check(parts(missingParts(chosen: (true, true), plusPresent: false, cachePresent: false)) == [true, true]
      && reinstallLabel((true, true)) == "Reinstall Demucs v4 + Stems Cache", "Reinstall Now with both off: both")
check(parts(missingParts(chosen: (true, false), plusPresent: true, cachePresent: false)) == [false, false]
      && parts(missingParts(chosen: (false, false), plusPresent: false, cachePresent: false)) == [false, false],
      "Reinstall Now: never what wasn't chosen, nothing when nothing chosen went missing")

// MARK: Stems Cache offline: from a saved payload.json at most 7 days old, with its bridge here

let day: TimeInterval = 86_400
check(cacheFromSavedPayload(unverified: false, age: 2 * day, bridgeVerified: true), "offline: a 2-day-old saved payload.json with its bridge: allowed")
check(cacheFromSavedPayload(unverified: false, age: 7 * day, bridgeVerified: true) && !cacheFromSavedPayload(unverified: false, age: 7 * day + 1, bridgeVerified: true),
      "offline: up to 7 days old, not a second more")
check(!cacheFromSavedPayload(unverified: false, age: day, bridgeVerified: false), "offline: the bridge missing or not matching its checksum: refused")
check(!cacheFromSavedPayload(unverified: true, age: day, bridgeVerified: true), "offline: after a download that didn't verify: refused")
check(!cacheFromSavedPayload(unverified: false, age: nil, bridgeVerified: true) && !cacheFromSavedPayload(unverified: false, age: -60, bridgeVerified: true),
      "offline: no saved payload.json, or one dated in the future: refused")

// MARK: the reinstall reminders: off when the watcher's LaunchAgent is there but launchd won't run it

let disabledList = """
disabled services = {
		"io.github.rbstemsplus.watcher.old" => enabled
		"com.example.agent" => disabled
		"io.github.rbstemsplus.watcher" => disabled
	}
"""
check(launchdDisabled(disabledList, label: agentLabel), "reminders: a disabled entry for the watcher is found")
check(!launchdDisabled(disabledList.replacingOccurrences(of: "\"io.github.rbstemsplus.watcher\" => disabled", with: "\"io.github.rbstemsplus.watcher\" => enabled"), label: agentLabel)
      && !launchdDisabled("disabled services = {\n\t\"io.github.rbstemsplus.watcher.old\" => disabled\n}", label: agentLabel)
      && !launchdDisabled("", label: agentLabel),
      "reminders: enabled, another label that starts the same, or no entry: not disabled")
check(launchdDisabled("\t\"io.github.rbstemsplus.watcher\" => true\n", label: agentLabel), "reminders: the older format (=> true) counts as disabled")
check(!watcherOff(plistThere: true, loaded: true, disabled: false), "reminders: loaded and enabled: on")
check(watcherOff(plistThere: true, loaded: false, disabled: true) && watcherOff(plistThere: true, loaded: true, disabled: true),
      "reminders: turned off in Login Items: off")
check(watcherOff(plistThere: true, loaded: false, disabled: false), "reminders: there but not loaded: off")
check(!watcherOff(plistThere: false, loaded: false, disabled: true), "reminders: no watcher installed: nothing to say")
check(watcherOffLine == "Reinstall reminders are off. Turn on RB Stems Plus Watcher in System Settings › General › Login Items & Extensions.", "reminders: the status line names the watcher helper")

// MARK: an action asked for earlier waits for rekordbox to be closed, never dead-ends

/// queuedStart with rekordbox open for `openFor` checks and these answers; returns the result and
/// how many times the user was asked.
func queued(updating: [Bool] = [false], openFor: Int, answers: [Bool]) -> (QueuedStart, Int) {
    var u = updating, open = openFor, a = answers, asked = 0
    let r = queuedStart(updating: { u.count > 1 ? u.removeFirst() : u[0] },
                        rekordboxOpen: { open -= 1; return open >= 0 },
                        tryAgain: { asked += 1; return a.isEmpty ? false : a.removeFirst() })
    return (r, asked)
}
check(queued(openFor: 0, answers: []) == (.start, 0), "queued action: rekordbox closed: starts at once, nothing asked")
check(queued(openFor: 2, answers: [true, true]) == (.start, 2), "queued action: rekordbox open: Try Again until it is closed, then starts")
check(queued(openFor: 3, answers: [true, false]) == (.cancelled, 2), "queued action: Cancel at rekordbox open: not started")
check(queued(updating: [true], openFor: 1, answers: [true]) == (.updating, 0), "queued action: rekordbox updating: refused, nothing asked")
check(queued(updating: [false, true], openFor: 1, answers: [true]) == (.updating, 1), "queued action: an update started while waiting: refused")
let req = tmp + "/request"
check(reinstallRequestAge(req) == nil && takeReinstallRequest(req) == nil, "request: none")
writeReinstallRequest(req)
check((reinstallRequestAge(req) ?? 99) < 5 && exists(req), "request: its age is read without taking it (kept while rekordbox is open)")
check((takeReinstallRequest(req, Deletable(inside: [], roots: [], files: [req], apps: [])) ?? 99) < 5 && !exists(req) && reinstallRequestAge(req) == nil, "request: taken once the reinstall starts or is turned down")

// MARK: the watcher's dialog: a reinstall only of what can be reinstalled

func dialog(_ model: Bool, _ cache: Bool, engineOK: Bool = true, rbOK: Bool = true, plusWorks: Bool = false) -> (title: String, message: String, buttons: [String]) {
    watcherDialog(modelOff: model, cacheOff: cache, engine: "0003", rekordbox: "7.2.20", engineOK: engineOK, rekordboxOK: rbOK, plusWorks: plusWorks)
}
let notYet = ["OK", "Don't Ask Again for This Version"], reinstall = ["Reinstall Now", "Remind Me Later", "Don't Ask Again for This Version"]
var w = dialog(false, true, plusWorks: true)
check(w.title == "rekordbox was updated" && w.message == "Stems Cache is off (Demucs v4 still works). Reinstall it to reuse the stems it has saved." && w.buttons == reinstall,
      "watcher: Stems Cache off: the approved text")
w = dialog(true, true)
check(w.buttons == reinstall && w.message.contains("Reinstall them"), "watcher: both off, both supported: reinstall them")
w = dialog(true, true, rbOK: false)
check(w.buttons == reinstall && w.message.contains("Reinstall Demucs v4 to separate tracks with it.") && w.message.contains("rekordbox 7.2.20") && !w.message.contains("Reinstall them"),
      "watcher: both off, rekordbox not supported: reinstall Demucs v4, and says Stems Cache stays off (\(w.message))")
w = dialog(true, true, engineOK: false)
check(w.buttons == reinstall && w.message.contains("Reinstall Stems Cache") && w.message.contains("STEMS Engine 0003") && !w.message.contains("needs Demucs v4")
      && w.message.contains("Demucs v4 isn't available for STEMS Engine 0003 yet"),
      "watcher: both off, STEMS Engine not supported: Stems Cache alone is offered (it works with rekordbox's own model) (\(w.message))")
w = dialog(true, true, engineOK: false, rbOK: false)
check(w.buttons == notYet && w.message.contains("STEMS Engine 0003") && w.message.contains("rekordbox 7.2.20"),
      "watcher: both off, neither version supported: names both (\(w.message))")
w = dialog(true, false, engineOK: false)
check(w.title == "rekordbox installed a new STEMS Engine" && w.buttons == notYet && w.message.hasPrefix("Demucs v4 is off. It isn't available for STEMS Engine 0003 yet."),
      "watcher: Demucs v4 off, STEMS Engine not supported")
w = dialog(false, true, rbOK: false)
check(w.title == "rekordbox was updated" && w.buttons == notYet && w.message.hasPrefix("Stems Cache is off. It doesn't support rekordbox 7.2.20 yet."),
      "watcher: Stems Cache off, rekordbox not supported")
w = dialog(true, false)
check(w.title == "rekordbox put its own stems model back" && w.buttons == reinstall
      && w.message == "Demucs v4 is off. Reinstall it to separate tracks with Demucs v4 instead of rekordbox's own model.", "watcher: Demucs v4 off: reinstall it")
rc = cacheRemoval(folder: false, root: false)
check(rc.0 == .removed && rc.1.isEmpty, "rekordbox and the root folder both gone: nothing to ask or run")
rc = cacheRemoval(folder: true, root: false, run: .rekordboxMissing)
check(rc.0 == .removed && rc.1 == ["uninstall"], "rekordbox gone during the run, no root folder: nothing asked")

// MARK: free space, as the bridge measures it

let gb: Int64 = 1_000_000_000
check(freeBytes("/x", finder: { _ in 70 * gb }, fallback: { _ in 1 }) == 70 * gb, "free space: Finder's count when it answers")
check(freeBytes("/x", finder: { _ in 0 }, fallback: { _ in 40 * gb }) == 40 * gb, "free space: statfs's when Finder's says 0")
check(freeBytes("/x", finder: { _ in nil }, fallback: { _ in 40 * gb }) == 40 * gb, "free space: statfs's when Finder's fails")
check(freeBytes("/x", finder: { _ in nil }, fallback: { _ in nil }) == nil, "free space: unknown when neither answers")
// the floor is the bridge's: 10% of the disk, at most 50 GB (of 10^9 bytes), 50 GB if the size is unknown
check(cacheFreeFloor(disk: 256 * gb) == 25_600_000_000, "a 256 GB disk: a 25.6 GB floor")
check(cacheFreeFloor(disk: 1000 * gb) == 50 * gb && cacheFreeFloor(disk: 2000 * gb) == 50 * gb, "a 1 TB or 2 TB disk: a 50 GB floor")
check(cacheFreeFloor(disk: 500 * gb) == 50 * gb && cacheFreeFloor(disk: 499 * gb) == 49_900_000_000, "50 GB from a 500 GB disk up")
check(cacheFreeFloor(disk: nil) == 50 * gb && cacheFreeFloor(disk: 0) == 50 * gb, "a disk whose size can't be read: a 50 GB floor")
check(cachePausedTip(25_600_000_000) == "Stems Cache stopped saving stems: your disk has less than 26 GB free."
      && cachePausedAtInstall(25_600_000_000) == "Stems Cache will stop saving stems while your disk has less than 26 GB free.",
      "the texts give the floor in whole GB, rounded (25.6 GB: 26)")
check(cachePausedTip(50 * gb) == "Stems Cache stopped saving stems: your disk has less than 50 GB free."
      && cachePausedAtInstall(50 * gb) == "Stems Cache will stop saving stems while your disk has less than 50 GB free."
      && cacheFloorGB(25_400_000_000) == 25 && cacheFloorGB(25_500_000_000) == 26, "50 GB says 50; 25.4 GB says 25, 25.5 GB 26")
check(cachePausedFloor(free: 25_600_000_000 - 1, disk: 256 * gb) == 25_600_000_000 && cachePausedFloor(free: 25_600_000_000, disk: 256 * gb) == nil,
      "a 256 GB disk: paused under 25.6 GB only (exactly at the floor saves, as in the bridge)")
check(cachePausedFloor(free: 50 * gb - 1, disk: 1000 * gb) == 50 * gb && cachePausedFloor(free: 50 * gb, disk: 1000 * gb) == nil
      && cachePausedFloor(free: 50 * gb - 1, disk: 2000 * gb) == 50 * gb && cachePausedFloor(free: 50 * gb, disk: 2000 * gb) == nil,
      "a 1 TB or 2 TB disk: paused under 50 GB only")
check(cachePausedFloor(free: 50 * gb - 1, disk: nil) == 50 * gb && cachePausedFloor(free: 50 * gb, disk: nil) == nil,
      "a disk whose size can't be read: paused under 50 GB only")
check(cachePausedFloor(free: nil, disk: 256 * gb) == nil && cachePausedFloor(free: nil, disk: nil) == nil, "unknown free space isn't paused")
check(cachePausedFloor(free: freeBytes("/x", finder: { _ in 0 }, fallback: { _ in 49 * gb }), disk: 1000 * gb) == 50 * gb,
      "paused by statfs's count when Finder's says 0")
var tfs = statfs()
check(statfs(tmp, &tfs) == 0 && diskBytes(tmp) == Int64(tfs.f_blocks) * Int64(tfs.f_bsize) && (diskBytes(tmp) ?? 0) > 0 && diskBytes(tmp + "/not-there") == nil,
      "the disk's size is statfs's blocks times their size, as in the bridge; none for a path that isn't there")
check((finderFreeBytes(tmp) ?? 0) > 0 && (statfsFreeBytes(tmp) ?? 0) > 0 && abs((freeBytes(tmp) ?? 0) - (finderFreeBytes(tmp) ?? 0)) < gb,   // measured a moment apart
      "the real measures answer for the temporary folder")
check(statfsFreeBytes(tmp + "/not-there") == nil && finderFreeBytes(tmp + "/not-there") == nil, "a path that isn't there has no count")
check(FileManager.default.fileExists(atPath: cacheVolumePath()) && cacheDir.hasPrefix(cacheVolumePath()), "the cache's disk is measured at the cache folder or above it")

// MARK: the status lights: on, installed but not working right now, off

check(plusLight(installed: true) == (.on, "Demucs v4 is on.") && plusLight(installed: false) == (.off, "Demucs v4 is off."),
      "Demucs v4 light: green when on, red when off")
/// the Stems Cache light with rekordbox's own model unless said otherwise: this account chose it,
/// rekordbox_model on, a bridge listing the model, STEMS Engine 0002, no low disk
func cl(installed: Bool = true, plusOn: Bool = false, chosen: Bool = true, rekordboxModel: Bool = true, bridgeModels: [String]? = [String(repeating: "a", count: 64)],
        model: String? = String(repeating: "a", count: 64), engine: String = "0002", pausedFloor: Int64? = nil) -> (look: LightLook, tip: String) {
    cacheLight(installed: installed, plusOn: plusOn, chosen: chosen, rekordboxModel: rekordboxModel, bridgeModels: bridgeModels, model: model, engine: engine, pausedFloor: pausedFloor)
}
check(cl(plusOn: true) == (.on, "Stems Cache is on.") && cl() == (.on, "Stems Cache is on."),
      "Stems Cache light: green when installed and saving, with Demucs v4 or with rekordbox's own (listed) model")
check(cl(installed: false, pausedFloor: 50 * gb) == (.off, "Stems Cache is off.") && cl(installed: false, plusOn: true, chosen: false) == (.off, "Stems Cache is off."),
      "Stems Cache light: red when not installed, whatever else")
check(cl(plusOn: true, chosen: false, rekordboxModel: false, bridgeModels: nil, model: nil) == (.on, "Stems Cache is on."),
      "Stems Cache light: with Demucs v4, green as in 1.0 (another account's bridge, an older bridge: it saves the Demucs v4 model's stems)")
check(cl(chosen: false) == (.limited, "Stems Cache was installed from another account on this Mac. Click Install Stems Cache to use it here."),
      "Stems Cache light: rekordbox's own model, Stems Cache from another account: yellow, pointing at Install Stems Cache")
check(cl(rekordboxModel: false) == (.limited, "Stems Cache doesn't save the stems of rekordbox's own model: rekordbox_model is 0 in config.ini."),
      "Stems Cache light: rekordbox_model turned off by hand: yellow, saying where")
check(cl(bridgeModels: nil) == (.limited, "Stems Cache needs an update to save the stems of rekordbox's own model. Click Reinstall Stems Cache."),
      "Stems Cache light: an older bridge (no list): yellow, Reinstall Stems Cache")
check(cl(model: String(repeating: "b", count: 64), engine: "0003")
      == (.limited, "Stems Cache doesn't support STEMS Engine 0003 yet. When an RB Stems Plus update supports it, click Reinstall Stems Cache."),
      "Stems Cache light: a model the bridge doesn't list: yellow, naming the STEMS Engine")
check(cl(model: nil) == (.on, "Stems Cache is on."), "Stems Cache light: the model's checksum not worked out yet: not judged")
check(cl(engine: "") == (.limited, "Stems Cache starts saving stems once rekordbox has downloaded its STEMS Engine."),
      "Stems Cache light: no STEMS Engine yet: yellow, saying when it starts")
check(cl(plusOn: true, pausedFloor: 25_600_000_000) == (.limited, "Stems Cache stopped saving stems: your disk has less than 26 GB free.")
      && cl(pausedFloor: 25_600_000_000) == (.limited, "Stems Cache stopped saving stems: your disk has less than 26 GB free."),
      "Stems Cache light: yellow under the free-space floor, giving the floor, with either model")
check(![cl(chosen: false), cl(rekordboxModel: false), cl(bridgeModels: nil), cl(model: String(repeating: "b", count: 64)), cl(engine: "")]
        .contains { $0.tip.contains("needs Demucs v4") || $0.tip.contains("only with Demucs v4") || $0.tip.contains("Install Demucs v4") },
      "Stems Cache light: never says it needs Demucs v4")

// MARK: the model feature is called Demucs v4 wherever the user reads it (the app stays RB Stems Plus)

func oldName(_ s: String) -> Bool { s.range(of: "(?<!RB )Stems Plus", options: .regularExpression) != nil }
check(oldName("Install Stems Plus") && !oldName("Remove RB Stems Plus Anyway"), "the old-name finder finds the feature, not the app")
// every source line but comment lines; Report.swift still reads the log lines 1.1.0 and earlier wrote
var stale: [String] = []
let srcDir = fm.currentDirectoryPath + "/app/Sources"
let sources = ((try? fm.contentsOfDirectory(atPath: srcDir)) ?? []).filter { $0.hasSuffix(".swift") }.sorted()
let oldLogLines = ["\"Stems Plus not installed\"", "\"Stems Plus: installed\"", "\"Stems Plus: already in place\""]
for f in sources {
    for (i, l) in ((try? String(contentsOfFile: srcDir + "/" + f, encoding: .utf8)) ?? "").components(separatedBy: "\n").enumerated() {
        if l.trimmingCharacters(in: .whitespaces).hasPrefix("//") { continue }
        let code = f == "Report.swift" ? oldLogLines.reduce(l) { $0.replacingOccurrences(of: $1, with: "") } : l
        if oldName(code) { stale.append("\(f):\(i + 1)") }
    }
}
check(sources.count > 10 && stale.isEmpty, "no source string calls the model feature Stems Plus: \(sources.count) files, \(stale)")

// MARK: what the installed bridge saves stems for

let caps = "rbstems-cache: rebuild=1 pioneer=" + String(repeating: "c", count: 64) + ";"
check(bridgeCacheModels(data: Data(("xx rbstems bridge: yy " + caps + " zz").utf8)) == [String(repeating: "c", count: 64)],
      "bridge list: the checksums after pioneer=")
check(bridgeCacheModels(data: Data(("rbstems bridge: rbstems-cache: rebuild=1 pioneer=" + String(repeating: "c", count: 64) + "," + String(repeating: "d", count: 64) + ";").utf8))?.count == 2,
      "bridge list: several checksums, comma-separated")
check(bridgeCacheModels(data: Data("rbstems bridge: real CPU provider missing".utf8)) == nil, "bridge list: 1.0's bridge has none (nil)")
check(bridgeCacheModels(data: Data(caps.utf8)) == nil, "bridge list: not a bridge (no \"rbstems bridge: \"): nil")
check(bridgeCacheModels(data: Data("rbstems bridge: rbstems-cache: rebuild=1 pioneer=notachecksum,ABC;".utf8)) == [],
      "bridge list: anything that isn't a lowercase checksum is dropped")

// MARK: rekordbox_model in config.ini

let cfgFile = tmp + "/config-rm.ini"
try? "# mine\nmax_gb=7\n[watcher]\nenabled=no\n".write(toFile: cfgFile, atomically: true, encoding: .utf8)
check(!configSets("rekordbox_model", file: cfgFile) && configSets("max_gb", file: cfgFile) && !configSets("enabled", file: cfgFile),
      "config: configSets sees only our keys (top level or [cache]), not another section's")
check(writeConfigValues([("rekordbox_model", "1")], file: cfgFile) && configSets("rekordbox_model", file: cfgFile)
      && (try? String(contentsOfFile: cfgFile, encoding: .utf8)) == "# mine\nmax_gb=7\nrekordbox_model=1\n[watcher]\nenabled=no\n",
      "config: a new key goes before the first section, every other line kept")
check(writeConfigValues([("rekordbox_model", "0")], file: cfgFile)
      && (try? String(contentsOfFile: cfgFile, encoding: .utf8)) == "# mine\nmax_gb=7\nrekordbox_model=0\n[watcher]\nenabled=no\n",
      "config: an existing key is replaced where it is")
var ignoredCfg: [String] = []
try? "rekordbox_model = yes\n".write(toFile: cfgFile, atomically: true, encoding: .utf8)
let rmOn = readConfig(file: cfgFile).rekordboxModel
try? "[cache]\nrekordbox_model=maybe\n".write(toFile: cfgFile, atomically: true, encoding: .utf8)
let rmBad = readConfig({ ignoredCfg.append($0) }, file: cfgFile).rekordboxModel
check(rmOn && !rmBad && ignoredCfg == ["config.ini: ignored rekordbox_model=maybe (allowed 1 or 0)"] && !readConfig(file: tmp + "/none.ini").rekordboxModel,
      "config: rekordbox_model is read like enabled; off when missing or not understood (as the bridge reads it)")
check(lightDrawing(.on).symbol == "checkmark.circle.fill" && lightDrawing(.limited).symbol == "exclamationmark.circle.fill"
      && lightDrawing(.off).symbol == "xmark.circle.fill" && Set([lightDrawing(.on).tint, lightDrawing(.limited).tint, lightDrawing(.off).tint]).count == 3,
      "the three looks differ in shape and colour")

// MARK: Pioneer's installer or updater: Apple's Installer by its path, not every "Installer"

/// A copy of this program sleeping under `path`'s name (pgrep -x sees that name), started with
/// `argv0` as its command line's first word (pgrep -f sees that). Returns its pid.
func sleeperAt(_ path: String, argv0: String? = nil) -> pid_t {
    mk((path as NSString).deletingLastPathComponent)
    if !exists(path) { try! fm.copyItem(atPath: me, toPath: path) }
    var pid: pid_t = 0
    let argv: [UnsafeMutablePointer<CChar>?] = [strdup(argv0 ?? path), strdup("--sleep"), strdup("30"), nil]
    return posix_spawn(&pid, path, nil, nil, argv, [nil]) == 0 ? pid : 0
}
let updatingAlready = pioneerUpdating()
if updatingAlready { print("note: an installer or updater is running on this Mac: the checks that nothing counts are skipped") }
let procs = tmp + "/procs"
for (what, path, argv0, counts) in [
    ("another app's updater named Installer (Sparkle's Installer.xpc)", procs + "/Sparkle/Installer.xpc/Contents/MacOS/Installer", nil, false),
    ("a process named Installer that isn't Apple's", procs + "/a/Installer", nil, false),
    ("Apple's Installer, by its path", procs + "/apple/Installer", "/System/Library/CoreServices/Installer.app/Contents/MacOS/Installer", true),
    ("installer, the command-line tool", procs + "/b/installer", nil, true),
    ("rekordbox's updater, Upmgr rekordbox", procs + "/c/Upmgr rekordbox", nil, true),
] as [(String, String, String?, Bool)] {
    let pid = sleeperAt(path, argv0: argv0)
    if counts || !updatingAlready { check(pid > 0 && pioneerUpdating() == counts, "\(what): \(counts ? "waited for" : "doesn't block anything")") }
    if pid > 0 { kill(pid, SIGKILL); waitpid(pid, nil, 0) }
}
check(cacheInstallBody.contains("running -f '\(appleInstallerPattern)'"), "the root scripts look for Apple's Installer by the same path")

// MARK: quitting waits only for the critical step

let cw = CriticalWork()
check(quitNow(own: false, cw), "quit: nothing running, it goes ahead")
cw.run {
    check(!quitNow(own: false, cw), "quit: refused while the critical step runs")
    check(quitNow(own: true, cw), "quit: the app's own quit goes ahead even then")
}
var fired = false
cw.run { cw.whenIdle(timeout: 30) { fired = true }; check(!fired, "quit: the reply waits for the step") }
check(fired && !cw.running, "quit: the reply comes as soon as the step ends")
let slow = DispatchSemaphore(value: 0), replied = DispatchSemaphore(value: 0)
DispatchQueue.global().async { cw.run { _ = slow.wait(timeout: .now() + 5) } }
while !cw.running { usleep(10_000) }
let t0 = Date()
cw.whenIdle(timeout: 0.5) { replied.signal() }
check(replied.wait(timeout: .now() + 3) == .success && Date().timeIntervalSince(t0) < 2, "quit: never held up longer than the timeout (logout, shutdown)")
slow.signal(); while cw.running { usleep(10_000) }
check(isRootScript("/usr/bin/sudo", ["-n", "/bin/bash", "-c", "true", "rbsp"]), "a root script counts as critical")
check(!isRootScript("/usr/bin/sudo", ["-S", "-p", "", "-v"]) && !isRootScript("/usr/bin/sudo", ["-n", "/usr/bin/true"]) && !isRootScript("/bin/bash", ["-c", "true"]),
      "the password check and other commands don't")
// the model step: critical during the copy and rename into rekordbox's folder only
var stepCritical: [String: Bool] = [:]
let watched = Models(dir: rbm, originals: orig, ours: [oursSha], gate: mgate, log: { line in
    if line.hasPrefix("save Pioneer's model: saved") { stepCritical["save"] = critical.running }
    if line.hasPrefix("Demucs v4: installed") { stepCritical["install"] = critical.running }
    if line.hasPrefix("restore Pioneer's model: Pioneer's stems model is back") { stepCritical["restore"] = critical.running }
})
clearOriginals(); put(dataA, rbModel); setEngine("0002")
check(!critical.running && watched.installOurs(from: ourSource, sha: oursSha).ok && watched.restore().ok && !critical.running, "install and restore end with nothing critical")
check(stepCritical == ["save": false, "install": true, "restore": true], "critical only while copying into rekordbox's folder: \(stepCritical)")

// MARK: one copy of the app at a time

let lockPath = tmp + "/app.lock"
func copyOfApp(_ args: [String]) -> (Process, FileHandle) {
    let p = Process(); p.executableURL = URL(fileURLWithPath: me); p.arguments = args
    let out = Pipe(); p.standardOutput = out
    try! p.run()
    return (p, out.fileHandleForReading)
}
func firstLine(_ h: FileHandle) -> String { String(decoding: h.availableData, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines) }
guard case .held(let held) = takeLock(lockPath, wait: 0) else { fatalError("can't take a fresh lock") }
check(fcntl(held, F_GETFD) & FD_CLOEXEC != 0, "the lock's descriptor is close-on-exec")
var t = Date()
var (p2, o2) = copyOfApp(["--try-lock", lockPath, "1"])
check(firstLine(o2) == "busy" && Date().timeIntervalSince(t) >= 0.9, "a second copy waits, then says it's already open")
p2.waitUntilExit()
// a child started while the app holds the lock doesn't inherit it (posix_spawn without
// CLOEXEC_DEFAULT: only the flag keeps it out)
var sleeper: pid_t = 0
let sleepArgs: [UnsafeMutablePointer<CChar>?] = [strdup("/bin/sleep"), strdup("10"), nil]
check(posix_spawn(&sleeper, "/bin/sleep", nil, nil, sleepArgs, [nil]) == 0, "a child process started")
close(held)
(p2, o2) = copyOfApp(["--try-lock", lockPath, "0"])
check(firstLine(o2) == "held", "the lock is free once the app has gone, though its child still runs")
p2.waitUntilExit()
kill(sleeper, SIGKILL); waitpid(sleeper, nil, 0)
// Quit & Reopen: the old copy quits during the wait
(p2, o2) = copyOfApp(["--hold-lock", lockPath, "1"])
check(firstLine(o2) == "held", "another copy holds the lock")
t = Date()
if case .held(let fd) = takeLock(lockPath, wait: 10) { check(Date().timeIntervalSince(t) < 5, "the new copy gets the lock once the old one has quit"); close(fd) }
else { check(false, "the new copy gets the lock once the old one has quit") }
p2.waitUntilExit()
// killed: the lock goes with the process
(p2, o2) = copyOfApp(["--hold-lock", lockPath, "60"])
check(firstLine(o2) == "held", "another copy holds the lock (to be killed)")
kill(p2.processIdentifier, SIGKILL); p2.waitUntilExit()
if case .held(let fd) = takeLock(lockPath, wait: 2) { check(true, "a killed copy's lock is released"); close(fd) }
else { check(false, "a killed copy's lock is released") }
try? fm.removeItem(atPath: lockPath); symlink(outside + "/victim", lockPath)
check(takeLock(lockPath, wait: 0) == .failed(String(cString: strerror(ELOOP))), "a lock path that is a link is refused")

// MARK: the busy marker: its writer by pid and start time, never a process that got the pid later

let bm = tmp + "/busy-marker"
func marker(_ text: String, mtime: Int? = nil) {
    try? fm.removeItem(atPath: bm); fm.createFile(atPath: bm, contents: Data(text.utf8))
    if let t = mtime { var tv = [timeval(tv_sec: t, tv_usec: 0), timeval(tv_sec: t, tv_usec: 0)]; utimes(bm, &tv) }
}
let myStart = processStartTime(getpid()) ?? -1
check(myStart > 0 && myStart <= Int(Date().timeIntervalSince1970), "this process's start time is known")
check(processStartTime(done.processIdentifier) == nil && processStartTime(0) == nil, "a process that has gone has no start time")
setBusyMarker(Action(id: "install", label: "Install Demucs v4"), at: bm)
check((try? String(contentsOfFile: bm, encoding: .utf8)) == "\(getpid())\n\(myStart)\ninstall\nInstall Demucs v4\n", "marker: pid, start time, then the action")
check(appBusy(bm) && leftoverAction(bm, wait: 0) == nil, "marker: this process's own: live, and no leftover")
marker("\(getpid())\n\(myStart + 1)\ninstall\nInstall Demucs v4\n")
check(!appBusy(bm) && leftoverAction(bm, wait: 0)?.id == "install", "marker: this pid with another start time (the pid used again): not live, a leftover")
let writer = sleeperAt(procs + "/marker/sleeper"), heldStart = processStartTime(writer) ?? -1
marker("\(writer)\n\(heldStart)\nuninstall-all\nUninstall RB Stems Plus completely\n")
check(writer > 0 && appBusy(bm) && leftoverAction(bm, wait: 2) == nil, "marker: another running writer: live, never taken for a leftover")
marker("\(writer)\n\(heldStart - 3600)\nuninstall-all\nUninstall RB Stems Plus completely\n")
check(!appBusy(bm) && leftoverAction(bm, wait: 0)?.id == "uninstall-all", "marker: a running process that got the writer's pid: not live, a leftover")
marker("\(done.processIdentifier)\n\(myStart)\ninstall\nInstall\n")
check(!appBusy(bm) && leftoverAction(bm, wait: 0)?.id == "install", "marker: a writer that has gone: not live, a leftover")
// the old format: no start time; its writer had started by the time the file was written
marker("\(writer)\nuninstall-all\nUninstall RB Stems Plus completely\n")
check(readBusyMarker(bm)?.start == nil && readBusyMarker(bm)?.action?.id == "uninstall-all" && appBusy(bm), "old marker: written while its process ran: live")
marker("\(writer)\nuninstall-all\nUninstall RB Stems Plus completely\n", mtime: heldStart - 60)
check(!appBusy(bm) && leftoverAction(bm, wait: 0)?.id == "uninstall-all", "old marker: the process with its pid started after it was written (pid used again, or a restart): not live")
marker("\(writer)\n")
check(readBusyMarker(bm)?.action == nil && readBusyMarker(bm)?.start == nil && appBusy(bm), "old update marker (a pid only): read, live while its process runs")
marker("\(writer)\n\(heldStart)\n")
check(readBusyMarker(bm)?.action == nil && appBusy(bm), "update marker (pid and start time): live while its process runs")
if writer > 0 { kill(writer, SIGKILL); waitpid(writer, nil, 0) }
check(!appBusy(bm), "update marker: not live once its process has gone")
// the update script's start time is the kernel's, whatever the time zone and language
let shell = Process()
shell.executableURL = URL(fileURLWithPath: "/bin/sh")
shell.arguments = ["-c", Controller.startedFunction + "\nstarted $$; exec /bin/sleep 10"]
shell.environment = ["PATH": "/usr/bin:/bin", "TZ": "Pacific/Chatham", "LC_ALL": "de_DE.UTF-8", "LANG": "de_DE.UTF-8"]
let shellOut = Pipe(); shell.standardOutput = shellOut
try! shell.run()
let shellStart = firstLine(shellOut.fileHandleForReading)
check(!shellStart.isEmpty && shellStart == processStartTime(shell.processIdentifier).map(String.init), "the update script's start time matches the app's (\(shellStart))")
shell.terminate(); shell.waitUntilExit()
check(runTool("/bin/sh", ["-c", Controller.startedFunction + "\nstarted \(done.processIdentifier) || echo none"]).1 == "none", "the update script: no start time for a process that has gone")

// MARK: logs: appended, never written through a link

let lg = tmp + "/logs/app.log"
appendLog(lg, "one"); appendLog(lg, "two")
let lgLines = ((try? String(contentsOfFile: lg, encoding: .utf8)) ?? "").split(separator: "\n")
check(lgLines.count == 2 && lgLines[0].hasSuffix("  one") && lgLines[1].hasSuffix("  two"), "a log gets each line at its end")
touch(outside + "/logvictim"); symlink(outside + "/logvictim", tmp + "/logs/link.log")
appendLog(tmp + "/logs/link.log", "planted")
check((try? String(contentsOfFile: outside + "/logvictim", encoding: .utf8)) == "x" && fileType(tmp + "/logs/link.log") == S_IFLNK, "a link planted at a log's place is never written through")

// MARK: the watcher helper, its registration and its lock (WatcherTests.swift)

watcherTests()

try? FileManager.default.removeItem(atPath: tmp)
print(failures == 0 ? "all passed" : "\(failures) FAILED")
exit(failures == 0 ? 0 : 1)
