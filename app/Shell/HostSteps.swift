// The AppKit shell over the shared C core (the private Windows plan's mac-core §3.5, M7): host->step,
// the flows' platform steps, over today's platform code. core.h ("The flows' steps") says what
// each kind takes, what it answers and which Swift function it runs; this file calls exactly
// that, in the same order, with the same logging those functions do themselves. A step answers
// with codes, numbers and raw facts only: the core writes every text. Where 1.3.0's Controller
// method mixed its platform work with the window (busy, setStatus, alert, refresh), its platform
// part is done here, marked "M7 seam:"; since M7's switch-over those Controller methods are gone
// (their last form is core/tests/swift/oracle's: app/Sources at 56d7c11 plus freeze-time
// fixes), so this is the one copy.
// Steps run on any core worker, possibly at once: the flow's shared state is ShellFlowState's
// (locked), the watcher's LaunchAgent steps take one lock, and nothing else here is shared.
// Every build (M7's switch-over: the app is this shell).
import AppKit

/// What a step may use besides today's free functions.
struct ShellStepEnv {
    let controller: Controller          // the app's window controller: only its UI-free methods
    let core: OpaquePointer?            // the core handle, for core_flow_progress from inside a step
    let log: (String) -> Void           // the app's log (today's Controller.log): any thread
    let quitNow: () -> Void             // SELF_DELETE / UPDATE_SWAP: the app quits at once without asking
}

/// installWatcher, removeWatcher and checkWatcher ran on the one work queue: never two at once.
private let shellWatcherStepLock = NSLock()

/// A step's result sheet.
private struct StepOut: FactSink {
    let out: OpaquePointer?
    func flag(_ f: core_fact, _ v: Bool) { core_fact_flag(out, f, v ? 1 : 0) }
    func i64(_ f: core_fact, _ v: Int64) { core_fact_i64(out, f, v) }
    func str(_ f: core_fact, _ s: String) { s.withCString { core_fact_str(out, f, $0, strlen($0)) } }
    /// A StemsSize as kind + bytes (core.h, LOCATION_NOW): 0 measuring, 1 measured, 2 failed,
    /// 3 not connected, -1 none.
    func size(_ s: StemsSize?, kind: core_fact, bytes: core_fact) {
        switch s {
        case nil: i64(kind, -1)
        case .measuring?: i64(kind, 0)
        case .measured(let b)?: i64(kind, 1); i64(bytes, b)
        case .failed?: i64(kind, 2)
        case .notConnected?: i64(kind, 3)
        }
    }
}

