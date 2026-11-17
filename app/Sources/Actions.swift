// The platform steps the core's flows call through the shell's host table (app/Shell/HostSteps.swift,
// M7's switch-over): the watcher's LaunchAgent (installed, removed, checked at launch) and the
// leftovers a stopped run left; and the pure rules the watcher and the steps still use. What the
// buttons do (install, reinstall, uninstall, the complete uninstall, the watcher's request) is the
// core's flows now (core/flows.c); the reinstall reminders' state is the core's (the steps answer
// the measurement, ShellFacts.watcherIsOffNow).
import AppKit

extension Controller {
    /// The LaunchAgent: the watcher helper (RB Stems Plus Watcher.app, inside the app) with
    /// --watch, started when rekordbox's app, its library, the model folder or rekordbox's
    /// settings folder change. macOS's background-item notice and Login Items name the helper,
    /// "RB Stems Plus Watcher". Only from a valid bundle with a valid helper (Safety.swift):
    /// launchd starts it by that path. The plist is replaced by a rename, never written through a
    /// link. Registered again (bootout, then bootstrap) whenever the plist or the helper's code
    /// changed since it was last registered (watcherRegisteredFile), so launchd never starts new
    /// code under an old registration.
    func installWatcher() {
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

    /// The LaunchAgent gone (bootout, then the plist). False: there was none.
    @discardableResult
    func removeWatcher() -> Bool {
        guard FileManager.default.fileExists(atPath: agentPlist) else { return false }
        runTool("/bin/launchctl", ["bootout", "\(guiDomain)/\(agentLabel)"])
        safeRemove(watcherRegisteredFile)
        log("watcher removed: \(safeRemove(agentPlist) ? "ok" : "FAILED")")
        return true
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

// MARK: - the complete uninstall's steps

/// MODELS_INFO's facts from one hashed originalsCheck (review MP1, MQ2): saved (unknown counts as
/// saved), verified (originalsKept: FACT_ORIGINALS_VERIFIED, which flows.c keeps the folder by)
/// and the Help page (modelHelpURL). Hashed, so "verified" means a copy's bytes checked.
func modelsInfoFacts(_ m: Models) -> (saved: Bool, verified: Bool, url: String) {
    let check = m.originalsCheck()
    return (!check.saved.isEmpty || check.unknown, originalsKept(m, check), modelHelpURL(m, check))
}

/// The troubleshooting section for rekordbox's own stems model not going back: putting the
/// saved copy back by hand when one verifies, else getting rekordbox's model without one.
/// The same rule as originalsKept (review MP5): the saved copy's page exactly when "anyway" keeps it.
func modelHelpURL(_ m: Models, _ check: OriginalsCheck? = nil) -> String {
    originalsKept(m, check) ? removeAnywayHelpURL : noSavedModelHelpURL
}

/// Whether "Remove RB Stems Plus anyway" keeps the originals folder, and says so: a saved copy
/// of Pioneer's model verifies, or one can't be checked (unknown, never "none"), and
/// removeOwnFolders will keep the folder (a link or a file there goes with the rest). The Swift
/// dialog and MODELS_INFO's FACT_ORIGINALS_VERIFIED (the core shell's) both use it, from one
/// originalsCheck (review MP1): `check`, or one taken now.
func originalsKept(_ m: Models, _ check: OriginalsCheck? = nil) -> Bool {
    let o = check ?? m.originalsCheck()
    return (!o.verified.isEmpty || o.unknown) && m.originalsStays
}

/// Deletes this account's own folders: support, the saved stems and the logs. With
/// `keepOriginals`, support's originals folder (the saved copies of Pioneer's model) stays and
/// everything else in support goes. Returns whether all went.
/// The originals folder is kept by identity (its device and inode, taken first and compared
/// with each entry's), never by name: a case-renamed Originals is still it. Any lookup or
/// listing that fails (but ENOENT) deletes nothing in support and returns false.
@discardableResult
func removeOwnFolders(keepOriginals: Bool, support: String = support, originals: String = originalsDir,
                      others: [String] = [defaultCacheDir, logDir], _ gate: Deletable = deletable) -> Bool {
    var ok = true
    var keep = stat(), keeping = false, unknown = false
    if keepOriginals {
        if lstat(originals, &keep) == 0 { keeping = keep.st_mode & S_IFMT == S_IFDIR } else { unknown = errno != ENOENT }
    }
    if unknown {
        ok = false                                              // can't tell: nothing in support goes
    } else if keeping {
        if let names = try? FileManager.default.contentsOfDirectory(atPath: support) {
            for name in names {
                var st = stat()
                guard lstat(support + "/" + name, &st) == 0 else { if errno != ENOENT { ok = false }; continue }
                if st.st_dev == keep.st_dev && st.st_ino == keep.st_ino { continue }
                ok = safeRemove(support + "/" + name, whole: false, gate) && ok
            }
        } else { ok = false }
    } else {
        ok = safeRemove(support, whole: true, gate)
    }
    for d in others { ok = safeRemove(d, whole: true, gate) && ok }
    return ok
}
