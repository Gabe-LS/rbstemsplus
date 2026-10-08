// What the buttons do: install, reinstall and uninstall Demucs v4 (our model, no admin) and
// Stems Cache (the bridge, one admin run), the complete uninstall, the watcher's LaunchAgent,
// and the watcher's "Reinstall now" request.
import AppKit

extension Controller {
    // MARK: install and reinstall

    /// Installs (or, with force, refreshes) the chosen parts, then records the choice and makes
    /// sure the watcher is installed. Stems Cache already in rekordbox (another account installed
    /// it) is only turned on for this account, unless forced: nothing downloaded, no password.
    /// Choosing Stems Cache also turns on saving rekordbox's own model's stems here
    /// (rekordbox_model). `reinstall`: worded as a reinstall (by default, when forced). Runs on the
    /// work queue.
    func install(model wantModel: Bool, cache wantCache: Bool, force: Bool, reinstall: Bool? = nil) {
        var wantModel = wantModel, wantCache = wantCache
        let optInOnly = wantCache && !force && stemsCachePresent()
        // a reinstall says so in its failures: "Reinstall failed", "couldn't be reinstalled"
        let again = reinstall ?? force
        let failedTitle = again ? "Reinstall failed" : "Install failed", done = again ? "reinstalled" : "installed"
        let offline = { () -> Void in
            _ = self.alert("Download failed", "Nothing was changed. Check your internet connection and try again.", ["OK"])
            self.busy(nil)
        }
        // payload.json: the latest. Offline, Demucs v4 may use the last good one (its files may
        // be here); Stems Cache only if that one was downloaded within the last 7 days and its
        // bridge is here and verified (cacheFromSavedPayload). Never after a refused download.
        setStatus("Checking for the latest files…")
        var fresh: Manifest?, unverified = false
        switch fetchManifestChecked(log) {
        case .ok(let m): fresh = m
        case .unverified: unverified = true
        case .offline: break
        }
        // a payload.json that didn't verify: say so, not "check your connection"
        let failed = unverified
            ? { () -> Void in _ = self.alert("Download failed", unverifiedDownloadMessage, ["OK"]); self.busy(nil) }
            : offline
        guard let m = fresh ?? savedManifest() else { log("no payload.json: nothing changed"); failed(); return }
        let age = manifestAge()
        if fresh == nil && wantCache && !optInOnly
            && !cacheFromSavedPayload(unverified: unverified, age: age, bridgeVerified: verified(m.bridgePath, sha: m.bridge.sha256, size: nil)) {
            log("Stems Cache needs payload.json downloaded now, or offline one downloaded within the last 7 days with its files here: nothing changed")
            failed(); return
        }
        log(fresh != nil ? "using the payload.json just downloaded (payload \(m.payloadVersion))"
                         : "offline: using the saved payload.json (payload \(m.payloadVersion), downloaded \(Int((age ?? 0) / 3600)) hours ago, signed with the \(m.signedBy ?? "?") key)")
        onMain { self.updateFound(m) }                  // the notice follows the latest payload.json
        if wantModel, let why = stemsPlusBlocked() { log("Demucs v4 not installed: \(why)"); wantModel = false }
        if wantCache && !optInOnly, let why = stemsCacheBlocked() { log("Stems Cache not installed: \(why)"); wantCache = false }
        var c = chosen()
        let modelFile = modelDir + "/hdemucs.onnx"
        let copyModel = wantModel && (force || sha256(modelFile) != m.model.sha256)
        // enough room for the downloads, the copies and the backups, before anything starts
        if let gb = spaceShort(model: copyModel ? m : nil, cache: wantCache && !optInOnly) {
            log("not enough free space (about \(gb) GB needed): nothing changed")
            _ = alert("Low disk space", "RB Stems Plus needs about \(gb) GB free. Nothing was changed. Free up some space and try again.", ["OK"])
            busy(nil); return
        }
        // every file the action needs, downloaded and verified before anything changes
        var got = Fetched.ok
        if copyModel {
            got = fetchAsset(m.model.file, to: m.modelPath, sha: m.model.sha256, size: m.model.size,
                             what: "the Demucs v4 model", status: setStatus, log: log)
        }
        if got == .ok && wantCache && !optInOnly {
            got = fetchAsset(m.bridge.file, to: m.bridgePath, sha: m.bridge.sha256, size: nil,
                             what: "Stems Cache", status: setStatus, log: log)
        }
        switch got {
        case .ok: break
        case .offline: log("download failed: nothing changed"); offline(); return
        case .noSpace:
            log("download: the disk is full: nothing changed")
            _ = alert("Low disk space", "The disk is full. Nothing was changed. Free up some space and try again.", ["OK"])
            busy(nil); return
        case .damaged:
            log("download damaged: nothing changed")
            _ = alert("Download failed", "A file didn't download correctly. Nothing was changed. Try again later.", ["OK"])
            busy(nil); return
        }
        if FileManager.default.fileExists(atPath: m.modelPath) { prunePayloads(keep: m) }   // the last good payload is now this one
        // the downloads may have taken minutes: rekordbox may have been opened meanwhile
        guard rekordboxClosed() else { log("rekordbox is open: nothing changed"); busy(nil); return }
        if wantModel {
            if copyModel {
                // Pioneer's model is saved first (by checksum, with its STEMS Engine version), and
                // only then replaced (Models.swift)
                setStatus("Installing Demucs v4…")
                let r = installedModels(log).installOurs(from: m.modelPath, sha: m.model.sha256)
                if !r.ok {
                    log("Demucs v4 not installed: \(r.message)")
                    _ = alert(failedTitle, "Demucs v4 couldn't be \(done). \(r.message) Nothing was changed.", ["OK"])
                    busy(nil); return
                }
                c.model = true
            } else { log("Demucs v4: already in place"); c.model = true }
        }
        if wantCache && optInOnly {
            log("Stems Cache: already in rekordbox (installed from another account): turned on for this account")
            c.cache = true
        } else if wantCache {
            setStatus("Preparing Stems Cache…")
            // Pioneer's library the bridge will load: in rekordbox, or (bridge already in) the
            // root-owned copy. Its checksum, taken now that it has passed the compatibility check,
            // goes into the script: root copies only that exact file.
            let ort = stemsCachePresent() ? rootOrt : rbLib
            let ortSha = sha256(ort) ?? "unreadable"
            log("Pioneer's ONNX Runtime: \(ort) \(ortSha.prefix(12))")
            // the bridge goes to root as bytes, read once here; root checks its own copy again
            if let bridge = FileManager.default.contents(atPath: m.bridgePath), sha256(data: bridge) == m.bridge.sha256 {
                let script = cacheInstallScript(bridge: bridge, bridgeSha: m.bridge.sha256, ortSha: ortSha, force: force)
                if runWithAppManagement(script, name: again ? "reinstall" : "install", failure: "Stems Cache couldn't be \(done)."), stemsCachePresent() { c.cache = true }
            } else {
                log("Stems Cache not installed: the downloaded bridge changed after it was checked")
                _ = alert(failedTitle, "Stems Cache couldn't be \(done). See the log in this window.", ["OK"])
            }
        }
        setChosen(model: c.model, cache: c.cache)
        if wantCache && c.cache {
            log("Stems Cache: saving rekordbox's own model's stems too (rekordbox_model=1): \(writeRekordboxModel(true) ? "ok" : "FAILED")")
        }
        if c.model || c.cache { installWatcher() }
        log("state: \(state())"); busy(nil)
    }