/// host->step (core.h "The flows' steps"): any core worker thread, possibly concurrently.
/// Returns 0 on success, else the step's own code. `out` is the core_facts sheet.
func shellStep(_ kind: core_step, _ args: [String], _ out: OpaquePointer?, _ env: ShellStepEnv) -> Int32 {
    let o = StepOut(out: out)
    func arg(_ i: Int) -> String { i < args.count ? args[i] : "" }
    let flow = ShellFlowState.shared

    switch kind {
    case CORE_STEP_OPEN_URL:                                    // openHelp
        guard let u = URL(string: arg(0)) else { return 1 }
        let ok = Thread.isMainThread ? NSWorkspace.shared.open(u) : DispatchQueue.main.sync { NSWorkspace.shared.open(u) }
        return ok ? 0 : 1

    case CORE_STEP_NOTIFY:                                      // args: title, body (the core's texts)
        guard !args.isEmpty else { return 1 }
        notify(title: arg(0), body: arg(1))
        return 0

    case CORE_STEP_REVEAL:
        guard !arg(0).isEmpty else { return 1 }
        let path = arg(0)
        DispatchQueue.main.async { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)]) }
        return 0

    case CORE_STEP_LAUNCH_AGENT_LOAD, CORE_STEP_LAUNCH_AGENT_UNLOAD, CORE_STEP_WATCHER_CHECK, CORE_STEP_WATCHER_STATE:
        // The reminders' state (M72): each step answers the reminders' measurement alone
        // (ShellFacts.watcherIsOffNow) as FACT_STEP_FLAG, and the core logs the line and shows the
        // status line when it changes: after LOAD (installWatcher) and UNLOAD (removeWatcher), at
        // start-up (WATCHER_CHECK: checkWatcher's repair first) and on activation (WATCHER_STATE:
        // the measurement only, never the repair). One lock: never during another watcher step.
        shellWatcherStepLock.lock(); defer { shellWatcherStepLock.unlock() }
        switch kind {
        case CORE_STEP_LAUNCH_AGENT_LOAD: env.controller.installWatcher()
        case CORE_STEP_LAUNCH_AGENT_UNLOAD: env.controller.removeWatcher()
        case CORE_STEP_WATCHER_CHECK: env.controller.checkWatcher()
        default: break
        }
        o.flag(FACT_STEP_FLAG, ShellFacts.watcherIsOffNow())
        return 0

    case CORE_STEP_DOCK:
        guard !arg(0).isEmpty else { return 1 }
        o.flag(FACT_STEP_FLAG, removeFromDock(arg(0)))
        return 0

    case CORE_STEP_SELF_DELETE:                                 // the end of removeOwnFiles
        let me = arg(0), dockRestarts = arg(1) == "1"
        // core.h: nonzero when the cleanup couldn't be started; the app then stays open (the
        // core logs it and says so)
        guard afterExit(poll: "0.5", ownPrefsDeleteScript + (me.isEmpty ? "" : selfDeleteScript + (dockRestarts ? dockRestartScript : "")), [me, bundleID]) else {
            return 1
        }
        env.quitNow()
        return 0

    case CORE_STEP_MODEL_COPY:
        guard let m = flow.manifest else { return 1 }
        let r = installedModels(env.log).installOurs(from: m.modelPath, sha: m.model.sha256)
        if r.ok { return 0 }
        setModelOutcome(o, copyOutcomeCode(r))
        return 1

    case CORE_STEP_DOWNLOAD:
        guard let m = flow.manifest else { return 1 }
        let got: Fetched
        switch arg(0) {
        case "model":
            got = fetchAsset(m.model.file, to: m.modelPath, sha: m.model.sha256, size: m.model.size, what: "the Demucs v4 model",
                             status: downloadProgress(env, part: m.modelPath + ".part", size: m.model.size), log: env.log)
        case "bridge":
            got = fetchAsset(m.bridge.file, to: m.bridgePath, sha: m.bridge.sha256, size: nil, what: "Stems Cache",
                             status: downloadProgress(env, part: m.bridgePath + ".part", size: nil), log: env.log)
        default: return 1
        }
        return fetchedCode(got)

    case CORE_STEP_REPORT_FACTS:
        // reportFacts' measurement, now: the core decides the report's lights from it
        let m = reportMeasure()
        ShellFlowState.shared.report = m
        ShellFacts.reportLights(o, m)
        return 0

    case CORE_STEP_REPORT_ZIP:
        // the lights the core decided (args: each look's code and text, whether Stems Cache's is the location's)
        func look(_ s: String) -> LightLook { s == "0" ? .on : s == "1" ? .limited : .off }
        let lights = ReportLights(plus: (look(arg(0)), arg(1)), cache: (look(arg(2)), arg(3)), cacheIsLocation: arg(4) == "1")
        let measure = ShellFlowState.shared.report ?? reportMeasure()
        ShellFlowState.shared.report = nil
        guard let made = buildReport(env.log, lights: lights, measure: measure) else { return 1 }
        o.str(FACT_STEP_PATH, made.zip)
        o.str(FACT_STEP_TEXT, redact(made.zip))
        o.str(FACT_STEP_URL, issueURL(title: made.title, details: made.details)?.absoluteString ?? "")
        return 0

    case CORE_STEP_REKORDBOX:
        o.flag(FACT_REKORDBOX_OPEN, rekordboxOpen())
        o.flag(FACT_PIONEER_UPDATING, pioneerUpdating())
        return 0

    case CORE_STEP_PAYLOAD:
        switch arg(0) {
        case "latest":                                          // install's: the latest, else the last good one
            var fresh: Manifest?, fetch: Int64 = 2
            switch fetchManifestChecked(env.log) {
            case .ok(let m): fresh = m; fetch = 0
            case .unverified: fetch = 1
            case .offline: fetch = 2
            }
            let m = fresh ?? savedManifest()
            flow.manifest = m
            payloadFacts(o, m, fetch: fetch)
        case "saved":
            let m = savedManifest()
            flow.manifest = m
            payloadFacts(o, m, fetch: 2)
        case "check":                                           // checkForUpdate's first part
            let saved = savedManifest()
            noteShownVersion(saved)                             // also clears a record a replaced app left
            payloadFacts(o, saved, fetch: 2)
            o.flag(FACT_STEP_FLAG, manifestDue(age: manifestAge(), savedApp: saved?.app.version, app: appVersion, blocked: blockedByVersionList))
        case "fetch":                                           // checkForUpdate's background download
            switch fetchManifestChecked(env.log) {
            case .ok(let m): payloadFacts(o, m, fetch: 0)
            case .unverified: payloadFacts(o, nil, fetch: 1)
            case .offline: payloadFacts(o, nil, fetch: 2)
            }
        default: return 1
        }
        return 0

    case CORE_STEP_BLOCKED:
        switch arg(0) {
        case "plus":                                            // stemsPlusBlocked's facts
            let engine = engineVersion()
            o.str(FACT_ENGINE, engine)
            o.flag(FACT_ENGINE_SUPPORTED, engineSupported(engine))
        case "cache":
            // M7 seam: stemsCacheBlocked returns only its sentence; its checks are made in its order
            // (ShellFacts.cacheBlocked, the cheap sheet's too), stopping at the first that fails, so
            // codesign runs only when Swift's would.
            ShellFacts.cacheBlocked(o, admin: userIsAdmin, installed: rekordboxInstalled(), version: rekordboxVersion(),
                                    present: stemsCachePresent(), list: compatibleList(), always: false)
        default: return 1
        }
        return 0

    case CORE_STEP_SPACE:
        var m: Manifest?
        if arg(0) == "1" { guard let fm = flow.manifest else { return 1 }; m = fm }
        o.i64(FACT_STEP_CODE, spaceNeeded(model: m, cache: arg(1) == "1"))
        return 0

    case CORE_STEP_PRUNE_PAYLOADS:
        if let m = flow.manifest, FileManager.default.fileExists(atPath: m.modelPath) { prunePayloads(keep: m) }
        return 0

    case CORE_STEP_CACHE_PREPARE:
        // M7 seam: install's own lines (Actions.swift:107-111), inside the Controller method.
        guard let m = flow.manifest else { return 1 }
        let ort = stemsCachePresent() ? rootOrt : rbLib
        let ortSha = sha256(ort) ?? "unreadable"
        env.log("Pioneer's ONNX Runtime: \(ort) \(ortSha.prefix(12))")
        guard let bridge = FileManager.default.contents(atPath: m.bridgePath), sha256(data: bridge) == m.bridge.sha256 else { return 1 }
        flow.setPrepared(bridge: bridge, bridgeSha: m.bridge.sha256, ortSha: ortSha)
        o.str(FACT_STEP_TEXT, ortSha)
        return 0

    case CORE_STEP_MODEL_RESTORE:
        let r = installedModels(env.log).restore()
        if r.ok { return 0 }
        setModelOutcome(o, r)
        return 1

    case CORE_STEP_MODELS_INFO:
        let models = installedModels { _ in }
        // "saved" alone: its own check, unhashed; unknown counts as saved (MP8)
        if arg(0) == "saved" { let c = models.originalsCheck(hashing: false); o.flag(FACT_ORIGINALS_SAVED, !c.saved.isEmpty || c.unknown); return 0 }
        // the rest from one hashed check (modelsInfoFacts, review MP1/MQ2); flows.c keeps the
        // originals folder by VERIFIED: also when a copy can't be checked (SR1)
        let facts = modelsInfoFacts(models)
        o.flag(FACT_ORIGINALS_SAVED, facts.saved)
        o.flag(FACT_ORIGINALS_VERIFIED, facts.verified)
        o.i64(FACT_STEP_CODE, leftBehindCode(modelLeftBehind(models)))   // M7 seam: read back (leftBehindCode)
        o.str(FACT_STEP_PATH, models.originals.replacingOccurrences(of: home, with: "~"))
        o.str(FACT_STEP_URL, facts.url)
        return 0

    case CORE_STEP_SET_CHOSEN:
        guard args.count >= 2 else { return 1 }
        setChosen(model: arg(0) == "1", cache: arg(1) == "1")
        return 0

    case CORE_STEP_CONFIG_WRITE:
        // writeConfig / writeRekordboxModel: max_gb and max_days as configNumber writes them
        guard !args.isEmpty, args.count % 2 == 0 else { o.str(FACT_STEP_PATH, configFile); return 1 }
        var pairs: [(key: String, value: String)] = []
        for i in stride(from: 0, to: args.count, by: 2) {
            let key = args[i], value = args[i + 1]
            switch key {
            case "rekordbox_model": pairs.append((key, value))
            case "max_gb", "max_days":
                guard let x = Double(value) else { o.str(FACT_STEP_PATH, configFile); return 1 }
                pairs.append((key, configNumber(x)))
            default: o.str(FACT_STEP_PATH, configFile); return 1
            }
        }
        guard writeConfigValues(pairs) else { o.str(FACT_STEP_PATH, configFile); return 1 }
        o.str(FACT_STEP_TEXT, pairs.map { "\($0.key)=\($0.value)" }.joined(separator: " "))
        return 0

    case CORE_STEP_CONFIG_SETS:
        guard !arg(0).isEmpty else { return 1 }
        o.flag(FACT_STEP_FLAG, configSets(arg(0)))
        return 0

    case CORE_STEP_BUSY_MARKER:
        setBusyMarker(args.count >= 2 ? Action(id: arg(0), label: arg(1)) : nil)
        return 0

    case CORE_STEP_STATE_LINE:
        o.str(FACT_STEP_TEXT, env.controller.state())
        return 0

    case CORE_STEP_CACHE_FOLDERS:                               // runUninstallCache's two folders
        o.flag(FACT_STEP_FLAG, fileType(rbFolder) != nil)
        o.flag(FACT_STEP_FLAG2, fileType(rootDir) != nil)
        return 0

    case CORE_STEP_LOCATION_NOW:                                // as uninstallEverything and removeOwnFiles read it
        let loc = LocationMonitor.shared.current(), snap = LocationMonitor.shared.snapshot()
        let dir = loc.status?.dir ?? ""
        o.str(FACT_LOC_DIR, dir)
        o.str(FACT_LOC_DRIVE, loc.drive ?? "")
        o.str(FACT_STEP_TEXT2, dir.isEmpty ? "" : driveName(dir))
        o.str(FACT_STEP_TEXT, dir.isEmpty ? "" : redact(dir))
        o.str(FACT_LOC_GATE_ID, loc.deletableGate?.id ?? "")
        let same = loc.status.flatMap(folderInUse) == snap.status.flatMap(folderInUse)
        o.size(same ? snap.size : nil, kind: FACT_LOC_SIZE_KIND, bytes: FACT_LOC_SIZE)
        return 0

    case CORE_STEP_REMOVE_LOCATION:
        guard args.count >= 3 else { o.i64(FACT_STEP_CODE, 7); return 1 }
        let r = removeInLocation(LocationGate(dir: arg(0), id: arg(1)), wholeFolder: arg(2) == "1") { n in
            if let c = env.core { core_flow_progress(c, CORE_PROGRESS_DELETE, Int64(n), 0) }
        }
        if r.ok { return 0 }
        setRemovalWhy(o, r.why ?? "")
        return 1

    case CORE_STEP_REMOVE_OWN_FOLDERS:
        return removeOwnFolders(keepOriginals: arg(0) == "1", others: [defaultCacheDir, logDir]) ? 0 : 1

    case CORE_STEP_APP_FATE:
        // M7 seam: removeOwnFiles' own lines (Actions.swift:528-530), inside the Controller method.
        let bundle = ownBundle(), owned = bundle.map { ownedByThisAccount($0) } ?? false
        let me = bundle.flatMap { b in owned && access((b as NSString).deletingLastPathComponent, W_OK) == 0 && access(b, W_OK) == 0 ? b : nil }
        o.i64(FACT_STEP_CODE, me != nil ? 0 : bundle != nil && !owned ? 1 : 2)
        if let me = me { o.str(FACT_STEP_PATH, me) }
        return 0

    case CORE_STEP_LEFTOVER:                                    // checkResume's first lines
        let left = leftoverAction()
        if left != nil { runTool("/usr/bin/sudo", ["-k"]) }     // the last run quit before it could forget the password
        o.str(FACT_STEP_TEXT, left?.id ?? "")
        o.str(FACT_STEP_TEXT2, left?.label ?? "")
        return 0

    case CORE_STEP_REQUEST:
        let take = arg(0) == "take"
        let age = take ? takeReinstallRequest() : reinstallRequestAge()
        o.i64(FACT_STEP_CODE, age.map { Int64($0) } ?? -1)
        if !take && age != nil { o.str(FACT_STEP_TEXT, reinstallRequestText()) }
        return 0

    case CORE_STEP_BRIDGE_SHA:
        o.str(FACT_STEP_TEXT, sha256(rbLib) ?? "")
        return 0

    case CORE_STEP_UPDATE_CHECK:                                // updateApp's two refusals
        guard let me = ownBundle() else { o.i64(FACT_STEP_CODE, 1); return 0 }
        o.i64(FACT_STEP_CODE, FileManager.default.isWritableFile(atPath: (me as NSString).deletingLastPathComponent) ? 0 : 2)
        return 0

    case CORE_STEP_UPDATE_SWAP:
        return updateSwap(env)

    case CORE_STEP_UPDATE_RESULT:
        // M7 seam: checkUpdateResult's reading, without its log line and alert (the core's).
        let result = FileManager.default.contents(atPath: updateDir + "/result").map { String(decoding: $0, as: UTF8.self) }
        safeRemove(updateDir)
        o.flag(FACT_STEP_FLAG, result != nil)
        if let r = result { o.str(FACT_STEP_TEXT, r) }
        return 0

    case CORE_STEP_WELCOME:
        // M7 seam: welcomeOnce's marker, without its alert (the core's).
        switch arg(0) {
        case "check": o.flag(FACT_STEP_FLAG, FileManager.default.fileExists(atPath: welcomedFile))
        case "done":
            try? FileManager.default.createDirectory(atPath: support, withIntermediateDirectories: true)
            FileManager.default.createFile(atPath: welcomedFile, contents: Data())
        default: return 1
        }
        return 0

    case CORE_STEP_PAUSED:                                      // cachePausedNow() and the snapshot's drive
        let snap = LocationMonitor.shared.snapshot()
        o.i64(FACT_STEP_CODE, snap.pausedFloor ?? -1)
        o.str(FACT_STEP_TEXT, snap.drive ?? "")
        return 0

    case CORE_STEP_SETTINGS:
        settingsFacts(o, arg(0) == "open" ? readConfigState(env.log) : readConfigState())
        return 0

    case CORE_STEP_CLEAR_THIS_MAC:                              // Settings' Clear on this Mac
        let ok = safeRemove(defaultCacheDir, whole: true)
        o.flag(FACT_STEP_FLAG, cacheIsLink())
        return ok ? 0 : 1

    case CORE_STEP_LOCATION_REMEASURE:
        LocationMonitor.shared.invalidateSize()
        LocationMonitor.shared.update(size: true)
        return 0

    case CORE_STEP_LOCATION_PROBE:                              // changeLocation's check of the folder chosen
        let folder = arg(0)
        guard !folder.isEmpty else { return 1 }
        let real = realPath(folder), facts = volumeFacts(real ?? folder)
        o.i64(FACT_STEP_CODE, locationRefusalFor(folder: folder, real: real, facts: facts).map(refusalCode) ?? 0)
        o.str(FACT_STEP_TEXT, real ?? "")
        o.str(FACT_STEP_TEXT2, redact(folder))
        o.str(FACT_LOC_DRIVE, facts?.name ?? (folder as NSString).lastPathComponent)
        o.str(FACT_STEP_PATH, facts?.mountPoint ?? "")
        return 0

    case CORE_STEP_LOCATION_DISCOVER:
        guard args.count >= 2 else { return 1 }
        let st = readConfigState(env.log), hint = readLocationHint()
        let current = st.location.chosenDir, currentID = st.location.chosenID
        let found = discover(folder: arg(0), mount: arg(1), relative: relativeOnDrive(current, hint: hint))
        switch found {
        case .none: o.i64(FACT_STEP_CODE, 0)
        case .notOurs(let dir): o.i64(FACT_STEP_CODE, 1); o.str(FACT_STEP_PATH, redact(dir))
        case .found(let dir, let id):
            o.i64(FACT_STEP_CODE, 2)
            o.str(FACT_LOC_DIR, dir)
            o.str(FACT_LOC_GATE_ID, id)
            o.str(FACT_STEP_PATH, redact(dir))
        }
        o.str(FACT_STEP_TEXT, current ?? "")
        o.str(FACT_STEP_TEXT2, currentID ?? "")
        o.str(FACT_LOC_DRIVE, current.map { driveName($0) } ?? "")
        return 0

    case CORE_STEP_LOCATION_SIZE:
        let dir = arg(0) == "this_mac" ? defaultCacheDir : arg(0)
        guard !dir.isEmpty, let size = entriesSize(dir) else { return 1 }
        o.i64(FACT_STEP_CODE, size)
        return 0

    case CORE_STEP_LOCATION_WRITABLE:
        guard !arg(0).isEmpty else { return 1 }
        o.flag(FACT_STEP_FLAG, locationWritable(arg(0)))
        return 0

    case CORE_STEP_LOCATION_CREATE:
        let real = arg(0)
        guard !real.isEmpty else { return 1 }
        o.str(FACT_STEP_PATH, redact(real))
        let made = createLocationFolder(in: real)
        guard let dir = made.dir, let id = made.id else {
            let e = made.error ?? "?", markerPrefix = "the marker couldn't be written ("
            if e == "exists" { o.i64(FACT_STEP_CODE, 1) }
            else if e.hasPrefix(markerPrefix) && e.hasSuffix(")") {
                o.i64(FACT_STEP_CODE, 3); o.str(FACT_STEP_TEXT, String(e.dropFirst(markerPrefix.count).dropLast()))
            } else { o.i64(FACT_STEP_CODE, 2); o.str(FACT_STEP_TEXT, e) }
            return 1
        }
        // the probe's file system (volumeFacts' fsType is statfs's f_fstypename of the folder)
        excludeFromBackup(dir, fsType: fileSystemType(real))
        o.str(FACT_LOC_DIR, dir)
        o.str(FACT_LOC_GATE_ID, id)
        return 0

    case CORE_STEP_LOCATION_WRITE:
        // M7 seam: commitLocation's and useThisMac's writes (LocationActions.swift), inside the
        // Controller methods.
        guard args.count >= 4 else { o.str(FACT_STEP_PATH, configFile); return 1 }
        let dir = arg(0), id = arg(1), mount = arg(2), created = arg(3) == "1"
        if dir.isEmpty {
            guard writeCacheLocation(dir: nil, id: nil) else { o.str(FACT_STEP_PATH, configFile); return 1 }
            safeRemove(pendingDeletionFile)
            o.str(FACT_STEP_TEXT, "")
            return 0
        }
        guard writeCacheLocation(dir: dir, id: id) else {
            if created { removeNewLocationFolder(dir) }
            o.str(FACT_STEP_PATH, configFile)
            return 1
        }
        writeLocationHint(hintFor(dir: dir, mount: mount, old: readLocationHint(), newID: created ? id : nil))
        o.str(FACT_STEP_TEXT, redact(dir))
        return 0

    case CORE_STEP_THIS_MAC_DELETE:
        let flagFile = URL(fileURLWithPath: pendingDeletionFile)
        switch arg(0) {
        case "check": o.flag(FACT_STEP_FLAG, thisMacDeletionNow(cachePresent: stemsCachePresent(), bridgeLocation: bridgeSaysLocation()))
        case "delete":
            let ok = removeThisMacStems()
            if !ok { try? Data().write(to: flagFile) }
            return ok ? 0 : 1
        case "defer": try? Data().write(to: flagFile)
        case "unflag": safeRemove(pendingDeletionFile)
        case "facts":                                           // retryThisMacDeletion's inputs
            let s = LocationMonitor.shared.current()
            o.flag(FACT_STEP_FLAG, fileType(pendingDeletionFile) != nil)
            o.flag(FACT_STEP_FLAG2, bridgeSaysLocation())
            var available = false
            if case .chosen(_, _, .available)? = s.status { available = true }
            o.i64(FACT_STEP_CODE, available ? 1 : 0)
            o.str(FACT_STEP_TEXT, s.status?.code ?? "?")
            o.i64(FACT_THIS_MAC_MODEL_FOLDERS, thisMacHasModelFolders() ? 1 : 0)
        default: return 1
        }
        return 0

    case CORE_STEP_MIGRATE_LEGACY:
        o.flag(FACT_STEP_FLAG, installedModels(env.log).migrateLegacy())
        return 0

    case CORE_STEP_REMOVE_LEFTOVERS:
        env.controller.removeLeftovers()
        return 0

    case CORE_STEP_FREE_SPACE:                                  // "doesn't fit": freeBytes(folder)
        guard let b = freeBytes(arg(0)) else { return 1 }
        o.i64(FACT_STEP_CODE, b)
        return 0

    case CORE_STEP_STOP_KEEPING:                                // Stop Keeping…'s removal
        return stopKeeping(arg(0)) ? 0 : 1

    case CORE_STEP_DATE_TEXT:                                   // the "kept until" line's date: dateOnly's style
        let iso = DateFormatter()
        iso.calendar = Calendar(identifier: .gregorian)
        iso.locale = Locale(identifier: "en_US_POSIX")
        iso.dateFormat = "yyyy-MM-dd HH"
        guard let d = iso.date(from: arg(0) + " 12") else { return 1 }
        let medium = DateFormatter(); medium.dateStyle = .medium; medium.timeStyle = .none
        o.str(FACT_STEP_TEXT, medium.string(from: d))
        return 0

    default:
        return 1
    }
}

