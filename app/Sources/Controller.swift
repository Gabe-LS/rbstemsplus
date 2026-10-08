// The app's window: the name and two status lights on top, the action column on the left, the
// log on the right. Also the helpers every action uses (log, busy state, dialogs). The actions
// themselves are in Actions.swift, the admin runs in Admin.swift.
import AppKit

final class Controller: NSObject {
    let window: NSWindow
    let text = NSTextView()
    let status = NSTextField(wrappingLabelWithString: "")
    let spinner = NSProgressIndicator()
    let plusIcon = NSImageView(), cacheIcon = NSImageView()
    let plusLabel = NSTextField(labelWithString: "Demucs v4"), cacheLabel = NSTextField(labelWithString: "Stems Cache")
    var buttons: [NSButton] = []
    var installPlusBtn = NSButton(), installCacheBtn = NSButton(), reinstallBtn = NSButton(), uninstallPlusBtn = NSButton(), uninstallCacheBtn = NSButton()
    var uninstallAllBtn = NSButton()
    let work = DispatchQueue(label: "rbstemsplus.work")
    var actionRunning = false
    /// The action that is changing things, recorded in the busy marker (see start in Actions.swift).
    var currentAction: Action?
    /// Set once the start-up sheets (welcome, resume) are done: the watcher's request waits for them.
    var startupDone = false
    /// Set while the watcher's request waits for rekordbox to be closed (its dialog is up): a
    /// reopen or activation meanwhile doesn't ask again.
    var requestWaiting = false
    /// A line shown under the reasons in the status line between actions (e.g. an update notice).
    var notice: String?
    /// Shown in the status line for this run when 1.0's saved copy of Pioneer's model turned out
    /// to be ours (Models.swift, migrateLegacy).
    var modelNotice: String?
    /// The watcher's LaunchAgent is there but launchd won't run it (Actions.swift, watcherOff).
    var watcherIsOff = false
    /// "Update RB Stems Plus" in the app menu, shown when a newer RB Stems Plus is out (Update.swift).
    var updateItem: NSMenuItem?

