// The AppKit shell's host tests (tests/mac/shell-test.sh): app/Shell over the core's test-hooks
// archive and today's app/Sources, on the host, with no window shown, nothing activated and no
// real step, admin run or dialog: the shell's steps, admin and dialog presentation are the
// test's stand-ins where they would act on this Mac. What is checked:
//   - the presenter (mac-core §3.5): a dialog asked while the app is idle starts within one frame
//     (the wake-up); the main queue keeps draining under its modal; NSApp.modalWindow is set while
//     it is up; one at a time, in call order; refused on main; nothing once shutting down;
//   - the core's dialog read into a form and drawn as today's Sheet: keys, enabled states, the
//     answer by index (a row's button too), the fields' texts;
//   - host->dialog and host->step through the core's test flows (the reply reaches the core);
//   - host->admin's attempt (ShellAdmin): sudo's calls as Controller.admin makes them, the
//     password loop, the log lines, "Operation not permitted" noted, cancel, forget, the bridge
//     bytes CACHE_PREPARE checked (and no run without them), the script byte for byte;
//   - render: the first render at start-up, a field, every button, picker and outline by its view
//     model field (never a menu twin's or AppKit's default), the Prepare tab's texts (the name,
//     the summary and its secondary note, Keep, Pause/Resume, the date), the run's row shown and
//     hidden, the Status tab's run line, the tracks table's reload, changed rows and appended
//     rows, the tab the core chose (no tab event back) and the one the user chose;
//   - the menus built from menus.def against 1.3.0's makeMenu (frozen, menus130): the same items,
//     titles, keys, modifiers and order;
//   - the helper runs: lines to the core (one with no newline at the end), the run's end;
//   - the location measured from the start (LocationMonitor, read-only), its changes to the core;
//   - the cheap facts (ShellFacts.swift): every cheap row of facts.def mapped (a new row fails
//     until it is), each written or left unset as its doc says on this Mac, its value today's read;
//     the location snapshot's facts for every status, pure.h's LOC_* and PROBE_* numbers; the start
//     config's paths; every step kind and Mac slow kind core.h has, handled by the host table;
//   - the core's view model fed by those facts decides as 1.3.0's Controller.refresh on this Mac
//     (its pieces in tests/mac/shell-oracle.swift): the lights, every button, the Reinstall title,
//     the status line;
//   - the start-up checks ({"ev":"shown"}) through the host table: their steps, the watcher's
//     off state from WATCHER_CHECK's fact to the core's log line and status line;
//   - the watcher over the core (WatcherShell.swift): its host table and the core's watcher flow end
//     to end, the lock and the again-mark real files in a scratch folder: a start during a check
//     refused and checked by the running watcher, the question as 1.3.0's, Remind Me Later,
//     Reinstall Now; the start-up refusals' words from the core's table;
//   - the steps' sentence readbacks: every failure message Models.swift writes, every reason
//     removeInLocation returns and every sentence of modelLeftBehind, read from today's sources
//     (the repository is the first argument), through modelOutcomeCode, removalCode and
//     leftBehindCode, each with its expected code (a new sentence fails until it is mapped).
import AppKit

var failures = 0, passed = 0
func check(_ ok: Bool, _ what: String) {
    if ok { passed += 1 } else { failures += 1; print("FAIL: \(what)") }
}

/// Main-thread work from the driver thread.
func onMain<T>(_ f: () -> T) -> T { DispatchQueue.main.sync(execute: f) }
/// Waits (driver thread) until `cond` holds on main, at most `seconds`.
func wait(_ seconds: Double = 5, _ cond: @escaping () -> Bool) -> Bool {
    let end = Date().addingTimeInterval(seconds)
    while Date() < end { if onMain(cond) { return true }; usleep(2000) }
    return onMain(cond)
}

let app = NSApplication.shared
app.setActivationPolicy(.prohibited)      // no Dock icon, never activated

/// An invisible panel, off screen, for modal sessions nobody sees.
func hiddenPanel() -> NSPanel {
    let p = NSPanel(contentRect: NSRect(x: -10000, y: -10000, width: 10, height: 10), styleMask: [.titled], backing: .buffered, defer: true)
    p.alphaValue = 0
    return p
}

func presenterTests() {
    // the wake-up: main idle in its run loop, a dialog asked from another thread starts at once
    usleep(200_000)                               // let main go idle
    var delays: [Double] = []
    for _ in 0..<20 {
        let asked = DispatchTime.now().uptimeNanoseconds
        let started: UInt64? = DialogPresenter.shared.present { DispatchTime.now().uptimeNanoseconds }
        if let s = started { delays.append(Double(s - asked) / 1e6) }
        usleep(50_000)
    }
    let worst = delays.max() ?? 999
    check(delays.count == 20 && worst < 16.7, "a dialog asked while idle starts within one frame (worst \(String(format: "%.2f", worst)) ms)")

    // under the modal: app-modal (NSApp.modalWindow), the main queue drains, one presentation
    var drainedUnderModal = false, modalSeen = false, outsideDispatch = false
    let r: Int? = DialogPresenter.shared.present {
        outsideDispatch = !Thread.callStackSymbols.joined().contains("_dispatch_main_queue")
        let p = hiddenPanel()
        DispatchQueue.main.async {
            modalSeen = NSApp.modalWindow === p
            drainedUnderModal = true
            NSApp.stopModal(withCode: .OK)
        }
        let code = NSApp.runModal(for: p)
        p.orderOut(nil)
        return code == .OK ? 7 : 0
    }
    check(r == 7, "the presentation's result comes back to the asking thread")
    check(drainedUnderModal, "the main queue keeps draining under the dialog")
    check(modalSeen, "NSApp.modalWindow is the dialog while it is up (app-modal)")
    check(outsideDispatch, "the presentation runs outside any dispatch block")

    // one at a time, in call order
    var order: [Int] = [], overlap = false, inside = 0
    let g = DispatchGroup()
    for i in 0..<4 {
        g.enter()
        Thread.detachNewThread {
            _ = DialogPresenter.shared.present { () -> Int in
                inside += 1; if inside > 1 { overlap = true }
                order.append(i)
                let p = hiddenPanel()
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.02) { NSApp.stopModal() }
                _ = NSApp.runModal(for: p)
                p.orderOut(nil)
                inside -= 1
                return i
            }
            g.leave()
        }
        usleep(5000)                              // the calls arrive in this order
    }
    g.wait()
    check(order == [0, 1, 2, 3] && !overlap, "dialogs one at a time, in call order (\(order))")
    check(onMain { DialogPresenter.shared.present { 1 } } == nil, "refused on main")

    // shutdown releases a presentation main hasn't started, and its body never runs (review K4)
    let own = DialogPresenter()
    let hold = DispatchSemaphore(value: 0), mainIn = DispatchSemaphore(value: 0)
    DispatchQueue.main.async { mainIn.signal(); hold.wait() }   // main busy: the scheduled presentation can't start
    mainIn.wait()
    var ran = false, result: Int? = 7
    let asked = DispatchGroup()
    asked.enter()
    Thread.detachNewThread { result = own.present { () -> Int in ran = true; return 1 }; asked.leave() }
    usleep(100_000)                               // the request is scheduled, main hasn't run it
    own.shutDown()
    let released = asked.wait(timeout: .now() + 2) == .success
    hold.signal()
    onMain {}                                     // main runs the stale block now
    let bodyRan = onMain { ran }
    check(released && result == nil && !bodyRan, "shutdown answers a pending presentation nil, its body never runs (\(released) \(String(describing: result)) \(bodyRan))")
}

func sheetTests() {
    onMain {
        var f = DialogForm()
        f.title = "Settings"
        f.buttons = ["Save", "Clear Saved Stems", "Cancel"]
        f.enabled = [true, false, true]
        f.left = [false, false, false]
        f.fields = [("Size limit (GB)", "100"), ("Days", "30")]
        f.rows = [("Stems Cache location: This Mac", ["Change…", "Save"], [true, false])]
        f.defaultButton = 0
        f.cancelButton = 2
        let s = CoreSheet(f)
        let b = s.sheet.buttonViews
        check(b.map { $0.title } == f.buttons, "the buttons in the core's order")
        check(b[0].keyEquivalent == "\r" && b[2].keyEquivalent == "\u{1b}" && b[1].keyEquivalent == "", "Return and Esc as the core says")
        check(b[0].isEnabled && !b[1].isEnabled, "the core's disabled button")
        check(s.sheet.rowButtons.count == 2 && s.sheet.rowButtons[0].isEnabled && !s.sheet.rowButtons[1].isEnabled, "the row's buttons and their states")
        check(s.sheet.inputs.map { $0.stringValue } == ["100", "30"], "the fields' first texts")
        s.sheet.rowButtons[1].isEnabled = true
        s.sheet.rowButtons[1].performClick(nil)          // "Save" in the row, not the bottom "Save"
        check(s.answer == .row(0, 1), "a row's button answers by index, not by title (\(s.answer))")
        s.sheet.buttonViews[2].performClick(nil)
        check(s.answer == .button(2), "a bottom button answers its index")
        var inline = DialogForm()
        inline.title = "Stems Cache update"; inline.buttons = ["Update", "Not Now", "Don't Ask Again…"]
        inline.enabled = [true, true, true]; inline.left = [false, false, true]; inline.inline = true; inline.cancelButton = 1
        let t = CoreSheet(inline)
        check(t.sheet.buttonViews[1].keyEquivalent == "\u{1b}" && t.sheet.buttonViews[2].keyEquivalent == "", "inline: Esc on Not Now only")
        check(coreText(T_PW_TITLE) == "Password needed" && coreText(T_PW_WRONG_TITLE) == "Wrong password", "the password sheet's texts from the core")
    }
}

func adminTests() {
    let bridge = Data("bridge bytes".utf8), sha = sha256(data: bridge)
    var calls: [[String]] = [], logs: [String] = [], asked: [Bool] = [], accepted = 0, denied = 0, input: Data?
    func admin(sudoRemembers: Bool, passwords: [String?], script: (Int32, String)) -> ShellAdmin {
        var pw = passwords
        return ShellAdmin(tool: { exe, args, inp in
            calls.append([exe] + args)
            if args == ["-n", "/usr/bin/true"] { return (sudoRemembers ? 0 : 1, "") }
            if args.first == "-S" { return (inp == Data("right\n".utf8) ? 0 : 1, "") }
            if args.first == "-n" { input = inp; return script }
            return (0, "")
        }, ask: { again in asked.append(again); return pw.isEmpty ? nil : pw.removeFirst() },
           log: { logs.append($0) }, accepted: { accepted += 1 }, notPermitted: { denied += 1 })
    }
    let a = admin(sudoRemembers: false, passwords: ["wrong", "right"], script: (1, "touch: Operation not permitted"))
    let rc = a.run(kind: Int32(CORE_ADMIN_CACHE_INSTALL.rawValue), version: "7.2.3", bridgeSha: sha, ortSha: "abc", force: false) { $0 == sha ? bridge : nil }
    let expected = cacheInstallScript(bridge: bridge, bridgeSha: sha, ortSha: "abc", force: false, version: "7.2.3")
    check(rc == 1, "the root script's exit code comes back (\(rc))")
    check(asked == [false, true], "the password asked until right, told when the last was wrong")
    check(logs == ["  wrong password", "  FAILED: touch: Operation not permitted [exit 1]"], "today's log lines (\(logs))")
    check(accepted == 1 && denied == 1, "progress once the password is accepted; Operation not permitted noted")
    check(calls.last == ["/usr/bin/sudo", "-n", "/bin/bash", "-c", expected.body, "rbsp"] + expected.args && input == bridge, "the install script byte for byte, the checked bridge on its stdin")
    calls = []; logs = []; asked = []
    let b = admin(sudoRemembers: true, passwords: [], script: (0, ""))
    check(b.run(kind: Int32(CORE_ADMIN_CACHE_UNINSTALL.rawValue), version: "7.2.3", bridgeSha: "", ortSha: "", force: false) == 0
          && asked.isEmpty && logs == ["  (sudo remembers the password)", "  ok: "], "sudo remembers: no password, today's lines (\(logs))")
    check(calls.last?.suffix(3) == ["uninstall", "7.2.3", rbBundleID][...], "the uninstall script with rekordbox's version")
    logs = []
    let c = admin(sudoRemembers: false, passwords: [nil], script: (0, ""))
    check(c.run(kind: Int32(CORE_ADMIN_REMOVE_ROOT_FOLDER.rawValue), version: "", bridgeSha: "", ortSha: "", force: false) == ShellAdmin.cancelled
          && logs == ["  FAILED: cancelled"], "cancelled at the password")
    calls = []
    check(c.run(kind: Int32(CORE_ADMIN_FORGET.rawValue), version: "", bridgeSha: "", ortSha: "", force: false) == 0 && calls == [["/usr/bin/sudo", "-k"]], "forget: sudo -k only")
    calls = []
    logs = []
    check(c.run(kind: Int32(CORE_ADMIN_CACHE_REINSTALL.rawValue), version: "7.2.3", bridgeSha: "other", ortSha: "", force: true) { _ in nil } == 1
          && calls.isEmpty && logs == ["  FAILED: not run (the bridge checked for this action isn't there)"],
          "no install without the bridge CACHE_PREPARE checked: a failure (1), not a cancel (review K5)")
    // sudo that can't start (runTool's -1): a failure with its error in the log, not a cancel (K5)
    logs = []
    let d = admin(sudoRemembers: true, passwords: [], script: (-1, "launch failed"))
    check(d.run(kind: Int32(CORE_ADMIN_CACHE_UNINSTALL.rawValue), version: "7.2.3", bridgeSha: "", ortSha: "", force: false) == 1
          && logs == ["  (sudo remembers the password)", "  FAILED: launch failed [exit -1]"], "a sudo that can't start returns 1 and logs why (\(logs))")
}

