// g3ax: the G3 tart run's driver for the apps that have no probe (the tag TEST builds of v1.0.0,
// v1.3.0 and any 1.3.x, and the real published release after publishing; release.md §6.1). It
// presses their buttons and menu items through Accessibility, from DJ's session, as a user does.
// tests/mac/g3.sh builds it into a small app bundle (so macOS's Accessibility permission names
// it, not the SSH session) and starts it as DJ with `open -W`; it writes its answer to OUT.
//
//   G3 Driver.app/Contents/MacOS/g3ax OUT trusted            "yes" or "no" (AXIsProcessTrusted)
//   ... OUT press BUNDLEID MS TITLE...    presses the first enabled button, tab or checkbox titled
//                                         TITLE in any of the app's windows, sheets or alerts, the
//                                         titles in order (each waited for up to MS ms); an
//                                         optional title ends with "?" (pressed when there)
//   ... OUT menu BUNDLEID MS ITEM...      the menu item with one of the titles ITEM (1.0.0 says
//                                         "Update RB Stems Plus", 1.3.0 adds "…") in any of the
//                                         menu bar's menus, waited for up to MS ms until it is
//                                         there and enabled
//   ... OUT text BUNDLEID MS TEXT         waits up to MS ms for a text containing TEXT in one of
//                                         the app's windows (static texts, titles, values), not
//                                         followed by a digit or by "." and a digit ("Version
//                                         1.4.1" isn't found in "Version 1.4.10")
//   ... OUT texts BUNDLEID                every text the app's windows show, one per line
// The first line of OUT is "ok" or "FAIL <why>"; the exit status says the same (0, 1); 3: no
// Accessibility permission (g3.sh then asks the owner to click instead).
import AppKit
import ApplicationServices

let a = CommandLine.arguments
guard a.count >= 3 else { exit(2) }
let outPath = a[1]
var outLines: [String] = []
func finish(_ status: Int32, _ first: String) -> Never {
    let text = ([first] + outLines).joined(separator: "\n") + "\n"
    print(text, terminator: "")
    // written in place (not atomically: no temporary file elsewhere)
    guard (try? Data(text.utf8).write(to: URL(fileURLWithPath: outPath))) != nil else { exit(2) }
    exit(status)
}

func attr(_ e: AXUIElement, _ name: String) -> AnyObject? {
    var v: AnyObject?
    return AXUIElementCopyAttributeValue(e, name as CFString, &v) == .success ? v : nil
}
func str(_ e: AXUIElement, _ name: String) -> String? { attr(e, name) as? String }
func children(_ e: AXUIElement) -> [AXUIElement] { (attr(e, kAXChildrenAttribute) as? [AXUIElement]) ?? [] }
func enabled(_ e: AXUIElement) -> Bool { (attr(e, kAXEnabledAttribute) as? Bool) ?? true }

func pid(_ bid: String) -> pid_t? { NSRunningApplication.runningApplications(withBundleIdentifier: bid).first?.processIdentifier }
func appElement(_ bid: String) -> AXUIElement? { pid(bid).map { AXUIElementCreateApplication($0) } }

/// Every element of the app's windows (sheets and alerts are windows' children or windows), depth-first.
func walk(_ app: AXUIElement, _ visit: (AXUIElement) -> Bool) {
    var stack = (attr(app, kAXWindowsAttribute) as? [AXUIElement]) ?? []
    var seen = 0
    while let e = stack.popLast(), seen < 20000 {
        seen += 1
        if visit(e) { return }
        stack.append(contentsOf: children(e).reversed())
    }
}

func pressable(_ e: AXUIElement, _ title: String) -> Bool {
    guard let role = str(e, kAXRoleAttribute),
          [kAXButtonRole, kAXRadioButtonRole, kAXCheckBoxRole, "AXTab"].contains(role) else { return false }
    let t = str(e, kAXTitleAttribute) ?? str(e, kAXDescriptionAttribute) ?? ""
    return t == title && enabled(e)
}

