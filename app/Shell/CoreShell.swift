// The AppKit shell over the shared C core (the private Windows plan's mac-core §3-4, M7). The core
// decides; this shell draws its snapshots and sends it what the user and the OS did:
//
//   - the life cycle, as core.h's Life cycle paragraph says: core_new, {"ev":"start"},
//     core_commit (the first render, before the window shows), the window shown, {"ev":"shown"};
//   - render: one pass per snapshot, only the sections whose revision moved, rows by their delta;
//     the previous snapshot is released once the next one is applied (ShellTabs.swift draws the
//     Status and Prepare tabs; this file the window's own views: lights, status line, spinner,
//     buttons, the tab, the log);
//   - events: activation, deactivation, deminiaturize, reopen, the tab the user chose, buttons,
//     menu items, close and quit (core_close_asked, which bypasses the flow queue);
//   - the menus, built from menus.def (core_menu_count/_at) and validated live (core_menu_state);
//     macOS's own items (About, Hide, Quit, Edit, Window) stay AppKit's;
//   - the Swift host table over today's platform code (facts, slow facts, steps, admin, the
//     helper, the queues, the dialogs through the presenter, the log).
//
// Shells hold view-handle state, never decisions (§3.4): the outline's echo guard, expansion,
// scroll, first responder. Nothing is decided here: every view takes its value from a snapshot
// field (a view the core has no field for keeps what Controller built).
//
// Every build (M7's switch-over, mac-core §8: from 1.4.0 the app is this shell; scripts/build-app.sh
// compiles app/Shell into releases and TEST builds alike). The harness's probe (app/Probe) is a TEST
// build's only: its marks go through probe(_:), which is nothing in a release.
import AppKit
import Darwin

/// The harness's timing and field-id marks (app/Probe): a TEST build's only.
@inline(__always) func probe(_ mark: StaticString) {
    #if TEST_BUILD
    probeMark(mark)
    #endif
}

/// A blocking host call's check that it isn't on main (test builds only; release builds: nothing).
@inline(__always) func notOnMain() {
    #if TEST_BUILD
    dispatchPrecondition(condition: .notOnQueue(.main))
    #endif
}

private func shellOf(_ ctx: UnsafeMutableRawPointer?) -> CoreShell { Unmanaged<CoreShell>.fromOpaque(ctx!).takeUnretainedValue() }

/// A C string with its length as Swift text (UTF-8, never NUL-terminated by force).
func cString(_ p: UnsafePointer<CChar>?, _ n: Int) -> String {
    guard let p = p else { return "" }
    return String(decoding: UnsafeRawBufferPointer(start: p, count: n), as: UTF8.self)
}

/// A snapshot string field.
func snapStr(_ s: OpaquePointer?, _ f: core_field) -> String {
    var n = 0
    return cString(core_str(s, f, &n), n)
}

/// The buttons the window draws, by the "id" of the {"ev":"click"} they send (flows.c click_ids;
/// the Prepare and Status tabs' ids are the menus' events where one exists: M4 confirms them).
enum ClickID {
    static let installPlus = "install_plus", reinstallPlus = "reinstall_plus", uninstallPlus = "uninstall_plus"
    static let installCache = "install_cache", reinstallCache = "reinstall_cache", uninstallCache = "uninstall_cache"
    static let reinstall = "reinstall", uninstallAll = "uninstall_all", settings = "settings"
    static let prepare = "prepare", checkAgain = "check_again", pause = "pause", stop = "stop"
    static let prepareAgain = "prepare_again", stopKeeping = "stop_keeping"
}

