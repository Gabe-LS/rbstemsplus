// The app's window: three toolbar tabs, Status and Prepare (the shell's, app/Shell/ShellTabs.swift)
// and Manage, built here: each feature's actions in the left column, the log on the right. Since
// M7's switch-over (mac-core §8) the window is the AppKit shell's (app/Shell): the core decides
// every light, button, text and action, the shell draws its snapshots into these views and sends
// the core what the user did. What stays here is the window itself, its views (the probe's field
// ids are their property names), the log, the work queue the core's flows run on, and the
// platform steps of the watcher's LaunchAgent and the leftovers (Actions.swift).
import AppKit

final class Controller: NSObject {
    let window: NSWindow
    let text = NSTextView()
    let status = NSTextField(wrappingLabelWithString: "")
    let spinner = NSProgressIndicator()
    let plusIcon = NSImageView(), cacheIcon = NSImageView()
    let plusLabel = NSTextField(labelWithString: "Demucs v4"), cacheLabel = NSTextField(labelWithString: "Stems Cache")
    var installPlusBtn = NSButton(), installCacheBtn = NSButton(), reinstallBtn = NSButton(), uninstallPlusBtn = NSButton(), uninstallCacheBtn = NSButton()
    var uninstallAllBtn = NSButton()
    /// each feature's own Reinstall, and Stems Cache's Settings… (the Manage tab)
    var reinstallPlusBtn = NSButton(), reinstallCacheBtn = NSButton(), settingsBtn = NSButton()
    var tabs: MainTabs!
    /// the core's flow thread (mac-core §3.2): today's serial work queue
    let work = DispatchQueue(label: "rbstemsplus.work")
    /// "Update RB Stems Plus" in the app menu, shown when the core says a newer RB Stems Plus is out.
    var updateItem: NSMenuItem?

