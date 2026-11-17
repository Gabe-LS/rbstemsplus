// The characterisation harness's probe (the private Windows plan's mac-core §2.3): what the window
// showed on every main run-loop turn, for the scenario traces (trace mode), and timestamps for
// the performance metrics P1-P16 (perf mode). TEST BUILDS ONLY: scripts/build-app.sh compiles
// this file only with -D TEST_BUILD, and the whole file is inside #if TEST_BUILD as well, so a
// release build contains none of it (tests/harness/check-release.sh proves it on the binary).
//
// It does nothing unless the harness wrote its configuration file,
//   ~/Library/Application Support/rbstemsplus-probe/probe.conf   (key=value lines)
//     mode=trace | perf | perf-min    perf-min: what the metrics need and nothing else (the
//                                     overhead check: no extra events, no self-timing)
//     out=<folder>                    where the records, the command FIFO and the acks are
//     helper=<path>                   the fake rbsp-prepare (tests/harness/fake-prepare.c)
// so a TEST build used for a manual round behaves as before.
//
// Today's code is recorded without editing it: the controls' setters are swizzled, a run-loop
// observer snapshots every registered control once per turn (before waiting), and the field ids
// are the controller's and the tabs' property names (Mirror), so the new shell can carry the
// same ids. The few timing points P3 needs inside today's code are one-line probeMark calls.
//
// Records (JSON lines, <out>/app-<pid>.jsonl): start, frame (the fields that changed this turn),
// set (setter level), ev (activation, reopen, dialogs, marks, commands, timing points). Perf mode
// keeps (time, event, field, number) in memory and writes <out>/perf-<pid>.tsv on `flush` and at
// exit. Never recorded: a password field's value, the log view's text (it holds clock times).
#if TEST_BUILD
import AppKit
import Darwin

// MARK: - entry points (the app's code calls only these)

/// Called from main.swift before anything else of the app or the watcher runs.
func probeStart(watcher: Bool) { Probe.shared.start(watcher: watcher) }

/// A timing point inside today's code (P3's trigger sites, the end of refresh()).
@inline(__always) func probeMark(_ name: StaticString) { if Probe.shared.mode != .off { Probe.shared.mark(name) } }

/// P7: the helper's line Prepare's reader just took (its reading thread). The estimate's last
/// line ({"estimate":…}) is the timing point "estimate.lastline", stamped now, recorded on main.
func probeHelperLine(_ line: Data) {
    guard Probe.shared.mode != .off, line.starts(with: Array("{\"estimate\"".utf8)) else { return }
    Probe.shared.markFromAnyThread("estimate.lastline")
}

/// The G1 API spike (app/Spike, mac-core §9 G1.7): its table is recorded as the Prepare tab's
/// "prepare.tracksTable" (P6, P7), and the tab's own table is then left out; `command` gets every
/// driver command first and returns true for the ones it took (select, scroll).
func probeSpike(table: NSTableView, command: @escaping (_ verb: String, _ arg: String, _ ack: @escaping (String) -> Void) -> Bool) {
    Probe.shared.spike(table, command)
}

#if CORE_LINKED
/// The G1 API spike's start, run on the driver's first `mark settled` (main, before its ack):
/// after the launch and the activation, so neither has the spike's second window, which 1.4.0
/// doesn't have (an activation with two windows: some 6.6 ms more in P3.longest.activate@first).
/// Until then a spike command (select, scroll) is refused: the scenario must mark settled first.
func probeSpikeStartOnSettled(_ start: @escaping () -> Void) { Probe.shared.spikeStart = start }
#endif

/// The AppKit shell over the core (app/Shell, the app since M7's switch-over): its Status and
/// Prepare tabs, with 1.3.0's tabs' property names, are recorded as "status" and "prepare", so the
/// field ids stay those of mac-A's traces.
func probeShellTabs(status: AnyObject, prepare: AnyObject) { Probe.shared.shellTabs = (status, prepare) }

/// The fake rbsp-prepare, when the harness configured one, for the app's own helper path only.
func probeFakeHelper(_ app: String) -> String? {
    guard app == Bundle.main.bundlePath, let h = Probe.shared.conf["helper"], !h.isEmpty else { return nil }
    return h
}

// MARK: - the probe

/// The app's shell (the driver's verbs act on its tabs).
private var probeShell: CoreShell? { (NSApp?.delegate as? AppDelegate)?.shell }

/// The playlist id of the outline's row `r` (its snapshot row's C_OUT_ID).
private func outlineID(_ o: NSOutlineView, _ r: Int) -> String? {
    guard let it = o.item(atRow: r) as? OutlineItem, let h = probeShell?.held else { return nil }
    return cellStr(h, C_OUT_ID, it.row)
}

final class Probe {
    enum Mode: String { case off, trace, perf, perfMin = "perf-min" }
    static let shared = Probe()
    var mode = Mode.off
    var conf: [String: String] = [:]
    private var outDir = ""
    private var out: FileHandle?
    private let writeQ = DispatchQueue(label: "rbsp.probe.write")
    private var timebase = mach_timebase_info_data_t()
    private var t0: UInt64 = 0
    private var watcher = false

    // field ids: every registered object, and the menu items by their first title
    private var ids: [ObjectIdentifier: String] = [:]
    private var objects: [(id: String, obj: AnyObject)] = []
    private var controller: Controller?
    private var tableBuilt = false
    private var lastBuildTurn = -1
    private var menusBuilt = false
    private var animating = Set<ObjectIdentifier>()

    // frames
    private var last: [String: String] = [:]
    private var turn = 0
    private var wake: UInt64 = 0
    private var lastChange: UInt64 = 0
    private var hookDepth = 0
    private var windowDrawn = false

    // perf mode: (time, kind, field, number), written on flush
    private struct PerfEvent { var t: UInt64; var kind: UInt8; var field: Int32; var n: Int64 }
    private var perf: [PerfEvent] = []
    private var perfNames: [String] = []
    private var perfIndex: [String: Int32] = [:]
    private var busyNs: UInt64 = 0                  // the probe's own recording time on the main thread
    private var waitNs: UInt64 = 0                  // the driver's waits checked on the main thread
    private var spikeTable: ObjectIdentifier?       // the G1 spike's table (probeSpike)
    private var spikeCommand: ((String, String, @escaping (String) -> Void) -> Bool)?
    #if CORE_LINKED
    fileprivate var spikeStart: (() -> Void)?       // probeSpikeStartOnSettled, until mark settled
    #endif
    fileprivate var shellTabs: (status: AnyObject, prepare: AnyObject)?   // probeShellTabs