    /// If there isn't room for an action, about how many GB it needs (rounded up to 0.1), else nil.
    /// Demucs v4: the model's download (what is still missing of it), its copy in rekordbox and
    /// Pioneer's saved model. Stems Cache: root's backup of rekordbox's executable and library
    /// (once per version), the library's copy for the bridge, the staging copy and the temporary
    /// copy codesign makes of the executable. Plus 200 MB to spare.
    func spaceShort(model m: Manifest?, cache: Bool) -> String? {
        func size(_ p: String) -> Int64 { Int64(fileSize(p) ?? 0) }
        var inHome: Int64 = 0, inRoot: Int64 = 0
        if let m = m, let s = m.model.size {
            let have = fileSize(m.modelPath) == s ? Int64(s) : size(m.modelPath + ".part")
            inHome += Int64(s) - have + Int64(s) + size(modelDir + "/hdemucs.onnx")
        }
        if cache {
            let exe = size(rbApp + "/Contents/MacOS/rekordbox"), lib = size(rbLib)
            // a saved set is complete only with its SHA256SUMS, written last: without it, root saves the files again
            let backedUp = FileManager.default.fileExists(atPath: rootDir + "/pioneer/\(rekordboxVersion())/SHA256SUMS")
            inRoot += (backedUp ? 0 : exe + lib) + 2 * lib + exe
        }
        let spare: Int64 = 200 << 20
        func device(_ path: String) -> Int? { (try? FileManager.default.attributesOfFileSystem(forPath: path))?[.systemNumber] as? Int }
        // one volume on most Macs: then the two add up
        let checks = device(home) == device("/Library") ? [(home, inHome + inRoot)] : [(home, inHome), ("/Library", inRoot)]
        for (path, need) in checks where need > 0 {
            guard let f = freeBytes(path) else { continue }          // unknown: let the copy say so
            if f < need + spare { return String(format: "%.1f", (Double(need + spare) / Double(1 << 30) * 10).rounded(.up) / 10) }
        }
        return nil
    }

