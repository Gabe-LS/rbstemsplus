// Settings' location row: Change… (choose a folder on an external drive) and Use This Mac, the
// switch dialog and its order (3.3.3), finding stems already on the drive (3.3.4), Clear Saved
// Stems in a chosen location (in the background), and the retry of This Mac's deletion at launch.
// Nothing moves: the new location starts empty, or with the stems already in it.
import AppKit

// MARK: - the texts (tested)

/// A size in the dialogs' words: "18.2 GB", "35 MB".
func sizeWords(_ bytes: Int64) -> String {
    let gb = Double(bytes) / 1_073_741_824
    return gb >= 0.1 ? String(format: "%.1f GB", gb) : "\(max(1, bytes / 1_048_576)) MB"
}

/// Settings' location label: "Stems Cache location: This Mac", "Stems Cache location: GLS › DJ".
func locationLabel(_ cfg: ConfigState, mounts: [Mount] = currentMounts()) -> String {
    if cfg.unreadable != nil { return "Stems Cache location: config.ini can't be read" }
    switch cfg.location {
    case .thisMac: return "Stems Cache location: This Mac"
    case .malformed: return "Stems Cache location: set wrongly in config.ini"
    case .chosen(let dir, _):
        let parent = (dir as NSString).deletingLastPathComponent
        let drive = driveName(dir, mounts: mounts)
        let mountPath = mountHolding(dir, mounts)?.path ?? "/Volumes/" + drive
        let below = (pathComponents(parent) ?? []).dropFirst(pathComponents(mountPath)?.count ?? 0)
        return "Stems Cache location: " + ([drive] + below).joined(separator: " › ")
    }
}

// The location dialogs say only what the switch being confirmed does: where stems will be saved,
// what it deletes now (and what that means), and what happens to the old drive's stems.

/// What the old location's stems become (a drive's stay on it), for the switch dialogs.
func oldDriveStays(drive: String) -> String { "The stems saved on \"\(drive)\" stay there." }

/// The switch dialog's text (3.3.3) for a drive `drive`: `found` the stems already in its
/// folder (nil: a new folder), `thisMac` the stems on this Mac that will be deleted, `oldDrive`
/// the name of the drive in use now.
func switchMessage(drive: String, found: Int64?, thisMac: Int64, oldDrive: String?) -> String {
    var s = found.map { "Stems Cache will use the \(sizeWords($0)) already saved on \"\(drive)\"." } ?? "Stems Cache will save stems on \"\(drive)\"."
    if thisMac > 0 { s += " The \(sizeWords(thisMac)) saved on this Mac will be deleted: rekordbox separates those tracks again the next time you load them." }
    if let o = oldDrive { s += " " + oldDriveStays(drive: o) }
    return s
}

/// Use This Mac's dialog: `oldDrive` the drive in use now, nil when config.ini's location was wrong.
func useThisMacMessage(oldDrive: String?) -> String {
    "Stems Cache will save stems on this Mac." + (oldDrive.map { " " + oldDriveStays(drive: $0) } ?? "")
}

/// Clear Saved Stems' confirmation in a chosen location.
func clearLocationMessage(drive: String) -> String {
    "Deletes all stems saved in this folder on \"\(drive)\", including any saved there by other Macs or accounts."
}

// MARK: - the actions

extension Controller {
    /// The floor Stems Cache would be paused under on the disk of the folder in use, from the
    /// background measurement (LocationMonitor): never a drive read on the main thread.
    func cachePausedNow() -> Int64? { LocationMonitor.shared.snapshot().pausedFloor }