    fileprivate func spike(_ t: NSTableView, _ command: @escaping (String, String, @escaping (String) -> Void) -> Bool) {
        spikeTable = ObjectIdentifier(t)
        ids[spikeTable!] = "spike.tracksTable"      // known: its setters never rebuild the table of ids
        objects.append(("spike.tracksTable", t))
        spikeCommand = command
    }

    // commands and waits
    private var waits: [(id: String, deadline: UInt64, test: () -> Bool)] = []
    private var dialogs = 0
    private var eventCounts: [String: Int] = [:]

    // MARK: start

    func start(watcher: Bool) {
        let path = home + "/Library/Application Support/rbstemsplus-probe/probe.conf"
        guard let text = try? String(contentsOfFile: path, encoding: .utf8) else { return }
        for line in text.split(separator: "\n") {
            let kv = line.split(separator: "=", maxSplits: 1).map(String.init)
            if kv.count == 2 { conf[kv[0].trimmingCharacters(in: .whitespaces)] = kv[1].trimmingCharacters(in: .whitespaces) }
        }
        guard let m = Mode(rawValue: conf["mode"] ?? ""), m != .off, let dir = conf["out"], !dir.isEmpty else { return }
        mach_timebase_info(&timebase)
        t0 = mach_absolute_time()
        mode = m
        outDir = dir
        self.watcher = watcher
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let pid = getpid()
        if watcher { startWatcher(pid); return }
        let file = dir + "/app-\(pid).jsonl"
        FileManager.default.createFile(atPath: file, contents: nil)
        out = FileHandle(forWritingAtPath: file)
        launchStore = launchOffsetNs(pid)
        emit("{\"k\":\"start\",\"t\":0,\"mode\":\"\(mode.rawValue)\",\"pid\":\(pid),\"launch_ns\":\(launchStore),\"arch\":\"\(arch())\"}")
        perf.reserveCapacity(1 << 16)
        installObservers()
        installHooks()
        startCommands()
        atexit { Probe.shared.flushPerf(); Probe.shared.writeQ.sync {} }
    }

    func now() -> UInt64 { (mach_absolute_time() - t0) * UInt64(timebase.numer) / UInt64(timebase.denom) }