    /// The LaunchAgent: the watcher helper (RB Stems Plus Watcher.app, inside the app) with
    /// --watch, started when rekordbox's app, its library, the model folder or rekordbox's
    /// settings folder change. macOS's background-item notice and Login Items name the helper,
    /// "RB Stems Plus Watcher". Only from a valid bundle with a valid helper (Safety.swift):
    /// launchd starts it by that path. The plist is replaced by a rename, never written through a
    /// link. Registered again (bootout, then bootstrap) whenever the plist or the helper's code
    /// changed since it was last registered (watcherRegisteredFile), so launchd never starts new
    /// code under an old registration.
    func installWatcher() {
        defer { updateWatcherState() }
        guard let bundle = ownBundle() else { log("watcher not installed: RB Stems Plus isn't in a valid app (\(Bundle.main.bundlePath))"); return }
        let exe = watcherExecutable(bundle)
        guard ownWatcher() == exe else {
            log("watcher not installed: \(helperProblem(watcherHelper(bundle), executable: exe) ?? "its executable is missing") (\(watcherHelper(bundle)))"); return
        }
        let paths = [rbApp + "/Contents/Info.plist", rbApp + "/Contents/Frameworks", modelDir, rbSettingsDir]
        let plist: [String: Any] = ["Label": agentLabel, "ProgramArguments": [exe, "--watch"],
                                    "WatchPaths": paths, "ThrottleInterval": 10]
        guard let data = try? PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0) else { return }
        let record = watcherIdentity(watcherHelper(bundle)).map { registrationRecord(exe: exe, identity: $0) }
        let recorded = FileManager.default.contents(atPath: watcherRegisteredFile).map { String(decoding: $0, as: UTF8.self) }
        if FileManager.default.contents(atPath: agentPlist) == data && (record == nil || record == recorded)
            && runTool("/bin/launchctl", ["print", "\(guiDomain)/\(agentLabel)"]).0 == 0 { return }
        try? FileManager.default.createDirectory(atPath: home + "/Library/LaunchAgents", withIntermediateDirectories: true)
        runTool("/bin/launchctl", ["bootout", "\(guiDomain)/\(agentLabel)"])
        safeRemove(watcherRegisteredFile)              // until this registration has worked
        writeAtomically(data, to: agentPlist)
        let ok = runTool("/bin/launchctl", ["bootstrap", guiDomain, agentPlist]).0 == 0
        if ok, let r = record { writeAtomically(Data(r.utf8), to: watcherRegisteredFile) }
        log("watcher: \(ok ? "installed" : "COULD NOT BE INSTALLED") (\(watcherName), code \(record.map { String($0.split(separator: "\n").last ?? "") } ?? "unknown"))")
    }

    func removeWatcher() {
        guard FileManager.default.fileExists(atPath: agentPlist) else { return }
        runTool("/bin/launchctl", ["bootout", "\(guiDomain)/\(agentLabel)"])
        safeRemove(watcherRegisteredFile)
        log("watcher removed: \(safeRemove(agentPlist) ? "ok" : "FAILED")")
        updateWatcherState()
    }

    /// Whether the reinstall reminders are off (watcherOff), for the status line: on the work
    /// queue, so never while installWatcher or removeWatcher is replacing the LaunchAgent. Never
    /// changes launchd's state: turning the reminders off is the user's choice (Login Items).
    func checkWatcherOn() { work.async { self.updateWatcherState() } }

    /// The same, now (work queue). Only on macOS 13 and later, where the user turns background
    /// items off in System Settings › General › Login Items, as the status line says.
    func updateWatcherState() {
        var off = false
        if #available(macOS 13, *) {
            off = watcherOff(plistThere: fileType(agentPlist) != nil,
                             loaded: runTool("/bin/launchctl", ["print", "\(guiDomain)/\(agentLabel)"]).0 == 0,
                             disabled: launchdDisabled(runTool("/bin/launchctl", ["print-disabled", guiDomain]).1, label: agentLabel))
        }
        onMain {
            guard off != self.watcherIsOff else { return }
            self.watcherIsOff = off
            if off { self.log("watcher: installed but not running (turned off in Login Items, or not loaded): reinstall reminders are off") }
            self.refresh()
        }
    }

    /// At launch, before anything else: the LaunchAgent must start this app's watcher helper, as
    /// registered with the helper's current code. One that starts anything else (an older copy,
    /// a moved app, 1.0's agent starting the app itself) is rewritten; one whose code changed
    /// since it was registered (an update, the install command) is registered again; from a copy
    /// that isn't a valid app, it is removed unless it starts another valid copy.
    func checkWatcher() {
        guard fileType(agentPlist) != nil else { return }
        let plist = FileManager.default.contents(atPath: agentPlist).flatMap { try? PropertyListSerialization.propertyList(from: $0, format: nil) } as? [String: Any]
        let target = (plist?["ProgramArguments"] as? [String])?.first
        let own = ownWatcher()
        let current = own.flatMap { exe in ownBundle().flatMap { watcherIdentity(watcherHelper($0)) }.map { registrationRecord(exe: exe, identity: $0) } }
        let recorded = FileManager.default.contents(atPath: watcherRegisteredFile).map { String(decoding: $0, as: UTF8.self) }
        switch agentDecision(target: target, own: own, recorded: recorded, current: current) {
        case .keep: break
        case .rewrite where target == own:
            log("watcher: its code changed since it was registered\(recorded == nil ? " (no record)" : ""): registering it again"); installWatcher()
        case .rewrite: log("watcher started \(target ?? "nothing"): pointing it at this app's \(watcherName)"); installWatcher()
        case .remove: log("watcher started \(target ?? "nothing"), not a valid app: removed"); removeWatcher()
        }
    }

    /// At launch: what a run that was stopped left behind, by exact name: our temporary model
    /// copies in rekordbox's model folder, half-saved originals (<sha256>.onnx.part), and an
    /// update's staging apps that carry our bundle ID.
    /// Not while another copy of the app is changing things.
    func removeLeftovers() {
        guard !appBusy() else { return }
        let parts = ((try? FileManager.default.contentsOfDirectory(atPath: originalsDir)) ?? [])
            .filter { $0.hasSuffix(".onnx.part") && isSHA256(String($0.dropLast(10))) }.map { originalsDir + "/" + $0 }
        for p in [modelDir + "/.hdemucs.new", modelDir + "/.hdemucs.restore"] + parts where fileType(p) != nil {
            log("leftover \((p as NSString).lastPathComponent) removed: \(safeRemove(p) ? "ok" : "FAILED")")
        }
        // the install command stops the app before it stages, so its names are leftovers here too
        for p in [updateStagingApp, updateOldApp, bootstrapNewApp, bootstrapOldApp]
        where p != Bundle.main.bundlePath && bundleIDOf(p) == bundleID {
            log("leftover \(p) removed: \(safeRemove(p) ? "ok" : "FAILED")")
        }
    }

    /// Checks before any action. Returns false (after telling the user) if it can't start now:
    /// while Pioneer's installer or rekordbox's updater runs, or rekordbox is open (in any account
    /// on this Mac: the process check sees them all).
    /// Seam: resume after quit (wait for rekordbox to quit, then carry on) replaces the alert here.
    func preflight() -> Bool {
        if pioneerUpdating(), let p = Controller.rootProblem(RootExit.installerRunning) { _ = alert(p.title, p.body, ["OK"]); return false }
        if rekordboxOpen() { _ = alert("rekordbox open", "Quit rekordbox (also in other accounts on this Mac), then try again.", ["OK"]); return false }
        return true
    }

    /// During an action, right before rekordbox or its model is changed: the user may have opened
    /// rekordbox during a download, the password or the App Management steps. Asks until it is
    /// closed; false if the user cancels. Any thread.
    func rekordboxClosed() -> Bool {
        while rekordboxOpen() {
            onMain { self.spinner.stopAnimation(nil) }
            let b = alert("rekordbox open", rekordboxOpenTryAgain, ["Try Again", "Cancel"])
            onMain { if self.actionRunning { self.spinner.startAnimation(nil) } }
            if b != "Try Again" { return false }
        }
        return true
    }

    /// Before an action the user asked for earlier (the watcher's Reinstall Now, Continue after
    /// "Action interrupted"): as rekordboxClosed, it asks until rekordbox is closed or the user
    /// cancels, instead of preflight's one-shot refusal. While Pioneer's installer or rekordbox's
    /// updater runs, it refuses as preflight does. True to start the action now. Main thread.
    func readyForQueuedAction() -> Bool {
        let r = queuedStart(updating: pioneerUpdating, rekordboxOpen: rekordboxOpen) {
            self.alert("rekordbox open", rekordboxOpenTryAgain, ["Try Again", "Cancel"]) == "Try Again"
        }
        switch r {
        case .start: return true
        case .updating:
            log("  rekordbox is updating: not started")
            if let p = Controller.rootProblem(RootExit.installerRunning) { _ = alert(p.title, p.body, ["OK"]) }
            return false
        case .cancelled:
            log("  rekordbox is open, the user cancelled: not started")
            return false
        }
    }

    // MARK: actions

    /// Starts an action that changes things: the busy marker names it (so the next launch can
    /// offer to finish it if the app quits half-way), the status line shows `status`, and `body`
    /// runs on the work queue. Main thread.
    func start(_ id: String, _ label: String, _ status: String, _ body: @escaping () -> Void) {
        currentAction = Action(id: id, label: label)
        busy(status)
        work.async(execute: body)
    }

    @objc func installPlusAction() {
        guard preflight(), confirm(installPlusBtn.title, "Install", installNote(installPlusBtn.title, reinstall: false, paused: nil)) else { return }
        startInstall("install-plus", installPlusBtn.title, model: true, cache: false)
    }

    @objc func installCacheAction() {
        // Stems Cache would start paused (its disk under the floor): said before it is installed
        guard preflight(), confirm(installCacheBtn.title, "Install", installNote(installCacheBtn.title, reinstall: false, paused: cachePausedFloor())) else { return }
        startInstall("install-cache", installCacheBtn.title, model: false, cache: true)
    }

    /// The busy marker records which parts (its id), so an interrupted install resumes only them.
    func startInstall(_ id: String, _ label: String, model: Bool, cache: Bool) {
        start(id, label, "Installing…") { self.install(model: model, cache: cache, force: false) }   // a part already in place is skipped
    }

    @objc func reinstallAction() {
        guard preflight(), confirm(reinstallBtn.title, "Reinstall", installNote(reinstallBtn.title, reinstall: true, paused: cachePausedFloor())) else { return }
        reinstallChosen()
    }

    /// Reinstalls exactly what the user chose, refreshing what is still there (the Reinstall button).
    func reinstallChosen() {
        let c = chosen()
        start("reinstall", reinstallBtn.title, "Reinstalling…") { self.install(model: c.model, cache: c.cache, force: true) }
    }

    /// Reinstalls only what the user chose and went missing (the watcher's "Reinstall Now"):
    /// Demucs v4 alone needs no password, and an intact Stems Cache isn't re-signed. False if nothing is
    /// missing (nothing started).
    @discardableResult
    func reinstallMissing() -> Bool {
        let m = missingParts(chosen: chosen(), plusPresent: stemsPlusPresent(), cachePresent: stemsCachePresent())
        guard m.model || m.cache else { log("  nothing is missing: nothing reinstalled"); return false }
        start("reinstall-missing", reinstallLabel(m), "Reinstalling…") { self.install(model: m.model, cache: m.cache, force: false, reinstall: true) }
        return true
    }

    @objc func uninstallStemsPlus() {
        guard preflight(), confirm(uninstallPlusBtn.title, "Uninstall", uninstallNote(uninstallPlusBtn.title)) else { return }
        startUninstallStemsPlus(uninstallPlusBtn.title)
    }

    /// Puts rekordbox's own model back: Stems Cache stays, and goes on saving stems with
    /// rekordbox's own model. No password.
    func startUninstallStemsPlus(_ label: String) {
        start("uninstall-plus", label, "Uninstalling…") {
            guard self.rekordboxClosed() else { self.log("rekordbox is open: Demucs v4 left in place"); self.busy(nil); return }
            let r = installedModels(self.log).restore()
            if r.ok {
                setChosen(model: false, cache: chosen().cache)
            } else if r.noSpace {
                _ = self.alert("Uninstall failed", "Demucs v4 couldn't be removed. \(r.message) Free up some space and try again.", ["OK"])
            } else {
                let b = self.alert("Uninstall failed", "Demucs v4 couldn't be removed. \(r.message) To get rekordbox's own stems model back, click Open Help.",
                                   ["OK", "Open Help"])
                if b == "Open Help" { self.openHelp(modelHelpURL(installedModels { _ in })) }
            }
            let c = chosen()
            if !c.model && !c.cache { self.removeWatcher() }            // nothing left to watch
            self.log("state: \(self.state())"); self.busy(nil)
        }
    }

    @objc func uninstallCache() {
        guard preflight(), confirm(uninstallCacheBtn.title, "Uninstall", uninstallNote(uninstallCacheBtn.title)) else { return }
        startUninstallCache(uninstallCacheBtn.title)
    }

    func startUninstallCache(_ label: String) {
        start("uninstall-cache", label, "Uninstalling Stems Cache…") {
            self.runUninstallCache(failure: "Stems Cache couldn't be removed.")
            if !stemsCachePresent() {
                setChosen(model: chosen().model, cache: false)
                if configSets("rekordbox_model") { writeRekordboxModel(false) }
            }
            self.log("state: \(self.state())"); self.busy(nil)
        }
    }

    @objc func uninstallEverything() {
        guard preflight() else { return }
        // the stems it saved, so the user sees what is freed
        let othersCache = cacheTraces() && !chosen().cache
        let noSavedModel = stemsPlusPresent() && installedModels { _ in }.savedOriginals().isEmpty
        guard alert("Uninstall RB Stems Plus?", uninstallEverythingMessage(othersCache: othersCache, noSavedModel: noSavedModel, stems: savedStemsSize()),
                    ["Uninstall", "Cancel"]) == "Uninstall" else { return }
        startUninstallEverything()
    }

    func startUninstallEverything() {
        start("uninstall-all", "Uninstall RB Stems Plus Completely", "Uninstalling RB Stems Plus…") {
            // Stems Cache, or only what it left in /Library after a rekordbox update removed it
            // (another account's may stay)
            var keepCache = false, removal = CacheRemoval.removed
            if cacheTraces() {
                if self.removeOtherAccountsCache() {
                    // rekordbox gone and the user keeps the copy of its files: nothing else goes
                    removal = self.runUninstallCache(failure: nil, rootFolderFailure: nil)
                    if removal == .keptRootFolder {
                        self.log("the copy of rekordbox's files stays: stopping, nothing else deleted"); self.busy(nil)
                        _ = self.alert("Uninstall stopped", keptRootFolderMessage, ["OK"])
                        return
                    }
                    if !stemsCachePresent() { setChosen(model: chosen().model, cache: false) }
                } else { keepCache = true }
            }
            if cacheTraces() && !keepCache {
                self.log("Stems Cache could not be removed: stopping, nothing else deleted"); self.busy(nil)
                if removal.userStopped { return }               // the user knows: no alert, as for every cancel
                _ = self.alert("Uninstall stopped", "Stems Cache couldn't be removed, so nothing else was deleted. \(cacheFailureReason(removal))", ["OK"])
                return
            }
            guard self.rekordboxClosed() else { self.log("rekordbox is open: stopping, nothing else deleted"); self.busy(nil); return }
            // our files (and Pioneer's saved model with them) go only once rekordbox's model is
            // verifiably not ours
            let models = installedModels(self.log)
            let r = models.restore()
            guard r.ok else {
                self.log("rekordbox's own model isn't back: stopping, nothing else deleted")
                self.offerRemoveAnyway(r, models)
                return
            }
            setChosen(model: false, cache: chosen().cache)
            self.removeOwnFiles("RB Stems Plus is uninstalled",
                                keepCache ? "rekordbox has its own stems model again. Stems Cache stays for the other account." : "rekordbox is back to how it was.",
                                help: nil, keepOriginals: false)
        }
    }

    /// Stems Cache is for the whole Mac. If this account never chose it, it was installed from
    /// another account: asks before removing it. True to remove it. Any thread.
    func removeOtherAccountsCache() -> Bool {
        if chosen().cache { return true }
        let b = alert("Stems Cache from another account", "Stems Cache was installed from another account on this Mac. Removing it turns it off in every account.", ["Remove", "Keep"])
        log("Stems Cache wasn't installed from this account: user chose \(b)")
        return b == "Remove"
    }

    /// Uninstall RB Stems Plus completely stopped before deleting anything, because rekordbox's
    /// own model couldn't be put back (`r`): says why, and offers to remove this account's own
    /// files anyway (removeAnywayChoice). Work queue.
    func offerRemoveAnyway(_ r: Outcome, _ models: Models) {
        switch removeAnywayChoice(r, models, ask: { self.alert($0, $1, $2) }, log: log) {
        case .stop: busy(nil)
        case .help(let url): busy(nil); openHelp(url)
        case .remove(let about, let keep):
            removeOwnFiles("RB Stems Plus is removed", about, help: keep ? removeAnywayHelpURL : noSavedModelHelpURL, keepOriginals: keep)
        }
    }

    /// The end of Uninstall RB Stems Plus completely, and of "Remove RB Stems Plus anyway": only
    /// this account's own files (the watcher, the support, cache and log folders, then the app
    /// once this process has exited), never rekordbox or the root folder. With `keepOriginals`
    /// the saved copies of Pioneer's model stay (removeOwnFolders). The app is deleted only if it
    /// is a valid app this account owns and can delete; otherwise the user is told. The final
    /// alert says `about` (what rekordbox has now) and what stays (ownFilesRemovedText), with
    /// "Open Help" (opening `help`) if there is one. Then quits.
    func removeOwnFiles(_ title: String, _ about: String, help: String?, keepOriginals: Bool) {
        removeWatcher()
        log("final state: \(state())")
        // Stems Cache still here though this account didn't choose it: another account's
        let othersCache = cacheTraces() && !chosen().cache, cacheLink = cacheIsLink()
        removeOwnFolders(keepOriginals: keepOriginals)                    // nothing is logged after this
        let bundle = ownBundle(), owned = bundle.map { ownedByThisAccount($0) } ?? false
        let me = bundle.flatMap { b in owned && access((b as NSString).deletingLastPathComponent, W_OK) == 0 && access(b, W_OK) == 0 ? b : nil }
        let fate: AppFate = me != nil ? .deleted : bundle != nil && !owned ? .otherAccount : .cantDelete
        let b = alert(title, ownFilesRemovedText(about, app: fate, othersCache: othersCache, cacheLink: cacheLink), help != nil ? ["Done", "Open Help"] : ["Done"])
        if b == "Open Help", let u = help { openHelp(u) }
        if let me = me { afterExit(poll: "0.5", selfDeleteScript, [me, bundleID]) }
        ownQuit = true                                  // this quit waits for nothing
        DispatchQueue.main.async { NSApp.terminate(nil) }
    }

    /// Opens a troubleshooting page (a section of docs/troubleshooting.md) in the browser. Any thread.
    func openHelp(_ url: String) {
        if let u = URL(string: url) { onMainSync { _ = NSWorkspace.shared.open(u) } }
    }

    /// rekordbox is gone (moved, or deleted), so Pioneer's files can't go back into it: asks
    /// whether to remove the root folder (Pioneer's installer reinstalls rekordbox whole); a
    /// rekordbox that was only moved should be put back first. True to remove it. Any thread.
    func offerRootFolderCleanup() -> Bool {
        let b = alert("rekordbox missing", rootFolderCleanupMessage, ["Cancel", "Remove"])
        log("  user chose: \(b)")
        return b == "Remove"
    }

    /// Stems Cache out (removeCache): Pioneer's three files back and the root folder removed (only
    /// the latter if a rekordbox update already removed the bridge); once rekordbox is gone, the
    /// root folder alone, if the user agrees. `failure`: the alert's text if the root run fails,
    /// `rootFolderFailure` if removing the root folder alone does; nil if the caller says so itself.
    @discardableResult
    func runUninstallCache(failure: String?, rootFolderFailure: String? = "RB Stems Plus couldn't remove its copy of rekordbox's files.") -> CacheRemoval {
        removeCache(CacheRemovalSteps(
            rekordboxFolder: { fileType(rbFolder) != nil },
            rootFolder: { fileType(rootDir) != nil },
            uninstall: { self.runRoot(cacheUninstallScript(), name: "uninstall", failure: failure) },   // no "rekordbox missing" alert: asked below
            askRootFolder: offerRootFolderCleanup,
            // as removeRootFolder (Admin.swift), with the script's exit code kept for the caller
            removeRootFolder: { self.runRoot(removeRootFolderScript(), name: "remove-root-folder", failure: rootFolderFailure) },
            log: log))
    }

    // MARK: an action the last run left half-done

    /// If the app quit half-way through an action (e.g. the user chose "Quit & Reopen" when macOS
    /// asked about App Management), offers to finish it. At launch; returns whether it resumed.
    func checkResume() -> Bool {
        guard let left = leftoverAction() else { return false }
        runTool("/usr/bin/sudo", ["-k"])                       // the last run quit before it could forget the password
        log("the last run quit during: \(left.label) (\(left.id))")
        let b = alert("Action interrupted", "RB Stems Plus quit before \"\(left.label)\" was done. Continue it now?", ["Continue", "Cancel"])
        log("  user chose: \(b)")
        // the marker stays until the action starts (start replaces it) or the user says no; while
        // rekordbox is updating it stays too, and the next launch offers the action again
        guard b == "Continue" else { setBusyMarker(nil); return false }
        if !readyForQueuedAction() {
            if !pioneerUpdating() { setBusyMarker(nil) }
            return false
        }
        switch left.id {
        case "install": startInstall(left.id, left.label, model: true, cache: true)       // 1.0's: both
        case "install-plus": startInstall(left.id, left.label, model: true, cache: false)
        case "install-cache": startInstall(left.id, left.label, model: false, cache: true)
        case "reinstall": reinstallChosen()
        case "reinstall-missing": if !reinstallMissing() { setBusyMarker(nil); return false }
        case "uninstall-plus": startUninstallStemsPlus(left.label)
        case "uninstall-cache": startUninstallCache(left.label)
        case "uninstall-all": startUninstallEverything()
        default: log("  unknown action: not resumed"); setBusyMarker(nil); return false
        }
        return true
    }

    // MARK: the watcher's request

    /// Runs the reinstall the watcher asked for ("Reinstall now"), at launch or when reopened.
    /// A request over an hour old (the app didn't open then), or one for something that is no
    /// longer missing (the user already reinstalled), is dropped: no password or re-sign for nothing.
    /// While rekordbox is open it asks until it is closed (readyForQueuedAction); the request is
    /// kept until the reinstall starts or the user says no. Main thread.
    func checkRequest() {
        guard startupDone, !actionRunning, !requestWaiting, let age = reinstallRequestAge() else { return }
        let c = chosen()
        if age > 3600 { takeReinstallRequest(); log("watcher's reinstall request is \(Int(age / 60)) minutes old: ignored"); return }
        let m = missingParts(chosen: c, plusPresent: stemsPlusPresent(), cachePresent: stemsCachePresent())
        if !m.model && !m.cache {
            takeReinstallRequest(); log("watcher asked to reinstall, but nothing is missing: ignored"); return
        }
        log("watcher asked to reinstall")
        requestWaiting = true
        let go = readyForQueuedAction()
        requestWaiting = false
        takeReinstallRequest()                       // started now, or turned down: handled either way
        guard go else { return }
        reinstallMissing()                           // what is missing now (rekordbox may have changed meanwhile)
    }
}

