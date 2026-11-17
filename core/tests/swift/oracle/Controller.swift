// The app's window: three toolbar tabs (PrepareView.swift): Status (the two status lights and the
// prepared playlists), Prepare, and Manage, built here: each feature's actions in the left
// column, the log on the right. Also the helpers every action uses (log, busy state, dialogs).
// The actions themselves are in Actions.swift, the admin runs in Admin.swift.
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
    /// each feature's own Reinstall, and Stems Cache's Settings… (the Manage tab)
    var reinstallPlusBtn = NSButton(), reinstallCacheBtn = NSButton(), settingsBtn = NSButton()
    var tabs: MainTabs!
    var statusTab: StatusTab!
    private var seenLocation: String?
    /// prepareChanged came during an action: its end checks, whatever kind of action it was
    private var changedDuringAction = false
    /// only coming back to the app came during a wait: its end checks quietly
    private var quietDuringAction = false
    var prepareTab: PrepareTab!
    let work = DispatchQueue(label: "rbstemsplus.work")
    var actionRunning = false
    /// The action that is changing things, recorded in the busy marker (see start in Actions.swift).
    var currentAction: Action?
    /// Set once the start-up sheets (welcome, resume) are done: the watcher's request waits for them.
    var startupDone = false
    /// Set while the watcher's request waits for rekordbox to be closed (its dialog is up): a
    /// reopen or activation meanwhile doesn't ask again.
    var requestWaiting = false
    /// Set once this run has asked "Stems Cache update": a payload.json downloaded later in the
    /// run doesn't ask again (Not Now holds until the next launch).
    var cacheUpdateAsked = false
    /// A line shown under the reasons in the status line between actions (e.g. an update notice).
    var notice: String?
    /// Shown in the status line for this run when 1.0's saved copy of Pioneer's model turned out
    /// to be ours (Models.swift, migrateLegacy).
    var modelNotice: String?
    /// What a deletion in the stems' location left (LocationActions.swift), for the status line.
    var locationNotice: String?
    /// The watcher's LaunchAgent is there but launchd won't run it (Actions.swift, watcherOff).
    var watcherIsOff = false
    /// "Update RB Stems Plus" in the app menu, shown when a newer RB Stems Plus is out (Update.swift).
    var updateItem: NSMenuItem?

    override init() {
        window = NSWindow(contentRect: NSRect(origin: .zero, size: tabSizes[Tab.manage.rawValue]),
                          styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        super.init()
        window.delegate = self
        window.title = "RB Stems Plus" + titleNote
        // two features, each installed on its own; none is the default button (neither is
        // recommended over the other)
        let installPlus = NSButton(title: "Install Demucs v4", target: self, action: #selector(installPlusAction))
        let installCache = NSButton(title: "Install Stems Cache", target: self, action: #selector(installCacheAction))
        let reinstall = NSButton(title: "Reinstall", target: self, action: #selector(reinstallAction))
        let reinstallPlus = NSButton(title: "Reinstall Demucs v4", target: self, action: #selector(reinstallPlusAction))
        let reinstallCache = NSButton(title: "Reinstall Stems Cache", target: self, action: #selector(reinstallCacheAction))
        let settings = NSButton(title: "Settings…", target: self, action: #selector(showSettings))
        let uninstallPlus = NSButton(title: "Uninstall Demucs v4", target: self, action: #selector(uninstallStemsPlus))
        let uninstall = NSButton(title: "Uninstall Stems Cache", target: self, action: #selector(uninstallCache))
        let uninstallAll = NSButton(title: "Uninstall RB Stems Plus Completely", target: self, action: #selector(uninstallEverything))
        // in the window: each feature's own; the combined Reinstall (the watcher's request, an
        // interrupted reinstall) and Uninstall Completely (the app menu) aren't shown
        buttons = [installPlus, reinstallPlus, uninstallPlus, installCache, reinstallCache, settings, uninstall]
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
        statusTab = StatusTab(self)
        prepareTab = PrepareTab(self)
        tabs = MainTabs()
        tabs.window = window
        tabs.tabStyle = .toolbar
        tabs.transitionOptions = []
        for (vc, name, symbol) in [(statusTab! as NSViewController, "Status", "checkmark.circle"), (prepareTab! as NSViewController, "Prepare", "music.note.list"),
                                   (manage, "Manage", "wrench.and.screwdriver")] {
            let item = NSTabViewItem(viewController: vc)
            item.label = name
            item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: name)
            tabs.addTabViewItem(item)
        }
        window.contentViewController = tabs
        if #available(macOS 11.0, *) { window.toolbarStyle = .preference }
        #if TEST_BUILD && CORE_LINKED
        // the AppKit shell over the core (app/Shell, a TEST app with shell=core): its tabs in place
        // of these; the core renders, shows the window and runs the start-up instead of the below
        if let setUp = coreShellSetUp {
            setUp(self)
            try? FileManager.default.createDirectory(atPath: originalsDir, withIntermediateDirectories: true)
            return
        }
        #endif
        // Status, or Manage the first time (nothing installed yet)
        let first: Tab = stemsPlusPresent() || stemsCachePresent() ? .status : .manage
        tabs.selectedTabViewItemIndex = first.rawValue
        tabs.tabView(tabs.tabView, didSelect: tabs.tabViewItems[first.rawValue])
        try? FileManager.default.createDirectory(atPath: originalsDir, withIntermediateDirectories: true)
        refresh()                      // buttons in their final state before the window appears
        window.center()
        window.makeKeyAndOrderFront(nil)
        log("started: RB Stems Plus \(versionText(shown: currentShownVersion()))\(titleNote), macOS \(ProcessInfo.processInfo.operatingSystemVersionString), user \(NSUserName()), bundle \(Bundle.main.bundlePath)\(bundleProblem(Bundle.main.bundlePath, executable: Bundle.main.executablePath).map { " (\($0))" } ?? "")")
        startupChecks()
    }

    /// Runs once the window is up: the state line (in the background), then, once the app's run
    /// loop runs (sheets need it), an action left half-done by the last run, then the watcher's
    /// request.
    func startupChecks() {
        // where the stems are saved, measured in the background from now on (LocationMonitor)
        LocationMonitor.shared.onChange = { [weak self] in
            guard let self = self else { return }
            #if TEST_BUILD
            probeMark("trigger.location")
            #endif
            self.refresh()
            // the first measurement, the disk under its floor or back, a drive gone or back: what
            // the Prepare and Status tabs check depends on them
            let s = LocationMonitor.shared.snapshot()
            // ... and the light's text, which they show as the reason they can't check
            let seen = "\(s.status == nil) \(s.pausedFloor ?? -1) \(s.status.flatMap(folderInUse) ?? "") \(s.lightTip ?? "")"
            if seen != self.seenLocation { self.seenLocation = seen; self.prepareChanged(quiet: true) }
        }
        LocationMonitor.shared.startObserving()
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
            // This Mac's stems left after a switch to a drive, under 3.3.3's conditions only
            self.retryThisMacDeletion()
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
            self.offerCacheUpdate()                             // skipped while an action runs
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
        #if TEST_BUILD && CORE_LINKED
        if coreShellSetUp != nil { return }        // the shell over the core (app/Shell): the core renders these views
        #endif
        onMain {
            #if TEST_BUILD
            probeMark("refresh.begin"); defer { probeMark("refresh.end") }
            #endif
            let plus = stemsPlusPresent(), cache = stemsCachePresent(), c = chosen(), cfg = readConfig()
            // where the stems are saved, and its disk's free-space floor as the bridge measures it
            // (only while it's on in config.ini): measured in the background (LocationMonitor)
            let loc = LocationMonitor.shared.snapshot()
            let paused = cache && cfg.enabled ? loc.pausedFloor : nil
            // with rekordbox's own model: whether the installed bridge saves its stems (by checksum,
            // worked out in the background the first time)
            let own = cache && !plus
            let light = cacheLight(installed: cache, plusOn: plus, chosen: c.cache, rekordboxModel: cfg.rekordboxModel,
                                   bridgeModels: own ? bridgeCacheModels() : nil,
                                   model: own ? currentModelSHA(done: { self.refresh() }) : nil,
                                   engine: engineVersion(), pausedFloor: paused, location: loc.lightTip, drive: loc.drive)
            let pl = plusLight(installed: plus)
            self.statusTab?.setTips(plus: pl.tip, cache: light.tip)
            for (icon, label, light) in [(self.plusIcon, self.plusLabel, pl), (self.cacheIcon, self.cacheLabel, light)] {
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
            self.reinstallPlusBtn.isEnabled = plus && c.model && plusWhy == nil
            self.reinstallCacheBtn.isEnabled = cache && c.cache && cacheWhy == nil
            self.settingsBtn.isEnabled = true
            self.uninstallPlusBtn.isEnabled = plus                     // rekordbox's model back: no password
            self.uninstallCacheBtn.isEnabled = traces && userIsAdmin
            for b in self.buttons where ![self.installPlusBtn, self.installCacheBtn, self.reinstallBtn, self.uninstallPlusBtn, self.uninstallCacheBtn,
                                          self.reinstallPlusBtn, self.reinstallCacheBtn].contains(b) { b.isEnabled = true }
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
            if let n = self.locationNotice { lines.append(n) }
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
            #if TEST_BUILD
            if msg == nil { probeMark("trigger.actionEnd") }
            #endif
            self.actionRunning = msg != nil
            // not a short wait (inBackground), unless something changed during it
            let ended = msg == nil && (self.currentAction != nil || self.changedDuringAction)
            let quietly = msg == nil && !ended && self.quietDuringAction
            if msg == nil { self.currentAction = nil; self.changedDuringAction = false; self.quietDuringAction = false }
            setBusyMarker(self.currentAction)
            self.status.stringValue = msg ?? ""
            if msg != nil { self.spinner.startAnimation(nil) } else { self.spinner.stopAnimation(nil) }
            if msg != nil { self.buttons.forEach { $0.isEnabled = false } } else { self.refresh() }
            if ended { self.prepareChanged() } else if quietly { self.prepareChanged(quiet: true) } else { self.prepareTab?.update() }
        }
    }

    /// busy for work that has just started and takes a while: the status line and spinner are on
    /// the Manage tab, so it comes to the front, from any tab.
    func busyInFront(_ msg: String) {
        onMain { self.showTab(.manage) }
        busy(msg)
    }

    /// Something the Prepare and Status tabs checked may have changed: an install, an uninstall,
    /// clearing saved stems, the settings, the location, or coming back to the app. A tab that isn't
    /// in front checks again when it is.
    /// `quiet`: coming back to the app or the window, where usually nothing changed: the tab
    /// showing checks again without clearing what it shows. Anything this app did is shown checked.
    func prepareChanged(quiet: Bool = false) {
        onMain {
            if !quiet { self.prepareTab?.stale = true; self.statusTab?.forget() }
            // not during an action: a password prompt makes the app inactive and active again, and
            // its end checks (quietly if only coming back to the app came meanwhile)
            if self.actionRunning { if quiet { self.quietDuringAction = true } else { self.changedDuringAction = true } }
            if let t = self.prepareTab, inFront(t), !self.actionRunning {
                t.reloadList(quiet: quiet && !t.stale)
                t.stale = false
            } else { self.prepareTab?.update() }
            self.statusTab?.reloadIfShowing()
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

/// Closing the window quits the app: not while an action runs; while a playlist is being
/// prepared, only once the user agrees to stop it.
extension Controller: NSWindowDelegate {
    func windowDidDeminiaturize(_ notification: Notification) { prepareChanged(quiet: true) }   // a tab now in front: nothing else says so
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if actionRunning { return false }
        return confirmStopPreparing("Quit")
    }

    /// While a playlist is being prepared: asks to stop it (`verb`: the button, e.g. "Quit"),
    /// then stops it and waits for its set to be written. True when nothing is being prepared now.
    func confirmStopPreparing(_ verb: String) -> Bool {
        guard let p = prepareTab, p.isRunning else { return true }
        let name = p.runPlaylist?.name ?? ""
        guard alert("“\(name)” is being prepared", "Stop preparing it? The tracks done so far stay prepared.", ["Stop and \(verb)", "Cancel"]) != "Cancel" else { return false }
        log("prepare: stopped for \(verb.lowercased())")
        return p.stopAndWait()
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
/// (`pausedFloor`: the floor its disk is under; `drive`: the chosen drive's name, nil on this
/// Mac), or when where it saves stems isn't usable now (`location`: the text, locationLight).
func cacheLight(installed: Bool, plusOn: Bool, chosen: Bool, rekordboxModel: Bool, bridgeModels: [String]?, model: String?,
                engine: String, pausedFloor: Int64?, location: String? = nil, drive: String? = nil) -> (look: LightLook, tip: String) {
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
    if let tip = location { return (.limited, tip) }
    if let floor = pausedFloor { return (.limited, cachePausedTip(floor, drive: drive)) }
    return (.on, "Stems Cache is on.")
}