// MARK: - the steps' parts

/// A payload.json's facts (PAYLOAD), and manifestSeen for one found (the version shown).
private func payloadFacts(_ o: StepOut, _ m: Manifest?, fetch: Int64) {
    o.i64(FACT_PAYLOAD_FETCH, fetch)
    o.flag(FACT_PAYLOAD_FOUND, m != nil)
    if let a = manifestAge(), a.isFinite { o.i64(FACT_PAYLOAD_AGE, Int64(a)) }
    o.str(FACT_STEP_PATH, modelDir + "/hdemucs.onnx")
    guard let m = m else { return }
    o.str(FACT_PAYLOAD_VERSION, m.payloadVersion)
    o.str(FACT_PAYLOAD_SIGNED_BY, m.signedBy ?? "")
    o.str(FACT_PAYLOAD_APP_VERSION, m.app.version)
    o.str(FACT_PAYLOAD_BRIDGE_SHA, m.bridge.sha256)
    o.flag(FACT_PAYLOAD_BRIDGE_HERE, verified(m.bridgePath, sha: m.bridge.sha256, size: nil))
    o.str(FACT_PAYLOAD_MODEL_SHA, m.model.sha256)
    manifestSeen(m)
}

/// fetchAsset's status lines as core_flow_progress(DOWNLOAD): -1 "Checking …", -2 "Downloading …"
/// without a percent, else the bytes of `part` so far of `size` (fetchAsset's own percent).
private func downloadProgress(_ env: ShellStepEnv, part: String, size: Int?) -> (String) -> Void {
    { line in
        guard let c = env.core else { return }
        if line.hasPrefix("Checking ") { core_flow_progress(c, CORE_PROGRESS_DOWNLOAD, -1, 0) }
        else if line.hasSuffix("%"), let s = size, s > 0 { core_flow_progress(c, CORE_PROGRESS_DOWNLOAD, Int64(min(fileSize(part) ?? 0, s)), Int64(s)) }
        else { core_flow_progress(c, CORE_PROGRESS_DOWNLOAD, -2, 0) }
    }
}

