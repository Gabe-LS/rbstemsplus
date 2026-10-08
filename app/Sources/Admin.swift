// Admin runs: a bash script run as root with sudo, as a child of this app, so App Management
// attributes the change to RB Stems Plus. macOS's own password dialog can't be used: it runs
// the command under Apple's authtrampoline, which App Management refuses without asking. The
// password is asked in this app, passed to sudo once per action, and forgotten afterwards (sudo -k).
import AppKit

extension Controller {
    /// The app's own password sheet. Returns nil if cancelled.
    func askPassword(_ again: Bool) -> String? {
        onMainSync {
            let r = Sheet(title: again ? "Wrong password" : "Password needed",
                          message: again ? "Try again so RB Stems Plus can make changes to rekordbox."
                                         : "Enter your password so RB Stems Plus can make changes to rekordbox.",
                          buttons: ["Continue", "Cancel"], password: "Password").run(on: window)
            return r.choice == "Continue" ? r.text : nil
        }
    }

    /// Why this user can't run admin steps, or nil if they can. sudo refuses standard accounts
    /// even with the right password, so they aren't asked for one (the Stems Cache buttons are
    /// off for them too, with the same reason in the status line).
    func adminBlocked() -> String? { userIsAdmin ? nil : needsAdminLine }

    /// What an admin run did. code: the script's exit code (Scripts.swift), or notRun /
    /// cancelled.
    struct AdminRun { var ok: Bool; var output: String; var code: Int32 }
    static let notRun: Int32 = -1, cancelled: Int32 = -2

    /// Runs a bash script as root, as a child of this app. Asks the password only when sudo
    /// doesn't remember one. The script is passed to root's bash as an argument, never through a
    /// file: nothing running as the user can change it between writing and running. Its values
    /// follow it as arguments, its input (the bridge) goes to its stdin. The password goes only
    /// to "sudo -S -v", on stdin. `status`: the status line once the password is accepted.
    func admin(_ script: AdminScript, status: String) -> AdminRun {
        if let why = adminBlocked() { return AdminRun(ok: false, output: why, code: Controller.notRun) }
        let run = { () -> AdminRun in
            let r = runTool("/usr/bin/sudo", ["-n", "/bin/bash", "-c", script.body, "rbsp"] + script.args, input: script.input)
            return AdminRun(ok: r.0 == 0, output: r.1 + (r.0 == 0 ? "" : " [exit \(r.0)]"), code: r.0)
        }
        if runTool("/usr/bin/sudo", ["-n", "/usr/bin/true"]).0 == 0 {
            log("  (sudo remembers the password)")
            busy(status)                                // e.g. after Try Again: not "Trying again…" for a minute
            return run()
        }
        // as macOS's own password dialogs: asked until it's right or the user cancels
        let accepted = askUntilAccepted(ask: askPassword, accepted: { pw in
            runTool("/usr/bin/sudo", ["-S", "-p", "", "-v"], input: Data((pw + "\n").utf8)).0 == 0
        }, wrong: { log("  wrong password") })
        guard accepted else { return AdminRun(ok: false, output: "cancelled", code: Controller.cancelled) }
        busy(status)
        return run()
    }

    /// What a root run came to. rekordboxMissing: nothing changed because rekordbox's folder is
    /// gone; the caller may offer to remove the root folder (removeRootFolder) once the user agrees.
    enum RootOutcome: Equatable {
        case done, failed(Int32), rekordboxMissing
        /// The root script's exit code (RootExit), or notRun / cancelled; nil when it succeeded.
        var code: Int32? {
            switch self {
            case .done: return nil
            case .failed(let c): return c
            case .rekordboxMissing: return RootExit.rekordboxMissing
            }
        }
        /// The root script's own reason, for a caller's alert (e.g. "Uninstall stopped"): nil when
        /// it succeeded, was cancelled, or its reason is only in the log.
        var problem: (title: String, body: String)? { code.flatMap { Controller.rootProblem($0) } }
    }

    /// A root script's exit code (RootExit) as an alert's title (no full stop) and explanation;
    /// nil for the codes whose explanation is in the log (and for notRun and cancelled).
    static func rootProblem(_ code: Int32) -> (title: String, body: String)? {
        switch code {
        case RootExit.alreadyRunning:
            return ("Already working", "RB Stems Plus is already changing rekordbox. Wait a moment, then try again.")
        case RootExit.installerRunning:
            return ("rekordbox updating", "Wait until rekordbox's installer or updater has finished, then try again.")
        case RootExit.rekordboxMissing:
            return ("rekordbox missing", "rekordbox isn't in the Applications folder. Install rekordbox from rekordbox.com, then try again.")
        case RootExit.notPioneers:
            return ("Reinstall rekordbox", "rekordbox isn't installed the way Pioneer's installer leaves it. Reinstall rekordbox from rekordbox.com, then try again.")
        case RootExit.noOriginals:
            return ("Reinstall rekordbox", "RB Stems Plus has no complete copy of rekordbox's original files. Reinstall rekordbox from rekordbox.com, then try again.")
        case RootExit.rollbackFailed:
            return ("Reinstall rekordbox", "Changing rekordbox failed, and its original files couldn't be put back. Reinstall rekordbox from rekordbox.com, then try again.")
        default:
            return nil
        }
    }

