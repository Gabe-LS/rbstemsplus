// RB Stems Plus: installs, reinstalls and uninstalls Demucs v4 (our model) and Stems Cache (the
// bridge) without Terminal, and includes the background watcher.
//
// Run from the watcher helper inside the app (Contents/Helpers/RB Stems Plus Watcher.app, a copy
// of this executable, started by the LaunchAgent), it is the watcher (app/Shell/WatcherShell.swift,
// over the core). Otherwise it is the app: the AppKit shell (app/Shell) over the shared C core
// (core/), which decides every light, button, text, menu item and action (M7's switch-over,
// mac-core §8); the window (Controller.swift) and today's platform code are the shell's host.
// Paths and names shared with the bridge and the release are in Paths.swift and Payload.swift.
import AppKit

/// The app's delegate: every event goes to the shell, which asks the core (close and quit by
/// core_close_asked, which bypasses the flow queue; the critical-work wait follows).
final class AppDelegate: NSObject, NSApplicationDelegate {
    var shell: CoreShell!
    /// the window (the probe finds the field ids' owner here)
    var controller: Controller? { shell?.controller }
    func applicationDidResignActive(_ notification: Notification) { shell?.deactivated() }
    func applicationWillTerminate(_ notification: Notification) { shell?.willTerminate() }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    /// Quitting (Cmd+Q, logout, shutdown): the core decides (a playlist being prepared is stopped
    /// first, if the user agrees); then it waits only while a root script runs or our model is
    /// copied into or renamed in rekordbox's folder (seconds; Shell.swift), and 60 seconds at most.
    /// The app's own quits never wait.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        shell?.shouldTerminate() ?? .terminateNow
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
    /// Coming back to the app: the core checks what changed meanwhile (e.g. rekordbox downloaded
    /// its STEMS Engine, or was updated) at once, then the watcher's request, if any.
    func applicationDidBecomeActive(_ notification: Notification) { shell?.activated() }
    /// "open -a" on an already open app arrives here, not as an activation
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        shell?.reopened()
        return true
    }
}

/// A start-up refusal's text from the core's table (texts.def, the pure text call: no core is made
/// yet), its slots filled; "" if the table refuses it.
func startupText(_ id: core_text_id, _ slots: [(core_slot_id, String)] = []) -> String {
    let copies = slots.map { strdup($0.1)! }
    defer { copies.forEach { free($0) } }
    var cs = zip(slots, copies).map { core_slot(id: Int32($0.0.0.rawValue), reserved: 0, value: UnsafePointer($0.1), len: strlen($0.1)) }
    var buf = [CChar](repeating: 0, count: 2048)
    var n = 0
    guard core_text(CORE_OS_MAC, id, &cs, Int32(cs.count), &buf, buf.count, &n) == Int32(CORE_OK) else { return "" }
    return String(decoding: buf.prefix(n).map { UInt8(bitPattern: $0) }, as: UTF8.self)
}

// "--swap <staged app> <app>": the update's exchange, run by the update script from the staged
// new app once the old one has quit (Update.swift). Before anything else: no window, no folder,
// nothing read but the two apps' Info.plist.
if CommandLine.arguments.count == 4 && CommandLine.arguments[1] == "--swap" {
    // the new app's shared C core checks itself first (mac-core §9 G1.4): a new app whose
    // self-test fails isn't swapped in, so the old app stays (updateScript: "failed swap")
    let selftest = core_selftest()
    if selftest != 0 { FileHandle.standardError.write(Data("the new app's core failed its self-test (check \(selftest))\n".utf8)); exit(1) }
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
    _ = Sheet(title: startupText(T_CANT_OPEN), message: why, buttons: [startupText(T_QUIT)]).runAlone()
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
    _ = Sheet(title: startupText(T_ALREADY_OPEN), message: startupText(T_ALREADY_OPEN_MSG), buttons: [startupText(T_OK)]).runAlone()
    exit(0)
case .failed(let why):
    NSApplication.shared.setActivationPolicy(.regular)
    _ = Sheet(title: startupText(T_CANT_OPEN),
              message: startupText(T_LOCK_UNUSABLE, [(CORE_SLOT_path, appLock.replacingOccurrences(of: home, with: "~")), (CORE_SLOT_why, why)]),
              buttons: [startupText(T_QUIT)]).runAlone()
    exit(1)
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
let delegate = AppDelegate()
app.delegate = delegate
let shell = CoreShell.make()                     // the window and the shell's tabs (app/Shell)
app.mainMenu = shell.makeMenu()                  // from the core's menus.def
delegate.shell = shell
// core_new, start, the first render, the window, shown. A core that refuses this build (its API
// version or ids, logged) is said as the other start-up refusals are, never a silent quit.
if !shell.start() {
    _ = Sheet(title: startupText(T_CANT_OPEN), message: "", buttons: [startupText(T_QUIT)]).runAlone()
    exit(1)
}
#if TEST_BUILD
coreSpikeStartIfAsked()                          // the G1 API spike (app/Spike), when the harness asks
#endif
app.activate(ignoringOtherApps: true)
app.run()