func until(_ ms: Int, _ test: () -> Bool) -> Bool {
    let end = Date().addingTimeInterval(Double(ms) / 1000)
    repeat {
        if test() { return true }
        usleep(200_000)
    } while Date() < end
    return false
}

func texts(_ app: AXUIElement) -> [String] {
    var r: [String] = []
    walk(app) { e in
        for k in [kAXValueAttribute, kAXTitleAttribute, kAXDescriptionAttribute] {
            if let s = str(e, k), !s.isEmpty { r.append(s) }
        }
        return false
    }
    return r
}

let verb = a[2]
if verb == "trusted" { finish(0, AXIsProcessTrusted() ? "ok yes" : "ok no") }
guard AXIsProcessTrusted() else { finish(3, "FAIL no Accessibility permission") }
guard a.count >= 4 else { finish(1, "FAIL usage") }
let bid = a[3]
guard let app = appElement(bid) else { finish(1, "FAIL not running: \(bid)") }

switch verb {
case "texts":
    outLines = texts(app)
    finish(0, "ok")
case "text":
    /// TEXT in s, at an occurrence not followed by a digit or by "." and a digit
    func shows(_ s: String, _ text: String) -> Bool {
        var from = s.startIndex
        while let r = s.range(of: text, range: from..<s.endIndex) {
            let rest = s[r.upperBound...]
            let next = rest.first, after = rest.dropFirst().first
            if !(next?.isNumber ?? false) && !(next == "." && (after?.isNumber ?? false)) { return true }
            from = s.index(after: r.lowerBound)
        }
        return false
    }
    guard a.count == 6, let ms = Int(a[4]) else { finish(1, "FAIL usage: text BUNDLEID MS TEXT") }
    if until(ms, { texts(app).contains { shows($0, a[5]) } }) { finish(0, "ok") }
    outLines = texts(app)
    finish(1, "FAIL no text \"\(a[5])\" within \(ms) ms (the window's texts follow)")
case "press":
    guard a.count >= 6, let ms = Int(a[4]) else { finish(1, "FAIL usage: press BUNDLEID MS TITLE...") }
    for raw in a[5...] {
        let optional = raw.hasSuffix("?")
        let title = optional ? String(raw.dropLast()) : raw
        var found: AXUIElement?
        let there = until(optional ? min(ms, 3000) : ms) {
            walk(app) { e in
                if pressable(e, title) { found = e; return true }
                return false
            }
            return found != nil
        }
        guard there, let b = found else {
            if optional { outLines.append("not there: \(title)"); continue }
            outLines = texts(app)
            finish(1, "FAIL no enabled \"\(title)\" within \(ms) ms (the window's texts follow)")
        }
        guard AXUIElementPerformAction(b, kAXPressAction as CFString) == .success else { finish(1, "FAIL \"\(title)\" didn't take the press") }
        outLines.append("pressed: \(title)")
        usleep(500_000)
    }
    finish(0, "ok")
case "menu":
    guard a.count >= 6, let ms = Int(a[4]) else { finish(1, "FAIL usage: menu BUNDLEID MS ITEM...") }
    let titles = Set(a[5...])
    var item: AXUIElement?
    let there = until(ms) {
        guard let bar = attr(app, kAXMenuBarAttribute), CFGetTypeID(bar) == AXUIElementGetTypeID() else { return false }
        // every top-level menu (the first is Apple's)
        for t in children(bar as! AXUIElement) {
            for menu in children(t) {
                for e in children(menu) where titles.contains(str(e, kAXTitleAttribute) ?? "") && enabled(e) { item = e; return true }
            }
        }
        return false
    }
    guard there, let m = item else { finish(1, "FAIL no enabled menu item \(titles.sorted()) within \(ms) ms") }
    guard AXUIElementPerformAction(m, kAXPressAction as CFString) == .success else { finish(1, "FAIL the menu item didn't take the press") }
    finish(0, "ok pressed: \(str(m, kAXTitleAttribute) ?? "")")
default:
    finish(1, "FAIL unknown verb \(verb)")
}
