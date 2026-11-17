// The G1 API spike's host smoke test (tests/mac/spike-smoke.sh): app/Spike/CoreSpike.swift with
// a stand-in for the probe, its table never shown, driven like the harness drives it in tart
// (select, scroll) with the harness's fake helper. It checks the mechanics (rows, results, the
// deltas' rows, the end of every run) and prints rough host timings; the measurement is tart's
// (tests/harness/scenarios/perf-spike*.sh, tests/perf/stats.py).
//
//   spike-smoke <fake helper folder>
import AppKit

// the probe's part the spike uses (app/Probe/Probe.swift's in the app)
final class Probe {
    static let shared = Probe()
    var conf: [String: String] = [:]
    var command: ((String, String, @escaping (String) -> Void) -> Bool)?
    var lastLines = 0
}
func probeSpike(table: NSTableView, command: @escaping (_ verb: String, _ arg: String, _ ack: @escaping (String) -> Void) -> Bool) { Probe.shared.command = command }
var settledStart: (() -> Void)?                                     // what the probe runs at "mark settled"
func probeSpikeStartOnSettled(_ start: @escaping () -> Void) { settledStart = start }
func probeHelperLine(_ line: Data) { if line.starts(with: Array("{\"estimate\"".utf8)) { DispatchQueue.main.async { Probe.shared.lastLines += 1 } } }
let compiledVersion = "1.4.0"

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { failures += 1; print("FAIL: \(what)") } else { print("ok: \(what)") } }

let dir = CommandLine.arguments[1]
Probe.shared.conf = ["spike": "1", "helper": dir + "/fake-prepare"]
let app = NSApplication.shared
app.setActivationPolicy(.prohibited)
let s = CoreSpike.shared
coreSpikeStartIfAsked()                            // as main.swift: the start waits for the mark
check(s.core == nil, "no core before mark settled")
settledStart?()                                    // the probe, at the driver's "mark settled"
check(s.core != nil, "core after mark settled")
check(s.core != nil && Probe.shared.command != nil, "the spike started (ids hash and API version checked)")

func cell(_ row: Int, _ col: String) -> String {
    let c = s.table.tableColumns.first { $0.identifier.rawValue == col }!
    return (s.tableView(s.table, viewFor: c, row: row) as? NSTableCellView)?.textField?.stringValue ?? "?"
}
func rows() -> Int { s.numberOfRows(in: s.table) }
func run(until: @escaping () -> Bool, _ seconds: Double) -> Bool {
    let end = Date().addingTimeInterval(seconds)
    while !until() && Date() < end { RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.01)) }
    return until()
}
func send(_ verb: String, _ arg: String) -> String {
    var ack = ""
    _ = Probe.shared.command?(verb, arg) { ack = $0 }
    return ack
}

for (id, n, label) in [("50", 5000, "5,000 rows, full speed"), ("51", 5000, "5,000 rows, 1 line/ms"), ("150", 20000, "20,000 rows, full speed")] {
    let before = Probe.shared.lastLines
    let gen0 = core_list_gen(s.held, T_PREP_TRACKS)
    let t0 = Date()
    check(send("select", id) == "ok", "select \(id) acked")
    // the table shows the new list (its list_gen moved) with all its rows
    let listed = run(until: { core_list_gen(s.held, T_PREP_TRACKS) != gen0 && rows() == n }, 60)
    let tList = Date().timeIntervalSince(t0)
    check(listed && cell(0, "name") == "big-1" && cell(n - 1, "name") == "big-\(n)", "\(label): the list's \(n) rows")
    let want = id == "51" ? "Ready" : "To prepare"        // the fake's 51 says "ready", the others "prepare"
    let done = run(until: { Probe.shared.lastLines > before && cell(n - 1, "result") == want }, 90)
    let tAll = Date().timeIntervalSince(t0)
    check(done && cell(0, "result") == want && cell(n / 2, "result") == want, "\(label): every row's result")
    check(core_test_helper_lines(s.core, 1) >= Int64(n) && core_test_helper_lines(s.core, 2) >= Int64(n), "\(label): lines counted per purpose")
    print(String(format: "  %@: list %.0f ms, list and estimate %.0f ms (host, rough)", label, tList * 1000, tAll * 1000))
}

// review Q32: a select while an estimate streams ends it: none of its later lines reach the new
// list's rows, and its helper is stopped. 52's list has no estimate, so its rows stay Checking…
// and any "Ready" would be 51's
do {
    let gen0 = core_list_gen(s.held, T_PREP_TRACKS)
    check(send("select", "51") == "ok", "select 51 (paced) acked")
    _ = run(until: { core_list_gen(s.held, T_PREP_TRACKS) != gen0 && rows() == 5000 && cell(200, "result") == "Ready" }, 30)
    check(cell(200, "result") == "Ready" && cell(4999, "result") != "Ready", "51's estimate is streaming")
    let gen1 = core_list_gen(s.held, T_PREP_TRACKS)
    check(send("select", "52") == "ok", "select 52 while 51's estimate streams")
    let done = run(until: { core_list_gen(s.held, T_PREP_TRACKS) != gen1 && rows() == 5000 }, 60)
    _ = run(until: { false }, 1.5)                        // time for any of 51's lines that would still come
    let leaked = (0..<5000).filter { cell($0, "result") != "Checking\u{2026}" }.count
    check(done && leaked == 0, "the superseded estimate's lines never reach the new rows (\(leaked) rows changed)")
    let fakes = (try? String(contentsOfFile: dir + "/log/argv.log", encoding: .utf8)) ?? ""
    check(fakes.contains("estimate-51"), "the fake helper's log has 51's estimate")
    let still = Process()
    still.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
    // the scratch folder by its own name: Foundation may drop /private from the helper's path
    still.arguments = ["-f", ((dir as NSString).deletingLastPathComponent as NSString).lastPathComponent + "/fake/fake-prepare --estimate --playlist 51"]
    still.standardOutput = FileHandle.nullDevice
    try? still.run(); still.waitUntilExit()
    check(still.terminationStatus == 1, "51's estimate helper was stopped (no such process left)")
}
check(send("scroll", "4999") == "ok" && send("tab", "Prepare") == "", "scroll is the spike's, other commands are the probe's")
check(send("select", "x") == "err spike: select ID", "a bad select is refused")
print(failures == 0 ? "spike-smoke: passed" : "spike-smoke: \(failures) failed")
exit(failures == 0 ? 0 : 1)
