// The AppKit shell over the shared C core (the private Windows plan's mac-core §3.5, M7): host->facts,
// the cheap facts, read on main on every refresh and never memoised. Each row of facts.def's cheap
// sheet is today's read, by the Swift function its doc names: the plain cheap rows, M6's rows the
// sheet also carries (stemsPlusBlocked's and stemsCacheBlocked's facts) and M4's view-model rows
// (config.ini, the location snapshot, prepareFacts' inputs, the kept sets). Nothing is decided
// here: a fact is a raw value, the core decides with it.
//
// Each read writes the parts the core asks for (core_facts_parts, the M7 review's M71), so the files
// are read as 1.3.0 reads them: refresh's rows (CORE_FACTS_REFRESH), prepareFacts' own reads only
// with CORE_FACTS_PREPARE, keptSets() only with CORE_FACTS_KEPT. Within a part, where today's code
// reads a fact only in some cases, the sheet does the same: FACT_BRIDGE_MODELS only with
// rekordbox's own model (refresh's `own`), FACT_LIB_SIGNED, FACT_ORT_VERSION and FACT_ORT_PREFIX
// only where stemsCacheBlocked reaches them (codesign runs only when Swift's would). A "not set
// when ..." in facts.def is a fact left unset.
//
// The location's facts are LocationMonitor's background snapshot as it is now, until its memo
// moves into the core (M4); its change is the {"ev":"location"} event (CoreShell.startLocation).
// Every build, like the rest of app/Shell.
import Foundation

/// Where the shell writes facts: the core's sheet (core_fact_*), or a test's recorder.
protocol FactSink {
    func flag(_ f: core_fact, _ v: Bool)
    func i64(_ f: core_fact, _ v: Int64)
    func str(_ f: core_fact, _ s: String)
    /// a string fact from raw bytes (config.ini's, as read: not necessarily UTF-8)
    func bytes(_ f: core_fact, _ raw: UnsafeRawBufferPointer)
}

extension FactSink {
    func bytes(_ f: core_fact, _ raw: UnsafeRawBufferPointer) { str(f, String(decoding: raw, as: UTF8.self)) }
}

/// The core's sheet, lent to host->facts, host->slow_fact or host->step (borrowed).
struct CoreFactSheet: FactSink {
    let out: OpaquePointer?
    func flag(_ f: core_fact, _ v: Bool) { core_fact_flag(out, f, v ? 1 : 0) }
    func i64(_ f: core_fact, _ v: Int64) { core_fact_i64(out, f, v) }
    func str(_ f: core_fact, _ s: String) { s.withCString { core_fact_str(out, f, $0, strlen($0)) } }
    func bytes(_ f: core_fact, _ raw: UnsafeRawBufferPointer) { core_fact_str(out, f, raw.bindMemory(to: CChar.self).baseAddress, raw.count) }
}

enum ShellFacts {
    static let refreshPart = UInt32(CORE_FACTS_REFRESH), preparePart = UInt32(CORE_FACTS_PREPARE), keptPart = UInt32(CORE_FACTS_KEPT)
    static let allParts = refreshPart | preparePart | keptPart

    /// host->facts (main): the parts asked for (core_facts_parts). `helper`: the prepare helper
    /// this shell spawns.
    static func cheap(_ o: FactSink, helper: String, parts: UInt32) {
        if parts & refreshPart != 0 { refresh(o, helper: helper) }
        if parts & preparePart != 0 { prepare(o) }
        if parts & keptPart != 0 { o.str(FACT_KEPT_SETS, keptSetsText(keptSets())) }   // StatusView.reload's keptSets()
    }

