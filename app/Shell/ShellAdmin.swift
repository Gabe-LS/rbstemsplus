// The AppKit shell over the shared C core (the private Windows plan's mac-core §3.5, M7): host->admin,
// one attempt of a root run with typed values only (the core never passes script text). It is
// today's Controller.admin (Admin.swift) without its window writes: the script from Scripts.swift
// byte for byte, sudo as today, the password asked until it's right or cancelled
// (askUntilAccepted), but the password sheet shown through the presenter (Presenter.swift)
// instead of onMainSync { Sheet.run }, so only the flow thread blocks. Progress goes through
// core_flow_progress (no spinner or status writes); "Operation not permitted" goes back through
// core_admin_note. The core runs the attempts (runRoot's loop: rekordbox before each, "Permission
// needed" between them, CORE_ADMIN_FORGET at the end). Every build (the app is this shell).
import Foundation

struct ShellAdmin {
    /// sudo and bash, as runTool runs them (a test passes its own)
    var tool: (_ exe: String, _ args: [String], _ input: Data?) -> (Int32, String) = { runTool($0, $1, input: $2) }
    /// the password sheet: told whether the last one was wrong; nil when cancelled
    var ask: (_ again: Bool) -> String?
    var log: (String) -> Void
    /// core_flow_progress(ADMIN_ATTEMPT) and core_admin_note, on the core the shell made
    var accepted: () -> Void
    var notPermitted: () -> Void

    /// Admin.swift's cancelled: what admin returns for a cancel at the password, and only then
    static let cancelled: Int32 = -2
    /// The run couldn't start (sudo didn't launch, or the checked bridge isn't there): exit code 1,
    /// which isn't one of root_problem's codes, so the core shows today's failure alert ("… See
    /// the log in this window."); the log line before it holds the reason. Today's Swift showed
    /// the error text itself in that alert (a recorded difference, mac-core.md §3.7).
    static let notStarted: Int32 = 1

    /// The root script for one admin kind, with the flow's checked bridge for the installs; nil
    /// when the install's bridge isn't the one CACHE_PREPARE checked (never run with other bytes).
    static func script(kind: Int32, version: String, bridgeSha: String, ortSha: String, force: Bool,
                       bridge: (String) -> Data? = { ShellFlowState.shared.preparedBridge(sha: $0) }) -> AdminScript? {
        switch kind {
        case Int32(CORE_ADMIN_CACHE_INSTALL.rawValue), Int32(CORE_ADMIN_CACHE_REINSTALL.rawValue):
            guard let b = bridge(bridgeSha) else { return nil }
            return cacheInstallScript(bridge: b, bridgeSha: bridgeSha, ortSha: ortSha, force: force, version: version)
        case Int32(CORE_ADMIN_CACHE_UNINSTALL.rawValue): return cacheUninstallScript(version: version)
        case Int32(CORE_ADMIN_REMOVE_ROOT_FOLDER.rawValue): return removeRootFolderScript()
        default: return nil
        }
    }

    /// host->admin. Returns the RootExit code (>= 0), or < 0: cancelled at the password.
    func run(kind: Int32, version: String, bridgeSha: String, ortSha: String, force: Bool,
             bridge: (String) -> Data? = { ShellFlowState.shared.preparedBridge(sha: $0) }) -> Int32 {
        if kind == Int32(CORE_ADMIN_FORGET.rawValue) {
            _ = tool("/usr/bin/sudo", ["-k"], nil)   // forget the password: the next action asks again
            return 0
        }
        guard let script = ShellAdmin.script(kind: kind, version: version, bridgeSha: bridgeSha, ortSha: ortSha, force: force, bridge: bridge) else {
            log("  FAILED: not run (the bridge checked for this action isn't there)")
            return ShellAdmin.notStarted
        }
        if tool("/usr/bin/sudo", ["-n", "/usr/bin/true"], nil).0 == 0 {
            log("  (sudo remembers the password)")
        } else {
            // as macOS's own password dialogs: asked until it's right or the user cancels
            let ok = askUntilAccepted(ask: ask, accepted: { pw in
                tool("/usr/bin/sudo", ["-S", "-p", "", "-v"], Data((pw + "\n").utf8)).0 == 0
            }, wrong: { log("  wrong password") })
            guard ok else {
                log("  FAILED: cancelled")
                return ShellAdmin.cancelled
            }
        }
        accepted()                                   // the status line: the run's own text (the core's)
        let r = tool("/usr/bin/sudo", ["-n", "/bin/bash", "-c", script.body, "rbsp"] + script.args, script.input)
        let output = r.1 + (r.0 == 0 ? "" : " [exit \(r.0)]")
        log("  \(r.0 == 0 ? "ok" : "FAILED"): \(output.replacingOccurrences(of: "\n", with: " | "))")
        if r.0 != 0 && output.contains("Operation not permitted") { notPermitted() }
        return r.0 < 0 ? ShellAdmin.notStarted : r.0       // sudo couldn't start: a failure, not a cancel
    }
}
