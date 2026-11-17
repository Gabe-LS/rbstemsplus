// Admin runs: a bash script run as root with sudo, as a child of this app, so App Management
// attributes the change to RB Stems Plus. macOS's own password dialog can't be used: it runs
// the command under Apple's authtrampoline, which App Management refuses without asking. The
// password is asked in this app, passed to sudo once per action, and forgotten afterwards (sudo -k).
// One attempt is the shell's host->admin (app/Shell/ShellAdmin.swift); the core runs the attempts,
// "Permission needed" and the alerts (M7's switch-over). The password sheet's loop stays here.
import AppKit

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
