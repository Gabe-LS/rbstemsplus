// RB Stems Plus: installs, reinstalls and uninstalls Demucs v4 (our model) and Stems Cache (the
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
    // an ellipsis: it asks before it updates
    let update = NSMenuItem(title: "Update RB Stems Plus…", action: #selector(Controller.updateApp), keyEquivalent: "")
    update.target = c
    update.isHidden = true                      // until a newer RB Stems Plus is out
    c.updateItem = update
    let uninstall = NSMenuItem(title: "Uninstall RB Stems Plus…", action: #selector(Controller.uninstallEverything), keyEquivalent: "")
    uninstall.target = c
    return [update, settings, uninstall]
}

func item(_ title: String, _ action: Selector, _ key: String = "", _ c: Controller) -> NSMenuItem {
    let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
    i.target = c
    return i
}

/// View, Prepare and Manage: every button of the window, in the menu bar too.
func windowMenus(_ c: Controller) -> [NSMenu] {
    let view = NSMenu(title: "View")
    view.addItem(item("Status", #selector(Controller.showStatusTab), "1", c))
    view.addItem(item("Prepare", #selector(Controller.showPrepareTab), "2", c))
    view.addItem(item("Manage", #selector(Controller.showManageTab), "3", c))
    let prepare = NSMenu(title: "Prepare")
    prepare.addItem(item("Prepare Playlist…", #selector(Controller.preparePlaylistMenu), "", c))
    prepare.addItem(item("Check Again", #selector(Controller.checkAgainMenu), "r", c))
    prepare.addItem(.separator())
    prepare.addItem(item("Pause", #selector(Controller.pauseMenu), "", c))
    prepare.addItem(item("Stop", #selector(Controller.stopMenu), ".", c))
    let manage = NSMenu(title: "Manage")
    for (name, actions) in [("Demucs v4", [("Install", #selector(Controller.installPlusAction)), ("Reinstall", #selector(Controller.reinstallPlusAction)),
                                            ("Uninstall…", #selector(Controller.uninstallStemsPlus))]),
                            ("Stems Cache", [("Install", #selector(Controller.installCacheAction)), ("Reinstall", #selector(Controller.reinstallCacheAction)),
                                              ("Uninstall…", #selector(Controller.uninstallCache))])] {
        let sub = NSMenu(title: name)
        for (t, a) in actions { sub.addItem(item(t, a, "", c)) }
        let parent = NSMenuItem(title: name, action: nil, keyEquivalent: "")
        parent.submenu = sub
        manage.addItem(parent)
    }
    manage.addItem(.separator())
    manage.addItem(item("Show Log", #selector(Controller.showLogMenu), "", c))
    return [view, prepare, manage]
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

/// The controller's menu items: as their window buttons are (off while an action runs).
extension Controller: NSMenuItemValidation {
    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        let preparing = prepareTab?.isRunning == true
        switch item.action {
        case #selector(showStatusTab), #selector(showPrepareTab), #selector(showManageTab), #selector(showLogMenu): return true
        case #selector(checkAgainMenu): return !actionRunning && !preparing && prepareTab?.selected != nil
        case #selector(pauseMenu):
            item.title = prepareTab?.run?.paused == true ? "Resume" : "Pause"
            return preparing
        case #selector(stopMenu): return preparing
        case #selector(installPlusAction): return !actionRunning && installPlusBtn.isEnabled
        case #selector(installCacheAction): return !actionRunning && installCacheBtn.isEnabled
        case #selector(reinstallPlusAction): return !actionRunning && reinstallPlusBtn.isEnabled
        case #selector(reinstallCacheAction): return !actionRunning && reinstallCacheBtn.isEnabled
        case #selector(uninstallStemsPlus): return !actionRunning && uninstallPlusBtn.isEnabled
        case #selector(uninstallCache): return !actionRunning && uninstallCacheBtn.isEnabled
        case #selector(uninstallEverything): return !actionRunning && uninstallAllBtn.isEnabled
        default: return !actionRunning
        }
    }
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
    for m in windowMenus(controller) {
        let i = NSMenuItem(); i.submenu = m; main.addItem(i)
    }
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
    #if TEST_BUILD && CORE_LINKED
    /// the AppKit shell over the core (app/Shell), when this TEST app runs as it: the core decides
    var shell: CoreShell?
    func applicationDidResignActive(_ notification: Notification) { shell?.deactivated() }
    func applicationWillTerminate(_ notification: Notification) { shell?.willTerminate() }
    #endif
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    /// Quitting (Cmd+Q, the Quit button, logout, shutdown) waits only while a root script runs or
    /// our model is copied into or renamed in rekordbox's folder (seconds; Shell.swift), and 60
    /// seconds at most. Any other time it goes ahead: the busy marker lets the next launch finish
    /// the action. The app's own quits never wait.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        #if TEST_BUILD && CORE_LINKED
        if let s = shell { return s.shouldTerminate() }
        #endif
        // a playlist being prepared: stopped first (its set written), if the user agrees
        if controller?.confirmStopPreparing("Quit") == false { return .terminateCancel }
        if quitNow() { return .terminateNow }
        controller?.log("quit: waiting for the step that is changing rekordbox to finish")
        critical.whenIdle(timeout: 60) { DispatchQueue.main.async { NSApp.reply(toApplicationShouldTerminate: true) } }
        return .terminateLater
    }
    /// macOS's standard About panel: icon, name, "Version 1.0.0" (the release's version,
    /// currentShownVersion; the build number, the app's own, left out), the copyright line from
    /// Info.plist, and the credits: the home page, the licence and that AlphaTheta has nothing to
    /// do with it
    @objc func showAbout() {
        let small = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
        let centered = NSMutableParagraphStyle(); centered.alignment = .center
        let plain: [NSAttributedString.Key: Any] = [.font: small, .foregroundColor: NSColor.labelColor, .paragraphStyle: centered]
        let credits = NSMutableAttributedString()
        var link = plain; link[.link] = URL(string: homeURL)
        credits.append(NSAttributedString(string: homeURL.replacingOccurrences(of: "https://", with: ""), attributes: link))
        credits.append(NSAttributedString(string: "\n\nFree and open source (MIT licence).\n\nNot made or supported by\nAlphaTheta / Pioneer DJ.", attributes: plain))
        NSApp.orderFrontStandardAboutPanel(options: [.applicationVersion: currentShownVersion(), .version: "", .credits: credits])
        NSApp.activate(ignoringOtherApps: true)
    }
    @objc func openHome() { if let u = URL(string: homeURL) { NSWorkspace.shared.open(u) } }
    @objc func openTroubleshooting() { if let u = URL(string: troubleshootingURL) { NSWorkspace.shared.open(u) } }
    @objc func openIssues() { if let u = URL(string: issuesPageURL) { NSWorkspace.shared.open(u) } }
    /// Coming back to the app: what changed meanwhile (e.g. rekordbox downloaded its STEMS Engine,
    /// or was updated) shows at once, then the watcher's request, if any.
    func applicationDidBecomeActive(_ notification: Notification) {
        #if TEST_BUILD && CORE_LINKED
        if let s = shell { s.activated(); return }
        #endif
        #if TEST_BUILD
        probeMark("trigger.activate")
        #endif
        LocationMonitor.shared.update()                 // where the stems are saved, in the background
        DispatchQueue.main.async { self.controller?.prepareChanged(quiet: true); self.controller?.refresh(); self.controller?.checkRequest(); self.controller?.checkWatcherOn() }
    }
    /// "open -a" on an already open app arrives here, not as an activation
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        #if TEST_BUILD && CORE_LINKED
        if let s = shell { s.reopened(); return true }
        #endif
        DispatchQueue.main.async { self.controller?.checkRequest() }
        return true
    }
}

// "--swap <staged app> <app>": the update's exchange, run by the update script from the staged
// new app once the old one has quit (Update.swift). Before anything else: no window, no folder,
// nothing read but the two apps' Info.plist.
if CommandLine.arguments.count == 4 && CommandLine.arguments[1] == "--swap" {
    #if CORE_LINKED
    // the new app's shared C core checks itself first (mac-core §9 G1.4): a new app whose
    // self-test fails isn't swapped in, so the old app stays (updateScript: "failed swap")
    let selftest = core_selftest()
    if selftest != 0 { FileHandle.standardError.write(Data("the new app's core failed its self-test (check \(selftest))\n".utf8)); exit(1) }
    #endif
    if let why = swapApps(CommandLine.arguments[2], CommandLine.arguments[3]) {
        FileHandle.standardError.write(Data((why + "\n").utf8)); exit(1)
    }
    exit(0)
}

// the watcher: run from the watcher helper (RB Stems Plus Watcher.app), or with --watch (1.0's
// LaunchAgent started the app itself so, until the app rewrites it)
let isWatcher = Bundle.main.bundleIdentifier == watcherBundleID || CommandLine.arguments.contains("--watch")
#if TEST_BUILD
probeStart(watcher: isWatcher)                   // the harness's probe (app/Probe), test builds only
#endif

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
#if TEST_BUILD && CORE_LINKED
if coreShellAsked() { CoreShell.arm() }          // the next controller is the shell's (app/Shell)
#endif
let controller = Controller()
#if TEST_BUILD && CORE_LINKED
if let shell = CoreShell.armed {
    app.mainMenu = shell.makeMenu()
    delegate.shell = shell
} else {
    app.mainMenu = makeMenu(controller)
}
#else
app.mainMenu = makeMenu(controller)
#endif
delegate.controller = controller
#if TEST_BUILD && CORE_LINKED
if let shell = CoreShell.armed, !shell.start() { exit(1) }   // core_new, start, the first render, the window, shown
#endif
#if TEST_BUILD && CORE_LINKED
coreSpikeStartIfAsked()                          // the G1 API spike (app/Spike), when the harness asks
#endif
app.activate(ignoringOtherApps: true)
app.run()