// MARK: - the watcher's Reinstall Now

/// What the watcher's Reinstall Now reinstalls: what this account chose and went missing.
func missingParts(chosen c: (model: Bool, cache: Bool), plusPresent: Bool, cachePresent: Bool) -> (model: Bool, cache: Bool) {
    (c.model && !plusPresent, c.cache && !cachePresent)
}

/// The action's label for those parts, as the Reinstall button names them.
func reinstallLabel(_ p: (model: Bool, cache: Bool)) -> String {
    "Reinstall " + (p.model && p.cache ? "Demucs v4 + Stems Cache" : p.cache ? "Stems Cache" : "Demucs v4")
}

// MARK: - offline installs

/// How old the saved payload.json may be for an offline Stems Cache install (by when it was downloaded).
let offlinePayloadMaxAge: TimeInterval = 7 * 86_400

/// Whether Stems Cache may be installed from the saved payload.json (signature-verified on every
/// read, savedManifest) when a fresh one couldn't be downloaded: only offline (never after a
/// download that didn't verify), only if it was downloaded within offlinePayloadMaxAge (`age`,
/// seconds ago; nil or in the future: no), and only if its bridge is here and matches its checksum.
/// That bounds how old a signed payload a process in the user's account could replay.
func cacheFromSavedPayload(unverified: Bool, age: TimeInterval?, bridgeVerified: Bool) -> Bool {
    guard !unverified, bridgeVerified, let a = age else { return false }
    return a >= 0 && a <= offlinePayloadMaxAge
}