    /// CORE_FACTS_REFRESH: Controller.refresh's reads, the flows' and prepareFacts' share of them.
    static func refresh(_ o: FactSink, helper: String) {
        let plus = stemsPlusPresent(), cache = stemsCachePresent(), c = chosen()
        let installed = rekordboxInstalled(), version = rekordboxVersion()
        let list = compatibleList()                         // once for every list check below
        o.flag(FACT_STEMS_PLUS_PRESENT, plus)
        o.flag(FACT_STEMS_CACHE_PRESENT, cache)
        o.flag(FACT_CHOSEN_MODEL, c.model)
        o.flag(FACT_CHOSEN_CACHE, c.cache)
        o.flag(FACT_CACHE_TRACES, cacheTraces())
        o.flag(FACT_REKORDBOX_INSTALLED, installed)
        o.flag(FACT_OWN_BUNDLE, ownBundle() != nil)
        o.flag(FACT_CACHE_IS_LINK, cacheIsLink())
        o.flag(FACT_USER_IS_ADMIN, userIsAdmin)
        o.str(FACT_REKORDBOX_VERSION, version)
        // stemsPlusBlocked's facts (engineVersion() is also the cache light's)
        let engine = engineVersion()
        o.str(FACT_ENGINE, engine)
        o.flag(FACT_ENGINE_SUPPORTED, list.stemsEngine.contains(engine))
        // stemsCacheBlocked's, in its order (prepareFacts reads rekordboxOK and ortThere too)
        cacheBlocked(o, admin: userIsAdmin, installed: installed, version: version, present: cache, list: list, always: true)
        // config.ini, by common/'s grammar (readConfigState): its bytes, and the values refresh,
        // prepareFacts and the raised limit's writeConfig read
        config(o, readConfigState(), bytes: FileManager.default.contents(atPath: configFile))
        // where the stems are saved: the background snapshot now
        location(o, LocationMonitor.shared.snapshot())
        // the cache light's: with rekordbox's own model, what the installed bridge saves
        // (refresh reads it only then), and the model file's key for the core's sha256 memo
        bridgeModels(o, plus: plus, cache: cache) { bridgeCacheModels() }
        var ms = stat()
        if lstat(modelDir + "/hdemucs.onnx", &ms) == 0 {     // attributesOfItem's: the item itself
            o.i64(FACT_MODEL_SIZE, Int64(ms.st_size))
            o.i64(FACT_MODEL_MTIME, Int64(ms.st_mtimespec.tv_sec) * 1_000_000_000 + Int64(ms.st_mtimespec.tv_nsec))
        }
        // prepareFacts' cheap checks (refresh's part: no file read)
        o.flag(FACT_APPLE_SILICON, appleSilicon)
        o.flag(FACT_HELPER_THERE, FileManager.default.isExecutableFile(atPath: helper))
    }

    /// CORE_FACTS_PREPARE: prepareFacts' own file reads (rekordbox's library, its settings, the
    /// saved payload.json).
    static func prepare(_ o: FactSink) {
        o.flag(FACT_BRIDGE_KEEPS, bridgeSaysKept())
        o.i64(FACT_STEMS_QUALITY, Int64(stemsQuality() ?? -1))
        var ours = [ourModel]
        if let m = savedManifest()?.model.sha256 { ours.append(m) }
        o.str(FACT_OUR_MODELS, ours.joined(separator: "\n"))
    }

    /// WATCH_KEPT "facts": prepareFacts' reads as the watcher made them (prepareFacts(measured:
    /// false)): no location measured, the model's sha256 not worked out; the cache light's inputs.
    static func watcherPrepare(_ o: FactSink, helper: String) {
        let plus = stemsPlusPresent(), cache = stemsCachePresent(), version = rekordboxVersion()
        o.flag(FACT_APPLE_SILICON, appleSilicon)
        o.flag(FACT_HELPER_THERE, FileManager.default.isExecutableFile(atPath: helper))
        o.flag(FACT_REKORDBOX_INSTALLED, rekordboxInstalled())
        o.str(FACT_REKORDBOX_VERSION, version)
        o.flag(FACT_REKORDBOX_SUPPORTED, rekordboxSupported(version))
        o.flag(FACT_STEMS_CACHE_PRESENT, cache)
        o.flag(FACT_STEMS_PLUS_PRESENT, plus)
        o.flag(FACT_CHOSEN_CACHE, chosen().cache)
        o.str(FACT_ENGINE, engineVersion())
        o.flag(FACT_ROOT_ORT_EXISTS, FileManager.default.fileExists(atPath: rootOrt))
        config(o, readConfigState(), bytes: nil)
        bridgeModels(o, plus: plus, cache: cache) { bridgeCacheModels() }
        prepare(o)
    }