/// DOWNLOAD's and UPDATE_SWAP's codes: 0 ok, 1 offline, 2 the disk is full, 3 damaged.
private func fetchedCode(_ f: Fetched) -> Int32 {
    switch f {
    case .ok: return 0
    case .offline: return 1
    case .noSpace: return 2
    case .damaged: return 3
    }
}

/// M7 seam: Models.Outcome carries only its sentence; the core_model_outcome is read back from
/// it (Models.swift's messages, M5's T_MODEL_*), with the OS error for the *_WHY ones. nil: an
/// unknown sentence (the step sets no code and the core uses its own fallback). Pure;
/// tests/mac/shell-test.swift passes every failure message Models.swift has through it.
func modelOutcomeCode(_ r: Outcome) -> (code: core_model_outcome, why: String)? {
    if r.noSpace { return (CORE_MODEL_NO_SPACE_BACK, "") }
    let m = r.message
    for (prefix, c) in [("The Demucs v4 model couldn't be copied: ", CORE_MODEL_NOT_COPIED_WHY),
                        ("The Demucs v4 model couldn't be put in place: ", CORE_MODEL_NOT_PLACED_WHY),
                        ("rekordbox's own stems model couldn't be put back: ", CORE_MODEL_NOT_BACK_WHY)]
    where m.hasPrefix(prefix) && m.hasSuffix(".") {
        return (c, String(m.dropFirst(prefix.count).dropLast()))
    }
    switch m {
    case unknownModelMessage: return (CORE_MODEL_UNKNOWN, "")
    case "rekordbox's stems model can't be read.": return (CORE_MODEL_UNREADABLE, "")
    case "rekordbox's stems model isn't a file.": return (CORE_MODEL_NOT_A_FILE, "")
    case "There isn't enough free space to save a copy of rekordbox's own stems model.": return (CORE_MODEL_NO_SPACE_SAVE, "")
    case "A copy of rekordbox's own stems model couldn't be saved.": return (CORE_MODEL_NOT_SAVED, "")
    case "There isn't enough free space to copy the Demucs v4 model.": return (CORE_MODEL_NO_SPACE_COPY, "")
    case "The Demucs v4 model didn't copy correctly.": return (CORE_MODEL_COPY_BAD, "")
    case "rekordbox's stems model changed meanwhile.": return (CORE_MODEL_CHANGED, "")
    case "There's no saved copy of rekordbox's own stems model on this Mac.": return (CORE_MODEL_NO_SAVED_COPY, "")
    case "There isn't enough free space to put rekordbox's own stems model back.": return (CORE_MODEL_NO_SPACE_BACK, "")
    case "rekordbox's own stems model couldn't be put back.": return (CORE_MODEL_NOT_BACK, "")
    // Models.folder(): a linked or unreadable model folder isn't back (Windows' models.c too)
    case "rekordbox's model folder is a link, so its stems model wasn't checked.": return (CORE_MODEL_NOT_BACK, "")
    case "rekordbox's model folder can't be read.": return (CORE_MODEL_NOT_BACK, "")
    case originalsUnreadableMessage: return (CORE_MODEL_NOT_BACK, "")   // restore's refusal, as Windows' models.c gives it
    case "rekordbox's stems model didn't verify after it was put back.": return (CORE_MODEL_NOT_VERIFIED, "")
    case "The saved copy of rekordbox's own stems model is damaged.": return (CORE_MODEL_SAVED_DAMAGED, "")
    default: return nil
    }
}