    override init() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 960, height: 400),
                          styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        super.init()
        window.delegate = self
        window.title = "RB Stems Plus" + titleNote
        let title = NSTextField(labelWithString: "RB Stems Plus")
        title.font = .boldSystemFont(ofSize: 16)
        // two features, each installed on its own; none is the default button (neither is
        // recommended over the other)
        let installPlus = NSButton(title: "Install Demucs v4", target: self, action: #selector(installPlusAction))
        let installCache = NSButton(title: "Install Stems Cache", target: self, action: #selector(installCacheAction))
        let reinstall = NSButton(title: "Reinstall", target: self, action: #selector(reinstallAction))
        let uninstallPlus = NSButton(title: "Uninstall Demucs v4", target: self, action: #selector(uninstallStemsPlus))
        let uninstall = NSButton(title: "Uninstall Stems Cache", target: self, action: #selector(uninstallCache))
        let uninstallAll = NSButton(title: "Uninstall RB Stems Plus Completely", target: self, action: #selector(uninstallEverything))
        let quit = NSButton(title: "Quit", target: NSApp, action: #selector(NSApplication.terminate(_:)))
        buttons = [installPlus, installCache, reinstall, uninstallPlus, uninstall, uninstallAll, quit]
        installPlusBtn = installPlus; installCacheBtn = installCache
        reinstallBtn = reinstall; uninstallPlusBtn = uninstallPlus; uninstallCacheBtn = uninstall
        uninstallAllBtn = uninstallAll
        spinner.style = .spinning; spinner.controlSize = .small; spinner.isDisplayedWhenStopped = false
        let statusRow = NSStackView(views: [spinner, status])
        status.preferredMaxLayoutWidth = 260
        status.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        statusRow.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.documentView = text
        scroll.borderType = .bezelBorder
        text.isEditable = false
        text.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        text.autoresizingMask = [.width]
        // top line: the app's name on the left, the two status lights on the right (a green check
        // circle when on, a yellow exclamation circle when installed but not working right now, a
        // red cross circle when off: shape and colour differ, for colour-blind users)
        let stateBox = NSStackView(views: [NSStackView(views: [plusIcon, plusLabel]), NSStackView(views: [cacheIcon, cacheLabel])])
        stateBox.spacing = 18
        let headerGap = NSView()
        headerGap.setContentHuggingPriority(.init(1), for: .horizontal)
        let header = NSStackView(views: [title, headerGap, stateBox])
        header.alignment = .centerY
        // below: the actions in the left third, the log (as tall as the actions) in the right two thirds
        let spacer = NSView()
        spacer.setContentHuggingPriority(.init(1), for: .vertical)
        let left = NSStackView(views: [installPlus, installCache, reinstall, uninstallPlus, uninstall, spacer, statusRow, uninstallAll, quit])
        left.orientation = .vertical
        left.alignment = .leading
        left.spacing = 10
        for b in buttons {
            b.translatesAutoresizingMaskIntoConstraints = false
            b.widthAnchor.constraint(equalTo: left.widthAnchor).isActive = true
        }
        let body = NSStackView(views: [left, scroll])
        body.alignment = .top
        body.spacing = 16
        let root = NSStackView(views: [header, body])
        root.orientation = .vertical
        root.alignment = .leading
        root.spacing = 14
        root.edgeInsets = NSEdgeInsets(top: 14, left: 16, bottom: 16, right: 16)
        window.contentView = root
        header.widthAnchor.constraint(equalTo: root.widthAnchor, constant: -32).isActive = true
        body.widthAnchor.constraint(equalTo: root.widthAnchor, constant: -32).isActive = true
        left.widthAnchor.constraint(equalTo: body.widthAnchor, multiplier: 1.0 / 3.0, constant: -8).isActive = true
        body.heightAnchor.constraint(equalTo: root.heightAnchor, constant: -(14 + 16 + 14 + 24)).isActive = true
        scroll.heightAnchor.constraint(equalTo: body.heightAnchor).isActive = true
        left.heightAnchor.constraint(equalTo: body.heightAnchor).isActive = true
        try? FileManager.default.createDirectory(atPath: originalsDir, withIntermediateDirectories: true)
        refresh()                      // buttons in their final state before the window appears
        window.center()
        window.makeKeyAndOrderFront(nil)
        log("started: RB Stems Plus \(appVersion) (\(appBuild))\(titleNote), macOS \(ProcessInfo.processInfo.operatingSystemVersionString), user \(NSUserName()), bundle \(Bundle.main.bundlePath)\(bundleProblem(Bundle.main.bundlePath, executable: Bundle.main.executablePath).map { " (\($0))" } ?? "")")
        startupChecks()
    }

    /// Runs once the window is up: the state line (in the background), then, once the app's run
    /// loop runs (sheets need it), an action left half-done by the last run, then the watcher's
    /// request.
    func startupChecks() {
        work.async {
            // first: the watcher registered for this app's code (after an update or the install
            // command), before anything here changes a folder it watches
            self.checkWatcher()
            self.updateWatcherState()
            // 1.0's saved copy of Pioneer's model, into the layout by checksum (once)
            if installedModels(self.log).migrateLegacy() {
                self.onMain { self.modelNotice = "There's no saved copy of rekordbox's own stems model, so uninstalling Demucs v4 can't put it back."; self.refresh() }
            }
            self.removeLeftovers()
            // an account that chose Stems Cache before it saved rekordbox's own model's stems: on
            // for it too, as Install Stems Cache sets it now
            if chosen().cache && !configSets("rekordbox_model") {
                self.log("Stems Cache: turned on for rekordbox's own stems model too (rekordbox_model=1): \(writeRekordboxModel(true) ? "ok" : "FAILED")")
            }
            self.log("state: \(self.state())")              // checksums: slow, so in the background
            _ = readConfig(self.log)                         // says which config.ini lines are ignored
        }
        DispatchQueue.main.async {
            self.welcomeOnce()
            self.checkUpdateResult()
            let resumed = self.checkResume()
            if resumed { _ = takeReinstallRequest() }           // the resumed action covers it
            self.startupDone = true
            if !resumed { self.checkRequest() }
            self.checkForUpdate()
            self.refresh()                                      // the update notice, if any
        }
    }

    /// The first launch's welcome: what the two features do, the macOS permission Stems Cache
    /// needs, and that the tool is unofficial. Never again after. No feature is called better:
    /// Demucs v4 is a different model, the user chooses.
    func welcomeOnce() {
        guard !FileManager.default.fileExists(atPath: welcomedFile) else { return }
        _ = alert("Welcome to RB Stems Plus", """
            Demucs v4 replaces rekordbox's own stems model with Meta's Demucs v4 separation model. The first separation of a track takes longer, and the two models split some sounds differently: listen and keep the one you prefer.

            Stems Cache saves the separated stems, with either model, so a track's stems load much faster the next time. It changes one file inside rekordbox, so macOS will ask you to allow RB Stems Plus to change rekordbox.

            RB Stems Plus is unofficial: it isn't made or supported by AlphaTheta or Pioneer DJ.
            """, ["Continue"])
        try? FileManager.default.createDirectory(atPath: support, withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: welcomedFile, contents: Data())
    }

    // MARK: helpers (callable from any thread)

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
    func onMainSync<T>(_ f: () -> T) -> T { Thread.isMainThread ? f() : DispatchQueue.main.sync(execute: f) }

    /// Sets every button's final state and label in one go (main thread), so none flashes.
    func refresh() {
        onMain {
            let plus = stemsPlusPresent(), cache = stemsCachePresent(), c = chosen(), cfg = readConfig()
            // Stems Cache's free-space floor, as the bridge measures it (only while it's on in config.ini)
            let paused = cache && cfg.enabled ? cachePausedFloor() : nil
            // with rekordbox's own model: whether the installed bridge saves its stems (by checksum,
            // worked out in the background the first time)
            let own = cache && !plus
            let light = cacheLight(installed: cache, plusOn: plus, chosen: c.cache, rekordboxModel: cfg.rekordboxModel,
                                   bridgeModels: own ? bridgeCacheModels() : nil,
                                   model: own ? currentModelSHA(done: { self.refresh() }) : nil,
                                   engine: engineVersion(), pausedFloor: paused)
            for (icon, label, light) in [(self.plusIcon, self.plusLabel, plusLight(installed: plus)), (self.cacheIcon, self.cacheLabel, light)] {
                let look = lightDrawing(light.look)
                icon.image = NSImage(systemSymbolName: look.symbol, accessibilityDescription: look.description)?
                    .withSymbolConfiguration(.init(pointSize: 15, weight: .regular))
                icon.contentTintColor = look.tint
                icon.toolTip = light.tip
                label.toolTip = light.tip
            }
            if self.actionRunning { return }           // the buttons stay off until the action ends
            // what can be installed on this Mac (Compat.swift); uninstalling is never gated, except
            // that removing Stems Cache needs an administrator like installing it. Uninstall Stems
            // Cache also removes what Stems Cache left in /Library after a rekordbox update.
            let plusWhy = stemsPlusBlocked(), cacheWhy = stemsCacheBlocked(), traces = cacheTraces()
            let names = { (p: Bool, k: Bool) in p && k ? "Demucs v4 + Stems Cache" : (k ? "Stems Cache" : "Demucs v4") }
            // each feature installs on its own; Stems Cache also when another account installed it
            // (then it only turns it on here); reinstall names what the user chose
            self.installPlusBtn.isEnabled = !plus && plusWhy == nil
            // in rekordbox already (from another account): turning it on here needs no administrator
            self.installCacheBtn.isEnabled = cache ? !c.cache && rekordboxInstalled() : cacheWhy == nil
            self.reinstallBtn.isEnabled = (c.model && plusWhy == nil) || (c.cache && cacheWhy == nil)
            self.reinstallBtn.title = "Reinstall " + names(c.model || !c.cache, c.cache)
            self.uninstallPlusBtn.isEnabled = plus                     // rekordbox's model back: no password
            self.uninstallCacheBtn.isEnabled = traces && userIsAdmin
            for b in self.buttons where ![self.installPlusBtn, self.installCacheBtn, self.reinstallBtn, self.uninstallPlusBtn, self.uninstallCacheBtn].contains(b) { b.isEnabled = true }
            self.uninstallAllBtn.isEnabled = !traces || userIsAdmin
            // the status line, between actions: why a feature can't be installed, then any notice
            var lines: [String] = []
            if ownBundle() == nil { lines.append(moveToApplications) }       // no watcher, update or self-delete until then
            if let why = plusWhy, !plus || c.model { lines.append(why) }
            // a standard account: why the Stems Cache buttons are off, removing included
            let standard = standardAccountLine(admin: userIsAdmin, cacheTraces: traces, cacheChosen: c.cache)
            if let why = cacheWhy, !cache || c.cache, !lines.contains(why), standard == nil || why != needsAdminLine { lines.append(why) }
            if let s = standard { lines.append(s) }
            if cacheIsLink() { lines.append(cacheLinkLine) }
            if self.watcherIsOff { lines.append(watcherOffLine) }
            if let n = self.modelNotice { lines.append(n) }
            if let n = self.notice { lines.append(n) }
            self.status.stringValue = lines.joined(separator: "\n")
        }
    }

    /// Shows the status line and spinner and disables every button while an action runs (msg),
    /// or restores them (nil). For an action that changes things (currentAction), also sets the
    /// busy marker the watcher waits for and the next launch resumes from.
    func busy(_ msg: String?) {
        onMain {
            self.actionRunning = msg != nil
            if msg == nil { self.currentAction = nil }
            setBusyMarker(self.currentAction)
            self.status.stringValue = msg ?? ""
            if msg != nil { self.spinner.startAnimation(nil) } else { self.spinner.stopAnimation(nil) }
            if msg != nil { self.buttons.forEach { $0.isEnabled = false } } else { self.refresh() }
        }
    }

    /// Changes the status line during an action (a step, download progress); the buttons stay as they are.
    func setStatus(_ s: String) { onMain { self.status.stringValue = s } }

    func alert(_ msg: String, _ info: String, _ buttons: [String], inline: Bool = false) -> String {
        onMainSync { Sheet(title: msg, message: info, buttons: buttons, inline: inline).run(on: window).choice }
    }

    /// A simple confirmation naming the action, e.g. "Install Stems Cache?" [Install] [Cancel],
    /// with `info` under it if any.
    func confirm(_ action: String, _ verb: String, _ info: String = "") -> Bool { alert(action + "?", info, [verb, "Cancel"]) == verb }

    /// One line describing what is installed, with the model's checksum (slow: run in the background).
    func state() -> String {
        let model = sha256(modelDir + "/hdemucs.onnx")?.prefix(12) ?? ""
        return "rekordbox \(rekordboxVersion()), engine \(engineVersion()), model \(model), bridge \(stemsCachePresent() ? "yes" : "no"), chosen \(chosen())"
    }
}

/// Closing the window quits the app: not while an action runs.
extension Controller: NSWindowDelegate {
    func windowShouldClose(_ sender: NSWindow) -> Bool { !actionRunning }
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

/// The Demucs v4 light: on or off, as rekordbox's model folder says.
func plusLight(installed: Bool) -> (look: LightLook, tip: String) {
    installed ? (.on, "Demucs v4 is on.") : (.off, "Demucs v4 is off.")
}

/// The Stems Cache light. Installed (`installed`: the bridge is in rekordbox) but not saving stems
/// right now (limited), with the tip saying why, when rekordbox has its own model (`plusOn` false)
/// and: rekordbox has no STEMS Engine yet (`engine` ""); this account didn't choose Stems Cache
/// (`chosen`: another account installed it); this account turned rekordbox's own model off in
/// config.ini (`rekordboxModel`); the installed bridge is older than saving rekordbox's own model's
/// stems (`bridgeModels` nil); or it doesn't list that model (`model`, its checksum; nil while
/// unknown: not judged). With either model, also when it stopped saving for lack of space
/// (`pausedFloor`: the floor its disk is under).
func cacheLight(installed: Bool, plusOn: Bool, chosen: Bool, rekordboxModel: Bool, bridgeModels: [String]?, model: String?,
                engine: String, pausedFloor: Int64?) -> (look: LightLook, tip: String) {
    guard installed else { return (.off, "Stems Cache is off.") }
    if !plusOn {
        if engine.isEmpty { return (.limited, "Stems Cache starts saving stems once rekordbox has downloaded its STEMS Engine.") }
        if !chosen { return (.limited, "Stems Cache was installed from another account on this Mac. Click Install Stems Cache to use it here.") }
        if !rekordboxModel { return (.limited, "Stems Cache doesn't save the stems of rekordbox's own model: rekordbox_model is 0 in config.ini.") }
        guard let list = bridgeModels else {
            return (.limited, "Stems Cache needs an update to save the stems of rekordbox's own model. Click Reinstall Stems Cache.")
        }
        if let m = model, !list.contains(m) {
            return (.limited, "Stems Cache doesn't support STEMS Engine \(engine) yet. When an RB Stems Plus update supports it, click Reinstall Stems Cache.")
        }
    }
    if let floor = pausedFloor { return (.limited, cachePausedTip(floor)) }
    return (.on, "Stems Cache is on.")
}
