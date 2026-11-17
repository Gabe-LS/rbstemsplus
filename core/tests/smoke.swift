// core.h as the AppKit shell will use it: imported as the bridging header, the core linked
// statically (core/build.sh lib --test-hooks), a host table of @convention(c) entries, events in,
// one render per turn, typed accessors and deltas out. No AppKit: a check of the API's Swift
// shape, not of the shell (M7). Built and run by `make core-test`.
import Foundation

var queue: [OpaquePointer] = []
var held: OpaquePointer?
var renders = 0
var changedRows: Int32 = 0
var failures = 0

func check(_ ok: Bool, _ what: String, line: Int = #line) {
    if !ok { failures += 1; print("FAIL smoke.swift:\(line): \(what)") }
}

func pump() { let q = queue; queue = []; q.forEach { core_run($0) } }

func str(_ s: OpaquePointer?, _ f: core_field) -> String {
    var n = 0
    let p = core_str(s, f, &n)!
    return String(decoding: UnsafeRawBufferPointer(start: p, count: n), as: UTF8.self)
}

var host = core_host()
host.size = UInt32(MemoryLayout<core_host>.size)
host.render = { _, snap, delta in
    renders += 1
    var n: Int32 = 0
    _ = core_delta_changed(delta, T_PREP_TRACKS, &n)
    changedRows += n
    if let old = held { core_snap_release(old) }
    held = snap
}
host.front_facts = { _ in UInt32(CORE_FRONT_ACTIVE) }
host.modal_open = { _ in 0 }
host.facts = { _, out in core_fact_flag(out, FACT_STEMS_PLUS_PRESENT, 1) }
host.post_main = { _, job in queue.append(job!) }
host.post_work = { _, _, job in core_job_drop(job) }
host.dialog = { _, _, _ in 1 }
host.close_reply = { _, _, _ in }
host.log = { _, line, n in print("core log: " + String(decoding: UnsafeRawBufferPointer(start: line, count: n), as: UTF8.self)) }

check(core_selftest() == 0, "core_selftest")
check(core_api_version() == CORE_API_VERSION, "the API version")
check(core_ids_hash() == coreIdsHash, "the ids hash matches the generated ids (a stale archive otherwise)")
let config = #"{"version":"1.4.0","os":"mac"}"#
let c = config.withCString { core_new(&host, CORE_ROLE_APP, $0, CORE_NTS) }
check(c != nil, "core_new")

let ev = #"{"ev":"activated"}"#
check(ev.withCString { core_event(c, $0, ev.utf8.count) } == CORE_OK, "an event by length")
check(core_event(c, #"{"ev":"start"}"#, CORE_NTS) == CORE_OK, "start")
core_commit(c)   // the first render, before the window shows
check(renders == 1, "the first render at start-up (\(renders))")
check(core_event(c, #"{"ev":"tab","tab":1}"#, CORE_NTS) == CORE_OK, "an event NUL-terminated")

// the G1 API spike's shape: 5,000 rows, then 5,000 row deltas
check(core_test_table_reset(c, T_PREP_TRACKS, 5000) == CORE_OK, "reset")
for i in 0..<5000 { _ = core_test_cell_str(c, C_TRK_NAME, Int32(i), "Track \(i)", CORE_NTS) }
_ = core_test_set_str(c, F_STATUS_LINE, "Stems Cache is on. ✓", CORE_NTS)
pump()
check(renders == 2 && core_rows(held, T_PREP_TRACKS) == 5000, "5,000 rows in one render")
check(core_delta_reload(nil, T_PREP_TRACKS) == 1, "a nil delta reloads")
check(str(held, F_STATUS_LINE) == "Stems Cache is on. ✓", "a string field")
for i in 0..<5000 {
    _ = core_test_cell_str(c, C_TRK_RESULT, Int32(i), "Ready", CORE_NTS)
    if i % 50 == 49 { pump() }
}
check(renders == 102 && changedRows == 5000, "5,000 deltas over 100 renders (\(renders), \(changedRows))")
var n = 0
let cell = core_cell_str(held, C_TRK_NAME, 4321, &n)!
check(String(decoding: UnsafeRawBufferPointer(start: cell, count: n), as: UTF8.self) == "Track 4321", "a cell")

var title: UnsafePointer<CChar>?
var tl = 0
let st = core_menu_state(c, M_PREP_PAUSE, &title, &tl)
check(st & UInt32(CORE_MENU_ENABLED) == 0 && String(cString: title!) == "Pause", "a menu's live state")

var buf = [CChar](repeating: 0, count: 256)
var len = 0
var slot = core_slot(id: Int32(CORE_SLOT_engine.rawValue), reserved: 0, value: nil, len: 0)
let r: Int32 = "3".withCString { v in
    slot.value = v
    slot.len = 1
    return core_text(CORE_OS_MAC, T_CACHE_ENGINE, &slot, 1, &buf, buf.count, &len)
}
check(r == CORE_OK && String(cString: buf).hasPrefix("Stems Cache doesn't support STEMS Engine 3 yet."), "a text with a slot")

if let s = held { core_snap_release(s) }
core_free(c)
pump()
print(failures == 0 ? "smoke.swift: passed" : "smoke.swift: \(failures) failed")
exit(failures == 0 ? 0 : 1)