/// MODEL_COPY's readback: Models.folder()'s two reasons and unreadable saved originals mean our
/// model wasn't copied (the state as the why), not that Pioneer's isn't back as MODEL_RESTORE
/// reads them (review MP9, SM1). Everything else as modelOutcomeCode. Pure; tests/mac/shell-test.swift checks both.
func copyOutcomeCode(_ r: Outcome) -> (code: core_model_outcome, why: String)? {
    switch r.message {
    case "rekordbox's model folder is a link, so its stems model wasn't checked.": return (CORE_MODEL_NOT_COPIED_WHY, "rekordbox's model folder is a link")
    case "rekordbox's model folder can't be read.": return (CORE_MODEL_NOT_COPIED_WHY, "rekordbox's model folder can't be read")
    // installOurs refuses before any copy when the saved originals can't be listed
    case originalsUnreadableMessage: return (CORE_MODEL_NOT_COPIED_WHY, "the saved copies of rekordbox's own stems model can't be read")
    default: return modelOutcomeCode(r)
    }
}

/// The outcome's code into the step's facts (FACT_STEP_CODE, and FACT_STEP_TEXT for the OS error).
private func setModelOutcome(_ o: StepOut, _ r: Outcome) {
    setModelOutcome(o, modelOutcomeCode(r))
}
private func setModelOutcome(_ o: StepOut, _ code: (code: core_model_outcome, why: String)?) {
    guard let c = code else { return }
    o.i64(FACT_STEP_CODE, Int64(c.code.rawValue))
    if !c.why.isEmpty { o.str(FACT_STEP_TEXT, c.why) }
}

