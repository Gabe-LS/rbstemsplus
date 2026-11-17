// The G1 API spike's skeleton table (the private Windows plan's mac-core §9 G1.7, M7): an AppKit
// NSTableView over the core's snapshot and row accessors, fed by the helper-line path the API
// review asked for (API-V1-NOTES open point 12): a real reader thread per helper run calling
// core_helper_line for every line, the stub parse (core_test_helper_stub) turning each into one
// typed main job, the real commit scheduler (one render per run-loop turn), and this shell holding
// the previous snapshot until the next render is applied. TEST BUILDS WITH THE CORE ONLY
// (scripts/build-app.sh compiles app/Spike only then; the whole file is inside the #if too).
//
// It runs inside the full app, beside its window, only when the harness's probe.conf says
// spike=1 (tests/harness/scenarios/perf-spike*.sh), so P10 measures the app as 1.4.0 will be: the
// app, the core and the rows. The probe records its table as "prepare.tracksTable"
// (probeSpike), so tests/perf/stats.py computes P6 and P7 from the same events as on mac-A:
//   select ID   the playlist's tracks (the fake helper's --tracks ID: one row appended per line),
//               then its estimate (--estimate --playlist ID: one row's result per line), as
//               PrepareTab.check and runEstimate run them; first an outline {"ev":"select"},
//               which ends an estimate still running (the stub drops its lines and stops it)
//   scroll ROW  the table to that row
// The parse is the core's stub (core_test_helper_stub), not M5's parser: P15 times that apart,
// and the stub calls M5's parser or goes once G1.7 is recorded. Never a release path.
#if TEST_BUILD && CORE_LINKED
import AppKit
import Darwin

/// main.swift, once the app is set up: the spike's window and core, when the harness asks, at
/// the driver's first `mark settled` (probeSpikeStartOnSettled): after the launch and the
/// activation, which then have only the app's own window, as 1.4.0 has. P10.idle and the select
/// commands (P6, P7) come after the mark, so they measure the app with the spike.
func coreSpikeStartIfAsked() {
    guard Probe.shared.conf["spike"] == "1" else { return }
    probeSpikeStartOnSettled { CoreSpike.shared.start() }
}

private let spikeFlowQ = DispatchQueue(label: "rbsp.spike.flow")     // CORE_WORK_FLOW: one serial queue

private func spikeLog(_ s: String) { FileHandle.standardError.write(Data(("core spike: " + s + "\n").utf8)) }

