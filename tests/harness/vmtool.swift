// vmtool: the harness driver's OS-level verbs, run in the VM's GUI session (tests/harness/lib.sh
// starts it as the logged-in user). Built on the host (tests/harness/vm.sh build):
//   xcrun swiftc -O -target arm64-apple-macos12.0 tests/harness/vmtool.swift -o vmtool
//
//   vmtool activate BUNDLEID     NSRunningApplication.activate from outside the app (no reopen event)
//   vmtool appleevent BUNDLEID   an activate Apple event ('misc'/'actv') sent to the app
//   vmtool click BUNDLEID        a synthesized mouse click on the app's front window's title bar
//                                (CGEvent: needs Accessibility for the sending process)
//   vmtool front                 the frontmost app's bundle id
//   vmtool windows BUNDLEID      the app's on-screen windows (CGWindowList), one per line
//   vmtool pidstart PID          the process's start time (the kernel's), µs since 1970
//   vmtool rusage PID            the process's own CPU time, and its reaped children's and their
//                                reaped descendants' (the kernel's rusage, proc_pid_rusage), ns:
//                                "cpu_ns=… child_cpu_ns=… child_elapsed_ns=…"; child_elapsed is
//                                their summed lifetimes, overlapping children counted twice (P16's
//                                own time: tests/perf/stats.py)
// Exit 0 when the request was sent; the probe's record says whether it took effect.
import AppKit

let a = CommandLine.arguments
func die(_ s: String) -> Never { FileHandle.standardError.write(Data((s + "\n").utf8)); exit(1) }
guard a.count >= 2 else { die("usage: vmtool activate|appleevent|click|front|windows [BUNDLEID] | pidstart|rusage PID") }

func app(_ id: String) -> NSRunningApplication {
    guard let r = NSRunningApplication.runningApplications(withBundleIdentifier: id).first else { die("not running: \(id)") }
    return r
}

func windows(_ pid: pid_t) -> [[String: Any]] {
    let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
    return list.filter { ($0[kCGWindowOwnerPID as String] as? pid_t) == pid && ($0[kCGWindowLayer as String] as? Int) == 0 }
}

switch a[1] {
case "front":
    print(NSWorkspace.shared.frontmostApplication?.bundleIdentifier ?? "")
case "activate":
    guard a.count == 3 else { die("usage: vmtool activate BUNDLEID") }
    let ok: Bool
    if #available(macOS 14.0, *) { ok = app(a[2]).activate() } else { ok = app(a[2]).activate(options: [.activateIgnoringOtherApps]) }
    print(ok ? "sent" : "refused")
case "appleevent":
    guard a.count == 3 else { die("usage: vmtool appleevent BUNDLEID") }
    let target = NSAppleEventDescriptor(bundleIdentifier: a[2])
    let ev = NSAppleEventDescriptor(eventClass: AEEventClass(kAEMiscStandards), eventID: AEEventID(kAEActivate),
                                    targetDescriptor: target, returnID: AEReturnID(kAutoGenerateReturnID), transactionID: AETransactionID(kAnyTransactionID))
    do { _ = try ev.sendEvent(options: [.neverInteract, .waitForReply], timeout: 10); print("sent") }
    catch { die("appleevent: \(error)") }
case "click":
    guard a.count == 3 else { die("usage: vmtool click BUNDLEID") }
    let r = app(a[2])
    guard let w = windows(r.processIdentifier).first, let b = w[kCGWindowBounds as String] as? [String: CGFloat] else { die("no window") }
    // the title bar's middle: a click there activates the app and changes nothing in it
    let p = CGPoint(x: (b["X"] ?? 0) + (b["Width"] ?? 0) / 2, y: (b["Y"] ?? 0) + 12)
    guard AXIsProcessTrusted() else { die("click: this process has no Accessibility permission") }
    for type in [CGEventType.leftMouseDown, .leftMouseUp] {
        CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
        usleep(30_000)
    }
    print("sent")
case "pidstart":
    // the kernel's start time of a process, in µs since 1970 (the probe's launch_ns is relative to
    // it: P14 joins the old app's records and the new app's through it)
    guard a.count == 3, let pid = Int32(a[2]) else { die("usage: vmtool pidstart PID") }
    var info = kinfo_proc(), size = MemoryLayout<kinfo_proc>.stride
    var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, pid]
    guard sysctl(&mib, 4, &info, &size, nil, 0) == 0, size > 0 else { die("no process \(pid)") }
    let st = info.kp_proc.p_un.__p_starttime
    print(Int64(st.tv_sec) * 1_000_000 + Int64(st.tv_usec))
case "rusage":
    // what the process did itself and what its children did: user and system time of the process,
    // of its children it reaped, and the children's lifetimes summed (each from its start to its
    // exit). The kernel counts these in mach time units (not ns on Apple silicon): converted here.
    guard a.count == 3, let pid = Int32(a[2]) else { die("usage: vmtool rusage PID") }
    var ri = rusage_info_v4()
    let rc = withUnsafeMutablePointer(to: &ri) { p in
        p.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) { proc_pid_rusage(pid, RUSAGE_INFO_V4, $0) }
    }
    guard rc == 0 else { die("rusage: no process \(pid) (\(String(cString: strerror(errno))))") }
    var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
    func ns(_ t: UInt64) -> UInt64 { t * UInt64(tb.numer) / UInt64(tb.denom) }
    print("cpu_ns=\(ns(ri.ri_user_time + ri.ri_system_time)) child_cpu_ns=\(ns(ri.ri_child_user_time + ri.ri_child_system_time)) child_elapsed_ns=\(ns(ri.ri_child_elapsed_abstime))")
case "windows":
    guard a.count == 3 else { die("usage: vmtool windows BUNDLEID") }
    for w in windows(app(a[2]).processIdentifier) { print(w[kCGWindowName as String] as? String ?? "", w[kCGWindowBounds as String] ?? "") }
default:
    die("unknown verb \(a[1])")
}