final class CoreShell: NSObject, NSWindowDelegate, NSMenuItemValidation {
    let controller: Controller
    private(set) var core: OpaquePointer?
    /// the snapshot the window shows: the last render's (main)
    private(set) var held: OpaquePointer?
    private var rendered = false
    /// renders so far (the probe's end mark of a trigger with nothing to draw)
    private var renders = 0
    /// the spinner as drawn: started or stopped only when the field moves (1.3.0's busy did so)
    private var spinning = false
    private var renderingTab = false
    private var logShownSeq: Int64 = 0
    private var tabObservation: NSKeyValueObservation?
    private(set) var statusTab: ShellStatusTab!
    private(set) var prepareTab: ShellPrepareTab!
    private var menuIDs: [ObjectIdentifier: core_menu_id] = [:]
    private var clickIDs: [ObjectIdentifier: String] = [:]
    /// the core's flow thread is today's serial work queue (Controller.work); the quit flow its own
    private let quitQueue = DispatchQueue(label: "rbstemsplus.quit")
    private let helpers = ShellHelpers()
    private let shaMemo = ShaMemo()
    /// a quit the core answers later (close_reply(QUIT))
    private var quitPending = false
    // A test's stand-ins (shell-test.swift), settable in test builds only; a release build has
    // the app's own, fixed.
    #if TEST_BUILD
    /// a test's own dialog presentation; the app's is the window's sheet
    var presentDialog: ((DialogForm) -> DialogAnswer?)?
    /// a test's own step runner; the app's is HostSteps.swift's shellStep
    var runStep: ((core_step, [String], OpaquePointer?) -> Int32)?
    /// a test's own admin; the app's is ShellAdmin over sudo
    var runAdmin: ((Int32, String, String, String, Bool) -> Int32)?
    /// a test's own helper path (the app's: prepareHelper(), the harness's fake when it has one)
    var helperPath: () -> String = { prepareHelper() }
    /// a test's log sink in place of the app's (the log file and the log view)
    var logged: ((String) -> Void)?
    /// a test's view of every event sent
    var onSend: ((String) -> Void)?
    /// a test's view of the core's timing points (core_test_set_mark), after the probe's mark
    var onMark: ((String) -> Void)?
    /// the answer to a terminateLater (a test's stand-in records it instead of quitting)
    var replyTerminate: (Bool) -> Void = { NSApp.reply(toApplicationShouldTerminate: $0) }
    #else
    let presentDialog: ((DialogForm) -> DialogAnswer?)? = nil
    let runStep: ((core_step, [String], OpaquePointer?) -> Int32)? = nil
    let runAdmin: ((Int32, String, String, String, Bool) -> Int32)? = nil
    let helperPath: () -> String = { prepareHelper() }
    let logged: ((String) -> Void)? = nil
    let onSend: ((String) -> Void)? = nil
    let replyTerminate: (Bool) -> Void = { NSApp.reply(toApplicationShouldTerminate: $0) }
    #endif

    init(_ c: Controller) {
        controller = c
        super.init()
    }

    /// The app's shell (main.swift): the window (Controller) with the shell's Status and Prepare
    /// tabs, its buttons sending their clicks to the core. Nothing is shown before start().
    static func make() -> CoreShell {
        var made: CoreShell!
        _ = Controller { c in
            made = CoreShell(c)
            made.statusTab = ShellStatusTab(made)
            made.prepareTab = ShellPrepareTab(made)
            return (made.statusTab, made.prepareTab)
        }
        made.wire()
        return made
    }

    // MARK: life cycle

    /// The window's delegate, the probe's field ids and the Manage buttons' clicks.
    private func wire() {
        controller.window.delegate = self
        #if TEST_BUILD
        probeShellTabs(status: statusTab, prepare: prepareTab)   // the probe's field ids: status.*, prepare.*
        #endif
        for (b, id) in [(controller.installPlusBtn, ClickID.installPlus), (controller.reinstallPlusBtn, ClickID.reinstallPlus),
                        (controller.uninstallPlusBtn, ClickID.uninstallPlus), (controller.installCacheBtn, ClickID.installCache),
                        (controller.reinstallCacheBtn, ClickID.reinstallCache), (controller.uninstallCacheBtn, ClickID.uninstallCache),
                        (controller.reinstallBtn, ClickID.reinstall), (controller.uninstallAllBtn, ClickID.uninstallAll),
                        (controller.settingsBtn, ClickID.settings)] {
            button(b, id)
        }
    }

    /// A button that sends {"ev":"click","id":id}.
    func button(_ b: NSButton, _ id: String) {
        clickIDs[ObjectIdentifier(b)] = id
        b.target = self
        b.action = #selector(clicked(_:))
    }

    /// The host table: every entry today's platform code (core.h "Host callbacks").
    func makeHost() -> core_host {
        var h = core_host()
        h.size = UInt32(MemoryLayout<core_host>.size)
        h.ctx = Unmanaged.passUnretained(self).toOpaque()      // the shell lives for the process
        h.render = { ctx, snap, delta in shellOf(ctx).render(snap!, delta) }
        h.front_facts = { ctx in shellOf(ctx).frontFacts() }
        h.modal_open = { ctx in shellOf(ctx).modalOpen() ? 1 : 0 }
        h.facts = { ctx, out in shellOf(ctx).cheapFacts(out) }
        h.post_main = { _, job in DispatchQueue.main.async { core_run(job) } }
        h.post_work = { ctx, kind, job in shellOf(ctx).post(kind, job) }
        h.dialog = { ctx, d, r in shellOf(ctx).dialog(d!, r!) }
        h.pick_folder = { _, start, n, r in ShellDialogs.pickFolder(start: cString(start, n == CORE_NTS ? strlen(start!) : n), r!) }
        // slow_fact, admin and step block: core.h calls them on a core worker or the flow thread,
        // never on main (G2.3: no slow host call on main; test builds check it)
        h.slow_fact = { ctx, kind, key, n, out in
            notOnMain()
            return shellOf(ctx).slowFact(kind, cString(key, n == CORE_NTS ? strlen(key!) : n), out)
        }
        h.helper_spawn = { ctx, run, argv, argc in
            let s = shellOf(ctx)
            let args = (0..<Int(argc)).map { String(cString: argv![$0]!) }
            return s.helpers.spawn(run, s.helperPath(), args, core: s.core)
        }
        h.helper_signal = { ctx, run, sig in shellOf(ctx).helpers.signal(run, sig) }
        h.admin = { ctx, kind, version, bundle, bridgeSha, ortSha, force in
            notOnMain()
            let s = shellOf(ctx)
            let v = version.map { String(cString: $0) } ?? "", b = bridgeSha.map { String(cString: $0) } ?? "", o = ortSha.map { String(cString: $0) } ?? ""
            _ = bundle                                       // rbBundleID: the scripts carry it themselves (Scripts.swift)
            return s.admin(kind, v, b, o, force != 0)
        }
        h.step = { ctx, kind, args, n, out in
            notOnMain()
            let a = (0..<Int(max(0, n))).map { args![$0].map { String(cString: $0) } ?? "" }
            return shellOf(ctx).step(kind, a, out)
        }
        h.close_reply = { ctx, what, proceed in shellOf(ctx).closeReply(what, proceed != 0) }
        h.log = { ctx, line, n in shellOf(ctx).log(cString(line, n)) }
        return h
    }