// MARK: - the reinstall reminders

/// Whether the reinstall reminders are off: the watcher's LaunchAgent is there (`plistThere`) but
/// launchd won't run it, because the user turned RB Stems Plus Watcher off in Login Items (launchd then
/// marks it `disabled` and unloads it) or it couldn't be loaded (`loaded`: launchctl print found it).
func watcherOff(plistThere: Bool, loaded: Bool, disabled: Bool) -> Bool { plistThere && (disabled || !loaded) }

/// Whether `launchctl print-disabled <domain>`'s output marks `label` disabled: a line
/// `"<label>" => disabled` (`=> true` in older macOS versions).
func launchdDisabled(_ output: String, label: String) -> Bool {
    for line in output.split(separator: "\n") {
        let t = line.trimmingCharacters(in: .whitespaces)
        guard t.hasPrefix("\"\(label)\"") else { continue }
        let state = t.dropFirst(label.count + 2).trimmingCharacters(in: .whitespaces)
        return state == "=> disabled" || state == "=> true"
    }
    return false
}

/// The status line while the reinstall reminders are off.
let watcherOffLine = "Reinstall reminders are off. Turn on \(watcherName) in System Settings › General › Login Items & Extensions."

/// The "rekordbox open" dialog's text during an action and before a queued one.
let rekordboxOpenTryAgain = "Quit rekordbox (also in other accounts on this Mac), then click Try Again."