/// M7 seam: removeInLocation's reason is a sentence; REMOVE_LOCATION's code is read back from it
/// (core.h: 1 not connected ... 7 some files couldn't be deleted), with the entry's name for 4
/// and 5 and the OS error for 4 ("<name>: <strerror>", split at the last ": "). Anything else
/// reads as 7. Pure; tests/mac/shell-test.swift passes every reason removeInLocation has through it.
func removalCode(_ why: String) -> (code: Int64, name: String, error: String) {
    switch why {
    case "not connected": return (1, "", "")
    case "the drive can't be opened": return (2, "", "")
    case "the drive can't be read": return (3, "", "")
    case "the folder's marker isn't the one expected": return (6, "", "")
    case "some files couldn't be deleted": return (7, "", "")
    case "the folder's marker can't be read": return (7, "", "")   // nothing deleted: as 7, the folder stays
    default:
        let other = " is on another drive"
        if why.hasSuffix(other) { return (5, String(why.dropLast(other.count)), "") }
        if let sep = why.range(of: ": ", options: .backwards) {   // "<name>: <strerror>"
            return (4, String(why[..<sep.lowerBound]), String(why[sep.upperBound...]))
        }
        return (7, "", "")
    }
}

/// The reason's code into the step's facts (FACT_STEP_CODE, FACT_STEP_TEXT2 the name, FACT_STEP_TEXT the error).
private func setRemovalWhy(_ o: StepOut, _ why: String) {
    let r = removalCode(why)
    o.i64(FACT_STEP_CODE, r.code)
    switch r.code {
    case 5: o.str(FACT_STEP_TEXT2, r.name)
    case 4: o.str(FACT_STEP_TEXT2, r.name); o.str(FACT_STEP_TEXT, r.error)
    default: break
    }
}