/// One menu's items as comparable lines: depth, title, key, modifiers, separator.
func menuLines(_ m: NSMenu, _ depth: Int = 0) -> [String] {
    m.items.flatMap { i -> [String] in
        // a menu bar item made with NSMenuItem() has no title of its own ("NSMenuItem" on macOS 26): its menu's
        let title = i.submenu != nil && (i.title.isEmpty || i.title == "NSMenuItem") ? i.submenu!.title : i.title
        let me = i.isSeparatorItem ? "\(depth) ---" : "\(depth) \(title) [\(i.keyEquivalent)] \(i.keyEquivalentModifierMask.rawValue) \(i.isHidden)"
        return [me] + (i.submenu.map { menuLines($0, depth + 1) } ?? [])
    }
}

/// 1.3.0's menu bar as its makeMenu (main.swift, removed at M7's switch-over) drew it, in
/// menuLines' form: depth, title (a bar item's: its menu's), [key], modifier mask, hidden. The
/// shell's menus.def bar was compared with makeMenu itself until then (equal at 93fc46e).
let menus130: [String] = {
    let cmd = NSEvent.ModifierFlags.command.rawValue
    func i(_ d: Int, _ t: String, _ k: String = "", hidden: Bool = false) -> String { "\(d) \(t) [\(k)] \(cmd) \(hidden)" }
    return [i(0, ""), i(1, "About RB Stems Plus"), "1 ---", i(1, "Update RB Stems Plus…", hidden: true), i(1, "Settings…", ","),
            i(1, "Uninstall RB Stems Plus…"), "1 ---", i(1, "Hide RB Stems Plus", "h"), "1 ---", i(1, "Quit RB Stems Plus", "q"),
            i(0, "Edit"), i(1, "Cut", "x"), i(1, "Copy", "c"), i(1, "Paste", "v"), i(1, "Select All", "a"),
            i(0, "View"), i(1, "Status", "1"), i(1, "Prepare", "2"), i(1, "Manage", "3"),
            i(0, "Prepare"), i(1, "Prepare Playlist…"), i(1, "Check Again", "r"), "1 ---", i(1, "Pause"), i(1, "Stop", "."),
            i(0, "Manage"), i(1, "Demucs v4"), i(2, "Install"), i(2, "Reinstall"), i(2, "Uninstall…"),
            i(1, "Stems Cache"), i(2, "Install"), i(2, "Reinstall"), i(2, "Uninstall…"), "1 ---", i(1, "Show Log"),
            i(0, "Window"), i(1, "Close", "w"), i(1, "Minimize", "m"),
            i(0, "Help"), i(1, "RB Stems Plus Help", "?"), i(1, "RB Stems Plus Troubleshooting"), i(1, "Report an Issue"), i(1, "Create Report")]
}()