/// What became of an action the user asked for earlier, before it starts: start it now; refused
/// because Pioneer's installer or rekordbox's updater runs (checked again once rekordbox is
/// closed); or cancelled while rekordbox was open. `tryAgain` asks the user once (true: Try Again).
enum QueuedStart: Equatable { case start, updating, cancelled }

func queuedStart(updating: () -> Bool, rekordboxOpen: () -> Bool, tryAgain: () -> Bool) -> QueuedStart {
    if updating() { return .updating }
    while rekordboxOpen() { guard tryAgain() else { return .cancelled } }
    return updating() ? .updating : .start
}

// MARK: - removing Stems Cache, step by step

/// What removing Stems Cache came to: nothing of it is left; a step failed (or was cancelled),
/// with the root script's exit code (or Controller.notRun / cancelled); or rekordbox is gone and
/// the user kept the root folder.
enum CacheRemoval: Equatable {
    case removed, failed(Int32), keptRootFolder
    /// The user stopped it (cancelled): they know, no alert.
    var userStopped: Bool { if case .failed(let c) = self { return Controller.userStopped(c) }; return false }
}

/// The steps of removing Stems Cache, as functions: the app's (runUninstallCache) or the tests'.
struct CacheRemovalSteps {
    var rekordboxFolder: () -> Bool                 // something is at /Applications/rekordbox 7
    var rootFolder: () -> Bool                      // the root folder is there
    var uninstall: () -> Controller.RootOutcome     // the root uninstall
    var askRootFolder: () -> Bool                   // the user agrees to remove the root folder
    var removeRootFolder: () -> Controller.RootOutcome   // root removes it
    var log: (String) -> Void
}