    /// Runs `f` off the main thread while the window stays responsive (its buttons off), and
    /// returns its result. Main thread.
    func inBackground<T>(_ status: String, _ f: @escaping () -> T) -> T {
        let wasRunning = actionRunning
        if !wasRunning { busy(status) }
        var result: T?
        DispatchQueue.global(qos: .userInitiated).async { let r = f(); DispatchQueue.main.async { result = r } }
        while result == nil { RunLoop.current.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05)) }
        if !wasRunning { busy(nil) }
        return result!
    }

    /// Settings' Change…: a folder on an external drive, checked (3.1.5-6), stems already there
    /// found (3.3.4), the switch dialog, then the order of 3.3.3. Main thread; rekordbox closed.
    func changeLocation() {
        guard preflight() else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose"
        panel.message = "Choose a folder on an external drive. Stems Cache saves stems in a folder named \"RB Stems Plus Cache\" inside it."
        panel.directoryURL = URL(fileURLWithPath: "/Volumes")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let folder = url.path
        let probe = inBackground("Checking the folder…") { () -> (String?, VolumeFacts?) in
            let real = realPath(folder)
            return (real, volumeFacts(real ?? folder))
        }
        let real = probe.0, facts = probe.1
        if let why = locationRefusalFor(folder: folder, real: real, facts: facts) {
            log("settings: \(redact(folder)) refused (\(why))")
            _ = alert("Can't save stems there", locationRefusal(why, drive: facts?.name ?? (folder as NSString).lastPathComponent), ["OK"])
            return
        }
        guard let chosenReal = real, let f = facts else { return }
        let drive = f.name
        let st = readConfigState(log), hint = readLocationHint()
        let current = st.location.chosenDir, currentID = st.location.chosenID
        let found = inBackground("Looking for saved stems on \"\(drive)\"…") {
            discover(folder: chosenReal, mount: f.mountPoint, relative: relativeOnDrive(current, hint: hint))
        }
        let oldDrive = current.map { driveName($0) }
        switch found {
        case .notOurs:
            _ = alert("Can't save stems there", "That \"RB Stems Plus Cache\" folder isn't RB Stems Plus's: its marker file is missing. Choose another folder.", ["OK"])
        case .found(let dir, let id) where dir == current && id.lowercased() == currentID?.lowercased():
            log("settings: \(redact(dir)) is already where stems are saved")
        case .found(let dir, let id) where id.lowercased() == currentID?.lowercased():
            // the same location, renamed or mounted elsewhere: only cache_dir changes
            let size = inBackground("Measuring…") { entriesSize(dir) ?? 0 }
            guard alert("Saved stems found", "Found your saved stems on \"\(drive)\"\(size > 0 ? " (\(sizeWords(size)))" : "").", ["Use Them", "Cancel"]) == "Use Them",
                  rekordboxClosed() else { return }
            commitLocation(dir: dir, id: id, mount: f.mountPoint, created: false, deleteThisMac: false)
        case .found(let dir, let id):
            guard inBackground("Checking the folder…", { locationWritable(dir) }) else {
                _ = alert("Can't save stems there", "This folder belongs to another account on this drive, which has ownership turned on.", ["OK"])
                return
            }
            let sizes = inBackground("Measuring…") { (entriesSize(dir) ?? 0, entriesSize(defaultCacheDir) ?? 0) }
            let msg = switchMessage(drive: drive, found: sizes.0, thisMac: current == nil ? sizes.1 : 0, oldDrive: oldDrive)
            guard alert("Use \"\(drive)\"?", msg, ["Use \"\(drive)\"", "Cancel"]) != "Cancel", rekordboxClosed() else { return }
            commitLocation(dir: dir, id: id, mount: f.mountPoint, created: false, deleteThisMac: current == nil)
        case .none:
            let thisMac = current == nil ? inBackground("Measuring…") { entriesSize(defaultCacheDir) ?? 0 } : 0
            let msg = switchMessage(drive: drive, found: nil, thisMac: thisMac, oldDrive: oldDrive)
            guard alert("Use \"\(drive)\"?", msg, ["Use \"\(drive)\"", "Cancel"]) != "Cancel", rekordboxClosed() else { return }
            // nothing is created before the user confirms
            let made = createLocationFolder(in: chosenReal)
            guard let dir = made.dir, let id = made.id else {
                log("settings: couldn't create our folder in \(redact(chosenReal)): \(made.error ?? "?")")
                _ = alert("Can't save stems there", made.error == "exists" ? "A folder named \"RB Stems Plus Cache\" appeared there meanwhile. Choose the folder again." :
                          "RB Stems Plus couldn't create its folder on \"\(drive)\" (\(made.error ?? "?")).", ["OK"])
                return
            }
            excludeFromBackup(dir, fsType: f.fsType)
            commitLocation(dir: dir, id: id, mount: f.mountPoint, created: true, deleteThisMac: current == nil)
        }
    }

    /// The commit (3.3.3): config.ini names the folder; then the hint; then, from this Mac, its
    /// stems are deleted (a failure leaves the pending-deletion flag for the next launch). A
    /// failed config.ini write removes the folder this run created. Main thread.
    func commitLocation(dir: String, id: String, mount: String, created: Bool, deleteThisMac: Bool) {
        guard writeCacheLocation(dir: dir, id: id) else {
            log("settings: couldn't write \(configFile)")
            if created { removeNewLocationFolder(dir) }
            _ = alert("Settings not saved", "RB Stems Plus couldn't write its settings file. See the log in this window.", ["OK"])
            return
        }
        writeLocationHint(hintFor(dir: dir, mount: mount, old: readLocationHint(), newID: created ? id : nil))
        prepareChanged()
        log("settings: stems are saved in \(redact(dir)) from rekordbox's next start\(created ? " (a new folder)" : "")")
        if deleteThisMac {
            // an older Stems Cache reinstalled meanwhile (from another account) still saves on this
            // Mac: then the guarded launch retry deletes them, once it's safe (3.4)
            if thisMacDeletionNow(cachePresent: stemsCachePresent(), bridgeLocation: bridgeSaysLocation()) {
                let ok = inBackground("Deleting the stems saved on this Mac…") { removeThisMacStems() }
                if !ok { try? Data().write(to: URL(fileURLWithPath: pendingDeletionFile)) }
                log("settings: the stems saved on this Mac deleted: \(ok ? "ok" : "FAILED (tried again at the next start)")")
            } else {
                try? Data().write(to: URL(fileURLWithPath: pendingDeletionFile))
                log("settings: the stems saved on this Mac stay for now: the installed Stems Cache is older and saves there (deleted once it's updated)")
            }
        }
        LocationMonitor.shared.invalidateSize()
        LocationMonitor.shared.update(size: true)
        refresh()
    }

    /// Settings' Use This Mac: the drive's stems stay on it. Main thread; rekordbox closed.
    func useThisMac() {
        guard preflight() else { return }
        let st = readConfigState(log)
        let old = st.location.chosenDir.map { driveName($0) }
        guard alert("Use This Mac?", useThisMacMessage(oldDrive: old), ["Use This Mac", "Cancel"]) == "Use This Mac", rekordboxClosed() else { return }
        guard writeCacheLocation(dir: nil, id: nil) else {
            _ = alert("Settings not saved", "RB Stems Plus couldn't write its settings file. See the log in this window.", ["OK"])
            return
        }
        safeRemove(pendingDeletionFile)
        log("settings: stems are saved on this Mac from rekordbox's next start")
        prepareChanged()
        LocationMonitor.shared.invalidateSize()
        LocationMonitor.shared.update(size: true)
        refresh()
    }

    /// Clear Saved Stems in a chosen location: its model folders and "._" files, through
    /// verified descriptors; the folder and its marker stay (the bridge never creates them). In
    /// the background, the window showing how far it is. Main thread.
    func clearLocation(_ gate: LocationGate, drive: String) {
        busyInFront("Clearing saved stems on \"\(drive)\"…")
        work.async {
            let r = removeInLocation(gate, wholeFolder: false) { n in self.setStatus("Clearing saved stems on \"\(drive)\"… \(n) files") }
            self.log("saved stems on \(redact(gate.dir)) cleared: \(r.ok ? "ok" : "FAILED (\(r.why ?? "?"))")")
            self.busy(nil)
            self.prepareChanged()
            LocationMonitor.shared.invalidateSize()
            LocationMonitor.shared.update(size: true)
            if !r.ok { _ = self.alert("Saved stems not cleared", "Some saved stems on \"\(drive)\" couldn't be deleted (\(r.why ?? "?")).", ["OK"]) }
        }
    }

    /// At launch (work queue): This Mac's stems that couldn't be deleted after a switch to a
    /// drive, only under 3.3.3's conditions.
    func retryThisMacDeletion() {
        let s = LocationMonitor.shared.current()
        // stems an older bridge saved on this Mac while it was installed (bridgeTooOld): the owner
        // decided they go as at a switch, through the same flag and conditions
        if fileType(pendingDeletionFile) == nil, let status = s.status,
           thisMacNeedsCleanup(status: status, bridgeLocation: bridgeSaysLocation(), modelFolders: thisMacHasModelFolders()) {
            try? Data().write(to: URL(fileURLWithPath: pendingDeletionFile))
            log("stems an older Stems Cache saved on this Mac while a drive was chosen: deleting them")
        }
        guard fileType(pendingDeletionFile) != nil else { return }
        guard let status = s.status, mayRetryThisMacDeletion(flag: true, status: status, bridgeLocation: bridgeSaysLocation(), rekordboxRunning: rekordboxOpen()) else {
            log("the stems left on this Mac aren't deleted yet: not now (\(s.status?.code ?? "?"))")
            return
        }
        let ok = removeThisMacStems()
        if ok { safeRemove(pendingDeletionFile) }
        log("the stems left on this Mac deleted: \(ok ? "ok" : "FAILED (tried again at the next start)")")
    }
}