final class CoreSpike: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    static let shared = CoreSpike()
    private(set) var core: OpaquePointer?
    private(set) var held: OpaquePointer?                 // the snapshot the table shows (the last render's)
    private var window: NSWindow!
    private(set) var table: NSTableView!
    private var allColumns = IndexSet()
    private var spawnArgs: [String] = []             // what the next helper_spawn runs (main)
    private var playlist = ""
    private let lock = NSLock()
    private var procs: [UInt64: Process] = [:]       // under lock: the runs' processes

    func start() {
        guard core == nil else { return }
        guard core_api_version() == CORE_API_VERSION, core_ids_hash() == coreIdsHash else {
            spikeLog("the core's API version or ids hash isn't this build's: not started"); return
        }
        makeWindow()
        var h = core_host()
        h.size = UInt32(MemoryLayout<core_host>.size)
        h.render = { _, snap, delta in CoreSpike.shared.apply(snap!, delta!) }
        h.front_facts = { _ in
            var bits = UInt32(0)
            if NSApp.isActive { bits |= UInt32(CORE_FRONT_ACTIVE) }
            if let w = CoreSpike.shared.window {
                bits |= UInt32(CORE_FRONT_WINDOW_LOADED)
                if w.isMiniaturized { bits |= UInt32(CORE_FRONT_MINIATURIZED) }
            }
            return bits | (UInt32(CORE_TAB_PREPARE.rawValue) << UInt32(CORE_FRONT_TAB_SHIFT))
        }
        h.modal_open = { _ in NSApp.modalWindow != nil || CoreSpike.shared.window?.attachedSheet != nil ? 1 : 0 }
        h.facts = { _, _ in }
        h.post_main = { _, job in DispatchQueue.main.async { core_run(job) } }
        h.post_work = { _, kind, job in
            switch kind {
            case CORE_WORK_FLOW: spikeFlowQ.async { core_run(job) }
            case CORE_WORK_WALK: DispatchQueue.global(qos: .utility).async { core_run(job) }
            case CORE_WORK_BACKGROUND: DispatchQueue.global(qos: .background).async { core_run(job) }
            default: DispatchQueue.global(qos: .userInitiated).async { core_run(job) }
            }
        }
        h.dialog = { _, _, _ in 1 }                   // the spike shows no dialog: Cancel
        h.close_reply = { _, _, _ in }
        h.log = { _, line, n in spikeLog(String(decoding: UnsafeRawBufferPointer(start: line, count: n), as: UTF8.self)) }
        h.helper_spawn = { _, run, _, _ in CoreSpike.shared.spawn(run) }
        h.helper_signal = { _, run, sig in
            let s = CoreSpike.shared
            s.lock.lock(); let p = s.procs[run]; s.lock.unlock()
            guard let p = p, p.isRunning else { return }
            switch sig {
            case CORE_HELPER_PAUSE: kill(p.processIdentifier, SIGSTOP)
            case CORE_HELPER_RESUME: kill(p.processIdentifier, SIGCONT)
            default: p.terminate()
            }
        }
        let config = "{\"os\":\"mac\",\"version\":\"\(compiledVersion)\"}"
        core = config.withCString { core_new(&h, CORE_ROLE_APP, $0, CORE_NTS) }
        guard let c = core else { spikeLog("core_new refused the host or the config"); return }
        _ = core_test_helper_stub(c, 1)
        _ = core_event(c, #"{"ev":"start"}"#, CORE_NTS)
        core_commit(c)                               // the first render: the table's empty rows
        #if !SPIKE_HOST_TEST                         // tests/mac/spike-smoke.sh: on the host, never shown
        window.makeKeyAndOrderFront(nil)
        #endif
        _ = core_event(c, #"{"ev":"shown"}"#, CORE_NTS)
        probeSpike(table: table) { [weak self] verb, arg, ack in self?.command(verb, arg, ack) ?? false }
        spikeLog("started")
    }

    private func makeWindow() {
        window = NSWindow(contentRect: NSRect(x: 160, y: 160, width: 720, height: 460), styleMask: [.titled, .resizable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = "RB Stems Plus: core spike (TEST BUILD)"
        window.isReleasedWhenClosed = false
        table = NSTableView()
        for (id, title, width) in [("name", "Track", 420.0), ("result", "Result", 240.0)] {
            let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
            col.title = title
            col.width = CGFloat(width)
            table.addTableColumn(col)
        }
        allColumns = IndexSet(integersIn: 0..<table.numberOfColumns)
        table.usesAlternatingRowBackgroundColors = true
        table.dataSource = self
        table.delegate = self
        let scroll = NSScrollView(frame: window.contentView!.bounds)
        scroll.autoresizingMask = [.width, .height]
        scroll.hasVerticalScroller = true
        scroll.documentView = table
        window.contentView!.addSubview(scroll)
    }

    // MARK: render: one pass, then the previous snapshot is released

    private func apply(_ snap: OpaquePointer, _ d: OpaquePointer) {
        let previous = held
        held = snap
        if core_delta_sections(d) & (UInt32(1) << S_PREPARE.rawValue) != 0 {
            let rows = core_rows(snap, T_PREP_TRACKS)
            if core_delta_reload(d, T_PREP_TRACKS) != 0 || core_delta_old_rows(d, T_PREP_TRACKS) != rows {
                table.reloadData()                   // a new list, or rows appended
            } else {
                var n: Int32 = 0
                if let ch = core_delta_changed(d, T_PREP_TRACKS, &n), n > 0 {
                    var set = IndexSet()
                    for i in 0..<Int(n) { set.insert(Int(ch[i])) }
                    table.reloadData(forRowIndexes: set, columnIndexes: allColumns)
                }
            }
        }
        if let p = previous { core_snap_release(p) }
    }

    func numberOfRows(in tableView: NSTableView) -> Int { held.map { Int(core_rows($0, T_PREP_TRACKS)) } ?? 0 }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard let col = tableColumn else { return nil }
        let cell: NSTableCellView
        if let v = tableView.makeView(withIdentifier: col.identifier, owner: self) as? NSTableCellView { cell = v } else {
            cell = NSTableCellView()
            cell.identifier = col.identifier
            let f = NSTextField(labelWithString: "")
            f.lineBreakMode = .byTruncatingMiddle
            f.translatesAutoresizingMaskIntoConstraints = false
            cell.addSubview(f)
            cell.textField = f
            NSLayoutConstraint.activate([f.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 2),
                                         f.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -2),
                                         f.centerYAnchor.constraint(equalTo: cell.centerYAnchor)])
        }
        var n = 0
        let column = col.identifier.rawValue == "name" ? C_TRK_NAME : C_TRK_RESULT
        if let p = core_cell_str(held, column, Int32(row), &n) {
            cell.textField?.stringValue = String(decoding: UnsafeRawBufferPointer(start: p, count: n), as: UTF8.self)
        }
        return cell
    }

    // MARK: the driver's commands (the probe's FIFO, main)

    private func command(_ verb: String, _ arg: String, _ ack: @escaping (String) -> Void) -> Bool {
        switch verb {
        case "select":
            guard let c = core, !arg.isEmpty, arg.allSatisfy({ $0.isNumber }) else { ack("err spike: select ID"); return true }
            ack("ok")
            playlist = arg
            // the choice, as the outline's click sends it: an estimate still running ends (review Q32)
            _ = core_event(c, "{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":-1,\"gen\":\(core_list_gen(held, T_PREP_OUTLINE))}", CORE_NTS)
            _ = core_test_table_reset(c, T_PREP_TRACKS, 0)          // a new list: Checking…
            spawnArgs = ["--tracks", arg]
            if core_test_helper_start(c, 1) == 0 { spikeLog("the tracks run didn't start") }
            return true
        case "scroll":
            guard let r = Int(arg) else { ack("err"); return true }
            table.scrollRowToVisible(min(r, max(0, table.numberOfRows - 1)))
            ack("ok")
            return true
        default:
            return false
        }
    }

    /// The tracks run is over (main, after its line jobs and its end job): the estimate, as
    /// PrepareTab.check starts it once the list is in.
    private func tracksDone(_ id: String) {
        guard let c = core, id == playlist else { return }
        spawnArgs = ["--estimate", "--playlist", id]
        if core_test_helper_start(c, 2) == 0 { spikeLog("the estimate run didn't start") }
    }

    // MARK: helper runs: the fake rbsp-prepare, a reader thread per run

    /// helper_spawn (main here, inside core_test_helper_start): only the pure functions may be
    /// called from it, so the reader thread does the rest.
    private func spawn(_ run: UInt64) -> Int32 {
        guard let path = Probe.shared.conf["helper"], !path.isEmpty, let c = core else { return 1 }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = spawnArgs
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch { return 1 }
        lock.lock(); procs[run] = p; lock.unlock()
        let tracks = spawnArgs.first == "--tracks" ? playlist : nil
        let t = Thread { CoreSpike.shared.readRun(c, run, p, out.fileHandleForReading.fileDescriptor, tracks: tracks) }
        t.name = "rbsp.spike.reader"
        t.qualityOfService = .userInitiated
        t.start()
        return 0
    }

    /// The reader thread: every line to core_helper_line as it comes (and to the probe, for P7's
    /// estimate.lastline, as Prepare's reader does), then core_helper_ended once.
    private func readRun(_ c: OpaquePointer, _ run: UInt64, _ p: Process, _ fd: Int32, tracks: String?) {
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
                var i = from
                while i < have {
                    if b[i] == 10 {
                        // the probe's own test ({"estimate"…), without a copy of every line
                        if i - from > 3, b[from + 2] == UInt8(ascii: "e") { probeHelperLine(Data(b[from..<i])) }
                        core_helper_line(c, run, UnsafeRawPointer(b.baseAddress! + from).assumingMemoryBound(to: CChar.self), i - from)
                        from = i + 1
                    }
                    i += 1
                }
            }
            if from > 0 {                            // keep only what isn't a whole line yet
                buf.withUnsafeMutableBytes { raw in _ = memmove(raw.baseAddress!, raw.baseAddress! + from, have - from) }
                have -= from
            }
        }
        p.waitUntilExit()
        lock.lock(); procs[run] = nil; lock.unlock()
        core_helper_ended(c, run, p.terminationStatus, nil, 0)
        if let id = tracks { DispatchQueue.main.async { CoreSpike.shared.tracksDone(id) } }
    }
}
#endif