    /// The process's start (the kernel's), in ns relative to t0: negative, the launch before the probe ran.
    private func launchOffsetNs(_ pid: pid_t) -> Int64 {
        var info = kinfo_proc(), size = MemoryLayout<kinfo_proc>.stride
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, pid]
        guard sysctl(&mib, 4, &info, &size, nil, 0) == 0 else { return 0 }
        let st = info.kp_proc.p_un.__p_starttime
        var tv = timeval(); gettimeofday(&tv, nil)
        let ago = (Int64(tv.tv_sec) - Int64(st.tv_sec)) * 1_000_000_000 + (Int64(tv.tv_usec) - Int64(st.tv_usec)) * 1000
        return Int64(now()) - ago
    }

    private func arch() -> String {
        var v: Int32 = 0, n = MemoryLayout<Int32>.size
        let translated = sysctlbyname("sysctl.proc_translated", &v, &n, nil, 0) == 0 && v == 1
        #if arch(arm64)
        return "arm64"
        #else
        return translated ? "x86_64-rosetta" : "x86_64"
        #endif
    }

    /// The watcher (P11): its start and, at exit, its run time, CPU time and peak footprint.
    private func startWatcher(_ pid: pid_t) {
        let launch = launchOffsetNs(pid)
        atexit {
            let p = Probe.shared
            var ru = rusage_info_v4()
            let ok = withUnsafeMutablePointer(to: &ru) { ptr in
                ptr.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) { proc_pid_rusage(getpid(), RUSAGE_INFO_V4, $0) }
            } == 0
            var t = timebase_ns(ru.ri_user_time + ru.ri_system_time)
            if !ok { t = 0 }
            let line = "{\"k\":\"watcher\",\"pid\":\(getpid()),\"launch_ns\":\(p.launchStore),\"exit_ns\":\(p.now()),\"cpu_ns\":\(t),\"peak_footprint\":\(ok ? ru.ri_lifetime_max_phys_footprint : 0)}\n"
            FileManager.default.createFile(atPath: p.outDir + "/watcher-\(getpid()).json", contents: Data(line.utf8))
        }
        launchStore = launch
    }
    private var launchStore: Int64 = 0

    // MARK: output

    private func emit(_ line: String) {
        guard let out = out else { return }
        let d = Data((line + "\n").utf8)
        writeQ.async { out.write(d) }
    }

    private func perfIdx(_ name: String) -> Int32 {
        if let i = perfIndex[name] { return i }
        let i = Int32(perfNames.count); perfNames.append(name); perfIndex[name] = i; return i
    }

    /// kinds: 0 turn end (n: the turn's busy ns), 1 field event, 2 timing point, 3 mark, 4 command, 5 row fetch (n: ns), 6 event
    private func perfAdd(_ kind: UInt8, _ name: String, _ n: Int64 = 0, at t: UInt64? = nil) {
        perf.append(PerfEvent(t: t ?? now(), kind: kind, field: perfIdx(name), n: n))
    }

    func flushPerf() {
        guard mode == .perf || mode == .perfMin, !perf.isEmpty || busyNs > 0 else { return }
        var s = "# mode=\(mode.rawValue) pid=\(getpid()) launch_ns=\(launchStore) probe_busy_ns=\(busyNs) wait_ns=\(waitNs) arch=\(arch())\n"
        s.reserveCapacity(perf.count * 32)
        let kinds = ["turn", "field", "tp", "mark", "cmd", "rowfetch", "ev"]
        for e in perf { s += "\(e.t)\t\(kinds[Int(e.kind)])\t\(perfNames[Int(e.field)])\t\(e.n)\n" }
        FileManager.default.createFile(atPath: outDir + "/perf-\(getpid()).tsv", contents: Data(s.utf8))
    }

    /// A timing point from another thread: its time taken now, recorded on the main thread.
    func markFromAnyThread(_ name: String) {
        let t = now()
        CFRunLoopPerformBlock(CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue) {
            let p = Probe.shared
            p.count(name)
            switch p.mode {
            case .trace: p.emit("{\"k\":\"ev\",\"t\":\(t),\"e\":\"tp\",\"v\":\(js(name))}")
            case .perf, .perfMin: p.perfAdd(2, name, 0, at: t)
            case .off: break
            }
        }
        CFRunLoopWakeUp(CFRunLoopGetMain())
    }

    func mark(_ name: StaticString) {
        let n = "\(name)"
        count(n)
        switch mode {
        case .trace: emit("{\"k\":\"ev\",\"t\":\(now()),\"e\":\"tp\",\"v\":\(js(n))}")
        case .perf, .perfMin: perfAdd(2, n)
        case .off: break
        }
    }

    fileprivate func event(_ e: String, _ v: String = "") {
        count(e)
        switch mode {
        case .trace: emit("{\"k\":\"ev\",\"t\":\(now()),\"e\":\(js(e)),\"v\":\(js(v))}")
        case .perf: perfAdd(6, e)
        case .perfMin: if ["didBecomeActive", "reopen", "windowDrawn", "dialog", "dialogEnd", "tabShown"].contains(e) { perfAdd(6, e) }
        case .off: break
        }
    }

    private func count(_ e: String) { eventCounts[e, default: 0] += 1 }

    // MARK: field ids

    /// The controller's, the tabs' and the menus' objects, by property name. Built once the
    /// window exists (its delegate is the controller), again as views load.
    private func buildTable() {
        if controller == nil {
            controller = (NSApp?.delegate as? AppDelegate)?.controller
        }
        guard let c = controller, let t = shellTabs else { return }
        register(c, prefix: "c")
        register(t.status, prefix: "status")
        register(t.prepare, prefix: "prepare")
        tableBuilt = true
        // the menus exist once main.swift has set them, after the controller
        if !menusBuilt, let m = NSApp.mainMenu { registerMenu(m, path: "menu"); menusBuilt = true }
        // views not held by a property (static labels): by tab and order, once their tab has loaded
        if mode == .trace {
            for (i, item) in (c.tabs?.tabViewItems ?? []).enumerated() where item.viewController?.isViewLoaded == true {
                var n = 0
                walk(item.viewController!.view) { v in
                    guard self.ids[ObjectIdentifier(v)] == nil, v is NSControl || v is NSProgressIndicator, !(v is NSScroller) else { return }
                    if v.superview is NSTableCellView || v is NSTableView { return }
                    let id = "\(["status", "prepare", "c"][min(i, 2)]).view\(n)"
                    n += 1
                    self.ids[ObjectIdentifier(v)] = id
                    self.objects.append((id, v))
                }
            }
        }
    }

    private func walk(_ v: NSView, _ f: (NSView) -> Void) {
        f(v)
        if v is NSTableView { return }
        for s in v.subviews { walk(s, f) }
    }

    private func register(_ owner: AnyObject, prefix: String) {
        for child in Mirror(reflecting: owner).children {
            guard let name = child.label else { continue }
            var value: Any = child.value
            let m = Mirror(reflecting: value)
            if m.displayStyle == .optional { guard let inner = m.children.first?.value else { continue }; value = inner }
            guard let obj = value as? NSObject, obj is NSView || obj is NSMenuItem else { continue }
            if obj is NSWindow { continue }
            let id = prefix + "." + name.replacingOccurrences(of: "$__lazy_storage_$_", with: "")
            if let known = ids[ObjectIdentifier(obj)], known != id { continue }        // held under another name already
            // a property may be given a new object (loadView replaces the placeholder tables): the id follows it
            if let i = objects.firstIndex(where: { $0.id == id }) {
                if objects[i].obj === obj { continue }
                ids[ObjectIdentifier(objects[i].obj)] = nil
                objects[i] = (id, obj)
            } else {
                objects.append((id, obj))
            }
            ids[ObjectIdentifier(obj)] = id
        }
    }

    private func registerMenu(_ menu: NSMenu, path: String) {
        for (i, item) in menu.items.enumerated() where !item.isSeparatorItem {
            // a menu bar item made with NSMenuItem() has no title of its own ("NSMenuItem" on macOS 26):
            // its menu's title, "App" for the app menu (no title either)
            let bare = item.title.isEmpty || item.title == "NSMenuItem"
            let name = !bare ? item.title : item.submenu?.title.isEmpty == false ? item.submenu!.title : path == "menu" && i == 0 ? "App" : "\(i)"
            var id = path + "." + name.replacingOccurrences(of: ".", with: "_")
            // macOS adds items of its own (Edit's AutoFill, Dictation, Emoji), some twice
            var n = 2
            while objects.contains(where: { $0.id == id }) && ids[ObjectIdentifier(item)] == nil { id = path + "." + name.replacingOccurrences(of: ".", with: "_") + "#\(n)"; n += 1 }
            if ids[ObjectIdentifier(item)] == nil {
                ids[ObjectIdentifier(item)] = id
                objects.append((id, item))
            }
            if let sub = item.submenu { registerMenu(sub, path: id) }
        }
    }

    /// The field table, for the scenario list (every view and menu item has an id).
    func fieldTable() -> String {
        buildTable()
        return objects.map { "\($0.id)\t\(type(of: $0.obj))\t\(oracle($0.id) ? "oracle" : "excluded: clock times")" }.joined(separator: "\n") + "\n"
    }
    private func oracle(_ id: String) -> Bool { id != "c.text" }

    // MARK: snapshots

    /// What every registered control shows now, one key per property.
    func snapshot() -> [String: String] {
        buildTable()
        var s: [String: String] = [:]
        let app = NSApplication.shared
        s["app.active"] = app.isActive ? "1" : "0"
        s["app.modal"] = app.modalWindow.map(describeDialog) ?? ""
        if let c = controller {
            let w = c.window
            s["win.title"] = w.title
            s["win.vis"] = w.isVisible ? "1" : "0"
            s["win.mini"] = w.isMiniaturized ? "1" : "0"
            s["win.key"] = w.isKeyWindow ? "1" : "0"
            s["win.sheet"] = w.attachedSheet.map(describeDialog) ?? ""
            s["tab.sel"] = "\(c.tabs?.selectedTabViewItemIndex ?? -1)"
        }
        // menus as they validate now (a modal dialog may disable them)
        if mode == .trace, let m = app.mainMenu { updateMenus(m) }
        for (id, obj) in objects where oracle(id) { props(id, obj, into: &s) }
        return s
    }

    /// One key's value now (perf mode's waits: no whole snapshot).
    func value(_ key: String) -> String {
        if key.hasPrefix("app.") || key.hasPrefix("win.") || key.hasPrefix("tab.") {
            let app = NSApplication.shared
            switch key {
            case "app.active": return app.isActive ? "1" : "0"
            case "app.modal": return app.modalWindow.map(describeDialog) ?? ""
            default:
                guard let c = controller else { return "" }
                switch key {
                case "win.vis": return c.window.isVisible ? "1" : "0"
                case "win.mini": return c.window.isMiniaturized ? "1" : "0"
                case "win.key": return c.window.isKeyWindow ? "1" : "0"
                case "win.title": return c.window.title
                case "win.sheet": return c.window.attachedSheet.map(describeDialog) ?? ""
                case "tab.sel": return "\(c.tabs?.selectedTabViewItemIndex ?? -1)"
                default: return ""
                }
            }
        }
        guard let o = objects.first(where: { key.hasPrefix($0.id + ".") }) else { return "" }
        var s: [String: String] = [:]
        props(o.id, o.obj, into: &s)
        return s[key] ?? ""
    }

    private func updateMenus(_ m: NSMenu) { m.update(); for i in m.items { if let s = i.submenu { updateMenus(s) } } }

    private func props(_ id: String, _ obj: AnyObject, into s: inout [String: String]) {
        if let item = obj as? NSMenuItem {
            s[id + ".title"] = item.title
            s[id + ".en"] = item.isEnabled ? "1" : "0"
            s[id + ".hid"] = item.isHidden ? "1" : "0"
            return
        }
        guard let v = obj as? NSView else { return }
        s[id + ".hid"] = v.isHidden ? "1" : "0"
        if let tip = v.toolTip, !(v is NSTableView) { s[id + ".tip"] = tip }
        switch v {
        case let o as NSOutlineView:
            rows(o, id, &s)
            for r in 0..<min(o.numberOfRows, 500) {
                let item = o.item(atRow: r)
                let cell = o.delegate?.outlineView?(o, viewFor: o.outlineTableColumn, item: item as Any) as? NSTableCellView
                let open = o.isExpandable(item) ? (o.isItemExpanded(item) ? "v " : "> ") : "  "
                s[id + ".r\(r)"] = String(repeating: "  ", count: o.level(forRow: r)) + open + (cell?.textField?.stringValue ?? "")
            }
        case let t as NSTableView:
            rows(t, id, &s)
            for r in 0..<min(t.numberOfRows, 500) {
                s[id + ".r\(r)"] = t.tableColumns.map { col in
                    (t.delegate?.tableView?(t, viewFor: col, row: r) as? NSTableCellView)?.textField?.stringValue ?? ""
                }.joined(separator: " | ")
            }
        case let i as NSImageView:
            s[id + ".img"] = i.image.map { $0.accessibilityDescription ?? "image \(Int($0.size.width))x\(Int($0.size.height))" } ?? ""
            s[id + ".tint"] = i.contentTintColor.map(rgb) ?? ""
        case let d as NSDatePicker:
            s[id + ".days"] = "\(Int((d.dateValue.timeIntervalSinceNow / 86400).rounded()))"
            s[id + ".en"] = d.isEnabled ? "1" : "0"
        case let b as NSButton:
            s[id + ".title"] = b.title
            s[id + ".en"] = b.isEnabled ? "1" : "0"
        case let f as NSSecureTextField:
            s[id + ".str"] = f.stringValue.isEmpty ? "" : "<secret>"
        case let f as NSTextField:
            s[id + ".str"] = f.attributedStringValue.string
            if f.isEditable { s[id + ".en"] = f.isEnabled ? "1" : "0" }
        case let p as NSProgressIndicator:
            if p.style == .spinning { s[id + ".anim"] = animating.contains(ObjectIdentifier(p)) ? "1" : "0" }
            else { s[id + ".val"] = String(format: "%.3f", p.doubleValue) }
        default: break
        }
    }

    private func rows(_ t: NSTableView, _ id: String, _ s: inout [String: String]) {
        s[id + ".rows"] = "\(t.numberOfRows)"
        s[id + ".sel"] = "\(t.selectedRow)"
        s[id + ".en"] = t.isEnabled ? "1" : "0"
    }

    private func rgb(_ c: NSColor) -> String {
        guard let s = c.usingColorSpace(.sRGB) else { return "?" }
        return String(format: "#%02x%02x%02x", Int((s.redComponent * 255).rounded()), Int((s.greenComponent * 255).rounded()), Int((s.blueComponent * 255).rounded()))
    }

    /// A dialog as the user sees it: its texts in order, its fields (a password never), its buttons.
    func describeDialog(_ w: NSWindow) -> String {
        var texts: [String] = [], buttons: [String] = []
        if let v = w.contentView {
            walk(v) { x in
                switch x {
                case let f as NSSecureTextField: texts.append(f.stringValue.isEmpty ? "[password]" : "[password: typed]")
                case let f as NSTextField: texts.append(f.isEditable ? "[\(f.stringValue)]" : f.stringValue)
                case let b as NSButton: buttons.append(b.title + (b.isEnabled ? "" : " (off)") + (b.keyEquivalent == "\r" ? "*" : ""))
                default: break
                }
            }
        }
        return texts.joined(separator: " / ") + " {" + buttons.joined(separator: ", ") + "}"
    }

    // MARK: the run loop

    private func installObservers() {
        let main = CFRunLoopGetMain()
        let after = CFRunLoopObserverCreateWithHandler(nil, CFRunLoopActivity.afterWaiting.rawValue, true, -2_000_000_000) { _, _ in
            Probe.shared.wake = mach_absolute_time()
        }
        let before = CFRunLoopObserverCreateWithHandler(nil, CFRunLoopActivity.beforeWaiting.rawValue, true, 2_100_000_000) { _, _ in
            Probe.shared.beforeWaiting()
        }
        CFRunLoopAddObserver(main, after, .commonModes)
        CFRunLoopAddObserver(main, before, .commonModes)
        let nc = NotificationCenter.default
        nc.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: nil) { _ in Probe.shared.event("didBecomeActive") }
        nc.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: nil) { _ in Probe.shared.event("didResignActive") }
        nc.addObserver(forName: NSWindow.didMiniaturizeNotification, object: nil, queue: nil) { _ in Probe.shared.event("didMiniaturize") }
        nc.addObserver(forName: NSWindow.didDeminiaturizeNotification, object: nil, queue: nil) { _ in Probe.shared.event("didDeminiaturize") }
        nc.addObserver(forName: NSApplication.willTerminateNotification, object: nil, queue: nil) { _ in Probe.shared.event("willTerminate"); Probe.shared.flushPerf() }
    }

    private func beforeWaiting() {
        let start = mach_absolute_time()
        turn += 1
        let t = now()
        let busy = wake == 0 ? 0 : Int64((start - wake) * UInt64(timebase.numer) / UInt64(timebase.denom))
        if !windowDrawn, let c = controller ?? (NSApp?.delegate as? AppDelegate)?.controller, c.window.isVisible {
            windowDrawn = true
            controller = c
            event("windowDrawn")
        }
        switch mode {
        // nothing is on screen before the window first shows (Controller.init's refresh runs
        // subprocesses whose waits turn the run loop while the window is still hidden)
        case .trace where windowDrawn || NSApp?.modalWindow != nil:
            let s = snapshot()
            var delta: [String: String?] = [:]
            for (k, v) in s where last[k] != v { delta[k] = v }
            for k in last.keys where s[k] == nil { delta[k] = .some(nil) }
            if !delta.isEmpty {
                last = s
                lastChange = t
                let body = delta.keys.sorted().map { k in "\(js(k)):\(delta[k]!.map(js) ?? "null")" }.joined(separator: ",")
                emit("{\"k\":\"frame\",\"t\":\(t),\"n\":\(turn),\"busy\":\(busy),\"d\":{\(body)}}")
            }
        case .perf, .perfMin:
            perfAdd(0, "turn", busy, at: t)
        case .off: return
        case .trace: break
        }
        let mid = mach_absolute_time()
        if mode != .perfMin { busyNs += (mid - start) * UInt64(timebase.numer) / UInt64(timebase.denom) }
        // the driver's waits: the harness's cost, the same in every mode, counted apart
        checkWaits()
        waitNs += (mach_absolute_time() - mid) * UInt64(timebase.numer) / UInt64(timebase.denom)
    }

    private func checkWaits() {
        guard !waits.isEmpty else { return }
        let t = now()
        waits = waits.filter { w in
            if w.test() { ack(w.id, "ok"); return false }
            if t > w.deadline { ack(w.id, "timeout"); return false }
            return true
        }
        // time-based conditions (quiet, deadlines) need turns: a 20 ms timer while waits are
        // pending (never a wake-up per turn, which would spin the main thread)
        if waits.isEmpty, let tm = waitTimer { CFRunLoopTimerInvalidate(tm); waitTimer = nil }
    }

    private var waitTimer: CFRunLoopTimer?
    private func armWaitTimer() {
        guard waitTimer == nil else { return }
        let tm = CFRunLoopTimerCreateWithHandler(nil, CFAbsoluteTimeGetCurrent() + 0.02, 0.02, 0, 0) { _ in }
        CFRunLoopAddTimer(CFRunLoopGetMain(), tm, .commonModes)
        waitTimer = tm
    }

    // MARK: setters (swizzled)

    /// A setter on a registered object: the setter level in trace mode, a field event in perf mode
    /// for the short list (the lights, the status line, the spinner, the sheet, the rows).
    fileprivate func setter(_ obj: AnyObject, _ sel: String, _ value: @autoclosure () -> String, n: Int64 = 0) {
        guard mode != .off, Thread.isMainThread else { return }
        let start = mach_absolute_time()
        defer { busyNs += (mach_absolute_time() - start) * UInt64(timebase.numer) / UInt64(timebase.denom) }
        // an object not known yet may be a property's new value: the ids again, at most once a turn
        // (only tables are replaced so; a table cell's label is never a field)
        if ids[ObjectIdentifier(obj)] == nil, !tableBuilt || (obj is NSTableView && lastBuildTurn != turn) { lastBuildTurn = turn; buildTable() }
        guard var id = ids[ObjectIdentifier(obj)] else { return }
        if let st = spikeTable {                    // the G1 spike: its table is the tracks table
            if ObjectIdentifier(obj) == st { id = "prepare.tracksTable" } else if id == "prepare.tracksTable" { return }
        }
        if mode != .trace { lastChange = now() }    // "quiet" in perf mode: no field set for a while (trace: no frame changed)
        if mode == .trace { emit("{\"k\":\"set\",\"t\":\(now()),\"f\":\(js(id)),\"s\":\"\(sel)\",\"v\":\(js(value()))}") }
        else if Probe.perfFields.contains(id) { perfAdd(1, id + ":" + sel, n) }
    }

    static let perfFields: Set<String> = ["c.plusIcon", "c.cacheIcon", "c.status", "c.spinner", "prepare.tracksTable", "prepare.info",
                                          "prepare.prepareBtn", "prepare.bar", "status.table", "prepare.outline"]

    fileprivate func spinner(_ p: NSProgressIndicator, _ on: Bool) {
        if on { animating.insert(ObjectIdentifier(p)) } else { animating.remove(ObjectIdentifier(p)) }
        setter(p, on ? "startAnimation" : "stopAnimation", on ? "1" : "0")
    }

    fileprivate func dialog(_ w: NSWindow, begin: Bool, origin: String = "") {
        guard mode != .off else { return }
        if begin { dialogs += 1 }
        let d = describeDialog(w)
        count(begin ? "dialog" : "dialogEnd")
        switch mode {
        case .trace: emit("{\"k\":\"ev\",\"t\":\(now()),\"e\":\"\(begin ? "dialog" : "dialogEnd")\",\"n\":\(dialogs),\"v\":\(js(d)),\"origin\":\(js(origin))}")
        case .perf, .perfMin: perfAdd(6, begin ? "dialog" : "dialogEnd", Int64(dialogs))
        case .off: break
        }
    }

    /// Where a modal dialog came from: whether the main queue is blocked under it (it was started
    /// inside a main-queue block, so DispatchQueue.main.async work waits for it), and whether the
    /// work queue is (an alert asked from the work queue: onMainSync). The work queue's answer
    /// comes as a later event: a marker block that runs at once if the queue is free.
    fileprivate func dialogOrigin() -> String {
        let stack = Thread.callStackSymbols.joined(separator: "\n")
        let mainQ = stack.contains("_dispatch_main_queue_callback_4CF") || stack.contains("_dispatch_main_queue_drain")
        let n = dialogs + 1
        if mode == .trace, let c = controller {
            let asked = now()
            c.work.async { Probe.shared.emit("{\"k\":\"ev\",\"t\":\(Probe.shared.now()),\"e\":\"workQueueFree\",\"n\":\(n),\"v\":\"\(Probe.shared.now() - asked)\"}") }
        }
        return mainQ ? "main-queue-block" : "event"
    }

    fileprivate func reopen() { event("reopen") }
    fileprivate func rowFetch(_ ns: UInt64) { if mode == .perf || mode == .perfMin { perfAdd(5, "prepare.tracksTable", Int64(ns)) } }

    // MARK: commands (tests/harness/lib.sh): one line per command on <out>/cmd, acked in <out>/acks

    private func startCommands() {
        let fifo = outDir + "/cmd"
        if fileType(fifo) != S_IFIFO { unlink(fifo); mkfifo(fifo, 0o600) }
        Thread.detachNewThread {
            while true {
                guard let f = fopen(fifo, "r") else { sleep(1); continue }
                var line: UnsafeMutablePointer<CChar>? = nil, cap = 0
                while getline(&line, &cap, f) > 0 {
                    let text = String(cString: line!).trimmingCharacters(in: .newlines)
                    // run-loop blocks run in every common mode, also under a modal dialog and inside
                    // a main-queue block's nested loop, where DispatchQueue.main.async would wait
                    CFRunLoopPerformBlock(CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue) { Probe.shared.command(text) }
                    CFRunLoopWakeUp(CFRunLoopGetMain())
                }
                free(line)
                fclose(f)
            }
        }
    }

    private func ack(_ id: String, _ result: String) {
        let line = "\(id) \(result)\n"
        writeQ.async {
            if let h = FileHandle(forWritingAtPath: self.outDir + "/acks") { h.seekToEndOfFile(); h.write(Data(line.utf8)); h.closeFile() }
            else { FileManager.default.createFile(atPath: self.outDir + "/acks", contents: Data(line.utf8)) }
        }
    }

    /// "<id> <verb> <args…>"
    private func command(_ text: String) {
        var parts = text.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: true).map(String.init)
        guard parts.count >= 2 else { return }
        let id = parts.removeFirst(), verb = parts.removeFirst()
        let arg = parts.first ?? ""
        buildTable()
        if mode == .trace { emit("{\"k\":\"ev\",\"t\":\(now()),\"e\":\"cmd\",\"v\":\(js(verb + " " + arg))}") }
        else { perfAdd(4, verb + " " + arg) }
        count("cmd")
        #if CORE_LINKED
        if let start = spikeStart {
            if verb == "mark" && arg == "settled" { spikeStart = nil; start() }
            else if verb == "select" || verb == "scroll" { return ack(id, "err spike: not started (mark settled first)") }
        }
        #endif
        if let take = spikeCommand, take(verb, arg, { [weak self] r in self?.ack(id, r) }) { return }
        let c = controller
        switch verb {
        case "mark": ack(id, "ok")
        case "click":
            guard let b = object(arg) as? NSButton else { return ack(id, "err no button \(arg)") }
            guard b.isEnabled, !b.isHidden else { return ack(id, "err disabled \(arg)") }
            ack(id, "ok"); b.performClick(nil)
        case "menu":
            guard let item = object(arg) as? NSMenuItem, let menu = item.menu else { return ack(id, "err no menu item \(arg)") }
            menu.update()
            guard item.isEnabled, !item.isHidden else { return ack(id, "err disabled \(arg)") }
            ack(id, "ok"); menu.performActionForItem(at: menu.index(of: item))
        case "answer":
            // the modal dialog's button with this title
            guard let w = NSApp.modalWindow, let b = buttons(w).first(where: { $0.title == arg }) else { return ack(id, "err no dialog button \(arg)") }
            guard b.isEnabled else { return ack(id, "err disabled \(arg)") }
            ack(id, "ok"); b.performClick(nil)
        case "fill":
            // "fill <n> <text>": the modal dialog's n-th editable field (never a password field: "password" types one)
            let a = arg.split(separator: " ", maxSplits: 1).map(String.init)
            guard let w = NSApp.modalWindow, let n = Int(a.first ?? "") else { return ack(id, "err no dialog") }
            let fields = editable(w).filter { !($0 is NSSecureTextField) }
            guard n < fields.count else { return ack(id, "err no field \(n)") }
            fields[n].stringValue = a.count > 1 ? a[1] : ""
            ack(id, "ok")
        case "password":
            guard let w = NSApp.modalWindow, let f = editable(w).first(where: { $0 is NSSecureTextField }) else { return ack(id, "err no password field") }
            f.stringValue = arg
            ack(id, "ok")
        case "tab":
            // as a click on the toolbar's tab item
            guard let w = c?.window, let item = w.toolbar?.items.first(where: { $0.label == arg }) else { return ack(id, "err no tab \(arg)") }
            ack(id, "ok")
            if let action = item.action, NSApp.sendAction(action, to: item.target, from: item) { break }
            if let i = c?.tabs.tabViewItems.firstIndex(where: { $0.label == arg }) { c?.tabs.selectedTabViewItemIndex = i }
        case "select":
            // "select <playlist id>": the outline's row, as a click on it (the row whose C_OUT_ID it is)
            guard let o = probeShell?.prepareTab.outline else { return ack(id, "err") }
            guard let row = (0..<o.numberOfRows).first(where: { outlineID(o, $0) == arg }) else { return ack(id, "err no row \(arg)") }
            ack(id, "ok"); o.selectRowIndexes([row], byExtendingSelection: false)
        case "collapse", "expand":
            guard let o = probeShell?.prepareTab.outline, let row = (0..<o.numberOfRows).first(where: { outlineID(o, $0) == arg }) else { return ack(id, "err no row \(arg)") }
            ack(id, "ok")
            if verb == "collapse" { o.collapseItem(o.item(atRow: row)) } else { o.expandItem(o.item(atRow: row)) }
        case "select-set":
            // the Status tab's n-th row (-1: none)
            guard let t = probeShell?.statusTab.table, let n = Int(arg) else { return ack(id, "err") }
            ack(id, "ok")
            if n < 0 { t.deselectAll(nil) } else { t.selectRowIndexes([n], byExtendingSelection: false) }
        case "doubleclick-set":
            // the row selected, then its double click (the table's doubleAction: Prepare Again)
            guard let st = probeShell?.statusTab, let n = Int(arg) else { return ack(id, "err") }
            st.table.selectRowIndexes([n], byExtendingSelection: false)
            ack(id, "ok"); _ = NSApp.sendAction(st.table.doubleAction!, to: st.table.target, from: st.table)
        case "until":
            // the date picker set as the user does, its action sent (the core holds the date)
            guard let d = probeShell?.prepareTab.until, let days = Double(arg) else { return ack(id, "err") }
            d.dateValue = Date().addingTimeInterval(days * 86400); ack(id, "ok")
            if let a = d.action { _ = NSApp.sendAction(a, to: d.target, from: d) }
        case "miniaturize": ack(id, "ok"); c?.window.miniaturize(nil)
        case "deminiaturize": ack(id, "ok"); c?.window.deminiaturize(nil)
        case "close": ack(id, "ok"); c?.window.performClose(nil)
        case "scroll":
            // "scroll <row>": the tracks table to that row (P8)
            guard let t = probeShell?.prepareTab.tracksTable, let r = Int(arg) else { return ack(id, "err") }
            t.scrollRowToVisible(min(r, max(0, t.numberOfRows - 1))); ack(id, "ok")
        case "fields":
            FileManager.default.createFile(atPath: outDir + "/fields.tsv", contents: Data(fieldTable().utf8)); ack(id, "ok")
        case "flush": flushPerf(); writeQ.async { self.ack(id, "ok") }
        case "quit": ack(id, "ok"); NSApp.terminate(nil)
        case "wait":
            // "wait <ms> <key>=<value>" (or <key>!=<value>): a frame where the field has that value
            let a = arg.split(separator: " ", maxSplits: 1).map(String.init)
            guard a.count == 2, let ms = UInt64(a[0]) else { return ack(id, "err usage") }
            let cond = a[1], neg = cond.contains("!=")
            let kv = cond.components(separatedBy: neg ? "!=" : "=")
            let key = kv[0], want = kv.dropFirst().joined(separator: neg ? "!=" : "=")
            addWait(id, ms) {
                let v = self.mode == .trace ? self.last[key] ?? "" : self.value(key)
                return neg ? v != want : v == want
            }
        case "quiet":
            // "quiet <ms> <timeout ms>": no field changed for that long (trace mode)
            let a = arg.split(separator: " ").compactMap { UInt64($0) }
            guard a.count == 2 else { return ack(id, "err usage") }
            let need = a[0] * 1_000_000
            lastChange = now()
            addWait(id, a[1]) { self.now() - self.lastChange >= need }
        case "event":
            // "event <ms> <name> <count>": the event (or timing point) has happened that many times
            let a = arg.split(separator: " ").map(String.init)
            guard a.count == 3, let ms = UInt64(a[0]), let n = Int(a[2]) else { return ack(id, "err usage") }
            addWait(id, ms) { (self.eventCounts[a[1]] ?? 0) >= n }
        case "count":
            ack(id, "ok \(eventCounts[arg] ?? 0)")
        default: ack(id, "err unknown \(verb)")
        }
    }

    private func addWait(_ id: String, _ ms: UInt64, _ test: @escaping () -> Bool) {
        if test() { ack(id, "ok"); return }
        waits.append((id, now() + ms * 1_000_000, test))
        armWaitTimer()
    }

    private func object(_ id: String) -> AnyObject? { objects.first { $0.id == id }?.obj }
    private func buttons(_ w: NSWindow) -> [NSButton] { var b: [NSButton] = []; if let v = w.contentView { walk(v) { if let x = $0 as? NSButton { b.append(x) } } }; return b }
    private func editable(_ w: NSWindow) -> [NSTextField] { var b: [NSTextField] = []; if let v = w.contentView { walk(v) { if let x = $0 as? NSTextField, x.isEditable { b.append(x) } } }; return b }

    // MARK: hooks

    private func installHooks() {
        let trace = mode == .trace
        typealias VB = @convention(c) (AnyObject, Selector, Bool) -> Void
        typealias VO = @convention(c) (AnyObject, Selector, AnyObject?) -> Void
        typealias VD = @convention(c) (AnyObject, Selector, Double) -> Void
        typealias V0 = @convention(c) (AnyObject, Selector) -> Void
        typealias VOO = @convention(c) (AnyObject, Selector, AnyObject?, AnyObject?) -> Void

        func hookBool(_ classes: [AnyClass], _ sel: Selector, _ name: String) {
            for cls in classes {
                hook(cls, sel) { (orig: IMP) -> Any in
                    let f = unsafeBitCast(orig, to: VB.self)
                    let b: @convention(block) (AnyObject, Bool) -> Void = { o, v in
                        let outer = Probe.shared.hookDepth == 0
                        Probe.shared.hookDepth += 1; f(o, sel, v); Probe.shared.hookDepth -= 1
                        if outer { Probe.shared.setter(o, name, v ? "1" : "0") }
                    }
                    return b
                }
            }
        }
        func hookObj(_ classes: [AnyClass], _ sel: Selector, _ name: String, _ show: @escaping (AnyObject?) -> String) {
            for cls in classes {
                hook(cls, sel) { (orig: IMP) -> Any in
                    let f = unsafeBitCast(orig, to: VO.self)
                    let b: @convention(block) (AnyObject, AnyObject?) -> Void = { o, v in
                        let outer = Probe.shared.hookDepth == 0
                        Probe.shared.hookDepth += 1; f(o, sel, v); Probe.shared.hookDepth -= 1
                        if outer { Probe.shared.setter(o, name, show(v)) }
                    }
                    return b
                }
            }
        }
        let str: (AnyObject?) -> String = { v in (v as? String) ?? (v as? NSAttributedString)?.string ?? "" }
        if trace {
            hookBool([NSControl.self, NSButton.self, NSTextField.self], #selector(setter: NSControl.isEnabled), "setEnabled")
            hookBool([NSView.self], #selector(setter: NSView.isHidden), "setHidden")
            hookObj([NSView.self], #selector(setter: NSView.toolTip), "setToolTip", str)
            hookObj([NSButton.self], #selector(setter: NSButton.title), "setTitle", str)
            hookObj([NSImageView.self], #selector(setter: NSImageView.contentTintColor), "setContentTintColor") { v in (v as? NSColor).map { Probe.shared.rgb($0) } ?? "" }
        } else {
            hookBool([NSControl.self, NSButton.self], #selector(setter: NSControl.isEnabled), "setEnabled")
        }
        hookObj([NSControl.self, NSTextField.self], #selector(setter: NSControl.stringValue), "setStringValue", str)
        hookObj([NSControl.self, NSTextField.self], #selector(setter: NSControl.attributedStringValue), "setAttributedStringValue", str)
        let imsel = #selector(setter: NSImageView.image)
        hook(NSImageView.self, imsel) { (orig: IMP) -> Any in
            let f = unsafeBitCast(orig, to: VO.self)
            let b: @convention(block) (NSImageView, AnyObject?) -> Void = { o, v in
                let before = o.image?.accessibilityDescription
                f(o, imsel, v)
                let now = (v as? NSImage)?.accessibilityDescription
                // perf mode: a set that keeps the look is no change on screen (P2: the first frame
                // with the final lights is the last look change); trace mode: every set (setter level)
                if Probe.shared.mode == .trace || before != now || o.image == nil { Probe.shared.setter(o, "setImage", now ?? "") }
            }
            return b
        }
        for (sel, on) in [(#selector(NSProgressIndicator.startAnimation(_:)), true), (#selector(NSProgressIndicator.stopAnimation(_:)), false)] {
            hook(NSProgressIndicator.self, sel) { (orig: IMP) -> Any in
                let f = unsafeBitCast(orig, to: VO.self)
                let b: @convention(block) (NSProgressIndicator, AnyObject?) -> Void = { o, v in f(o, sel, v); Probe.shared.spinner(o, on) }
                return b
            }
        }
        let dsel = #selector(setter: NSProgressIndicator.doubleValue)
        hook(NSProgressIndicator.self, dsel) { (orig: IMP) -> Any in
            let f = unsafeBitCast(orig, to: VD.self)
            let b: @convention(block) (AnyObject, Double) -> Void = { o, v in f(o, dsel, v); Probe.shared.setter(o, "setDoubleValue", String(format: "%.3f", v)) }
            return b
        }
        let rsel = #selector(NSTableView.reloadData as (NSTableView) -> () -> Void)
        hook(NSTableView.self, rsel) { (orig: IMP) -> Any in
            let f = unsafeBitCast(orig, to: V0.self)
            let b: @convention(block) (NSTableView) -> Void = { o in f(o, rsel); Probe.shared.setter(o, "reloadData", "\(o.numberOfRows)", n: Int64(o.numberOfRows)) }
            return b
        }
        let isel = #selector(NSTableView.reloadData(forRowIndexes:columnIndexes:))
        hook(NSTableView.self, isel) { (orig: IMP) -> Any in
            let f = unsafeBitCast(orig, to: VOO.self)
            let b: @convention(block) (NSTableView, AnyObject?, AnyObject?) -> Void = { o, r, c in
                f(o, isel, r, c)
                let first = (r as? IndexSet)?.first ?? -1
                Probe.shared.setter(o, "reloadDataForRowIndexes", "\(first)", n: Int64(first))
            }
            return b
        }
        if mode == .perf || mode == .perfMin {
            // P8: the time of each row fetch by the tracks table
            let vsel = #selector(ShellPrepareTab.tableView(_:viewFor:row:))
            typealias VF = @convention(c) (AnyObject, Selector, AnyObject, AnyObject?, Int) -> AnyObject?
            hook(ShellPrepareTab.self, vsel) { (orig: IMP) -> Any in
                let f = unsafeBitCast(orig, to: VF.self)
                let b: @convention(block) (AnyObject, AnyObject, AnyObject?, Int) -> AnyObject? = { o, t, col, row in
                    let s = mach_absolute_time()
                    let r = f(o, vsel, t, col, row)
                    let p = Probe.shared
                    p.rowFetch((mach_absolute_time() - s) * UInt64(p.timebase.numer) / UInt64(p.timebase.denom))
                    return r
                }
                return b
            }
        }
        hookModal()
        hookReopen()
        hookTabs()
    }

    /// P5: "tabShown" once the tab view controller has put the selected tab's view in (MainTabs'
    /// own didSelect runs first: the title and the size).
    private func hookTabs() {
        typealias VO2 = @convention(c) (AnyObject, Selector, AnyObject, AnyObject?) -> Void
        let sel = #selector(NSTabViewController.tabView(_:didSelect:))
        hook(MainTabs.self, sel) { (orig: IMP) -> Any in
            let f = unsafeBitCast(orig, to: VO2.self)
            let b: @convention(block) (AnyObject, AnyObject, AnyObject?) -> Void = { o, tv, item in
                f(o, sel, tv, item)
                Probe.shared.event("tabShown", (item as? NSTabViewItem)?.label ?? "")
            }
            return b
        }
    }

    private func hookModal() {
        typealias VR = @convention(c) (AnyObject, Selector, AnyObject) -> Int
        let msel = #selector(NSApplication.runModal(for:))
        hook(NSApplication.self, msel) { (orig: IMP) -> Any in
            let f = unsafeBitCast(orig, to: VR.self)
            let b: @convention(block) (AnyObject, NSWindow) -> Int = { app, w in
                let p = Probe.shared
                p.dialog(w, begin: true, origin: p.mode == .trace ? p.dialogOrigin() : "")
                let r = f(app, msel, w)
                p.dialog(w, begin: false)
                return r
            }
            return b
        }
    }

    private func hookReopen() {
        typealias VR = @convention(c) (AnyObject, Selector, AnyObject, Bool) -> Bool
        let sel = #selector(NSApplicationDelegate.applicationShouldHandleReopen(_:hasVisibleWindows:))
        hook(AppDelegate.self, sel) { (orig: IMP) -> Any in
            let f = unsafeBitCast(orig, to: VR.self)
            let b: @convention(block) (AnyObject, AnyObject, Bool) -> Bool = { d, app, v in Probe.shared.reopen(); return f(d, sel, app, v) }
            return b
        }
    }

    /// Replaces `sel` on `cls` (added there when inherited, so the superclass keeps its own).
    private func hook(_ cls: AnyClass, _ sel: Selector, _ make: (IMP) -> Any) {
        guard let m = class_getInstanceMethod(cls, sel) else { return }
        // a subclass that only inherits the method gets no hook of its own: the superclass's records it
        if let sup = class_getSuperclass(cls), let sm = class_getInstanceMethod(sup, sel),
           method_getImplementation(sm) == method_getImplementation(m) { return }
        let imp = imp_implementationWithBlock(make(method_getImplementation(m)))
        if !class_addMethod(cls, sel, imp, method_getTypeEncoding(m)) { method_setImplementation(m, imp) }
    }
}

private func timebase_ns(_ t: UInt64) -> UInt64 {
    var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
    return t * UInt64(tb.numer) / UInt64(tb.denom)
}

/// A JSON string.
private func js(_ s: String) -> String {
    var o = "\""
    for u in s.unicodeScalars {
        switch u {
        case "\"": o += "\\\""
        case "\\": o += "\\\\"
        case "\n": o += "\\n"
        case "\r": o += "\\r"
        case "\t": o += "\\t"
        default: if u.value < 0x20 { o += String(format: "\\u%04x", u.value) } else { o.unicodeScalars.append(u) }
        }
    }
    return o + "\""
}
#endif