    /// The window, hidden, with its views in their start state. `tabs` makes the Status and Prepare
    /// tabs' view controllers (the shell's), given this controller; Manage is built here. The
    /// shell renders the first snapshot and shows the window (CoreShell.start).
    init(tabs makeTabs: (Controller) -> (status: NSViewController, prepare: NSViewController)) {
        window = NSWindow(contentRect: NSRect(origin: .zero, size: tabSizes[Tab.manage.rawValue]),
                          styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        super.init()
        window.title = "RB Stems Plus" + titleNote
        // two features, each installed on its own; none is the default button (neither is
        // recommended over the other)
        // (their clicks go to the core: CoreShell.button sets each one's target and action)
        let installPlus = NSButton(title: "Install Demucs v4", target: nil, action: nil)
        let installCache = NSButton(title: "Install Stems Cache", target: nil, action: nil)
        let reinstall = NSButton(title: "Reinstall", target: nil, action: nil)
        let reinstallPlus = NSButton(title: "Reinstall Demucs v4", target: nil, action: nil)
        let reinstallCache = NSButton(title: "Reinstall Stems Cache", target: nil, action: nil)
        let settings = NSButton(title: "Settings…", target: nil, action: nil)
        let uninstallPlus = NSButton(title: "Uninstall Demucs v4", target: nil, action: nil)
        let uninstall = NSButton(title: "Uninstall Stems Cache", target: nil, action: nil)
        let uninstallAll = NSButton(title: "Uninstall RB Stems Plus Completely", target: nil, action: nil)
        // in the window: each feature's own; the combined Reinstall (the watcher's request, an
        // interrupted reinstall) and Uninstall Completely (the app menu) aren't shown
        let buttons = [installPlus, reinstallPlus, uninstallPlus, installCache, reinstallCache, settings, uninstall]
        installPlusBtn = installPlus; installCacheBtn = installCache
        reinstallBtn = reinstall; uninstallPlusBtn = uninstallPlus; uninstallCacheBtn = uninstall
        reinstallPlusBtn = reinstallPlus; reinstallCacheBtn = reinstallCache; settingsBtn = settings
        uninstallAllBtn = uninstallAll
        spinner.style = .spinning; spinner.controlSize = .small; spinner.isDisplayedWhenStopped = false
        let statusRow = NSStackView(views: [spinner, status])
        status.preferredMaxLayoutWidth = 240
        status.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        statusRow.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.documentView = text
        scroll.borderType = .bezelBorder
        text.isEditable = false
        text.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        text.autoresizingMask = [.width]
        // Manage: each feature's actions in the left third, the log in the right two thirds
        let spacer = NSView()
        spacer.setContentHuggingPriority(.init(1), for: .vertical)
        let left = NSStackView(views: [label("Demucs v4", bold: true), installPlus, reinstallPlus, uninstallPlus,
                                       label("Stems Cache", bold: true), installCache, reinstallCache, settings, uninstall, spacer, statusRow])
        left.orientation = .vertical
        left.alignment = .leading
        left.spacing = 8
        left.setCustomSpacing(18, after: uninstallPlus)
        for b in buttons {
            b.translatesAutoresizingMaskIntoConstraints = false
            b.widthAnchor.constraint(equalTo: left.widthAnchor).isActive = true
        }
        let body = NSStackView(views: [left, scroll])
        body.alignment = .top
        body.spacing = 16
        body.edgeInsets = NSEdgeInsets(top: 16, left: 20, bottom: 16, right: 20)
        body.frame = NSRect(origin: .zero, size: tabSizes[Tab.manage.rawValue])
        left.widthAnchor.constraint(equalTo: body.widthAnchor, multiplier: 1.0 / 3.0, constant: -20).isActive = true
        scroll.heightAnchor.constraint(equalTo: body.heightAnchor, constant: -32).isActive = true
        left.heightAnchor.constraint(equalTo: body.heightAnchor, constant: -32).isActive = true
        let manage = NSViewController()
        manage.view = body
        manage.preferredContentSize = tabSizes[Tab.manage.rawValue]
        let (statusTab, prepareTab) = makeTabs(self)
        tabs = MainTabs()
        tabs.window = window
        tabs.tabStyle = .toolbar
        tabs.transitionOptions = []
        for (vc, name, symbol) in [(statusTab, "Status", "checkmark.circle"), (prepareTab, "Prepare", "music.note.list"),
                                   (manage, "Manage", "wrench.and.screwdriver")] {
            let item = NSTabViewItem(viewController: vc)
            item.label = name
            item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: name)
            tabs.addTabViewItem(item)
        }
        window.contentViewController = tabs
        if #available(macOS 11.0, *) { window.toolbarStyle = .preference }
        try? FileManager.default.createDirectory(atPath: originalsDir, withIntermediateDirectories: true)
    }

    // MARK: helpers (callable from any thread)

    /// The log file and the log view (the shell's host->log and its own lines).
    func log(_ s: String) {
        appendLog(logPath, s)
        let f = DateFormatter(); f.dateFormat = "HH:mm:ss"
        let line = "\(f.string(from: Date()))  \(s)\n"
        onMain {
            self.text.textStorage?.append(NSAttributedString(string: line, attributes: [.font: self.text.font as Any, .foregroundColor: NSColor.textColor]))
            self.text.scrollToEndOfDocument(nil)
        }
    }

    func onMain(_ f: @escaping () -> Void) { Thread.isMainThread ? f() : DispatchQueue.main.async(execute: f) }

    /// One line describing what is installed, with the model's checksum (slow: run in the background).
    func state() -> String {
        let model = sha256(modelDir + "/hdemucs.onnx")?.prefix(12) ?? ""
        return "rekordbox \(rekordboxVersion()), engine \(engineVersion()), model \(model), bridge \(stemsCachePresent() ? "yes" : "no"), chosen \(chosen())"
    }
}

// MARK: - the status lights

/// How a status light looks: on (installed and working), limited (installed but not working
/// right now) or off (not installed).
enum LightLook: Equatable { case on, limited, off }

/// The symbol, colour and VoiceOver description of each look: a green check circle, a yellow
/// exclamation circle, a red cross circle (shape and colour differ, for colour-blind users).
func lightDrawing(_ look: LightLook) -> (symbol: String, tint: NSColor, description: String) {
    switch look {
    case .on: return ("checkmark.circle.fill", .systemGreen, "on")
    case .limited: return ("exclamationmark.circle.fill", .systemYellow, "installed, not working right now")
    case .off: return ("xmark.circle.fill", .systemRed, "off")
    }
}