    /// Start-up (core.h Life cycle). False when the core refused the host, the config or this
    /// build's ids (the window isn't shown then: main.swift says "RB Stems Plus can't open" and quits).
    @discardableResult
    func start(show: Bool = true, shown: Bool = true) -> Bool {
        guard core == nil else { return true }
        guard core_api_version() == CORE_API_VERSION, core_ids_hash() == coreIdsHash else {
            log("core: the core's API version or ids hash isn't this build's: not started")
            return false
        }
        var h = makeHost()
        let config = ShellConfig.json()
        core = config.withCString { core_new(&h, CORE_ROLE_APP, $0, CORE_NTS) }
        guard let c = core else { log("core: core_new refused the host or the config"); return false }
        #if TEST_BUILD
        // the probe's timing points the core decides: trigger.actionEnd where A's Controller.busy(nil) marks it
        _ = core_test_set_mark(c, { ctx, name in
            guard let name = name else { return }
            shellOf(ctx).coreMark(String(cString: name))
        }, Unmanaged.passUnretained(self).toOpaque())
        #endif
        _ = send(#"{"ev":"start"}"#)
        core_commit(c)                                       // the first render: the views' start state
        observeTabs()
        if show {
            controller.window.center()
            controller.window.makeKeyAndOrderFront(nil)
        }
        probe("shell.core")                              // B's launches told apart by the probe, not the log
        log("started: RB Stems Plus \(versionText(shown: currentShownVersion()))\(titleNote), macOS \(ProcessInfo.processInfo.operatingSystemVersionString), user \(NSUserName()), bundle \(Bundle.main.bundlePath)\(bundleProblem(Bundle.main.bundlePath, executable: Bundle.main.executablePath).map { " (\($0))" } ?? "")")
        if shown { _ = send(#"{"ev":"shown"}"#) }
        startLocation()
        return true
    }

    /// Where the stems are saved, measured in the background from now on, as Controller's
    /// startupChecks does: LocationMonitor's own mount, unmount and rename observers, its 10-minute
    /// size and the bridge-state watch. Each change of its snapshot is one {"ev":"location"} (its
    /// onChange, which the core handles as today's; one trigger.location mark, where A marks it).
    /// M4 seam: once the LocationMonitor memo is the core's, mount, unmount, rename and
    /// bridge_state go as their own events instead, still one trigger mark per change.
    private func startLocation() {
        LocationMonitor.shared.onChange = { [weak self] in self?.send(#"{"ev":"location"}"#) }
        LocationMonitor.shared.startObserving()
    }

    /// A timing point the core decides (core_test_set_mark, main, inside the core's own work):
    /// trigger.actionEnd, A's mark in Controller.busy(nil), for P3.actionEnd (tests/perf/stats.py
    /// pairs it with the next refresh.end, which the core's busy(nil) refresh renders). A TEST
    /// build's only, as the probe and onMark are.
    #if TEST_BUILD
    func coreMark(_ name: String) {
        if name == "trigger.actionEnd" { probeMark("trigger.actionEnd") }
        onMark?(name)
    }
    #endif

    /// One event to the core (main). B's timing point for P3 is here: the trigger's name, as A's
    /// probeMark at today's trigger sites (tests/perf/stats.py pairs it with the next refresh.end).
    /// A trigger the core handles with nothing to draw (the same lights and texts) gets no render,
    /// so its end is marked once the core's own turn is over (its commit job, posted during
    /// core_event, runs first): A's refresh() ran, and marked its end, on every trigger.
    @discardableResult
    func send(_ json: String) -> Int32 {
        guard let c = core else { return Int32(CORE_E_STATE) }
        onSend?(json)
        var trigger = true
        switch json {
        case #"{"ev":"activated"}"#: probe("trigger.activate")
        case _ where json.hasPrefix(#"{"ev":"mount""#) || json.hasPrefix(#"{"ev":"unmount""#) || json.hasPrefix(#"{"ev":"rename""#)
                     || json == #"{"ev":"bridge_state"}"# || json == #"{"ev":"location"}"#: probe("trigger.location")
        default: trigger = false
        }
        let rendersBefore = renders
        let r = json.withCString { core_event(c, $0, strlen($0)) }
        if r != Int32(CORE_OK) { log("core: event refused (\(r)): \(json)") }
        #if TEST_BUILD
        if trigger {
            DispatchQueue.main.async {
                if self.renders == rendersBefore { probe("refresh.begin"); probe("refresh.end") }   // handled, nothing to draw
            }
        }
        #else
        _ = trigger; _ = rendersBefore
        #endif
        return r
    }

    /// A JSON string literal (the events' members: ids, paths, names).
    static func quote(_ s: String) -> String {
        var o = "\""
        for u in s.unicodeScalars {
            switch u {
            case "\"": o += "\\\""
            case "\\": o += "\\\\"
            case "\n": o += "\\n"
            case "\r": o += "\\r"
            case "\t": o += "\\t"
            case _ where u.value < 0x20: o += String(format: "\\u%04x", u.value)
            default: o.unicodeScalars.append(u)
            }
        }
        return o + "\""
    }

    // MARK: events

    @objc func clicked(_ b: NSButton) {
        guard let id = clickIDs[ObjectIdentifier(b)] else { return }
        send("{\"ev\":\"click\",\"id\":\(CoreShell.quote(id))}")
    }

    @objc func menuChosen(_ item: NSMenuItem) {
        guard let id = menuIDs[ObjectIdentifier(item)] else { return }
        var mi = core_menu_item()
        let n = core_menu_count(CORE_OS_MAC)
        for i in 0..<n where core_menu_at(CORE_OS_MAC, i, &mi) == Int32(CORE_OK) && mi.id == Int32(id.rawValue) {
            let ev = mi.event.map { String(cString: $0) } ?? ""
            if !ev.isEmpty { send("{\"ev\":\"menu\",\"id\":\(CoreShell.quote(ev))}") }
            return
        }
    }

    /// The tab the user chose (a toolbar click). A tab the core chose by render sends nothing back.
    private func observeTabs() {
        tabObservation = controller.tabs.observe(\.selectedTabViewItemIndex, options: [.new]) { [weak self] tabs, _ in
            guard let self = self, !self.renderingTab else { return }
            self.send("{\"ev\":\"tab\",\"tab\":\(tabs.selectedTabViewItemIndex)}")
        }
    }

    /// Coming back to the app: the location measured again in the background (main.swift's
    /// applicationDidBecomeActive), then the event.
    func activated() {
        LocationMonitor.shared.update()
        send(#"{"ev":"activated"}"#)
    }
    func deactivated() { send(#"{"ev":"deactivated"}"#) }
    func reopened() { send(#"{"ev":"reopened"}"#) }
    func windowDidDeminiaturize(_ notification: Notification) { send(#"{"ev":"deminiaturized"}"#) }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        guard let c = core else { return true }
        return Int(core_close_asked(c, CORE_CLOSE_WINDOW)) == CORE_CLOSE_NOW
    }

    /// AppDelegate.applicationShouldTerminate in the shell: the core decides; the CriticalWork
    /// wait still follows a "now" or a proceed (core.h Close and quit).
    func shouldTerminate() -> NSApplication.TerminateReply {
        guard let c = core else { return .terminateNow }
        switch Int(core_close_asked(c, CORE_CLOSE_QUIT)) {
        case CORE_CLOSE_NOW: return terminateAfterCriticalWork() ? .terminateNow : .terminateLater
        case CORE_CLOSE_LATER: quitPending = true; return .terminateLater
        default: return .terminateCancel
        }
    }

    /// true: quit now; false: the critical step's wait answers terminateLater's reply.
    private func terminateAfterCriticalWork() -> Bool {
        if quitNow() { return true }
        log("quit: waiting for the step that is changing rekordbox to finish")
        critical.whenIdle(timeout: 60) { DispatchQueue.main.async { self.replyTerminate(true) } }
        return false
    }

    /// host->close_reply: from the core's own main job. The action is posted, never done inside.
    private func closeReply(_ what: core_close, _ proceed: Bool) {
        DispatchQueue.main.async {
            switch what {
            case CORE_CLOSE_QUIT:
                guard self.quitPending else { return }
                self.quitPending = false
                if !proceed { self.replyTerminate(false); return }
                if self.terminateAfterCriticalWork() { self.replyTerminate(true) }
            case CORE_CLOSE_WINDOW:
                if proceed { self.controller.window.performClose(nil) }
            default: break
            }
        }
    }

    /// The app is quitting: dialogs not shown yet answer "not shown" (core.h Dialogs).
    func willTerminate() { DialogPresenter.shared.shutDown() }

    // MARK: render

    private func render(_ snap: OpaquePointer, _ delta: OpaquePointer?) {
        probe("refresh.begin")
        let previous = held
        held = snap
        let all = !rendered || delta == nil
        let moved = all ? ~UInt32(0) : core_delta_sections(delta)
        func did(_ s: core_section) -> Bool { moved & (UInt32(1) << s.rawValue) != 0 }
        if did(S_APP) { renderApp(snap) }
        if did(S_STATUS) || did(S_APP) { statusTab?.render(snap, all ? nil : delta) }
        if did(S_PREPARE) { prepareTab?.render(snap, all ? nil : delta) }
        // S_SETTINGS: the Settings sheet is the core's dialog (host->dialog)
        rendered = true
        renders += 1
        if let p = previous { core_snap_release(p) }
        probe("refresh.end")
    }

    private func renderApp(_ s: OpaquePointer) {
        let c = controller
        let title = snapStr(s, F_WINDOW_TITLE)
        if !title.isEmpty, c.window.title != title { c.window.title = title }   // empty: MainTabs' (the tab's name)
        let status = snapStr(s, F_STATUS_LINE)
        if c.status.stringValue != status { c.status.stringValue = status }
        let spin = core_flag(s, F_SPINNER) != 0
        if spin != spinning {
            spinning = spin
            if spin { c.spinner.startAnimation(nil) } else { c.spinner.stopAnimation(nil) }
        }
        // the lights: drawn once the view model gives their tip (before M4 the look's 0 would read "on")
        for (icon, label, look, tip, ax) in [(c.plusIcon, c.plusLabel, F_LIGHT_PLUS_LOOK, F_LIGHT_PLUS_TIP, F_LIGHT_PLUS_AX),
                                             (c.cacheIcon, c.cacheLabel, F_LIGHT_CACHE_LOOK, F_LIGHT_CACHE_TIP, F_LIGHT_CACHE_AX)] {
            let t = snapStr(s, tip)
            guard !t.isEmpty else { continue }
            let l = core_i64(s, look)
            let drawing = lightDrawing(l == 0 ? .on : l == 1 ? .limited : .off)    // pure.h LOOK_ON, LOOK_LIMITED, LOOK_OFF
            let a = snapStr(s, ax)
            icon.image = NSImage(systemSymbolName: drawing.symbol, accessibilityDescription: a.isEmpty ? drawing.description : a)?
                .withSymbolConfiguration(.init(pointSize: 15, weight: .regular))
            icon.contentTintColor = drawing.tint
            icon.toolTip = t
            label.toolTip = t
        }
        statusTab?.setTips(plus: snapStr(s, F_LIGHT_PLUS_TIP), cache: snapStr(s, F_LIGHT_CACHE_TIP))
        // every Manage button by its field (Controller.refresh and busy: all off during an action)
        for (b, f) in [(c.installPlusBtn, F_INSTALL_PLUS_ENABLED), (c.installCacheBtn, F_INSTALL_CACHE_ENABLED),
                       (c.reinstallBtn, F_REINSTALL_ENABLED), (c.reinstallPlusBtn, F_REINSTALL_PLUS_ENABLED),
                       (c.reinstallCacheBtn, F_REINSTALL_CACHE_ENABLED), (c.settingsBtn, F_SETTINGS_ENABLED),
                       (c.uninstallPlusBtn, F_UNINSTALL_PLUS_ENABLED), (c.uninstallCacheBtn, F_UNINSTALL_CACHE_ENABLED),
                       (c.uninstallAllBtn, F_UNINSTALL_ALL_ENABLED)] {
            let on = core_flag(s, f) != 0
            if b.isEnabled != on { b.isEnabled = on }
        }
        let reinstall = snapStr(s, F_REINSTALL_TITLE)
        if !reinstall.isEmpty, c.reinstallBtn.title != reinstall { c.reinstallBtn.title = reinstall }
        c.updateItem?.isHidden = core_flag(s, F_UPDATE_VISIBLE) == 0
        let tab = Int(core_i64(s, F_TAB))
        if tab >= 0, tab < c.tabs.tabViewItems.count, c.tabs.selectedTabViewItemIndex != tab {
            renderingTab = true
            c.tabs.selectedTabViewItemIndex = tab
            renderingTab = false
        }
        let seq = core_i64(s, F_LOG_SHOW_SEQ)
        if seq != logShownSeq { logShownSeq = seq; if rendered { c.text.scrollToEndOfDocument(nil) } }
    }

    // MARK: menus

    /// The menu bar from menus.def: View, Prepare, Manage and Help and the app menu's own items
    /// from the table, macOS's standard items (About, Hide, Quit, Edit, Window) AppKit's own.
    func makeMenu() -> NSMenu {
        let bar = NSMenu()
        var menus: [Int32: NSMenu] = [:]               // a parent item's id -> its submenu
        var mi = core_menu_item()
        for i in 0..<core_menu_count(CORE_OS_MAC) where core_menu_at(CORE_OS_MAC, i, &mi) == Int32(CORE_OK) {
            let title = mi.title >= 0 ? coreText(core_text_id(UInt32(mi.title))) : ""
            let parent: NSMenu? = mi.menu < 0 ? nil : menus[mi.menu]
            switch Int(mi.kind) {
            case Int(CORE_MENU_SEPARATOR): parent?.addItem(.separator())
            case Int(CORE_MENU_NATIVE):
                if let p = parent { native(Int(mi.id), title, into: p) } else { bar.addItem(nativeMenu(Int(mi.id), title)) }
            default:
                let key = mi.key.map { String(cString: $0) } ?? ""
                let event = mi.event.map { String(cString: $0) } ?? ""
                // a menu bar item has no title of its own (as main.swift makes them); its menu has
                // one, except the app menu's
                let item = parent == nil ? NSMenuItem() : NSMenuItem(title: title, action: event.isEmpty ? nil : #selector(menuChosen(_:)), keyEquivalent: key)
                if !event.isEmpty { item.target = self }
                if !key.isEmpty { item.keyEquivalentModifierMask = Self.modifiers(mi.mods) }   // else AppKit's default, as today's
                menuIDs[ObjectIdentifier(item)] = core_menu_id(UInt32(mi.id))
                if event.isEmpty {                     // a menu: the app menu, View, Manage › Demucs v4, …
                    let sub = NSMenu(title: mi.id == Int32(M_APP.rawValue) ? "" : title)
                    item.submenu = sub
                    menus[mi.id] = sub
                    if mi.id == Int32(M_HELP.rawValue) { NSApp.helpMenu = sub }
                }
                if mi.id == Int32(M_APP_UPDATE.rawValue) { item.isHidden = true; controller.updateItem = item }   // until a newer RB Stems Plus is out
                (parent ?? bar).addItem(item)
            }
        }
        return bar
    }

    static func modifiers(_ m: UInt32) -> NSEvent.ModifierFlags {
        var f: NSEvent.ModifierFlags = []
        if m & UInt32(CORE_MOD_CMD_OR_CTRL) != 0 { f.insert(.command) }
        if m & UInt32(CORE_MOD_SHIFT) != 0 { f.insert(.shift) }
        if m & UInt32(CORE_MOD_ALT) != 0 { f.insert(.option) }
        return f
    }

    /// macOS's own item inside a menu (main.swift makeMenu's): About, Hide, Quit.
    private func native(_ id: Int, _ title: String, into m: NSMenu) {
        switch id {
        case Int(M_APP_ABOUT.rawValue): m.addItem(withTitle: title, action: #selector(AppDelegate.showAbout), keyEquivalent: "")
        case Int(M_APP_HIDE.rawValue): m.addItem(withTitle: title, action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        case Int(M_APP_QUIT.rawValue): m.addItem(withTitle: title, action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        default: break
        }
    }

    /// macOS's own menus (main.swift makeMenu's): Edit and Window.
    private func nativeMenu(_ id: Int, _ title: String) -> NSMenuItem {
        let item = NSMenuItem()
        let m = NSMenu(title: title)
        switch id {
        case Int(M_EDIT.rawValue):
            m.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
            m.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
            m.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
            m.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        case Int(M_WINDOW.rawValue):
            m.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
            m.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        default: break
        }
        item.submenu = m
        return item
    }

    /// Validation stays pull (§3.8): the core's state now, its title (Pause/Resume) included.
    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        guard let id = menuIDs[ObjectIdentifier(item)], let c = core else { return false }
        var title: UnsafePointer<CChar>?
        var n = 0
        let st = core_menu_state(c, id, &title, &n)
        if n > 0, let t = title { item.title = cString(t, n) }
        item.isHidden = st & UInt32(CORE_MENU_HIDDEN) != 0
        item.state = st & UInt32(CORE_MENU_CHECKED) != 0 ? .on : .off
        return st & UInt32(CORE_MENU_ENABLED) != 0
    }

    // MARK: the host table's entries

    /// front_facts: the raw facts, read live at the call (main).
    private func frontFacts() -> UInt32 {
        var bits = UInt32(0)
        if NSApp.isActive { bits |= UInt32(CORE_FRONT_ACTIVE) }
        let w = controller.window
        if controller.tabs.isViewLoaded { bits |= UInt32(CORE_FRONT_WINDOW_LOADED) }
        if w.isMiniaturized { bits |= UInt32(CORE_FRONT_MINIATURIZED) }
        let tab = UInt32(max(0, controller.tabs.selectedTabViewItemIndex)) & 0xFF
        return bits | (tab << UInt32(CORE_FRONT_TAB_SHIFT))
    }

    /// modal_open: a modal dialog or a sheet is up now (main).
    private func modalOpen() -> Bool { NSApp.modalWindow != nil || controller.window.attachedSheet != nil }

    /// facts: the cheap facts, today's reads, on every refresh (main). Never memoised (ShellFacts.swift).
    private func cheapFacts(_ out: OpaquePointer?) { ShellFacts.cheap(CoreFactSheet(out: out), helper: helperPath(), parts: core_facts_parts(out)) }

    /// post_work: today's queues per kind (§3.2, §3.11): the flow on the serial work queue.
    private func post(_ kind: core_work, _ job: OpaquePointer?) {
        switch kind {
        case CORE_WORK_FLOW: controller.work.async { core_run(job) }
        case CORE_WORK_QUIT: quitQueue.async { core_run(job) }
        case CORE_WORK_WALK: DispatchQueue.global(qos: .utility).async { core_run(job) }
        case CORE_WORK_BACKGROUND: DispatchQueue.global(qos: .background).async { core_run(job) }
        default: DispatchQueue.global(qos: .userInitiated).async { core_run(job) }
        }
    }

    /// dialog: flow only, blocks; through the presenter (or a test's stand-in).
    private func dialog(_ d: OpaquePointer, _ r: OpaquePointer) -> Int32 {
        ShellDialogs.dialog(d, r) { form in
            if let p = presentDialog { return p(form) }
            return ShellDialogs.presentSheet(form) { [weak self] in self?.controller.window }
        }
    }

    /// slow_fact: a worker. The file sha256s memoised by path, size and mtime (§3.5).
    private func slowFact(_ kind: core_slow, _ key: String, _ out: OpaquePointer?) -> Int32 {
        switch kind {
        case CORE_SLOW_MODEL_SHA, CORE_SLOW_BRIDGE_SHA:
            guard let sha = shaMemo.sha256(key) else { return 1 }
            sha.withCString { core_fact_str(out, FACT_SHA256, $0, strlen($0)) }
            return 0
        case CORE_SLOW_ALPHATHETA:
            core_fact_flag(out, FACT_SIGNATURE_OK, signedByPioneer(key) ? 1 : 0)
            return 0
        case CORE_SLOW_SIZE_WALK:
            guard let n = entriesSize(key) else { return 1 }
            core_fact_i64(out, FACT_SIZE_BYTES, n)
            return 0
        case CORE_SLOW_DRIVE:
            guard let free = freeBytes(key) else { return 1 }
            core_fact_i64(out, FACT_FREE_BYTES, free)   // M4 seam: FACT_DRIVE_STATE's values aren't defined yet
            return 0
        case CORE_SLOW_LOCATION:
            // Not measured here yet: the host keeps its own snapshot (LocationMonitor) in the cheap
            // facts and sends {"ev":"location"} on its change, as core.h allows (nonzero). Moving
            // the shell onto the core's memo also needs mount, unmount, rename and bridge_state as
            // events, LocationMonitor's size walk kept for Settings (M4/M7 seam).
            return 1
        default: return 1
        }
    }

    /// admin: flow only, blocks; one attempt (ShellAdmin.swift).
    private func admin(_ kind: Int32, _ version: String, _ bridgeSha: String, _ ortSha: String, _ force: Bool) -> Int32 {
        if let a = runAdmin { return a(kind, version, bridgeSha, ortSha, force) }
        let c = core
        let w = controller.window
        return ShellAdmin(ask: { again in ShellDialogs.askPassword(again) { w } },
                          log: { [weak self] in self?.log($0) },
                          accepted: { core_flow_progress(c, CORE_PROGRESS_ADMIN_ATTEMPT, 1, 0) },
                          notPermitted: { core_admin_note(c, Int32(CORE_ADMIN_NOTE_NOT_PERMITTED)) })
            .run(kind: kind, version: version, bridgeSha: bridgeSha, ortSha: ortSha, force: force)
    }

    /// step: any core worker (HostSteps.swift).
    private func step(_ kind: core_step, _ args: [String], _ out: OpaquePointer?) -> Int32 {
        if let s = runStep { return s(kind, args, out) }
        let env = ShellStepEnv(controller: controller, core: core, log: { [weak self] in self?.log($0) },
                               quitNow: { ownQuit = true; DispatchQueue.main.async { NSApp.terminate(nil) } })
        return shellStep(kind, args, out, env)
    }

    /// log: the one path for every core line and the shell's own (today's Controller.log: the log
    /// file and the log view). Any thread.
    func log(_ s: String) {
        if let l = logged { l(s) } else { controller.log(s) }
    }
}

/// The start config (core_new): the OS, the version and the paths the core needs (Paths.swift):
/// every key core_new lists that the Mac core reads (no Mac code reads tmp or watcher).
enum ShellConfig {
    static func json() -> String {
        let paths = ["home": home, "support": support, "app": Bundle.main.bundlePath, "helper": prepareHelper(),
                     "cache_default": defaultCacheDir, "log": logPath,
                     // the model's sha256 key and the prepare helper's arguments (prepareBaseArgs)
                     "model": modelDir + "/hdemucs.onnx", "root_ort": rootOrt, "rekordbox": rbApp, "kept": keptDir]
        let p = paths.keys.sorted().map { "\(CoreShell.quote($0)):\(CoreShell.quote(paths[$0]!))" }.joined(separator: ",")
        return "{\"os\":\"mac\",\"version\":\(CoreShell.quote(compiledVersion)),\"paths\":{\(p)}}"
    }
}

/// The file sha256s, memoised by path, size and mtime (the bridge's sha memo's rule): a changed
/// file is read again. Any thread.
final class ShaMemo {
    private let lock = NSLock()
    private var memo: [String: (size: Int64, mtime: Double, sha: String)] = [:]

    func sha256(_ path: String) -> String? {
        var st = stat()
        guard stat(path, &st) == 0 else { return nil }
        let size = Int64(st.st_size), mtime = Double(st.st_mtimespec.tv_sec) + Double(st.st_mtimespec.tv_nsec) / 1e9
        lock.lock()
        if let m = memo[path], m.size == size, m.mtime == mtime { lock.unlock(); return m.sha }
        lock.unlock()
        guard let sha = fileSHA256(path) else { return nil }
        lock.lock(); memo[path] = (size, mtime, sha); lock.unlock()
        return sha
    }
}

/// Payload.swift's sha256(_:) (CryptoKit), named apart from the memo's own method.
private func fileSHA256(_ path: String) -> String? { sha256(path) }

/// The helper's runs (Prepare.swift's process handling): a process and a reader thread per run,
/// every line to core_helper_line as it comes, then core_helper_ended once. Thread-safe.
final class ShellHelpers {
    private let lock = NSLock()
    private var runs: [UInt64: (process: Process, paused: Bool)] = [:]

    /// helper_spawn: only the pure functions may be called from inside it, so the reader thread
    /// does the rest. 0 started (one core_helper_ended follows), else not started.
    func spawn(_ run: UInt64, _ path: String, _ args: [String], core: OpaquePointer?) -> Int32 {
        guard let c = core else { return 1 }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        let out = Pipe(), err = Pipe()
        p.standardOutput = out; p.standardError = err; p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch { return 1 }
        lock.lock(); runs[run] = (p, false); lock.unlock()
        var errData = Data()
        let errDone = DispatchSemaphore(value: 0)
        DispatchQueue.global().async { errData = err.fileHandleForReading.readDataToEndOfFile(); errDone.signal() }
        let t = Thread { [weak self] in
            ShellHelpers.read(c, run, out.fileHandleForReading.fileDescriptor)
            p.waitUntilExit()
            errDone.wait()
            self?.lock.lock(); self?.runs[run] = nil; self?.lock.unlock()
            let e = String(decoding: errData, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            e.withCString { core_helper_ended(c, run, p.terminationStatus, $0, strlen($0)) }
        }
        t.name = "rbsp.helper.reader"
        t.qualityOfService = .userInitiated
        t.start()
        return 0
    }

    /// helper_signal: pause (SIGSTOP), resume (SIGCONT), stop (SIGTERM; a paused run is let go so
    /// it can stop), as PrepareRun does.
    func signal(_ run: UInt64, _ sig: core_helper_sig) {
        lock.lock(); defer { lock.unlock() }
        guard var r = runs[run], r.process.isRunning else { return }
        let pid = r.process.processIdentifier
        switch sig {
        case CORE_HELPER_PAUSE: if !r.paused { kill(pid, SIGSTOP); r.paused = true }
        case CORE_HELPER_RESUME: if r.paused { kill(pid, SIGCONT); r.paused = false }
        default:
            kill(pid, SIGTERM)
            if r.paused { kill(pid, SIGCONT); r.paused = false }
        }
        runs[run] = r
    }

    /// The reader: whole lines, without their newline, as they come (the probe's P7 point too).
    private static func read(_ c: OpaquePointer, _ run: UInt64, _ fd: Int32) {
        var buf = [UInt8](repeating: 0, count: 1 << 16)
        var have = 0
        while true {
            if have == buf.count { buf += [UInt8](repeating: 0, count: buf.count) }
            let n = buf.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress! + have, $0.count - have) }
            if n < 0 && errno == EINTR { continue }
            if n <= 0 { break }
            have += n
            var from = 0
            buf.withUnsafeBytes { raw in
                let b = raw.bindMemory(to: UInt8.self)
                for i in from..<have where b[i] == 10 {
                    #if TEST_BUILD
                    if i - from > 3, b[from + 2] == UInt8(ascii: "e") { probeHelperLine(Data(b[from..<i])) }
                    #endif
                    core_helper_line(c, run, UnsafeRawPointer(b.baseAddress! + from).assumingMemoryBound(to: CChar.self), i - from)
                    from = i + 1
                }
            }
            if from > 0 {
                buf.withUnsafeMutableBytes { raw in _ = memmove(raw.baseAddress!, raw.baseAddress! + from, have - from) }
                have -= from
            }
        }
        if have > 0 { buf.withUnsafeBytes { raw in core_helper_line(c, run, raw.bindMemory(to: CChar.self).baseAddress!, have) } }   // a last line without its newline
    }
}