/// Removing Stems Cache: the root uninstall while rekordbox's folder is there. Once it is gone
/// (before the run, or the run found it gone), the root folder is removed only if the user
/// agrees. rekordbox's folder decides, not rekordbox.app: root refuses a folder without
/// rekordbox.app (RootExit.notPioneers) and keeps the root folder while the folder is there.
func removeCache(_ s: CacheRemovalSteps) -> CacheRemoval {
    if s.rekordboxFolder() {
        switch s.uninstall() {
        case .done: return .removed
        case .failed(let code): return .failed(code)
        case .rekordboxMissing: break                // gone during the run
        }
    }
    guard s.rootFolder() else { return .removed }
    s.log("rekordbox isn't in the Applications folder: asking to remove the copy of its files")
    guard s.askRootFolder() else { return .keptRootFolder }
    switch s.removeRootFolder() {
    case .done: return .removed
    case .failed(let code): return .failed(code)
    case .rekordboxMissing: return .failed(RootExit.rekordboxMissing)
    }
}

/// Why removing Stems Cache failed, for "Uninstall stopped": the root script's own reason when
/// it has one (Controller.rootProblem), else where to look.
func cacheFailureReason(_ r: CacheRemoval) -> String {
    if case .failed(let code) = r {
        if let p = Controller.rootProblem(code) { return p.body }
        if code == RootExit.rolledBack { return "rekordbox has its original files again. See the log in this window." }
    }
    return "See the log in this window."
}

/// Said when the user kept the copy of rekordbox's files (Cancel at "rekordbox missing"), which
/// stops Uninstall RB Stems Plus completely.
let keptRootFolderMessage = "The copy of rekordbox's files was kept, so nothing else was deleted. If you moved rekordbox, put it back in the Applications folder, then uninstall again."

/// The question when rekordbox's folder is gone but the root folder is still there.
let rootFolderCleanupMessage = "rekordbox isn't in the Applications folder. If you moved it, click Cancel and put it back first. Otherwise RB Stems Plus can remove the copy of rekordbox's files it kept; Pioneer's installer can always reinstall rekordbox."

// MARK: - "Remove RB Stems Plus anyway"

/// What the user chose when rekordbox's own model couldn't be put back: stop, stop and open the
/// troubleshooting page at `url`, or remove this account's own files, saying `about` at the end
/// and keeping the saved originals if `keepOriginals`.
enum AnywayChoice: Equatable { case stop, help(url: String), remove(about: String, keepOriginals: Bool) }