    /// REPORT_FACTS: the lights' inputs as reportFacts read them, with `m`'s measurement (the
    /// location now, the model's sha256 now).
    static func reportLights(_ o: FactSink, _ m: ReportMeasure) {
        let plus = stemsPlusPresent(), cache = stemsCachePresent(), cfg = readConfig()
        o.flag(FACT_STEMS_PLUS_PRESENT, plus)
        o.flag(FACT_STEMS_CACHE_PRESENT, cache)
        o.flag(FACT_CHOSEN_CACHE, chosen().cache)
        o.flag(FACT_CONFIG_ENABLED, cfg.enabled)
        o.flag(FACT_CONFIG_RB_MODEL, cfg.rekordboxModel)
        o.str(FACT_ENGINE, engineVersion())
        if cache, let models = bridgeCacheModels() { o.str(FACT_BRIDGE_MODELS, models.joined(separator: "\n")) }
        o.str(FACT_STEP_TEXT, isSHA256(m.modelSha) ? m.modelSha : "")
        location(o, m.loc)
        o.i64(FACT_LOC_PAUSED_FLOOR, m.loc.status.flatMap(folderInUse).flatMap { cachePausedFloor($0) } ?? -1)
    }

    /// config.ini's rows: its bytes (nil: no file) and readConfigState's values.
    static func config(_ o: FactSink, _ st: ConfigState, bytes: Data?) {
        if let d = bytes { d.withUnsafeBytes { raw in o.bytes(FACT_CONFIG_TEXT, raw) } }
        o.flag(FACT_CONFIG_ENABLED, st.config.enabled)
        o.flag(FACT_CONFIG_RB_MODEL, st.config.rekordboxModel)
        o.str(FACT_CONFIG_MAX_GB, configNumber(st.config.maxGB))
        o.str(FACT_CONFIG_MAX_DAYS, configNumber(st.config.maxDays))
        if let why = st.unreadable { o.str(FACT_CONFIG_UNREADABLE, why) }
        switch st.location {
        case .thisMac: o.i64(FACT_CONFIG_LOCATION, 0); o.str(FACT_CHOSEN_LOCATION, "")
        case .chosen(let dir, _): o.i64(FACT_CONFIG_LOCATION, 1); o.str(FACT_CONFIG_DIR, dir); o.str(FACT_CHOSEN_LOCATION, dir)
        case .malformed: o.i64(FACT_CONFIG_LOCATION, 2); o.str(FACT_CHOSEN_LOCATION, "")
        }
    }

    /// FACT_BRIDGE_MODELS: read (`models`) only with rekordbox's own model, as refresh's `own`.
    static func bridgeModels(_ o: FactSink, plus: Bool, cache: Bool, _ models: () -> [String]?) {
        guard cache && !plus, let m = models() else { return }
        o.str(FACT_BRIDGE_MODELS, m.joined(separator: "\n"))
    }

    /// The library reads stemsCacheBlocked makes (a test's stand-ins in shell-test).
    struct LibReads {
        var exists: (String) -> Bool = { FileManager.default.fileExists(atPath: $0) }
        var signed: (String) -> Bool = { signedByPioneer($0) }
        var version: (String) -> String? = { dylibVersion($0) }
    }

    /// stemsCacheBlocked()'s facts, in its order, stopping where it stops (the BLOCKED step's
    /// "cache" and the cheap sheet). `always`: the cheap ones before the library (rekordboxOK,
    /// ortThere and the prototype's folder) are written whatever comes first, for prepareFacts;
    /// the library's signature (codesign), its version and the list's prefix only where
    /// stemsCacheBlocked reaches them.
    static func cacheBlocked(_ o: FactSink, admin: Bool, installed: Bool, version: String, present: Bool,
                             list: Manifest.Compatible, always: Bool, reads: LibReads = LibReads()) {
        let supported = listed(version, in: list.rekordbox)   // rekordboxSupported(version)
        let reached = admin && installed
        if always || reached { o.flag(FACT_REKORDBOX_SUPPORTED, supported) }
        guard always || (reached && supported) else { return }
        // 1.3.0's order: the root copy only with Stems Cache in (the prototype's check), the
        // prototype's folder only when that copy is missing; the cheap sheet reads both always
        // (prepareFacts' ortThere, MP2)
        let rootThere = (always || present) && reads.exists(rootOrt)
        if always || present { o.flag(FACT_ROOT_ORT_EXISTS, rootThere) }
        let protoAsked = always || (present && !rootThere)
        let prototype = protoAsked && reads.exists(prototypeRootDir)
        if protoAsked { o.flag(FACT_PROTOTYPE_EXISTS, prototype) }
        if !always && present && !rootThere && prototype { return }   // the test version: before exists(lib)
        let lib = present ? rootOrt : rbLib
        let exists = reads.exists(lib)
        o.flag(FACT_LIB_EXISTS, exists)
        guard reached, supported, !(present && !rootThere && prototype), exists else { return }
        let signed = reads.signed(lib)
        o.flag(FACT_LIB_SIGNED, signed)
        guard signed else { return }
        if let v = reads.version(lib) { o.str(FACT_ORT_VERSION, v) }
        o.str(FACT_ORT_PREFIX, list.ortVersionPrefix)
    }