func shellTests() {
    var logs: [String] = [], sent: [String] = [], forms: [DialogForm] = [], steps: [core_step] = []
    let shell: CoreShell = onMain {
        let s = CoreShell.make()
        let controller = s.controller
        s.logged = { l in DispatchQueue.main.async { logs.append(l) } }
        s.onSend = { sent.append($0) }
        s.presentDialog = { f in DialogPresenter.shared.present { forms.append(f); return DialogAnswer(choice: .button(1), fields: []) } }
        s.runStep = { k, a, out in
            DispatchQueue.main.async { steps.append(k) }
            // the start-up checks: the reminders off, the welcome shown before (no dialog)
            if k == CORE_STEP_WATCHER_CHECK || (k == CORE_STEP_WELCOME && a.first == "check") { core_fact_flag(out, FACT_STEP_FLAG, 1) }
            return 0
        }
        s.runAdmin = { _, _, _, _, _ in ShellAdmin.cancelled }
        s.helperPath = { "/bin/echo" }
        check(controller.tabs.tabViewItems.count == 3 && controller.tabs.tabViewItems[0].viewController === s.statusTab
              && controller.tabs.tabViewItems[1].viewController === s.prepareTab && controller.tabs.tabViewItems.map { $0.label } == ["Status", "Prepare", "Manage"],
              "the window's tabs: the shell's Status and Prepare, then Manage")
        check(!controller.window.isVisible, "the window isn't shown before start()")
        check(controller.window.delegate === s && controller.installPlusBtn.target === s, "the window's delegate and the buttons' target are the shell")
        _ = s.prepareTab.view; _ = s.statusTab.view
        return s
    }
    let ok = onMain { shell.start(show: false, shown: false) }
    check(ok && onMain { shell.held != nil }, "start-up: core_new, start, the first render")
    let c = shell.core!

    // the location (review K1): measured from the start, as today's startupChecks starts it; its
    // changes reach the core; Settings' size stops being "measuring…" (read-only on this Mac)
    check(wait(30) { sent.contains(#"{"ev":"location"}"#) && LocationMonitor.shared.snapshot().status != nil },
          "the location is measured from the start and its change goes to the core (\(sent))")
    check(wait(120) { settingsSize(readConfigState(), LocationMonitor.shared.snapshot()) != .measuring },
          "Settings' stems size isn't left measuring (\(String(describing: settingsSize(readConfigState(), LocationMonitor.shared.snapshot()))))")

    // the view model on the shell's cheap facts: today's refresh on this Mac (once the model's
    // sha256, which both work out in the background, is known)
    let modelThere = (try? FileManager.default.attributesOfItem(atPath: modelDir + "/hdemucs.onnx")) != nil
    _ = wait(60) { !modelThere || !(stemsCachePresent() && !stemsPlusPresent()) || currentModelSHA(done: {}) != nil }
    var mismatch = ""
    check(wait(60) {
        guard let s = core_snap_current(c) else { return false }
        defer { core_snap_release(s) }
        let today = todayRefresh(), core = coreRefresh(s)
        mismatch = today == core ? "" : "today \(today)\n  core  \(core)"
        return mismatch.isEmpty
    }, "the core's lights, buttons and status line on the shell's facts are today's refresh on this Mac\(mismatch.isEmpty ? "" : ":\n  " + mismatch)")

    // menus: from menus.def, as 1.3.0's makeMenu drew them (menus130)
    onMain {
        let today = menus130, ours = menuLines(shell.makeMenu())
        check(today == ours, "the menus from menus.def are 1.3.0's: \(zip(today, ours).first { $0 != $1 }.map { "\($0) vs \($1)" } ?? "\(today.count) vs \(ours.count) items")")
    }

    // a field, rendered once per turn
    onMain { _ = core_test_set_str(c, F_STATUS_LINE, "Installing Stems Cache…", CORE_NTS) }
    check(wait { shell.controller.status.stringValue == "Installing Stems Cache…" }, "a field reaches its view by the next render")

    /// a view model field now (M4's refresh decides it from this Mac's facts)
    func field(_ f: core_field) -> Bool {
        guard let s = core_snap_current(c) else { return false }
        defer { core_snap_release(s) }
        return core_flag(s, f) != 0
    }
    // every button by its field (M4's fields, the switch-over's parity): the Manage tab's nine,
    // Prepare Again and Stop Keeping…, Check Again and Prepare; never AppKit's default
    func fieldsMatch() -> Bool {
        let k = shell.controller, st = shell.statusTab!, p = shell.prepareTab!
        let pairs: [(NSControl, core_field)] = [
            (k.installPlusBtn, F_INSTALL_PLUS_ENABLED), (k.installCacheBtn, F_INSTALL_CACHE_ENABLED), (k.reinstallBtn, F_REINSTALL_ENABLED),
            (k.reinstallPlusBtn, F_REINSTALL_PLUS_ENABLED), (k.reinstallCacheBtn, F_REINSTALL_CACHE_ENABLED), (k.settingsBtn, F_SETTINGS_ENABLED),
            (k.uninstallPlusBtn, F_UNINSTALL_PLUS_ENABLED), (k.uninstallCacheBtn, F_UNINSTALL_CACHE_ENABLED), (k.uninstallAllBtn, F_UNINSTALL_ALL_ENABLED),
            (st.againBtn, F_STATUS_AGAIN_ENABLED), (st.stopBtn, F_STATUS_STOP_ENABLED),
            (p.checkBtn, F_PREP_CHECK_ENABLED), (p.prepareBtn, F_PREP_PREPARE_ENABLED), (p.until, F_PREP_UNTIL_ENABLED), (p.outline, F_PREP_OUTLINE_ENABLED)]
        return pairs.allSatisfy { $0.0.isEnabled == field($0.1) }
    }
    check(wait { fieldsMatch() }, "every Manage, Status and Prepare control is its field's state")
    for f in [F_REINSTALL_PLUS_ENABLED, F_UNINSTALL_ALL_ENABLED, F_STATUS_AGAIN_ENABLED, F_STATUS_STOP_ENABLED, F_PREP_CHECK_ENABLED, F_PREP_OUTLINE_ENABLED] {
        let was = onMain { field(f) }
        onMain { _ = core_test_set_i64(c, f, was ? 0 : 1) }
        check(wait { fieldsMatch() && field(f) != was }, "field \(f.rawValue) turned \(was ? "off" : "on"): its control follows")
        onMain { _ = core_test_set_i64(c, f, was ? 1 : 0) }
        check(wait { fieldsMatch() && field(f) == was }, "... and back")
    }
    // the Prepare tab's texts (PrepareTab.update's): the name, the summary and its secondary note,
    // the button's and Pause's titles, the run line beside the Status buttons, the date
    onMain {
        _ = core_test_set_str(c, F_PREP_NAME, "Friday set", CORE_NTS)
        _ = core_test_set_str(c, F_PREP_INFO, "12 tracks to prepare.", CORE_NTS)
        _ = core_test_set_str(c, F_PREP_INFO_SUB, "Last time: 3 tracks done.", CORE_NTS)
        _ = core_test_set_str(c, F_PREP_PREPARE_TITLE, "Keep", CORE_NTS)
        _ = core_test_set_str(c, F_STATUS_RUN_LINE, "Preparing “Friday set”: 40%", CORE_NTS)
    }
    func infoParts() -> (String, NSColor?, NSColor?) {
        let a = shell.prepareTab.info.attributedStringValue
        let last = max(0, a.length - 1)
        return (a.string, a.length > 0 ? a.attribute(.foregroundColor, at: 0, effectiveRange: nil) as? NSColor : nil,
                a.length > 0 ? a.attribute(.foregroundColor, at: last, effectiveRange: nil) as? NSColor : nil)
    }
    check(wait { shell.prepareTab.nameLabel.stringValue == "Friday set" && shell.prepareTab.prepareBtn.title == "Keep"
                 && shell.statusTab.runLine.stringValue == "Preparing “Friday set”: 40%" },
          "the playlist's name, the Keep title and the Status tab's run line are the core's")
    check(onMain { let (t, first, last) = infoParts(); return t == "12 tracks to prepare.\nLast time: 3 tracks done." && first == .labelColor && last == .secondaryLabelColor },
          "the summary in the label colour, its note after a line break in the secondary colour (\(onMain { infoParts() }))")
    onMain { _ = core_test_set_str(c, F_PREP_INFO_SUB, "", CORE_NTS) }
    check(wait { infoParts().0 == "12 tracks to prepare." }, "no note: the summary alone")
    // the same text split differently: drawn again (the note's colour follows the split)
    onMain { _ = core_test_set_str(c, F_PREP_INFO, "12 tracks to prepare.\nLast time: 3 tracks done.", CORE_NTS) }
    check(wait { let (t, _, last) = infoParts(); return t == "12 tracks to prepare.\nLast time: 3 tracks done." && last == .labelColor },
          "the same words as summary alone: all in the label colour")
    onMain { _ = core_test_set_str(c, F_PREP_INFO, "12 tracks to prepare.", CORE_NTS); _ = core_test_set_str(c, F_PREP_INFO_SUB, "Last time: 3 tracks done.", CORE_NTS) }
    check(wait { infoParts().2 == .secondaryLabelColor }, "... split into summary and note: the note in the secondary colour again")
    let later = ShellPrepareTab.day.string(from: Date().addingTimeInterval(40 * 86400))
    onMain { _ = core_test_set_str(c, F_PREP_UNTIL, later, CORE_NTS) }
    check(wait { ShellPrepareTab.day.string(from: shell.prepareTab.until.dateValue) == later }, "the date picker shows the core's date (\(later))")
    // SO13: Prepare › Prepare Playlist… as the first visit to the Prepare tab: the tab the switch
    // loads renders before it is in a window, so the focus request waits for viewDidAppear
    let focusNow: Int64 = onMain {
        guard let s = core_snap_current(c) else { return 0 }
        defer { core_snap_release(s) }
        return core_i64(s, F_PREP_FOCUS_SEQ)
    }
    onMain { _ = core_test_set_i64(c, F_PREP_FOCUS_SEQ, focusNow + 1) }
    check(wait { shell.held.map { core_i64($0, F_PREP_FOCUS_SEQ) } == focusNow + 1 }, "a new focus request is rendered")
    let notLoaded: ShellPrepareTab = onMain { ShellPrepareTab(shell) }
    let tabWindow: NSWindow = onMain {
        _ = notLoaded.view   // loadView's render: no window yet
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 600), styleMask: [.titled], backing: .buffered, defer: true)
        w.isReleasedWhenClosed = false
        w.contentView = notLoaded.view
        return w
    }
    check(onMain { tabWindow.firstResponder !== notLoaded.outline }, "a Prepare tab loaded with no window: nothing focused yet")
    onMain { notLoaded.viewDidAppear() }
    check(onMain { tabWindow.firstResponder === notLoaded.outline }, "... and the outline focused once it appears in its window")
    onMain { tabWindow.contentView = nil }
    // a run: its bar, Pause and Stop in place of Check Again, Prepare and the gap; Pause's title
    func runRow(_ running: Bool) -> Bool {
        let p = shell.prepareTab!
        return [p.bar, p.pauseBtn, p.stopBtn].allSatisfy { $0.isHidden == !running } && [p.checkBtn, p.prepareBtn, p.runGap].allSatisfy { $0.isHidden == running }
    }
    check(onMain { runRow(field(F_PREP_RUNNING)) }, "the run's row as F_PREP_RUNNING says")
    onMain { _ = core_test_set_i64(c, F_PREP_RUNNING, 1); _ = core_test_set_str(c, F_PREP_PAUSE_TITLE, "Resume", CORE_NTS); _ = core_test_set_f64(c, F_PREP_PROGRESS, 0.4) }
    check(wait { runRow(true) && shell.prepareTab.pauseBtn.title == "Resume" && shell.prepareTab.bar.doubleValue == 0.4 }, "a run: the bar, Pause (titled Resume) and Stop shown")
    onMain { _ = core_test_set_i64(c, F_PREP_RUNNING, 0) }
    check(wait { runRow(false) }, "... and Check Again and Prepare again after it")

    // the tracks: a new list, changed rows, appended rows
    let tracks = { shell.prepareTab.tracksTable }
    func cell(_ row: Int, _ col: Int) -> String {
        let t = tracks()
        return (shell.prepareTab.tableView(t, viewFor: t.tableColumns[col], row: row) as? NSTableCellView)?.textField?.stringValue ?? "?"
    }
    onMain {
        _ = core_test_table_reset(c, T_PREP_TRACKS, 3)
        for i in 0..<3 { _ = core_test_cell_str(c, C_TRK_NAME, Int32(i), "Track \(i)", CORE_NTS); _ = core_test_cell_str(c, C_TRK_RESULT, Int32(i), "Checking…", CORE_NTS) }
    }
    check(wait { tracks().numberOfRows == 3 && cell(2, 0) == "Track 2" }, "a new list reloads the table")
    onMain { _ = core_test_cell_str(c, C_TRK_RESULT, 1, "Ready", CORE_NTS) }
    check(wait { cell(1, 1) == "Ready" && cell(0, 1) == "Checking…" }, "a changed row shows its new cell")
    onMain { _ = core_test_table_append(c, T_PREP_TRACKS, 2); _ = core_test_cell_str(c, C_TRK_NAME, 4, "Track 4", CORE_NTS) }
    check(wait { tracks().numberOfRows == 5 && cell(4, 0) == "Track 4" }, "appended rows are added")

    // the tab: the core's choice sends nothing back; the user's goes to the core
    sent = []
    onMain { _ = core_test_set_i64(c, F_TAB, 2) }
    check(wait { shell.controller.tabs.selectedTabViewItemIndex == 2 } && onMain { !sent.contains { $0.contains("\"tab\"") } }, "the core's tab is shown, no tab event back")
    onMain { shell.controller.tabs.selectedTabViewItemIndex = 1 }
    check(onMain { sent.contains(#"{"ev":"tab","tab":1}"#) }, "the user's tab goes to the core (\(sent))")
    check(wait { shell.held.map { core_i64($0, F_TAB) } == 1 }, "... and comes back as the core's")

    // the outline's selection the core made sends no select back
    sent = []
    onMain {
        _ = core_test_table_reset(c, T_PREP_OUTLINE, 3)
        _ = core_test_cell_i64(c, C_OUT_PARENT, 0, -1); _ = core_test_cell_i64(c, C_OUT_PARENT, 1, 0); _ = core_test_cell_i64(c, C_OUT_PARENT, 2, -1)
        _ = core_test_cell_i64(c, C_OUT_OPEN, 0, 1)            // the core's decision: the folder shown open
        _ = core_test_set_i64(c, F_PREP_SELECTED_ROW, 1)
    }
    check(wait { shell.prepareTab.outline.numberOfRows == 3 && shell.prepareTab.outline.selectedRow == 1 } && onMain { !sent.contains { $0.contains("\"select\"") || $0.contains("\"folder\"") } },
          "the outline from flattened rows: a folder open as the core says, its child selected, no select or folder event echoed (\(sent))")
    onMain { shell.prepareTab.outline.collapseItem(shell.prepareTab.outline.item(atRow: 0)) }
    check(onMain { sent.first?.hasPrefix(#"{"ev":"folder","row":0,"open":false,"gen":"#) == true && sent.filter { $0.contains("\"select\"") }.count == 1 },
          "a folder the user closes goes to the core, then one select for the hidden child (\(sent))")

    // host->dialog and host->step through the core's test flows
    onMain { _ = core_test_flow(c, Int32(CORE_TEST_FLOW_DIALOG)) }
    check(wait { logs.contains { $0.hasPrefix("test flow: button 1") } }, "the dialog's answer reaches the flow (\(logs.filter { $0.hasPrefix("test flow") }))")
    check(forms.first.map { $0.buttons == ["OK", "Cancel"] && $0.cancelButton == 1 && $0.title == "RB Stems Plus" } == true, "the form read from the core's dialog")
    onMain { _ = core_test_flow(c, Int32(CORE_TEST_FLOW_STEP)) }
    // (the Prepare tab's date style, CORE_STEP_DATE_TEXT, is asked on a worker as its date is set)
    check(wait { logs.contains("test flow: step 0") } && onMain { steps.filter { $0 != CORE_STEP_DATE_TEXT } == [CORE_STEP_OPEN_URL] },
          "a step reaches the host and its result the flow (\(steps))")

    // the helper: lines to the core as they come, then its end
    let run = onMain { core_test_helper_start(c, 0) }
    check(run != 0 && wait { core_test_helper_lines(c, 0) == 1 }, "the helper's line reaches the core")
    let noNewline = NSTemporaryDirectory() + "rbsp-shell-test-helper.sh"
    FileManager.default.createFile(atPath: noNewline, contents: Data("#!/bin/sh\nprintf 'line'\n".utf8), attributes: [.posixPermissions: 0o700])
    defer { try? FileManager.default.removeItem(atPath: noNewline) }
    shell.helperPath = { noNewline }              // a line with no newline at the end: still a line
    _ = onMain { core_test_helper_start(c, 1) }
    check(wait { core_test_helper_lines(c, 1) == 1 }, "a last line without its newline is a line")

    // the menus' live validation (review K6): the core's rule, its Pause/Resume title
    onMain {
        let bar = shell.makeMenu()
        func item(_ path: [String]) -> NSMenuItem? {
            var m: NSMenu? = bar, found: NSMenuItem?
            for t in path { found = m?.items.first { $0.title == t || $0.submenu?.title == t }; m = found?.submenu }
            return found
        }
        let status = item(["View", "Status"]), uninstall = item(["Manage", "Demucs v4", "Uninstall…"]), pause = item(["Prepare", "Pause"])
        check(status.map { shell.validateMenuItem($0) } == true, "View › Status is on")
        check(uninstall.map { shell.validateMenuItem($0) } == field(F_UNINSTALL_PLUS_ENABLED),
              "Manage › Demucs v4 › Uninstall… follows the view model's field (no action running)")
        _ = core_test_set_preparing(c, 1)
        check(pause.map { shell.validateMenuItem($0) && $0.title == "Pause" } == true, "Prepare › Pause on while preparing, titled by the core")
        _ = core_test_set_preparing(c, 0)
    }

    // close and quit (review K6): the core decides; a quit waits for a step changing rekordbox
    check(onMain { shell.windowShouldClose(shell.controller.window) }, "the window may close with nothing running")
    var replies: [Bool] = []
    shell.replyTerminate = { replies.append($0) }
    let stepDone = DispatchSemaphore(value: 0), stepIn = DispatchSemaphore(value: 0)
    Thread.detachNewThread { critical.run { stepIn.signal(); stepDone.wait() } }
    stepIn.wait()
    logs = []
    check(onMain { shell.shouldTerminate() } == .terminateLater, "quit during a critical step: terminateLater")
    check(wait { logs.contains("quit: waiting for the step that is changing rekordbox to finish") }, "... with today's log line")
    check(onMain { replies.isEmpty }, "... and no reply while the step runs")
    stepDone.signal()
    check(wait { replies == [true] }, "the step's end answers the quit (\(replies))")
    check(onMain { shell.shouldTerminate() } == .terminateNow, "quit with nothing running: now")

    // the start-up checks through the host table ({"ev":"shown"}): startupChecks' steps in its
    // order, the reminders off from WATCHER_CHECK's fact (the core's line once, the status line)
    onMain { steps = []; logs = [] }
    _ = onMain { shell.send(#"{"ev":"shown"}"#) }
    let offLine = "watcher: installed but not running (turned off in Login Items, or not loaded): reinstall reminders are off"
    check(wait(10) { steps.contains(CORE_STEP_PAYLOAD) && shell.controller.status.stringValue.contains(watcherOffLine) },
          "the start-up checks ran through the host table and the reminders line shows (\(onMain { steps.map { $0.rawValue } }))")
    check(onMain { Array(steps.prefix(3)) == [CORE_STEP_WATCHER_CHECK, CORE_STEP_MIGRATE_LEGACY, CORE_STEP_REMOVE_LEFTOVERS] },
          "startupChecks' order: the watcher, the legacy model, the leftovers (\(onMain { steps.map { $0.rawValue } }))")
    check(wait { logs.contains(offLine) } && onMain { logs.filter { $0 == offLine }.count == 1 }, "the core logs the reminders' line once")
    check(onMain { !steps.contains { !hostStepKinds.contains($0.rawValue) } }, "every step the start-up asked is one the host table handles")

    // the core's trigger.actionEnd (core_test_set_mark) reaches the shell's probe point once per
    // action end: Help › Create Report… (busy, REPORT_ZIP's stand-in, busy(nil), "Report saved")
    var marks: [String] = []
    onMain { shell.onMark = { marks.append($0) }; forms = [] }
    _ = onMain { shell.send(#"{"ev":"menu","id":"create_report"}"#) }
    check(wait(10) { forms.contains { $0.title == "Report saved" } }, "Create Report… ran through the host table (\(onMain { forms.map { $0.title } }))")
    check(wait { marks == ["trigger.actionEnd"] } && onMain { steps.contains(CORE_STEP_REPORT_ZIP) },
          "the action's end is one trigger.actionEnd from the core (\(onMain { marks }))")
    onMain { shell.onMark = nil }
}

// MARK: the cheap facts (ShellFacts.swift) against facts.def and today's reads

/// A fact sheet a test reads back.
final class FactRecorder: FactSink {
    var flags: [UInt32: Bool] = [:], ints: [UInt32: Int64] = [:], strs: [UInt32: String] = [:]
    func flag(_ f: core_fact, _ v: Bool) { flags[f.rawValue] = v }
    func i64(_ f: core_fact, _ v: Int64) { ints[f.rawValue] = v }
    func str(_ f: core_fact, _ s: String) { strs[f.rawValue] = s }
    func has(_ f: core_fact) -> Bool { flags[f.rawValue] != nil || ints[f.rawValue] != nil || strs[f.rawValue] != nil }
    func b(_ f: core_fact) -> Bool? { flags[f.rawValue] }
    func i(_ f: core_fact) -> Int64? { ints[f.rawValue] }
    func s(_ f: core_fact) -> String? { strs[f.rawValue] }
}

/// The step kinds (core.h's raw values) host->step handles: filled by coverageTests.
var hostStepKinds = Set<UInt32>()

/// `NAME = n` and plain `NAME` entries of one C enum body, numbered as C numbers them.
func enumValues(_ body: Substring) -> [String: Int] {
    var out: [String: Int] = [:], next = 0
    let noComments = body.replacingOccurrences(of: #"/\*[\s\S]*?\*/"#, with: "", options: .regularExpression)
    for part in noComments.split(separator: ",") {
        let t = part.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let r = t.range(of: #"^[A-Z][A-Z0-9_]*"#, options: .regularExpression) else { continue }
        let name = String(t[r])
        if let eq = t.firstIndex(of: "="), let v = Int(t[t.index(after: eq)...].trimmingCharacters(in: .whitespaces)) { next = v }
        out[name] = next
        next += 1
    }
    return out
}

/// Every `prefix` + [A-Z0-9_]+ token in `s`, in order (macOS 12: no Regex).
func tokens<S: StringProtocol>(_ s: S, _ prefix: String) -> [String] {
    var out: [String] = [], rest = Substring(s)
    while let r = rest.range(of: prefix) {
        var e = r.upperBound
        while e < rest.endIndex, rest[e].isASCII, rest[e].isUppercase || rest[e].isNumber || rest[e] == "_" { e = rest.index(after: e) }
        if e > r.upperBound { out.append(String(rest[r.lowerBound..<e])) }
        rest = rest[e...]
    }
    return out
}

/// The text between `start` and the next `end` in `src` ("" when not found).
func between(_ src: String, _ start: String, _ end: String) -> Substring {
    guard let a = src.range(of: start), let b = src.range(of: end, range: a.upperBound..<src.endIndex) else { return "" }
    return src[a.upperBound..<b.lowerBound]
}

/// Every step kind and Mac slow kind of core.h has its case in the host table (a kind added to
/// core.h fails here until the shell handles it); pure.h's location numbers are ShellFacts'.
func coverageTests(_ root: String) {
    let h = (try? String(contentsOfFile: root + "/core/core.h", encoding: .utf8)) ?? ""
    let hostSteps = (try? String(contentsOfFile: root + "/app/Shell/HostSteps.swift", encoding: .utf8)) ?? ""
    let watcherSteps = (try? String(contentsOfFile: root + "/app/Shell/WatcherShell.swift", encoding: .utf8)) ?? ""
    let shell = (try? String(contentsOfFile: root + "/app/Shell/CoreShell.swift", encoding: .utf8)) ?? ""
    let pure = (try? String(contentsOfFile: root + "/core/pure.h", encoding: .utf8)) ?? ""
    check(!h.isEmpty && !hostSteps.isEmpty && !shell.isEmpty && !pure.isEmpty, "core.h, pure.h, HostSteps.swift and CoreShell.swift read")
    /// the NAMEs with `prefix` on the source's `case` lines
    func cases(_ src: String, _ prefix: String) -> Set<String> {
        var out = Set<String>()
        for line in src.split(separator: "\n") where line.trimmingCharacters(in: .whitespaces).hasPrefix("case ") {
            out.formUnion(tokens(line, prefix))
        }
        return out
    }
    let steps = enumValues(between(h, "typedef enum core_step {", "} core_step;"))
    check(steps.count > 50 && steps["CORE_STEP_STOP_KEEPING"] == Int(CORE_STEP_STOP_KEEPING.rawValue), "core.h's step kinds read (\(steps.count))")
    let handled = cases(hostSteps, "CORE_STEP_").union(cases(watcherSteps, "CORE_STEP_"))
    let missing = Set(steps.keys).subtracting(handled)
    check(missing.isEmpty, "every step kind of core.h has its case in HostSteps.swift or, the watcher's, WatcherShell.swift (missing: \(missing.sorted()))")
    hostStepKinds = Set(handled.compactMap { steps[$0].map(UInt32.init) })
    var slowBody = between(h, "typedef enum core_slow {", "} core_slow;")
    // the Windows kinds (#ifdef CORE_WIN_POLICY ... #endif) are W-core's; kinds after the block are the Mac's too
    while let a = slowBody.range(of: "#ifdef CORE_WIN_POLICY"), let b = slowBody.range(of: "#endif", range: a.upperBound..<slowBody.endIndex) {
        slowBody = Substring(String(slowBody[..<a.lowerBound]) + String(slowBody[b.upperBound...]))
    }
    let slow = enumValues(slowBody)
    let slowMissing = Set(slow.keys).subtracting(cases(shell, "CORE_SLOW_"))
    check(slow.count >= 5 && slowMissing.isEmpty, "every Mac slow kind of core.h has its case in CoreShell.slowFact (missing: \(slowMissing.sorted()))")

    // pure.h's LOC_* and PROBE_* are ShellFacts' numbers
    let locs = enumValues(Substring("LOC_THIS_MAC" + between(pure, "enum { LOC_THIS_MAC", "};")))
    let probes = enumValues(Substring("PROBE_AVAILABLE" + between(pure, "enum { PROBE_AVAILABLE", "};")))
    let statuses: [(LocationStatus, String)] = [(.thisMac, "LOC_THIS_MAC"), (.chosen(dir: "/d", id: "i", probe: .available), "LOC_CHOSEN"),
                                                (.bridgeTooOld(dir: "/d", id: "i"), "LOC_BRIDGE_TOO_OLD"), (.configUnreadable, "LOC_CONFIG_UNREADABLE"),
                                                (.configMalformedLocation, "LOC_CONFIG_MALFORMED")]
    check(locs.count == statuses.count && statuses.allSatisfy { locs[$0.1].map(Int64.init) == ShellFacts.locKind($0.0) }, "LocationStatus as pure.h's LOC_* (\(locs))")
    let allProbes: [LocationProbe] = [.available, .notConnected, .folderMissing, .markerMissingOrWrong, .link, .network, .readOnly, .noPermission]
    func snake(_ s: String) -> String { "PROBE_" + s.reduce("") { $0 + ($1.isUppercase ? "_" + String($1) : String($1).uppercased()) } }
    check(probes.count == allProbes.count && allProbes.allSatisfy { probes[snake($0.rawValue)].map(Int64.init) == ShellFacts.probeCode($0) },
          "LocationProbe as pure.h's PROBE_* (\(probes))")
}

/// The location snapshot's facts for each status (no drive touched: no mounts).
func locationFactTests() {
    func sheet(_ s: LocationSnapshot) -> FactRecorder { let r = FactRecorder(); ShellFacts.location(r, s, mounts: []); return r }
    let none = sheet(LocationSnapshot())
    check(none.b(FACT_LOC_MEASURED) == false && !none.has(FACT_LOC_KIND) && none.i(FACT_LOC_PAUSED_FLOOR) == -1 && none.b(FACT_REKORDBOX_RUNNING) == false
          && !none.has(FACT_LOC_IN_USE) && !none.has(FACT_LOC_DRIVE) && !none.has(FACT_LOC_BRIDGE_STATE), "not measured yet: no status, no floor")
    let mac = sheet(LocationSnapshot(status: .thisMac))
    check(mac.b(FACT_LOC_MEASURED) == true && mac.i(FACT_LOC_KIND) == 0 && !mac.has(FACT_LOC_PROBE) && !mac.has(FACT_LOC_STATUS_DRIVE)
          && mac.s(FACT_LOC_IN_USE) == defaultCacheDir, "this Mac: its folder in use")
    let dir = "/Volumes/Stick/" + locationFolderName
    var s = LocationSnapshot(status: .chosen(dir: dir, id: "id", probe: .notConnected), rekordboxRunning: true, pausedFloor: 5 << 30)
    s.drive = "Stick"
    s.bridge = BridgeState(pid: 1, start: 2, location: dir, state: "unavailable", reason: "not-connected")
    let away = sheet(s)
    check(away.i(FACT_LOC_KIND) == 1 && away.i(FACT_LOC_PROBE) == 1 && away.s(FACT_LOC_STATUS_DRIVE) == driveName(dir, mounts: [])
          && !away.has(FACT_LOC_IN_USE) && away.s(FACT_LOC_DRIVE) == "Stick" && away.i(FACT_LOC_PAUSED_FLOOR) == 5 << 30 && away.b(FACT_REKORDBOX_RUNNING) == true,
          "a chosen drive not connected: its probe and drive, nothing in use")
    check(away.s(FACT_LOC_BRIDGE_STATE) == "unavailable" && away.s(FACT_LOC_BRIDGE_REASON) == "not-connected" && away.s(FACT_LOC_BRIDGE_DRIVE) == driveName(dir, mounts: []),
          "the bridge's live state, reason and drive")
    s.status = .chosen(dir: dir, id: "id", probe: .available)
    s.bridge = BridgeState(pid: 1, start: 2, location: "", state: "pending", reason: "")
    let there = sheet(s)
    check(there.i(FACT_LOC_PROBE) == 0 && there.s(FACT_LOC_IN_USE) == dir && there.s(FACT_LOC_BRIDGE_STATE) == "pending" && !there.has(FACT_LOC_BRIDGE_DRIVE),
          "a chosen drive available: in use; a bridge saving on this Mac has no drive")
    let old = sheet(LocationSnapshot(status: .bridgeTooOld(dir: dir, id: "id")))
    check(old.i(FACT_LOC_KIND) == 2 && !old.has(FACT_LOC_PROBE) && old.s(FACT_LOC_STATUS_DRIVE) == driveName(dir, mounts: []) && old.s(FACT_LOC_IN_USE) == defaultCacheDir,
          "an older bridge: its drive named, this Mac in use")
    for (st, k) in [(LocationStatus.configUnreadable, Int64(3)), (.configMalformedLocation, 4)] {
        let r = sheet(LocationSnapshot(status: st))
        check(r.i(FACT_LOC_KIND) == k && !r.has(FACT_LOC_IN_USE) && !r.has(FACT_LOC_STATUS_DRIVE), "config.ini wrong (\(k)): nothing in use")
    }
    let sets = [KeptSet(name: "a1", until: Date(timeIntervalSince1970: 2_000_000_000), entries: 3, files: 7), KeptSet(name: "b2", until: nil, entries: 0, files: 0)]
    check(ShellFacts.keptSetsText(sets) == "a1\t\(ShellFacts.dateOnly.string(from: Date(timeIntervalSince1970: 2_000_000_000)))\t7\nb2\t\t0",
          "the kept sets: name, until as the Status tab shows it (\"\" unknown), files")
}

/// M73: stemsCacheBlocked's order in ShellFacts.cacheBlocked, branch by branch, with stand-in reads:
/// the facts written and the library's signature and version read only where Swift's reaches them;
/// config.ini's rows from fixtures (unreadable, a chosen folder); FACT_BRIDGE_MODELS from a bridge's
/// line, only with rekordbox's own model.
func sheetFixtureTests() {
    let list = Manifest.Compatible(rekordbox: ["7.2.19"], ortVersionPrefix: "1.18.", stemsEngine: ["0002"])
    struct Case { var name: String; var admin = true, installed = true, version = "7.2.19.0342", present = false
                  var root = true, prototype = false, lib = true, signed = true, ort: String? = "1.18.0"
                  var step: Set<String>; var reads: [String] }
    let files: Set<String> = ["FACT_REKORDBOX_SUPPORTED", "FACT_ROOT_ORT_EXISTS", "FACT_PROTOTYPE_EXISTS", "FACT_LIB_EXISTS"]
    let er = "exists " + rootOrt, ep = "exists " + prototypeRootDir, el = "exists " + rbLib
    let lib3: Set<String> = ["FACT_REKORDBOX_SUPPORTED", "FACT_LIB_EXISTS", "FACT_LIB_SIGNED"]
    // the BLOCKED step's facts and reads, in stemsCacheBlocked's order (MP2)
    let cases: [Case] = [
        Case(name: "standard account", admin: false, step: [], reads: []),
        Case(name: "no rekordbox", installed: false, step: [], reads: []),
        Case(name: "rekordbox not listed", version: "7.1.0", step: ["FACT_REKORDBOX_SUPPORTED"], reads: []),
        Case(name: "the prototype's bridge", present: true, root: false, prototype: true, step: ["FACT_REKORDBOX_SUPPORTED", "FACT_ROOT_ORT_EXISTS", "FACT_PROTOTYPE_EXISTS"], reads: [er, ep]),
        Case(name: "no library", lib: false, step: ["FACT_REKORDBOX_SUPPORTED", "FACT_LIB_EXISTS"], reads: [el]),
        Case(name: "not Pioneer's", signed: false, step: lib3, reads: [el, "signed " + rbLib]),
        Case(name: "Pioneer's, 1.18", step: lib3.union(["FACT_ORT_VERSION", "FACT_ORT_PREFIX"]), reads: [el, "signed " + rbLib, "version " + rbLib]),
        Case(name: "Pioneer's, version unreadable", ort: nil, step: lib3.union(["FACT_ORT_PREFIX"]), reads: [el, "signed " + rbLib, "version " + rbLib]),
        Case(name: "Stems Cache in: the root copy", present: true,
             step: lib3.union(["FACT_ROOT_ORT_EXISTS", "FACT_ORT_VERSION", "FACT_ORT_PREFIX"]), reads: [er, er, "signed " + rootOrt, "version " + rootOrt]),
        Case(name: "Stems Cache in, its root copy missing", present: true, root: false,
             step: ["FACT_REKORDBOX_SUPPORTED", "FACT_ROOT_ORT_EXISTS", "FACT_PROTOTYPE_EXISTS", "FACT_LIB_EXISTS"], reads: [er, ep, er]),
    ]
    let names: [String: core_fact] = ["FACT_REKORDBOX_SUPPORTED": FACT_REKORDBOX_SUPPORTED, "FACT_ROOT_ORT_EXISTS": FACT_ROOT_ORT_EXISTS,
                                      "FACT_PROTOTYPE_EXISTS": FACT_PROTOTYPE_EXISTS, "FACT_LIB_EXISTS": FACT_LIB_EXISTS, "FACT_LIB_SIGNED": FACT_LIB_SIGNED,
                                      "FACT_ORT_VERSION": FACT_ORT_VERSION, "FACT_ORT_PREFIX": FACT_ORT_PREFIX]
    for k in cases {
        for always in [false, true] {
            var reads: [String] = []
            var lr = ShellFacts.LibReads()
            lr.exists = { p in reads.append("exists " + p); return p == rootOrt ? k.root : p == prototypeRootDir ? k.prototype : p == rbLib ? k.lib : false }
            lr.signed = { p in reads.append("signed " + p); return k.signed }
            lr.version = { p in reads.append("version " + p); return k.ort }
            let r = FactRecorder()
            ShellFacts.cacheBlocked(r, admin: k.admin, installed: k.installed, version: k.version, present: k.present, list: list, always: always, reads: lr)
            let got = Set(names.filter { r.has($0.value) }.map { $0.key })
            // the cheap sheet: the file checks always (prepareFacts' too), the library's signature
            // and version only where the step reads them
            let want = always ? k.step.union(files) : k.step
            let wantReads = always ? [er, ep, "exists " + (k.present ? rootOrt : rbLib)] + k.reads.filter { !$0.hasPrefix("exists ") } : k.reads
            check(got == want && reads == wantReads,
                  "cacheBlocked, \(k.name)\(always ? " (cheap sheet)" : " (BLOCKED step)"): facts \(got.sorted()) reads \(reads)")
        }
    }
    let ok = FactRecorder()
    var lr = ShellFacts.LibReads(); lr.exists = { _ in true }; lr.signed = { _ in true }; lr.version = { _ in "1.17.3" }
    ShellFacts.cacheBlocked(ok, admin: true, installed: true, version: "7.2.19", present: false, list: list, always: false, reads: lr)
    check(ok.b(FACT_LIB_SIGNED) == true && ok.s(FACT_ORT_VERSION) == "1.17.3" && ok.s(FACT_ORT_PREFIX) == "1.18." && ok.b(FACT_REKORDBOX_SUPPORTED) == true,
          "the values: signed, the library's version, the list's prefix (the core decides the mismatch)")

    // config.ini fixtures
    let dir = NSTemporaryDirectory() + "rbsp-shell-test-config-\(getpid())"
    try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(atPath: dir) }
    func sheet(_ text: [UInt8]) -> (FactRecorder, ConfigState) {
        let file = dir + "/config.ini"
        FileManager.default.createFile(atPath: file, contents: Data(text))
        let st = readConfigState(file: file), r = FactRecorder()
        ShellFacts.config(r, st, bytes: FileManager.default.contents(atPath: file))
        return (r, st)
    }
    let chosenDir = "/Volumes/Stick/" + locationFolderName, id = "0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0"
    let (ch, chSt) = sheet(Array("[cache]\ncache_dir=\"\(chosenDir)\"\ncache_id=\(id)\nmax_gb=5.5\nmax_days=7\nenabled=0\n".utf8))
    check(chSt.location.chosenDir == chosenDir && ch.i(FACT_CONFIG_LOCATION) == 1 && ch.s(FACT_CONFIG_DIR) == chosenDir && ch.s(FACT_CHOSEN_LOCATION) == chosenDir
          && !ch.has(FACT_CONFIG_UNREADABLE) && ch.s(FACT_CONFIG_MAX_GB) == "5.5" && ch.s(FACT_CONFIG_MAX_DAYS) == "7" && ch.b(FACT_CONFIG_ENABLED) == false,
          "config.ini with a chosen folder: its location, folder and numbers")
    let (bad, badSt) = sheet([0x6d, 0x61, 0x78, 0x00, 0x0a])   // a NUL byte: config.ini can't be read
    check(badSt.unreadable != nil && bad.s(FACT_CONFIG_UNREADABLE) == badSt.unreadable && bad.i(FACT_CONFIG_LOCATION) == 2 && !bad.has(FACT_CONFIG_DIR)
          && bad.s(FACT_CHOSEN_LOCATION) == "" && bad.has(FACT_CONFIG_TEXT),
          "config.ini unreadable: why, set wrongly, no folder (\(badSt.unreadable ?? "-"))")
    let none = FactRecorder()
    ShellFacts.config(none, readConfigState(file: dir + "/missing.ini"), bytes: nil)
    check(none.i(FACT_CONFIG_LOCATION) == 0 && !none.has(FACT_CONFIG_TEXT) && !none.has(FACT_CONFIG_UNREADABLE) && none.s(FACT_CONFIG_MAX_GB) == "20",
          "no config.ini: this Mac, the defaults, no text")

    // FACT_BRIDGE_MODELS: a bridge's line, read only with rekordbox's own model
    let a = String(repeating: "a", count: 64), b = String(repeating: "b", count: 64)
    let bridge = Data("xx rbstems bridge: 1.4 rbstems-cache: rebuild=1 kept=1 pioneer=\(a),\(b); yy".utf8)
    var asked = 0
    let own = FactRecorder()
    ShellFacts.bridgeModels(own, plus: false, cache: true) { asked += 1; return bridgeCacheModels(data: bridge) }
    check(own.s(FACT_BRIDGE_MODELS) == a + "\n" + b && asked == 1, "the bridge's models with rekordbox's own model, one a line")
    for (plus, cache) in [(true, true), (false, false), (true, false)] {
        let r = FactRecorder()
        ShellFacts.bridgeModels(r, plus: plus, cache: cache) { asked += 1; return [a] }
        check(!r.has(FACT_BRIDGE_MODELS), "no bridge models read with Demucs v4 \(plus ? "on" : "off"), Stems Cache \(cache ? "on" : "off")")
    }
    check(asked == 1, "the bridge read only with rekordbox's own model")
    let old = FactRecorder()
    ShellFacts.bridgeModels(old, plus: false, cache: true) { bridgeCacheModels(data: Data("rbstems bridge: 1.0".utf8)) }
    check(!old.has(FACT_BRIDGE_MODELS), "1.0's bridge, without the line: not set")
}

/// The whole cheap sheet on this Mac (read-only): every cheap row of facts.def mapped here, each
/// written or left unset as its doc says, its value today's read.
func cheapSheetTests(_ root: String) {
    let def = (try? String(contentsOfFile: root + "/core/facts.def", encoding: .utf8)) ?? ""
    check(!def.isEmpty, "facts.def read")
    // the cheap rows: the first section, M4's section and the M6 rows its comment says the sheet carries
    func names(_ s: Substring) -> Set<String> { Set(tokens(s, "FACT_")) }
    let m4 = between(def, "/* the view model's cheap facts (M4)", "#undef FACT")
    // the Windows-only rows (os win) are the Windows host's
    let winRows = Set(def.split(separator: "\n").filter { $0.hasPrefix("FACT(") && $0.split(separator: ",").dropFirst(2).first?.trimmingCharacters(in: .whitespaces) == "win" }.compactMap { tokens($0, "FACT_").first })
    let cheapRows = names(between(def, "/* cheap (host->facts) */", "/* slow (host->slow_fact) */")).union(names(m4)).subtracting(winRows)

    let (r, loc, ok) = onMain { () -> (FactRecorder, LocationSnapshot, Bool) in
        let before = LocationMonitor.shared.snapshot(), r = FactRecorder()
        ShellFacts.cheap(r, helper: prepareHelper(), parts: ShellFacts.allParts)
        return (r, before, LocationMonitor.shared.snapshot() == before)
    }
    check(ok, "the location snapshot held still while the sheet was read")
    let pf = onMain { prepareFacts(app: Bundle.main.bundlePath) }
    let st = readConfigState(), plus = stemsPlusPresent(), cache = stemsCachePresent(), c = chosen()
    let why = stemsCacheBlocked(), unsupportedOrt = "Stems Cache doesn't support ONNX Runtime "
    let reachesSign = why == nil || why == "rekordbox's ONNX Runtime isn't the original: reinstall rekordbox." || why!.hasPrefix(unsupportedOrt)
    let signed = why == nil || why!.hasPrefix(unsupportedOrt)
    let lib = cache ? rootOrt : rbLib, fm = FileManager.default
    let modelThere = (try? fm.attributesOfItem(atPath: modelDir + "/hdemucs.onnx")) != nil
    let chosenStatus: Bool = { if case .chosen? = loc.status { return true }; return false }()
    let tooOld: Bool = { if case .bridgeTooOld? = loc.status { return true }; return false }()
    enum Want { case set, unset }
    func want(_ b: Bool) -> Want { b ? .set : .unset }
    // every cheap row: written always, or exactly when its doc says
    let rows: [String: (core_fact, Want)] = [
        "FACT_STEMS_PLUS_PRESENT": (FACT_STEMS_PLUS_PRESENT, .set), "FACT_STEMS_CACHE_PRESENT": (FACT_STEMS_CACHE_PRESENT, .set),
        "FACT_CHOSEN_LOCATION": (FACT_CHOSEN_LOCATION, .set), "FACT_CONFIG_TEXT": (FACT_CONFIG_TEXT, want(fm.contents(atPath: configFile) != nil)),
        "FACT_CACHE_TRACES": (FACT_CACHE_TRACES, .set), "FACT_REKORDBOX_INSTALLED": (FACT_REKORDBOX_INSTALLED, .set),
        "FACT_OWN_BUNDLE": (FACT_OWN_BUNDLE, .set), "FACT_CACHE_IS_LINK": (FACT_CACHE_IS_LINK, .set), "FACT_USER_IS_ADMIN": (FACT_USER_IS_ADMIN, .set),
        "FACT_CHOSEN_MODEL": (FACT_CHOSEN_MODEL, .set), "FACT_CHOSEN_CACHE": (FACT_CHOSEN_CACHE, .set), "FACT_REKORDBOX_VERSION": (FACT_REKORDBOX_VERSION, .set),
        "FACT_ENGINE": (FACT_ENGINE, .set), "FACT_ENGINE_SUPPORTED": (FACT_ENGINE_SUPPORTED, .set),
        "FACT_REKORDBOX_SUPPORTED": (FACT_REKORDBOX_SUPPORTED, .set), "FACT_ROOT_ORT_EXISTS": (FACT_ROOT_ORT_EXISTS, .set),
        "FACT_PROTOTYPE_EXISTS": (FACT_PROTOTYPE_EXISTS, .set), "FACT_LIB_EXISTS": (FACT_LIB_EXISTS, .set),
        "FACT_LIB_SIGNED": (FACT_LIB_SIGNED, want(reachesSign)), "FACT_ORT_VERSION": (FACT_ORT_VERSION, want(signed && dylibVersion(lib) != nil)),
        "FACT_ORT_PREFIX": (FACT_ORT_PREFIX, want(signed)),
        "FACT_CONFIG_ENABLED": (FACT_CONFIG_ENABLED, .set), "FACT_CONFIG_RB_MODEL": (FACT_CONFIG_RB_MODEL, .set),
        "FACT_CONFIG_MAX_GB": (FACT_CONFIG_MAX_GB, .set), "FACT_CONFIG_MAX_DAYS": (FACT_CONFIG_MAX_DAYS, .set),
        "FACT_CONFIG_UNREADABLE": (FACT_CONFIG_UNREADABLE, want(st.unreadable != nil)), "FACT_CONFIG_LOCATION": (FACT_CONFIG_LOCATION, .set),
        "FACT_CONFIG_DIR": (FACT_CONFIG_DIR, want(st.location.chosenDir != nil)),
        "FACT_LOC_MEASURED": (FACT_LOC_MEASURED, .set), "FACT_LOC_KIND": (FACT_LOC_KIND, want(loc.status != nil)),
        "FACT_LOC_PROBE": (FACT_LOC_PROBE, want(chosenStatus)), "FACT_LOC_STATUS_DRIVE": (FACT_LOC_STATUS_DRIVE, want(chosenStatus || tooOld)),
        "FACT_LOC_BRIDGE_STATE": (FACT_LOC_BRIDGE_STATE, want(loc.bridge != nil)), "FACT_LOC_BRIDGE_REASON": (FACT_LOC_BRIDGE_REASON, want(loc.bridge != nil)),
        "FACT_LOC_BRIDGE_DRIVE": (FACT_LOC_BRIDGE_DRIVE, want(loc.bridge.map { !$0.location.isEmpty } ?? false)),
        "FACT_REKORDBOX_RUNNING": (FACT_REKORDBOX_RUNNING, .set), "FACT_LOC_PAUSED_FLOOR": (FACT_LOC_PAUSED_FLOOR, .set),
        "FACT_LOC_IN_USE": (FACT_LOC_IN_USE, want(loc.status.flatMap(folderInUse) != nil)), "FACT_LOC_DRIVE": (FACT_LOC_DRIVE, want(loc.drive != nil)),
        "FACT_BRIDGE_MODELS": (FACT_BRIDGE_MODELS, want(cache && !plus && bridgeCacheModels() != nil)), "FACT_BRIDGE_KEEPS": (FACT_BRIDGE_KEEPS, .set),
        "FACT_MODEL_SIZE": (FACT_MODEL_SIZE, want(modelThere)), "FACT_MODEL_MTIME": (FACT_MODEL_MTIME, want(modelThere)),
        "FACT_APPLE_SILICON": (FACT_APPLE_SILICON, .set), "FACT_HELPER_THERE": (FACT_HELPER_THERE, .set),
        "FACT_STEMS_QUALITY": (FACT_STEMS_QUALITY, .set), "FACT_OUR_MODELS": (FACT_OUR_MODELS, .set), "FACT_KEPT_SETS": (FACT_KEPT_SETS, .set),
    ]
    let unmapped = cheapRows.subtracting(rows.keys), extra = Set(rows.keys).subtracting(cheapRows)
    check(cheapRows.count >= 45 && unmapped.isEmpty && extra.isEmpty,
          "every cheap row of facts.def is mapped here (\(cheapRows.count) rows; unmapped \(unmapped.sorted()), not in facts.def \(extra.sorted()))")
    for (name, (f, w)) in rows.sorted(by: { $0.key < $1.key }) {
        check(r.has(f) == (w == .set), "\(name) \(w == .set ? "written" : "left unset") on this Mac")
    }
    // the parts (M71): each part's rows are facts.def's, and a read writes only the parts asked for
    func partRows(_ start: String, _ end: String) -> Set<String> {
        Set(between(def, start, end).split(separator: "\n").filter { $0.hasPrefix("FACT(") }.compactMap { tokens($0, "FACT_").first })
    }
    // a Windows-only row appended after them is the Windows host's (written with REFRESH there)
    let prepareRows = partRows("/* CORE_FACTS_PREPARE", "/* CORE_FACTS_KEPT").subtracting(winRows)
    let keptRows = partRows("/* CORE_FACTS_KEPT", "#undef FACT").subtracting(winRows)
    check(prepareRows == ["FACT_BRIDGE_KEEPS", "FACT_STEMS_QUALITY", "FACT_OUR_MODELS"] && keptRows == ["FACT_KEPT_SETS"],
          "facts.def's PREPARE and KEPT parts (\(prepareRows.sorted()), \(keptRows.sorted()))")
    func written(_ parts: UInt32) -> Set<String> {
        let p = onMain { () -> FactRecorder in let p = FactRecorder(); ShellFacts.cheap(p, helper: prepareHelper(), parts: parts); return p }
        return Set(rows.filter { p.has($0.value.0) }.map { $0.key })
    }
    let refreshOnly = written(ShellFacts.refreshPart), prepareOnly = written(ShellFacts.preparePart), keptOnly = written(ShellFacts.keptPart)
    check(refreshOnly.isDisjoint(with: prepareRows.union(keptRows)) && prepareOnly == prepareRows && keptOnly == keptRows,
          "a read writes only the parts it asks for (REFRESH \(refreshOnly.count) rows, PREPARE \(prepareOnly.sorted()), KEPT \(keptOnly.sorted()))")
    check(refreshOnly.union(prepareOnly).union(keptOnly) == Set(rows.filter { r.has($0.value.0) }.map { $0.key }),
          "the three parts together are the whole sheet")
    // the values: today's reads (prepareFacts' for what preparing checks)
    check(r.b(FACT_STEMS_PLUS_PRESENT) == plus && r.b(FACT_STEMS_CACHE_PRESENT) == cache && r.b(FACT_CHOSEN_MODEL) == c.model && r.b(FACT_CHOSEN_CACHE) == c.cache
          && r.b(FACT_CACHE_TRACES) == cacheTraces() && r.b(FACT_REKORDBOX_INSTALLED) == rekordboxInstalled() && r.b(FACT_OWN_BUNDLE) == (ownBundle() != nil)
          && r.b(FACT_CACHE_IS_LINK) == cacheIsLink() && r.b(FACT_USER_IS_ADMIN) == userIsAdmin && r.s(FACT_REKORDBOX_VERSION) == rekordboxVersion(),
          "the installed features, chosen(), rekordbox, the account: today's reads")
    let engine = engineVersion()
    check(r.s(FACT_ENGINE) == engine && r.b(FACT_ENGINE_SUPPORTED) == engineSupported(engine) && r.b(FACT_REKORDBOX_SUPPORTED) == pf.rekordboxOK
          && r.b(FACT_ROOT_ORT_EXISTS) == pf.ortThere && r.b(FACT_PROTOTYPE_EXISTS) == fm.fileExists(atPath: prototypeRootDir) && r.b(FACT_LIB_EXISTS) == fm.fileExists(atPath: lib)
          && (r.b(FACT_LIB_SIGNED).map { $0 == signed } ?? true) && (r.s(FACT_ORT_PREFIX).map { $0 == compatibleList().ortVersionPrefix } ?? true)
          && (r.s(FACT_ORT_VERSION).map { $0 == dylibVersion(lib) } ?? true),
          "stemsPlusBlocked's and stemsCacheBlocked's facts: today's reads (blocked: \(why ?? "no"))")
    let loc012: Int64 = { switch st.location { case .thisMac: return 0; case .chosen: return 1; case .malformed: return 2 } }()
    check(r.b(FACT_CONFIG_ENABLED) == pf.enabled && r.b(FACT_CONFIG_RB_MODEL) == st.config.rekordboxModel && r.s(FACT_CONFIG_MAX_GB) == configNumber(pf.maxGB)
          && r.s(FACT_CONFIG_MAX_DAYS) == configNumber(readConfig().maxDays) && r.s(FACT_CONFIG_UNREADABLE) == pf.configUnreadable
          && r.i(FACT_CONFIG_LOCATION) == loc012 && r.s(FACT_CONFIG_DIR) == st.location.chosenDir && r.s(FACT_CHOSEN_LOCATION) == (st.location.chosenDir ?? ""),
          "config.ini's values: readConfigState's (FACT_CONFIG_MAX_DAYS included)")
    check(r.b(FACT_LOC_MEASURED) == (loc.status != nil) && r.b(FACT_REKORDBOX_RUNNING) == loc.rekordboxRunning && r.i(FACT_LOC_PAUSED_FLOOR) == (loc.pausedFloor ?? -1)
          && r.s(FACT_LOC_IN_USE) == loc.status.flatMap(folderInUse) && r.s(FACT_LOC_DRIVE) == loc.drive
          && r.i(FACT_LOC_KIND) == loc.status.map(ShellFacts.locKind),
          "the location: LocationMonitor's snapshot now")
    check(r.b(FACT_APPLE_SILICON) == pf.appleSilicon && r.b(FACT_HELPER_THERE) == pf.helperThere && r.b(FACT_BRIDGE_KEEPS) == pf.bridgeKeeps
          && r.i(FACT_STEMS_QUALITY) == Int64(pf.quality ?? -1) && r.s(FACT_OUR_MODELS) == pf.ourModels.joined(separator: "\n"),
          "prepareFacts' inputs: today's")
    check(r.s(FACT_KEPT_SETS) == ShellFacts.keptSetsText(keptSets()), "the kept sets: keptSets()")
    if let size = r.i(FACT_MODEL_SIZE) {
        check(size == Int64((try? fm.attributesOfItem(atPath: modelDir + "/hdemucs.onnx")[.size] as? Int) ?? -1), "the model file's size")
    }

    // the start config: every path the core reads, Paths.swift's
    let config = ShellConfig.json()
    let obj = (try? JSONSerialization.jsonObject(with: Data(config.utf8))) as? [String: Any]
    let paths = obj?["paths"] as? [String: String] ?? [:]
    check(obj?["os"] as? String == "mac" && paths["model"] == modelDir + "/hdemucs.onnx" && paths["root_ort"] == rootOrt && paths["rekordbox"] == rbApp
          && paths["kept"] == keptDir && paths["cache_default"] == defaultCacheDir && paths["home"] == home && paths["support"] == support,
          "the start config's paths: the model's sha256 key and the prepare helper's arguments included (\(paths.keys.sorted()))")
}

/// Controller.refresh()'s decisions (1.3.0's Controller.swift, refresh) on this Mac now, as 1.3.0's
/// Swift made them (its lights are the report's, Report.swift; the rest tests/mac/shell-oracle.swift): what the core's view model, fed by the shell's cheap facts, must show. No action
/// running, the reminders on and no notice (the start-up checks haven't run yet).
struct RefreshView: Equatable, CustomStringConvertible {
    var plusLook: Int64 = 0, plusTip = "", cacheLook: Int64 = 0, cacheTip = ""
    var installPlus = false, installCache = false, reinstall = false, reinstallTitle = ""
    var reinstallPlus = false, reinstallCache = false, settings = false, uninstallPlus = false, uninstallCache = false, uninstallAll = false
    var status = ""
    var description: String {
        "lights \(plusLook)/\(cacheLook) \(plusTip.debugDescription) \(cacheTip.debugDescription); buttons \([installPlus, installCache, reinstall, reinstallPlus, reinstallCache, settings, uninstallPlus, uninstallCache, uninstallAll].map { $0 ? 1 : 0 }) \(reinstallTitle.debugDescription); status \(status.debugDescription)"
    }
}

func lookCode(_ l: LightLook) -> Int64 { l == .on ? 0 : l == .limited ? 1 : 2 }

func todayRefresh() -> RefreshView {
    var v = RefreshView()
    let plus = stemsPlusPresent(), cache = stemsCachePresent(), c = chosen(), cfg = readConfig()
    let loc = LocationMonitor.shared.snapshot()
    let paused = cache && cfg.enabled ? loc.pausedFloor : nil
    let own = cache && !plus
    let light = cacheLight(installed: cache, plusOn: plus, chosen: c.cache, rekordboxModel: cfg.rekordboxModel,
                           bridgeModels: own ? bridgeCacheModels() : nil, model: own ? currentModelSHA(done: {}) : nil,
                           engine: engineVersion(), pausedFloor: paused, location: loc.lightTip, drive: loc.drive)
    let pl = plusLight(installed: plus)
    (v.plusLook, v.plusTip, v.cacheLook, v.cacheTip) = (lookCode(pl.look), pl.tip, lookCode(light.look), light.tip)
    let plusWhy = stemsPlusBlocked(), cacheWhy = stemsCacheBlocked(), traces = cacheTraces()
    let names = { (p: Bool, k: Bool) in p && k ? "Demucs v4 + Stems Cache" : (k ? "Stems Cache" : "Demucs v4") }
    v.installPlus = !plus && plusWhy == nil
    v.installCache = cache ? !c.cache && rekordboxInstalled() : cacheWhy == nil
    v.reinstall = (c.model && plusWhy == nil) || (c.cache && cacheWhy == nil)
    v.reinstallTitle = "Reinstall " + names(c.model || !c.cache, c.cache)
    v.reinstallPlus = plus && c.model && plusWhy == nil
    v.reinstallCache = cache && c.cache && cacheWhy == nil
    v.settings = true
    v.uninstallPlus = plus
    v.uninstallCache = traces && userIsAdmin
    v.uninstallAll = !traces || userIsAdmin
    var lines: [String] = []
    if ownBundle() == nil { lines.append(moveToApplications) }
    if let why = plusWhy, !plus || c.model { lines.append(why) }
    let standard = standardAccountLine(admin: userIsAdmin, cacheTraces: traces, cacheChosen: c.cache)
    if let why = cacheWhy, !cache || c.cache, !lines.contains(why), standard == nil || why != needsAdminLine { lines.append(why) }
    if let s = standard { lines.append(s) }
    if cacheIsLink() { lines.append(cacheLinkLine) }
    v.status = lines.joined(separator: "\n")
    return v
}

/// The same decisions read from the core's snapshot.
func coreRefresh(_ s: OpaquePointer) -> RefreshView {
    var v = RefreshView()
    v.plusLook = core_i64(s, F_LIGHT_PLUS_LOOK); v.plusTip = snapStr(s, F_LIGHT_PLUS_TIP)
    v.cacheLook = core_i64(s, F_LIGHT_CACHE_LOOK); v.cacheTip = snapStr(s, F_LIGHT_CACHE_TIP)
    func on(_ f: core_field) -> Bool { core_flag(s, f) != 0 }
    v.installPlus = on(F_INSTALL_PLUS_ENABLED); v.installCache = on(F_INSTALL_CACHE_ENABLED)
    v.reinstall = on(F_REINSTALL_ENABLED); v.reinstallTitle = snapStr(s, F_REINSTALL_TITLE)
    v.reinstallPlus = on(F_REINSTALL_PLUS_ENABLED); v.reinstallCache = on(F_REINSTALL_CACHE_ENABLED); v.settings = on(F_SETTINGS_ENABLED)
    v.uninstallPlus = on(F_UNINSTALL_PLUS_ENABLED); v.uninstallCache = on(F_UNINSTALL_CACHE_ENABLED); v.uninstallAll = on(F_UNINSTALL_ALL_ENABLED)
    v.status = snapStr(s, F_STATUS_LINE)
    return v
}

// MARK: the steps' sentence readbacks (review K6), against the sentences today's Swift has

/// The string literals in Swift source, in order, each with its interpolations as "\u{1}".
/// A small scanner: strings (with nested interpolations) and comments, nothing else.
func literals(_ src: Substring) -> [String] {
    var out: [String] = [], i = src.startIndex
    func next(_ k: String.Index) -> String.Index { src.index(after: k) }
    while i < src.endIndex {
        let ch = src[i]
        if ch == "/", next(i) < src.endIndex, src[next(i)] == "/" {        // a line comment
            while i < src.endIndex && src[i] != "\n" { i = next(i) }
            continue
        }
        guard ch == "\"" else { i = next(i); continue }
        var s = "", j = next(i)
        while j < src.endIndex && src[j] != "\"" {
            if src[j] == "\\", next(j) < src.endIndex {
                let e = src[next(j)]
                if e == "(" {                                       // an interpolation: to its matching paren
                    var depth = 1, k = src.index(j, offsetBy: 2)
                    while k < src.endIndex && depth > 0 {
                        if src[k] == "(" { depth += 1 } else if src[k] == ")" { depth -= 1 }
                        k = next(k)
                    }
                    s += "\u{1}"; j = k; continue
                }
                s.append(e == "n" ? "\n" : e); j = src.index(j, offsetBy: 2); continue
            }
            s.append(src[j]); j = next(j)
        }
        out.append(s)
        i = j < src.endIndex ? next(j) : j
    }
    return out
}

/// The text of each call that starts with `start` in `src`, to its matching paren (strings skipped).
func calls(_ start: String, in src: String) -> [Substring] {
    var out: [Substring] = [], from = src.startIndex
    while let r = src.range(of: start, range: from..<src.endIndex) {
        var depth = 0, k = r.lowerBound, inString = false
        while k < src.endIndex {
            let c = src[k]
            if c == "\"" && (k == src.startIndex || src[src.index(before: k)] != "\\") { inString.toggle() }
            if !inString { if c == "(" { depth += 1 } else if c == ")" { depth -= 1; if depth == 0 { break } } }
            k = src.index(after: k)
        }
        out.append(src[r.lowerBound..<min(src.index(after: k), src.endIndex)])
        from = r.upperBound
    }
    return out
}

/// A function's body in `src`, from its declaration to the first closing brace at column 0.
func body(of decl: String, in src: String) -> String {
    guard let r = src.range(of: decl), let end = src.range(of: "\n}\n", range: r.upperBound..<src.endIndex) else { return "" }
    return String(src[r.lowerBound..<end.upperBound])
}

func readbackTests(_ root: String) {
    let models = (try? String(contentsOfFile: root + "/app/Sources/Models.swift", encoding: .utf8)) ?? ""
    let location = (try? String(contentsOfFile: root + "/app/Sources/Location.swift", encoding: .utf8)) ?? ""
    check(!models.isEmpty && !location.isEmpty, "today's Models.swift and Location.swift read")

    // Models.Outcome: every failure message Models.swift writes (fail(...), Outcome(ok: false, ...)),
    // the OS error of an interpolated one standing for "\u{1}"
    let err = "Input/output error"
    let expected: [String: (core_model_outcome, String)?] = [
        unknownModelMessage: (CORE_MODEL_UNKNOWN, ""),
        "rekordbox's stems model can't be read.": (CORE_MODEL_UNREADABLE, ""),
        "rekordbox's stems model isn't a file.": (CORE_MODEL_NOT_A_FILE, ""),
        "There isn't enough free space to save a copy of rekordbox's own stems model.": (CORE_MODEL_NO_SPACE_SAVE, ""),
        "A copy of rekordbox's own stems model couldn't be saved.": (CORE_MODEL_NOT_SAVED, ""),
        "There isn't enough free space to copy the Demucs v4 model.": (CORE_MODEL_NO_SPACE_COPY, ""),
        "The Demucs v4 model couldn't be copied: \u{1}.": (CORE_MODEL_NOT_COPIED_WHY, err),
        "The Demucs v4 model didn't copy correctly.": (CORE_MODEL_COPY_BAD, ""),
        "rekordbox's stems model changed meanwhile.": (CORE_MODEL_CHANGED, ""),
        "The Demucs v4 model couldn't be put in place: \u{1}.": (CORE_MODEL_NOT_PLACED_WHY, err),
        "There's no saved copy of rekordbox's own stems model on this Mac.": (CORE_MODEL_NO_SAVED_COPY, ""),
        "There isn't enough free space to put rekordbox's own stems model back.": (CORE_MODEL_NO_SPACE_BACK, ""),
        "rekordbox's own stems model couldn't be put back.": (CORE_MODEL_NOT_BACK, ""),
        "rekordbox's own stems model couldn't be put back: \u{1}.": (CORE_MODEL_NOT_BACK_WHY, err),
        "rekordbox's stems model didn't verify after it was put back.": (CORE_MODEL_NOT_VERIFIED, ""),
        "The saved copy of rekordbox's own stems model is damaged.": (CORE_MODEL_SAVED_DAMAGED, ""),
        // rekordbox's model folder linked or unreadable: not back, as Windows' models.c gives it
        "rekordbox's model folder is a link, so its stems model wasn't checked.": (CORE_MODEL_NOT_BACK, ""),
        "rekordbox's model folder can't be read.": (CORE_MODEL_NOT_BACK, ""),
        // fail(originalsUnreadableMessage), a constant the literal scan can't see (added below):
        // restore's refusal, not back (as Windows' models.c gives it); MODEL_COPY reads it below
        "The saved copies of rekordbox's own stems model can't be read.": (CORE_MODEL_NOT_BACK, ""),
        // saveOriginal's refusal to save our own model as Pioneer's: no core_model_outcome (it isn't
        // reachable from installOurs, which saves only a model that isn't ours)
        "rekordbox's stems model is the Demucs v4 model, so it wasn't saved as rekordbox's own.": nil,
    ]
    // fail(unknownModelMessage) and fail(originalsUnreadableMessage): constants, not literals
    var found: [(message: String, noSpace: Bool)] = [(unknownModelMessage, false), (originalsUnreadableMessage, false)]
    check(calls("fail(", in: models).contains { $0.contains("originalsUnreadableMessage") }, "Models.swift fails with originalsUnreadableMessage (in the readback by hand)")
    // .notChecked(why): Models.folder()'s reasons, which restore (and install) fail with
    for call in calls("fail(", in: models) + calls("Outcome(ok: false", in: models) + calls(".notChecked(", in: models) {
        let noSpace = call.contains("noSpace: true")
        for l in literals(call) where l != "No space" && !l.isEmpty { found.append((l, noSpace)) }
    }
    check(found.count >= 18, "Models.swift's failure messages found (\(found.count))")
    for f in found {
        guard let want = expected[f.message] else { check(false, "Models.swift has a failure message the readback doesn't know: \(f.message)"); continue }
        let got = modelOutcomeCode(Outcome(ok: false, message: f.message.replacingOccurrences(of: "\u{1}", with: err), noSpace: f.noSpace))
        check(got?.code == want?.0 && got?.why == want?.1,
              "\(f.message.replacingOccurrences(of: "\u{1}", with: "…")) -> \(String(describing: want?.0.rawValue)) (got \(String(describing: got?.code.rawValue)), \(got?.why ?? "-"))")
    }
    check(Set(found.map { $0.message }) == Set(expected.keys), "every message the table names is one Models.swift writes")
    check(modelOutcomeCode(Outcome(ok: false, message: "anything else")) == nil, "an unknown message sets no code")
    // MODEL_COPY reads Models.folder()'s two reasons and unreadable saved originals as "not
    // copied", with the state as the why (review MP9, SM1); MODEL_RESTORE keeps them as "not
    // back"; every other message the same
    let copyWhy: [String: String] = [
        "rekordbox's model folder is a link, so its stems model wasn't checked.": "rekordbox's model folder is a link",
        "rekordbox's model folder can't be read.": "rekordbox's model folder can't be read",
        "The saved copies of rekordbox's own stems model can't be read.": "the saved copies of rekordbox's own stems model can't be read",
    ]
    for f in found {
        let r = Outcome(ok: false, message: f.message.replacingOccurrences(of: "\u{1}", with: err), noSpace: f.noSpace)
        let got = copyOutcomeCode(r), plain = modelOutcomeCode(r)
        if let why = copyWhy[f.message] {
            check(got?.code == CORE_MODEL_NOT_COPIED_WHY && got?.why == why && plain?.code == CORE_MODEL_NOT_BACK,
                  "MODEL_COPY: \(f.message) -> not copied (\(why)); MODEL_RESTORE: not back")
        } else {
            check(got?.code == plain?.code && got?.why == plain?.why, "MODEL_COPY reads \(f.message.replacingOccurrences(of: "\u{1}", with: "…")) as MODEL_RESTORE does")
        }
    }
    check(Set(copyWhy.keys).isSubset(of: Set(found.map { $0.message })), "MODEL_COPY's folder and originals reasons are ones Models.swift writes")
    check(copyOutcomeCode(Outcome(ok: false, message: "anything else")) == nil, "MODEL_COPY: an unknown message sets no code")

    // MODELS_INFO: FACT_ORIGINALS_VERIFIED, which flows.c keeps the originals folder by, is
    // originalsKept (a copy that verifies, or one that can't be checked, in a folder that stays);
    // with the only copy at mode 000 it is set (review J1)
    let hostSteps = (try? String(contentsOfFile: root + "/app/Shell/HostSteps.swift", encoding: .utf8)) ?? ""
    check(hostSteps.contains("let facts = modelsInfoFacts(models)") && hostSteps.contains("o.flag(FACT_ORIGINALS_VERIFIED, facts.verified)")
          && hostSteps.contains("o.flag(FACT_ORIGINALS_SAVED, facts.saved)") && hostSteps.contains("o.str(FACT_STEP_URL, facts.url)")
          && hostSteps.contains("let c = models.originalsCheck(hashing: false); o.flag(FACT_ORIGINALS_SAVED, !c.saved.isEmpty || c.unknown); return 0")
          && !hostSteps.contains("originalsCheck(hashing: arg(0)"),
          "MODELS_INFO sets its facts and URL from modelsInfoFacts (one hashed check, review MQ2); \"saved\" alone from its own unhashed one")
    check(hostSteps.contains("setModelOutcome(o, copyOutcomeCode(r))"), "MODEL_COPY sets its code through copyOutcomeCode (MP9)")
    let mt = NSTemporaryDirectory() + "rbsp-shell-test-models-\(getpid())", mo = mt + "/originals"
    let sha = String(repeating: "c", count: 64)
    try? FileManager.default.createDirectory(atPath: mo, withIntermediateDirectories: true)
    FileManager.default.createFile(atPath: mo + "/\(sha).onnx", contents: Data("a saved copy".utf8))
    chmod(mo + "/\(sha).onnx", 0o000)
    let unreadable = Models(dir: mt + "/rb", originals: mo, ours: [], gate: Deletable(inside: [], roots: [], files: [], apps: []))
    let keptUnknown = originalsKept(unreadable), verified = unreadable.verifiedOriginals()
    chmod(mo + "/\(sha).onnx", 0o644)
    try? FileManager.default.removeItem(atPath: mt)
    check(verified.isEmpty && keptUnknown, "MODELS_INFO: the only saved copy at mode 000 isn't verified, and the folder is kept (FACT_ORIGINALS_VERIFIED)")

    // removeInLocation: every reason it returns, the entry's name and the OS error standing for "\u{1}"
    let fn = body(of: "func removeInLocation(", in: location)
    let reasons = calls("return (", in: fn).flatMap { literals($0) }.filter { !$0.isEmpty && $0 != "._" }
    let name = "Stems: 2024", oserr = "Operation not permitted"       // a name with ": " in it: split at the last one
    let wantRemoval: [String: (Int64, String, String)] = [
        "not connected": (1, "", ""), "the drive can't be opened": (2, "", ""), "the drive can't be read": (3, "", ""),
        "\u{1}: \u{1}": (4, name, oserr), "\u{1} is on another drive": (5, name, ""),
        "the folder's marker isn't the one expected": (6, "", ""), "some files couldn't be deleted": (7, "", ""),
        "the folder's marker can't be read": (7, "", ""),
    ]
    check(Set(reasons) == Set(wantRemoval.keys), "removeInLocation's reasons are the table's (\(Set(reasons).subtracting(wantRemoval.keys)))")
    for r in Set(reasons) {
        var parts = [name, oserr][...]
        var text = ""
        for ch in r { if ch == "\u{1}" { text += parts.popFirst() ?? "" } else { text.append(ch) } }
        let got = removalCode(text), want = wantRemoval[r]
        check(want.map { got == $0 } == true, "\(text) -> \(String(describing: want)) (got \(got))")
    }
    check(removalCode("anything else") == (7, "", ""), "an unknown reason reads as 7")

    // modelLeftBehind: each of its sentences
    let left = literals(Substring(body(of: "func modelLeftBehind(", in: models)))
    let wantLeft: [String: Int64] = ["rekordbox has no stems model.": 0, "rekordbox's stems model can't be read.": 1,
                                     "rekordbox still has the Demucs v4 model.": 2, "rekordbox's stems model isn't one RB Stems Plus knows.": 3]
    check(Set(left) == Set(wantLeft.keys), "modelLeftBehind's sentences are the table's (\(left))")
    for l in left { check(leftBehindCode(l) == wantLeft[l], "\(l) -> \(String(describing: wantLeft[l])) (got \(leftBehindCode(l)))") }
}

/// The watcher over the core (WatcherShell.swift): its host table and the core's watcher flow,
/// the lock and the again-mark real files in a scratch folder, every other step a stand-in.
/// One start, `missing` Demucs v4 or not; `competing`: another start comes in during the first
/// check (Watcher.swift's race test until the switch-over). Returns the steps, the log, the
/// forms shown and whether the core said WATCHER_DONE.
var watcherShellsKept: [WatcherShell] = []
func watcherRun(missing: Bool, competing: Bool, answer: Int = 1) -> (steps: [String], log: [String], forms: [DialogForm], done: Bool, lockGone: Bool, againGone: Bool, refused: [String]) {
    let dir = NSTemporaryDirectory() + "rbsp-shell-test-watch-\(UUID().uuidString)"
    try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(atPath: dir) }
    let lock = dir + "/watcher.lock", again = dir + "/watcher.again"
    let gate = Deletable(inside: [dir], roots: [], files: [], apps: [])
    var refused: [String] = []
    _ = takeWatcherLock(lock, again: again, gate: gate, log: { _ in })
    let lk = NSLock()
    var steps: [String] = [], log: [String] = [], forms: [DialogForm] = [], facts = 0
    let ended = DispatchSemaphore(value: 0)
    let w = WatcherShell(app: "/Applications/RB Stems Plus.app", log: { l in lk.lock(); log.append(l); lk.unlock() })
    w.lockPath = lock; w.againPath = again; w.gate = gate
    watcherShellsKept.append(w)                                       // the core keeps its host's ctx for good
    w.done = { ended.signal() }
    w.presentDialog = { f in lk.lock(); forms.append(f); lk.unlock(); return DialogAnswer(choice: .button(answer), fields: []) }
    w.runStep = { k, a, out in
        let o = CoreFactSheet(out: out)
        lk.lock(); steps.append("\(k.rawValue) \(a.joined(separator: " "))"); lk.unlock()
        switch k {
        case CORE_STEP_WATCH_LOCK: return w.step(k, a, out)          // the real lock and mark
        case CORE_STEP_WATCH_FACTS:
            facts += 1
            if competing && facts == 1 {                              // a start while this check runs
                _ = takeWatcherLock(lock, again: again, gate: gate, log: { l in refused.append(l) })
            }
            o.flag(FACT_CHOSEN_MODEL, true); o.flag(FACT_STEMS_PLUS_PRESENT, !missing)
            o.str(FACT_REKORDBOX_VERSION, "7.2.19.0342"); o.str(FACT_ENGINE, "0002")
            o.flag(FACT_ENGINE_SUPPORTED, true); o.flag(FACT_REKORDBOX_SUPPORTED, true)
            o.str(FACT_STEP_TEXT2, "1.4.0")
            return 0
        default: return 0                                             // rekordbox closed, nothing busy
        }
    }
    let started = onMain { w.start() }                                 // the core's main thread is the process's
    let done = started && ended.wait(timeout: .now() + 10) == .success
    lk.lock(); defer { lk.unlock() }
    return (steps, log, forms, done, fileType(lock) == nil, fileType(again) == nil, refused)
}

func watcherShellTests() {
    // nothing missing, and a start during the check: refused, logged, looked at again by this one
    var r = watcherRun(missing: false, competing: true)
    check(r.done, "watcher: the core's watcher flow ends with WATCHER_DONE")
    check(r.refused == ["another check is running: it will look again"], "watcher race: the start during the check is refused and says so (\(r.refused))")
    check(r.log.filter { $0 == "nothing to ask" }.count == 2 && r.log.contains("something changed during the check: looking again"),
          "watcher race: the running watcher checks again for it (\(r.log))")
    check(r.lockGone && r.againGone, "watcher race: the lock is free after, the again-mark gone")
    check(r.log.first?.hasPrefix("run: RB Stems Plus Watcher 1.4.0; rekordbox 7.2.19.0342, engine 0002, Demucs v4 on") == true, "watcher: the run line (\(r.log))")
    // Demucs v4 missing: the core's question through the host's dialog, Remind Me Later
    r = watcherRun(missing: true, competing: false)
    check(r.done && r.forms.count == 1 && r.forms[0].title == "rekordbox put its own stems model back"
          && r.forms[0].buttons == ["Reinstall Now", "Remind Me Later", "Don't Ask Again for This Version"] && r.forms[0].inline,
          "watcher: the question, inline, as 1.3.0's watcherPrompt (\(r.forms.map { $0.title }))")
    check(r.log.contains("asking: model:0002") && r.log.contains("user chose: Remind Me Later") && r.lockGone,
          "watcher: Remind Me Later: logged, the lock let go (\(r.log))")
    // Reinstall Now: the step that writes the request and opens the app after this process
    r = watcherRun(missing: true, competing: false, answer: 0)
    check(r.done && r.steps.contains("\(CORE_STEP_WATCH_REINSTALL.rawValue) ") && r.lockGone, "watcher: Reinstall Now: the request and the app opened (\(r.steps))")

    // the prepared playlists' check's reads (WATCH_KEPT), against 1.3.0's on this Mac's live
    // facts: prepareFacts(measured: false), keptSets(), a set's run looked for
    let app = Bundle.main.bundlePath, w = WatcherShell(app: app, log: { _ in })
    let f = FactRecorder()
    check(w.kept("facts", "", "", f) == 0, "watcher: the check's facts read")
    let pf = prepareFacts(app: app, measured: false), st = readConfigState()
    check(f.b(FACT_APPLE_SILICON) == pf.appleSilicon && f.b(FACT_HELPER_THERE) == pf.helperThere && f.b(FACT_REKORDBOX_INSTALLED) == pf.rekordboxInstalled
          && f.s(FACT_REKORDBOX_VERSION) == pf.rekordboxVersion && f.b(FACT_REKORDBOX_SUPPORTED) == pf.rekordboxOK
          && f.b(FACT_STEMS_CACHE_PRESENT) == pf.cacheInstalled && f.b(FACT_STEMS_PLUS_PRESENT) == pf.plusOn && f.b(FACT_BRIDGE_KEEPS) == pf.bridgeKeeps
          && f.b(FACT_ROOT_ORT_EXISTS) == pf.ortThere && f.b(FACT_CONFIG_ENABLED) == pf.enabled && f.s(FACT_CONFIG_MAX_GB) == configNumber(pf.maxGB)
          && f.s(FACT_CONFIG_UNREADABLE) == pf.configUnreadable && f.i(FACT_STEMS_QUALITY) == Int64(pf.quality ?? -1)
          && f.b(FACT_CHOSEN_CACHE) == chosen().cache && f.s(FACT_ENGINE) == engineVersion() && f.b(FACT_CONFIG_RB_MODEL) == st.config.rekordboxModel,
          "watcher: the check's facts are prepareFacts(measured: false)'s reads")
    check(!f.has(FACT_MODEL_SIZE) && !f.has(FACT_LOC_MEASURED), "watcher: no model checksum worked out, no location measured (as 1.3.0's watcher)")
    let ks = FactRecorder()
    check(w.kept("sets", "", "", ks) == 0 && ks.s(FACT_KEPT_SETS) == ShellFacts.keptSetsText(keptSets()), "watcher: the kept sets: keptSets()")
    let rn = FactRecorder()
    check(w.kept("running", "rbsp-shell-test-no-such-set", "", rn) == 0 && rn.b(FACT_STEP_FLAG) == false, "watcher: a set nobody prepares isn't running")
    check(w.kept("nonsense", "", "", FactRecorder()) == 1, "watcher: an unknown WATCH_KEPT op is refused")
}

/// The report's lights' inputs (REPORT_FACTS) against reportFacts' own reads on this Mac, with one
/// measurement (a snapshot taken here and passed in, as REPORT_ZIP then uses it).
func reportLightsTests() {
    let loc = LocationMonitor.shared.snapshot(), sha = String(repeating: "c", count: 64)
    let r = FactRecorder()
    ShellFacts.reportLights(r, ReportMeasure(loc: loc, modelSha: sha))
    let plus = stemsPlusPresent(), cache = stemsCachePresent(), cfg = readConfig()
    check(r.b(FACT_STEMS_PLUS_PRESENT) == plus && r.b(FACT_STEMS_CACHE_PRESENT) == cache && r.b(FACT_CHOSEN_CACHE) == chosen().cache
          && r.b(FACT_CONFIG_ENABLED) == cfg.enabled && r.b(FACT_CONFIG_RB_MODEL) == cfg.rekordboxModel && r.s(FACT_ENGINE) == engineVersion(),
          "report: the lights' inputs are reportFacts' reads")
    check(r.s(FACT_BRIDGE_MODELS) == (cache ? bridgeCacheModels()?.joined(separator: "\n") : nil), "report: the bridge's models only with Stems Cache in rekordbox")
    check(r.s(FACT_STEP_TEXT) == sha, "report: the model's sha256 as measured")
    let r2 = FactRecorder()
    ShellFacts.reportLights(r2, ReportMeasure(loc: loc, modelSha: "missing"))
    check(r2.s(FACT_STEP_TEXT) == "", "report: a model that isn't there: no sha256 (the light doesn't judge it)")
    check(r.b(FACT_LOC_MEASURED) == (loc.status != nil) && r.i(FACT_LOC_KIND) == loc.status.map(ShellFacts.locKind) && r.s(FACT_LOC_DRIVE) == loc.drive
          && r.i(FACT_LOC_PAUSED_FLOOR) == (loc.status.flatMap(folderInUse).flatMap { cachePausedFloor($0) } ?? -1),
          "report: the location as measured, the floor the folder in use is under now")
}

/// The start-up refusals (main.swift's startupText): 1.3.0's words, from the core's table.
func startupTextTests() {
    check(startupText(T_CANT_OPEN) == "RB Stems Plus can't open" && startupText(T_QUIT) == "Quit" && startupText(T_OK) == "OK",
          "start-up: the refusal's title and buttons")
    check(startupText(T_ALREADY_OPEN) == "Already open" && startupText(T_ALREADY_OPEN_MSG) == "Use the RB Stems Plus window that is already open.",
          "start-up: another copy already open")
    check(startupText(T_LOCK_UNUSABLE, [(CORE_SLOT_path, "~/Library/Application Support/rbstemsplus/app.lock"), (CORE_SLOT_why, "Not a directory")])
          == "~/Library/Application Support/rbstemsplus/app.lock can't be used (Not a directory). Move it to the Trash, then open RB Stems Plus again.",
          "start-up: an unusable lock, its path and reason in the slots")
    check(startupText(T_LOCK_UNUSABLE) == "", "start-up: a text missing its slots is refused, never shown with holes")
}

Thread.detachNewThread {
    startupTextTests()
    watcherShellTests()
    reportLightsTests()
    presenterTests()
    sheetTests()
    adminTests()
    let root = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."
    readbackTests(root)
    coverageTests(root)
    locationFactTests()
    sheetFixtureTests()
    shellTests()
    cheapSheetTests(root)                     // once the shell has measured the location
    DialogPresenter.shared.shutDown()
    check(DialogPresenter.shared.present { 1 } == nil, "nothing is shown once shutting down")
    print("shell-test: \(passed) passed, \(failures) failed")
    exit(failures == 0 ? 0 : 1)
}
app.run()