    /// Whether a run's code means the user stopped it: cancelled (the password, Permission needed,
    /// rekordbox open). They know: no alert says so.
    static func userStopped(_ code: Int32) -> Bool { code == cancelled }

    /// Retries an admin script while App Management blocks it, guiding the user. Returns success.
    /// An alert says why it failed (see runRoot), also when rekordbox is missing.
    @discardableResult
    func runWithAppManagement(_ script: AdminScript, name: String, failure: String?) -> Bool {
        runRoot(script, name: name, failure: failure, alertMissing: true) == .done
    }

    /// Removes the root folder alone, once rekordbox is gone (a run came to .rekordboxMissing, or
    /// rekordbox's folder is missing) and the user agreed (offerRootFolderCleanup). Root checks
    /// again that rekordbox's folder is missing. `failure`: as for runRoot. Returns success.
    @discardableResult
    func removeRootFolder(failure: String? = "RB Stems Plus couldn't remove its copy of rekordbox's files.") -> Bool {
        runRoot(removeRootFolderScript(), name: "remove-root-folder", failure: failure) == .done
    }

    /// What a root run (by its `name`) says in the status line while it runs, and its failure
    /// alert's title. Only the Stems Cache install takes long: it saves, copies and signs rekordbox
    /// (cacheInstallScript), about a minute in all.
    static func rootRunTexts(_ name: String) -> (status: String, failedTitle: String) {
        switch name {
        case "install": return ("Installing Stems Cache (this takes about a minute)…", "Install failed")
        case "reinstall": return ("Reinstalling Stems Cache (this takes about a minute)…", "Reinstall failed")
        case "remove-root-folder": return ("Removing the copy of rekordbox's files…", "Uninstall failed")
        default: return ("Uninstalling Stems Cache…", "Uninstall failed")
        }
    }

    /// Retries an admin script while App Management blocks it, guiding the user. rekordbox is
    /// checked before every attempt: it may have been opened during the password or App
    /// Management steps (root checks again). If the run fails for another reason than the user
    /// cancelling, an alert says so (none if `failure` is nil): the root script's own reason when
    /// it has one (rootProblem), else rootRunTexts' title over `failure`. A missing rekordbox gets
    /// that alert only with `alertMissing`.
    func runRoot(_ script: AdminScript, name: String, failure: String?, alertMissing: Bool = false) -> RootOutcome {
        var last: AdminRun?
        for attempt in 1...4 {
            guard rekordboxClosed() else { log("  rekordbox is open: stopped"); return .failed(Controller.cancelled) }
            log("admin run (\(name)), attempt \(attempt)")
            let r = admin(script, status: Controller.rootRunTexts(name).status)
            last = r
            log("  \(r.ok ? "ok" : "FAILED"): \(r.output.replacingOccurrences(of: "\n", with: " | "))")
            if r.ok { break }
            if r.code == RootExit.rekordboxOpen { continue }     // rekordbox was opened just now: ask to quit it
            if !r.output.contains("Operation not permitted") || attempt == 4 { break }
            onMain { self.spinner.stopAnimation(nil); self.status.stringValue = "" }   // still busy: the marker stays
            let b = alert("Permission needed",
                          """
                          1. In the notification "RB Stems Plus was prevented from modifying apps", click Allow.
                          2. Turn on RB Stems Plus in the list, then confirm.
                          3. When macOS offers to quit and reopen RB Stems Plus, choose Later.
                          4. Come back here and click Try Again.
                          """,
                          ["Try Again", "Cancel"])
            log("  user chose: \(b)")
            if b != "Try Again" { last = AdminRun(ok: false, output: "cancelled", code: Controller.cancelled); break }
            busy("Trying again…")
        }
        // forget the password: nothing else can reuse it, and the next action asks once again
        runTool("/usr/bin/sudo", ["-k"])
        guard let r = last, !r.ok else { return .done }
        let missing = r.code == RootExit.rekordboxMissing
        // the user knows when they cancelled or mistyped; anything else gets an alert
        if let failure = failure, !Controller.userStopped(r.code), !missing || alertMissing {
            if let p = Controller.rootProblem(r.code) {
                _ = alert(p.title, p.body, ["OK"])
            } else {
                let why = r.code == Controller.notRun ? r.output
                    : r.code == RootExit.rolledBack ? "rekordbox has its original files again. See the log in this window."
                    : "See the log in this window."
                _ = alert(Controller.rootRunTexts(name).failedTitle, failure + " " + why, ["OK"])
            }
        }
        return missing ? .rekordboxMissing : .failed(r.code)
    }
}

/// The password sheet's loop: `ask` (told whether the last password was wrong) until `accepted`
/// takes one, with `wrong` after each that isn't; no limit on tries. False once the user cancels
/// (`ask` returns nil).
func askUntilAccepted(ask: (Bool) -> String?, accepted: (String) -> Bool, wrong: () -> Void) -> Bool {
    var again = false
    while let pw = ask(again) {
        if accepted(pw) { return true }
        wrong(); again = true
    }
    return false
}
