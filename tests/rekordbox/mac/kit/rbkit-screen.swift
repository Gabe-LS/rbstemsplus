// rbkit-screen: the Mac driver's screen tools in the VM (tests/rekordbox/README.md). Built by
// vm-session.sh stage into "RBKit Screen.app" (io.github.rbstemsplus.rbkit.screen), so that macOS's
// Screen Recording permission names it and not the SSH session; started as DJ with `open -W -n -g`,
// it writes its answer to OUT. Kept small and rarely changed: the permission is for this build's
// code hash, and a changed build needs the owner's click again (README, "Setting up once").
//
//   rbkit-screen OUT access         "ok screen yes" or "ok screen no" (the Screen Recording permission)
//   rbkit-screen OUT shot PNG       the whole screen into PNG (macOS's screencapture, run by this app)
//   rbkit-screen OUT windows        the on-screen windows (CGWindowList), after "ok N": one line each,
//                                   "PROCESS | TITLE | WxH | LAYER | ID" (TITLE empty without the
//                                   permission: macOS shows other apps' titles only to an app that has it)
//   rbkit-screen OUT testwindow     a window titled "RBSP watch test", kept open until this process is
//                                   ended (the dialog watch's own test); "ok shown PID" once it is up
// The first line of OUT is "ok ..." or "FAIL <why>"; the exit status says the same (0, 1); 2: bad
// arguments.
import AppKit

let a = CommandLine.arguments
guard a.count >= 3 else { exit(2) }
let outPath = a[1]
func finish(_ status: Int32, _ text: String) -> Never {
    guard (try? Data((text + "\n").utf8).write(to: URL(fileURLWithPath: outPath))) != nil else { exit(2) }
    exit(status)
}

switch a[2] {
case "access":
    finish(0, "ok screen " + (CGPreflightScreenCaptureAccess() ? "yes" : "no"))
case "shot":
    guard a.count == 4 else { finish(2, "FAIL usage: shot PNG") }
    try? FileManager.default.removeItem(atPath: a[3])
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
    p.arguments = ["-x", "-t", "png", a[3]]
    let err = Pipe(); p.standardError = err; p.standardOutput = err
    do { try p.run() } catch { finish(1, "FAIL screencapture: \(error)") }
    p.waitUntilExit()
    let said = String(decoding: err.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
    let size = ((try? FileManager.default.attributesOfItem(atPath: a[3]))?[.size] as? Int) ?? 0
    if p.terminationStatus != 0 || size == 0 {
        finish(1, "FAIL screencapture exit \(p.terminationStatus)\(said.isEmpty ? "" : ": " + said) (Screen Recording: \(CGPreflightScreenCaptureAccess() ? "yes" : "no"))")
    }
    finish(0, "ok shot " + a[3])
case "windows":
    guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else {
        finish(1, "FAIL window list unavailable")
    }
    var lines: [String] = []
    for w in list {
        let owner = (w[kCGWindowOwnerName as String] as? String) ?? "?"
        // a bar in a title is shown as "/": the fields stay apart
        let title = ((w[kCGWindowName as String] as? String) ?? "").replacingOccurrences(of: "|", with: "/").replacingOccurrences(of: "\n", with: " ")
        let b = (w[kCGWindowBounds as String] as? [String: CGFloat]) ?? [:]
        let layer = (w[kCGWindowLayer as String] as? Int) ?? -1
        let id = (w[kCGWindowNumber as String] as? Int) ?? 0
        lines.append("\(owner) | \(title) | \(Int(b["Width"] ?? 0))x\(Int(b["Height"] ?? 0)) | \(layer) | \(id)")
    }
    finish(0, (["ok \(lines.count)"] + lines).joined(separator: "\n"))
case "testwindow":
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    let w = NSWindow(contentRect: NSRect(x: 200, y: 200, width: 480, height: 200), styleMask: [.titled], backing: .buffered, defer: false)
    w.title = "RBSP watch test"
    let t = NSTextField(labelWithString: "The rekordbox test kit's watch test: this window closes by itself.")
    t.frame = NSRect(x: 20, y: 90, width: 440, height: 24)
    w.contentView?.addSubview(t)
    w.orderFrontRegardless()
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
        try? Data("ok shown \(getpid())\n".utf8).write(to: URL(fileURLWithPath: outPath))
    }
    // never more than 10 minutes, whatever happens to the kit
    DispatchQueue.main.asyncAfter(deadline: .now() + 600) { exit(0) }
    app.run()
default:
    finish(2, "FAIL unknown verb \(a[2])")
}