    /// The location snapshot's facts (LocationMonitor.swift's LocationSnapshot): its status, the
    /// bridge's live state, rekordbox running, the paused floor, the folder in use, the drive.
    static func location(_ o: FactSink, _ s: LocationSnapshot, mounts: [Mount] = currentMounts()) {
        o.flag(FACT_LOC_MEASURED, s.status != nil)
        o.flag(FACT_REKORDBOX_RUNNING, s.rekordboxRunning)
        o.i64(FACT_LOC_PAUSED_FLOOR, s.pausedFloor ?? -1)
        if let d = s.drive { o.str(FACT_LOC_DRIVE, d) }
        if let st = s.status {
            o.i64(FACT_LOC_KIND, locKind(st))
            switch st {
            case .chosen(let dir, _, let probe):
                o.i64(FACT_LOC_PROBE, probeCode(probe))
                o.str(FACT_LOC_STATUS_DRIVE, driveName(dir, mounts: mounts))
            case .bridgeTooOld(let dir, _):
                o.str(FACT_LOC_STATUS_DRIVE, driveName(dir, mounts: mounts))
            default: break
            }
            if let f = folderInUse(st) { o.str(FACT_LOC_IN_USE, f) }
        }
        if let b = s.bridge {
            o.str(FACT_LOC_BRIDGE_STATE, b.state)
            o.str(FACT_LOC_BRIDGE_REASON, b.reason)
            if !b.location.isEmpty { o.str(FACT_LOC_BRIDGE_DRIVE, driveName(b.location, mounts: mounts)) }
        }
    }

    /// LocationStatus as pure.h's LOC_* (LOC_THIS_MAC 0, LOC_CHOSEN 1, LOC_BRIDGE_TOO_OLD 2,
    /// LOC_CONFIG_UNREADABLE 3, LOC_CONFIG_MALFORMED 4; shell-test reads pure.h to check them).
    static func locKind(_ s: LocationStatus) -> Int64 {
        switch s {
        case .thisMac: return 0
        case .chosen: return 1
        case .bridgeTooOld: return 2
        case .configUnreadable: return 3
        case .configMalformedLocation: return 4
        }
    }

    /// LocationProbe as pure.h's PROBE_* (in LocationProbe's order; shell-test checks them).
    static func probeCode(_ p: LocationProbe) -> Int64 {
        switch p {
        case .available: return 0
        case .notConnected: return 1
        case .folderMissing: return 2
        case .markerMissingOrWrong: return 3
        case .link: return 4
        case .network: return 5
        case .readOnly: return 6
        case .noPermission: return 7
        }
    }

    /// PrepareView.swift's dateOnly (private there): the Status tab's "until" column.
    static let dateOnly: DateFormatter = { let f = DateFormatter(); f.dateStyle = .medium; f.timeStyle = .none; return f }()

    /// keptSets() as FACT_KEPT_SETS: a set a line, its name, a tab, its until date as the Status
    /// tab shows it ("" unknown), a tab, its manifest's file count.
    static func keptSetsText(_ sets: [KeptSet]) -> String {
        sets.map { "\($0.name)\t\($0.until.map { dateOnly.string(from: $0) } ?? "")\t\($0.files)" }.joined(separator: "\n")
    }

    /// updateWatcherState's measurement without its main-thread half (the core logs the line and
    /// shows the status line when it changes): whether the reinstall reminders are off.
    static func watcherIsOffNow() -> Bool {
        guard #available(macOS 13, *) else { return false }
        return watcherOff(plistThere: fileType(agentPlist) != nil,
                          loaded: runTool("/bin/launchctl", ["print", "\(guiDomain)/\(agentLabel)"]).0 == 0,
                          disabled: launchdDisabled(runTool("/bin/launchctl", ["print-disabled", guiDomain]).1, label: agentLabel))
    }
}