/// M7 seam: modelLeftBehind returns its sentence; MODELS_INFO's case is read back from it
/// (core.h: 0 no model, 1 unreadable, 2 Demucs v4, 3 one RB Stems Plus doesn't know, also for
/// any other sentence). Pure; tests/mac/shell-test.swift passes every sentence it has through it.
func leftBehindCode(_ sentence: String) -> Int64 {
    switch sentence {
    case "rekordbox has no stems model.": return 0
    case "rekordbox's stems model can't be read.": return 1
    case "rekordbox still has the Demucs v4 model.": return 2
    default: return 3
    }
}

/// LOCATION_PROBE's code for a refusal (core.h: 1 internal disk ... 9 unreadable).
private func refusalCode(_ r: LocationRefusal) -> Int64 {
    switch r {
    case .internalDisk: return 1
    case .network: return 2
    case .readOnly: return 3
    case .timeMachine: return 4
    case .insideOurFolder: return 5
    case .link: return 6
    case .fileSystem: return 7
    case .path: return 8
    case .unreadable: return 9
    }
}

/// statfs's f_fstypename of `path` ("" if it can't be read): volumeFacts' fsType.
private func fileSystemType(_ path: String) -> String {
    var fs = statfs()
    guard statfs(path, &fs) == 0 else { return "" }
    return withUnsafePointer(to: &fs.f_fstypename) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
}

/// M7 seam: spaceShort (Actions.swift:132) returns its rounded GB text; SPACE needs its measure,
/// need + 200 MiB in bytes for the first volume short of it, 0 when there's room. The same lines.
private func spaceNeeded(model m: Manifest?, cache: Bool) -> Int64 {
    func size(_ p: String) -> Int64 { Int64(fileSize(p) ?? 0) }
    var inHome: Int64 = 0, inRoot: Int64 = 0
    if let m = m, let s = m.model.size {
        let have = fileSize(m.modelPath) == s ? Int64(s) : size(m.modelPath + ".part")
        inHome += Int64(s) - have + Int64(s) + size(modelDir + "/hdemucs.onnx")
    }
    if cache {
        let exe = size(rbApp + "/Contents/MacOS/rekordbox"), lib = size(rbLib)
        let backedUp = FileManager.default.fileExists(atPath: rootDir + "/pioneer/\(rekordboxVersion())/SHA256SUMS")
        inRoot += (backedUp ? 0 : exe + lib) + 2 * lib + exe
    }
    let spare: Int64 = 200 << 20
    func device(_ path: String) -> Int? { (try? FileManager.default.attributesOfFileSystem(forPath: path))?[.systemNumber] as? Int }
    let checks = device(home) == device("/Library") ? [(home, inHome + inRoot)] : [(home, inHome), ("/Library", inRoot)]
    for (path, need) in checks where need > 0 {
        guard let f = freeBytes(path) else { continue }
        if f < need + spare { return need + spare }
    }
    return 0
}

