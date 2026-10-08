// RB Stems Plus: installs, reinstalls and uninstalls Stems Plus (our model) and Stems Cache (the
// bridge) without Terminal, and includes the background watcher.
//
// Run from the watcher helper inside the app (Contents/Helpers/RB Stems Plus Watcher.app, a copy
// of this executable, started by the LaunchAgent), it is the watcher (Watcher.swift). Otherwise
// it is the app: the window (Controller.swift), the actions
// (Actions.swift), the admin runs (Admin.swift) and the dialogs (Sheet.swift). Paths and names
// shared with the bridge and the release are in Paths.swift and Payload.swift.
import AppKit

/// Items added to the app menu, above "Hide RB Stems Plus": the controller's own.
func extraAppMenuItems(_ c: Controller) -> [NSMenuItem] {
    let settings = NSMenuItem(title: "Settings…", action: #selector(Controller.showSettings), keyEquivalent: ",")
    settings.target = c
    let update = NSMenuItem(title: "Update RB Stems Plus", action: #selector(Controller.updateApp), keyEquivalent: "")
    update.target = c
    update.isHidden = true                      // until a newer RB Stems Plus is out
    c.updateItem = update
    return [update, settings]
}

/// The Help menu's items: the project's home page, its troubleshooting page and issues page, and
/// the controller's report.
func helpMenuItems(_ c: Controller) -> [NSMenuItem] {
    let home = NSMenuItem(title: "RB Stems Plus Help", action: #selector(AppDelegate.openHome), keyEquivalent: "?")
    let troubleshooting = NSMenuItem(title: "RB Stems Plus Troubleshooting", action: #selector(AppDelegate.openTroubleshooting), keyEquivalent: "")
    let issue = NSMenuItem(title: "Report an Issue", action: #selector(AppDelegate.openIssues), keyEquivalent: "")
    let report = NSMenuItem(title: "Create Report", action: #selector(Controller.createReport), keyEquivalent: "")
    report.target = c
    return [home, troubleshooting, issue, report]
}

/// The controller's menu items are off while an action runs.
extension Controller: NSMenuItemValidation {
    func validateMenuItem(_ item: NSMenuItem) -> Bool { !actionRunning }
}

/// The standard menus: without them Cmd+Q, Cmd+W and Cmd+C/V/X/A (e.g. pasting into the
/// password field) do nothing.
func makeMenu(_ controller: Controller) -> NSMenu {
    let main = NSMenu()
    let appItem = NSMenuItem(); main.addItem(appItem)
    let appMenu = NSMenu()
    appMenu.addItem(withTitle: "About RB Stems Plus", action: #selector(AppDelegate.showAbout), keyEquivalent: "")
    appMenu.addItem(.separator())
    let extras = extraAppMenuItems(controller)
    if !extras.isEmpty { extras.forEach(appMenu.addItem); appMenu.addItem(.separator()) }
    appMenu.addItem(withTitle: "Hide RB Stems Plus", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
    appMenu.addItem(.separator())
    appMenu.addItem(withTitle: "Quit RB Stems Plus", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    appItem.submenu = appMenu
    let editItem = NSMenuItem(); main.addItem(editItem)
    let edit = NSMenu(title: "Edit")
    edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
    edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
    edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
    edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
    editItem.submenu = edit
    let windowItem = NSMenuItem(); main.addItem(windowItem)
    let win = NSMenu(title: "Window")
    win.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
    win.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
    windowItem.submenu = win
    let helpItem = NSMenuItem(); main.addItem(helpItem)
    let help = NSMenu(title: "Help")
    helpMenuItems(controller).forEach(help.addItem)
    helpItem.submenu = help
    NSApp.helpMenu = help
    return main
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var controller: Controller?
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    /// Quitting (Cmd+Q, the Quit button, logout, shutdown) waits only while a root script runs or
    /// our model is copied into or renamed in rekordbox's folder (seconds; Shell.swift), and 60
    /// seconds at most. Any other time it goes ahead: the busy marker lets the next launch finish
    /// the action. The app's own quits never wait.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if quitNow() { return .terminateNow }
        controller?.log("quit: waiting for the step that is changing rekordbox to finish")
        critical.whenIdle(timeout: 60) { DispatchQueue.main.async { NSApp.reply(toApplicationShouldTerminate: true) } }
        return .terminateLater
    }
    /// macOS's standard About panel: icon, name, "Version 1.0.0" (the build number, the same, left
    /// out), the copyright line from Info.plist, and the credits: the home page, the licence and
    /// that AlphaTheta has nothing to do with it
    @objc func showAbout() {
        let small = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
        let centered = NSMutableParagraphStyle(); centered.alignment = .center
        let plain: [NSAttributedString.Key: Any] = [.font: small, .foregroundColor: NSColor.labelColor, .paragraphStyle: centered]
        let credits = NSMutableAttributedString()
        var link = plain; link[.link] = URL(string: homeURL)
        credits.append(NSAttributedString(string: homeURL.replacingOccurrences(of: "https://", with: ""), attributes: link))
        credits.append(NSAttributedString(string: "\n\nFree and open source (MIT licence).\n\nNot made or supported by\nAlphaTheta / Pioneer DJ.", attributes: plain))
        NSApp.orderFrontStandardAboutPanel(options: [.version: "", .credits: credits])
        NSApp.activate(ignoringOtherApps: true)
    }
    @objc func openHome() { if let u = URL(string: homeURL) { NSWorkspace.shared.open(u) } }
    @objc func openTroubleshooting() { if let u = URL(string: troubleshootingURL) { NSWorkspace.shared.open(u) } }
    @objc func openIssues() { if let u = URL(string: issuesPageURL) { NSWorkspace.shared.open(u) } }
    /// Coming back to the app: what changed meanwhile (e.g. rekordbox downloaded its STEMS Engine,
    /// or was updated) shows at once, then the watcher's request, if any.
    func applicationDidBecomeActive(_ notification: Notification) {
        DispatchQueue.main.async { self.controller?.refresh(); self.controller?.checkRequest(); self.controller?.checkWatcherOn() }
    }
    /// "open -a" on an already open app arrives here, not as an activation
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        DispatchQueue.main.async { self.controller?.checkRequest() }
        return true
    }
}

// "--swap <staged app> <app>": the update's exchange, run by the update script from the staged
// new app once the old one has quit (Update.swift). Before anything else: no window, no folder,
// nothing read but the two apps' Info.plist.
if CommandLine.arguments.count == 4 && CommandLine.arguments[1] == "--swap" {
    if let why = swapApps(CommandLine.arguments[2], CommandLine.arguments[3]) {
        FileHandle.standardError.write(Data((why + "\n").utf8)); exit(1)
    }
    exit(0)
}

// the watcher: run from the watcher helper (RB Stems Plus Watcher.app), or with --watch (1.0's
// LaunchAgent started the app itself so, until the app rewrites it)
let isWatcher = Bundle.main.bundleIdentifier == watcherBundleID || CommandLine.arguments.contains("--watch")

// before anything creates a folder: never as root, never without a plain home folder or with
// a folder of ours that is a link (Safety.swift). The watcher can't write its log then: macOS's
// log has the line.
if let why = startupProblem() {
    if isWatcher { NSLog("%@", "\(watcherName): not checking: \(why)"); exit(0) }
    NSApplication.shared.setActivationPolicy(.regular)
    _ = Sheet(title: "RB Stems Plus can't open", message: why, buttons: ["Quit"]).runAlone()
    exit(1)
}

if isWatcher { runWatcher() }

// one copy of the app at a time. macOS's "Quit & Reopen" (App Management) starts the new copy
// while the old one is still quitting: wait for it a little. The lock is held until this
// process ends.
try? FileManager.default.createDirectory(atPath: support, withIntermediateDirectories: true)
switch takeLock(appLock, wait: 10) {
case .held: break
case .busy:
    NSApplication.shared.setActivationPolicy(.regular)
    _ = Sheet(title: "Already open", message: "Use the RB Stems Plus window that is already open.", buttons: ["OK"]).runAlone()
    exit(0)
case .failed(let why):
    NSApplication.shared.setActivationPolicy(.regular)
    _ = Sheet(title: "RB Stems Plus can't open",
              message: "\(appLock.replacingOccurrences(of: home, with: "~")) can't be used (\(why)). Move it to the Trash, then open RB Stems Plus again.",
              buttons: ["Quit"]).runAlone()
    exit(1)
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let delegate = AppDelegate()
app.delegate = delegate
let controller = Controller()
app.mainMenu = makeMenu(controller)
delegate.controller = controller
app.activate(ignoringOtherApps: true)
app.run()