/// Uninstall RB Stems Plus completely couldn't put rekordbox's own model back (`r`, from
/// Models.restore). A lack of free space only says so: freeing some and trying again works, so
/// nothing is offered. Otherwise "Remove RB Stems Plus anyway" is offered, and Open Help (which
/// stops, nothing deleted: the page ends with "uninstall again"). The user's answer comes from
/// `ask`: title, text, buttons. It never deletes a saved copy of Pioneer's model that verifies:
/// then it keeps the originals folder and says where it is.
func removeAnywayChoice(_ r: Outcome, _ m: Models, ask: (String, String, [String]) -> String, log: (String) -> Void) -> AnywayChoice {
    if r.noSpace {
        _ = ask("Uninstall stopped", "\(r.message) Nothing was deleted. Free up some space and try again.", ["OK"])
        return .stop
    }
    let stays = modelLeftBehind(m), keep = !m.verifiedOriginals().isEmpty
    let b = ask("Uninstall stopped",
                "\(r.message) Nothing was deleted.\n\n\(removeAnywayButton) deletes only its own files"
                + (keep ? " and keeps its saved copy of rekordbox's own stems model. " : ". ")
                + (r.message == unknownModelMessage ? "" : stays + " ")          // said just before
                + "To get rekordbox's own stems model back, click Open Help.",
                ["Cancel", removeAnywayButton, "Open Help"])
    log("  user chose: \(b)\(keep ? " (the saved copy of Pioneer's model stays)" : "")")
    if b == "Open Help" { return .help(url: keep ? removeAnywayHelpURL : noSavedModelHelpURL) }
    guard b == removeAnywayButton else { return .stop }
    return .remove(about: keep ? "\(stays) The saved copy of rekordbox's own stems model stays in \(m.originals.replacingOccurrences(of: home, with: "~")). To put it back, click Open Help."
                               : "\(stays) To get rekordbox's own stems model back, click Open Help.",
                   keepOriginals: keep)
}

let removeAnywayButton = "Remove RB Stems Plus Anyway"

/// The troubleshooting section for rekordbox's own stems model not going back: putting the
/// saved copy back by hand when one verifies, else getting rekordbox's model without one.
func modelHelpURL(_ m: Models) -> String { m.verifiedOriginals().isEmpty ? noSavedModelHelpURL : removeAnywayHelpURL }

/// The Install and Reinstall confirmations' text, for the button's `label` (which names the
/// features): what the command does. `paused`: Stems Cache's floor when its disk is under it,
/// else nil (said only when the label names Stems Cache).
func installNote(_ label: String, reinstall: Bool, paused: Int64?) -> String {
    let plus = label.contains("Demucs v4"), cache = label.contains("Stems Cache")
    var s: String
    if reinstall {
        s = "Installs the latest version of " + (plus && cache ? "Demucs v4 and Stems Cache" : cache ? "Stems Cache" : "Demucs v4") + " again."
    } else if plus && cache {
        s = "Replaces rekordbox's stems model with Demucs v4, and saves the stems it separates so they load much faster the next time."
    } else if cache {
        s = "Saves the stems rekordbox separates, with either stems model, so they load much faster the next time."
    } else {
        s = "Replaces rekordbox's stems model with Demucs v4. rekordbox's own model is kept, so it can be put back."
    }
    if cache, let floor = paused { s += " " + cachePausedAtInstall(floor) }
    return s
}

/// The Uninstall Demucs v4 and Uninstall Stems Cache confirmations' text, for the button's
/// `label`: what the command does, and that the saved stems stay.
func uninstallNote(_ label: String) -> String {
    label.contains("Stems Cache") ? "Stops saving stems and puts rekordbox's original files back. The stems already saved are kept."
                                  : "Puts rekordbox's own stems model back."
}

/// The complete uninstall's confirmation. Normally rekordbox goes back to exactly how it was;
/// not when Stems Cache is another account's, or when there's no saved copy of rekordbox's own
/// stems model to put back: then it states that fact only (the uninstall asks when it gets there). `stems`: the saved stems' size, e.g. " (1.2 GB)", or "".
func uninstallEverythingMessage(othersCache: Bool, noSavedModel: Bool, stems: String) -> String {
    var about: String
    if !othersCache && !noSavedModel {
        about = "rekordbox goes back to exactly how it was."
    } else {
        about = noSavedModel ? "There's no saved copy of rekordbox's own stems model to put back."
                             : "rekordbox gets its own stems model back."
        if othersCache { about += " Stems Cache was installed from another account on this Mac." }
    }
    return about + " Your music and rekordbox library aren't touched.\n\nRB Stems Plus will be deleted, including the stems it saved\(stems)."
}

/// Deletes this account's own folders: support, the saved stems and the logs. With
/// `keepOriginals`, support's originals folder (the saved copies of Pioneer's model) stays and
/// everything else in support goes. Returns whether all went.
@discardableResult
func removeOwnFolders(keepOriginals: Bool, support: String = support, originals: String = originalsDir,
                      others: [String] = [cacheDir, logDir], _ gate: Deletable = deletable) -> Bool {
    var ok = true
    if keepOriginals && fileType(originals) == S_IFDIR {
        for name in (try? FileManager.default.contentsOfDirectory(atPath: support)) ?? [] where support + "/" + name != originals {
            ok = safeRemove(support + "/" + name, whole: false, gate) && ok
        }
    } else {
        ok = safeRemove(support, whole: true, gate)
    }
    for d in others { ok = safeRemove(d, whole: true, gate) && ok }
    return ok
}

/// What happens to the app at the end of Uninstall RB Stems Plus completely.
enum AppFate { case deleted, otherAccount, cantDelete }

/// The final alert's text: `about` (what rekordbox has now), then what stays. `othersCache`:
/// Stems Cache is still installed for another account, which loses the app it uses.
/// `cacheLink`: the saved stems folder is a link, which RB Stems Plus never deletes.
func ownFilesRemovedText(_ about: String, app: AppFate, othersCache: Bool, cacheLink: Bool) -> String {
    var s = about
    switch app {
    case .deleted: if othersCache { s += " Other accounts on this Mac that use RB Stems Plus need to paste the install command again." }
    case .otherAccount: s += " RB Stems Plus stays in the Applications folder: it belongs to another account on this Mac."
    case .cantDelete: s += " RB Stems Plus couldn't delete itself: drag it to the Trash."
    }
    if cacheLink { s += " " + cacheLinkLine }
    return s
}