/// The Settings sheet's facts now (showSettings' reads): config.ini as `st`, the background
/// snapshot, settingsStems.
private func settingsFacts(_ o: StepOut, _ st: ConfigState) {
    let loc = LocationMonitor.shared.snapshot(), mounts = currentMounts()
    o.str(FACT_SET_GB, configNumber(st.config.maxGB))
    o.str(FACT_SET_DAYS, configNumber(st.config.maxDays))
    o.flag(FACT_SET_UNREADABLE, st.unreadable != nil)
    o.flag(FACT_SET_CAN_CHOOSE, !stemsCachePresent() || bridgeSaysLocation())
    o.size(settingsSize(st, loc), kind: FACT_SET_SIZE_KIND, bytes: FACT_SET_SIZE)
    switch settingsStems(st, loc).clear {
    case nil: o.i64(FACT_SET_CLEAR, 0)
    case .thisMac?: o.i64(FACT_SET_CLEAR, fileType(defaultCacheDir) == S_IFDIR ? 1 : 2)
    case .drive(let g)?:
        o.i64(FACT_SET_CLEAR, 3)
        o.str(FACT_LOC_DIR, g.dir)
        o.str(FACT_LOC_GATE_ID, g.id)
        o.str(FACT_LOC_DRIVE, loc.drive ?? driveName(g.dir))
        o.str(FACT_STEP_PATH, redact(g.dir))
    }
    // the location row's label parts (labelPath): the drive in FACT_STEP_TEXT, the folders below it
    if st.unreadable != nil { o.i64(FACT_SET_LOCATION, 2) }
    else {
        switch st.location {
        case .thisMac: o.i64(FACT_SET_LOCATION, 0)
        case .malformed: o.i64(FACT_SET_LOCATION, 3)
        case .chosen(let dir, _):
            o.i64(FACT_SET_LOCATION, 1)
            o.str(FACT_SET_LABEL_PATH, labelPath(dir: dir, mounts: mounts).below.joined(separator: "/"))
        }
    }
    o.str(FACT_STEP_TEXT, st.location.chosenDir.map { driveName($0, mounts: mounts) } ?? "")
}

/// M7 seam: settingsStems returns the size as its text; the StemsSize it shows, by its rules.
func settingsSize(_ st: ConfigState, _ snap: LocationSnapshot) -> StemsSize? {
    if st.unreadable != nil { return nil }
    switch st.location {
    case .thisMac: return snap.status == .thisMac ? snap.size : .measuring
    case .malformed: return nil
    case .chosen(let dir, let id):
        switch snap.status {
        case .bridgeTooOld(let d, _)? where d == dir: return snap.size
        case .chosen(let d, let i, .available)? where d == dir && i.lowercased() == id.lowercased(): return snap.size
        case .chosen(let d, _, _)? where d == dir: return .notConnected
        default: return .measuring
        }
    }
}

/// M7 seam: updateApp's work block (Update.swift:164-189), inside the Controller method with its
/// alerts: the app's zip downloaded, unpacked and checked, then updateScript after exit and the
/// app quits. Returns only on failure: 1 offline, 2 the disk is full, 3 anything else.
private func updateSwap(_ env: ShellStepEnv) -> Int32 {
    guard let m = ShellFlowState.shared.manifest, let me = ownBundle() else { return 3 }
    safeRemove(updateDir)
    let zip = updateDir + "/" + m.app.zip, unpacked = updateDir + "/new"
    let got = fetchAsset(m.app.zip, to: zip, sha: m.app.sha256, size: nil, what: "RB Stems Plus \(m.app.version)",
                         status: downloadProgress(env, part: zip + ".part", size: nil), log: env.log)
    let unzip = got == .ok ? runTool("/usr/bin/ditto", ["-x", "-k", zip, unpacked]) : (1, "")
    let newApp = unpacked + "/RB Stems Plus.app"
    let info = NSDictionary(contentsOfFile: newApp + "/Contents/Info.plist")
    guard got == .ok, unzip.0 == 0,
          info?["CFBundleIdentifier"] as? String == bundleID,
          info?["CFBundleShortVersionString"] as? String == m.app.version else {
        env.log("update: \(got == .ok ? "the zip isn't the expected app (\(unzip.1))" : "download failed")")
        safeRemove(updateDir)
        return got == .ok ? 3 : fetchedCode(got)
    }
    // the swap couldn't be started: the app stays open and says the update failed (3). It checks
    // every 0.05 s whether this app has quit: at 0.2 s the new app often started a whole check
    // later (G2 P14, 2026-11-17: the handover took ~320 or ~520 ms by where the exit fell)
    guard afterExit(poll: "0.05", Controller.updateScript,
                    [me, newApp, updateDir, logPath, m.app.version, bundleID, busyFile, lsregister, "/usr/bin/open"]) else {
        env.log("update: the swap after quitting couldn't be started")
        safeRemove(updateDir)
        return 3
    }
    env.log("update: \(appVersion) → \(m.app.version), replacing the app after it quits")
    env.quitNow()
    return 0
}
