// The watcher over the shared C core (the private Windows plan's mac-core §4 Watcher.swift: "the
// watcher's shell"): RB Stems Plus Watcher.app (the helper inside the app, started by the
// LaunchAgent with --watch) makes the core in its watcher role, sends {"ev":"start"} and runs the
// main run loop until the core's close_reply(CORE_CLOSE_WATCHER_DONE). The core decides: the
// rounds, waiting for rekordbox to settle, what went missing, the question, its texts and what
// each answer does, and the prepared playlists' check as rekordbox opens (core/flows.c, the watcher's
// flow). This file only answers its steps with today's platform code (Watcher.swift's lock,
// State.swift's files, Compat.swift's lists, the prepare helper, the notification) and shows its one
// dialog as 1.3.0 did: a window of its own, no Dock icon.
// No window, no snapshot, no tables (§3.11: the watcher creates only what its rounds need).
import AppKit

/// Watcher mode (main.swift): never returns. Every start writes at least one line to watcher.log,
/// also when it stops at once.
func runWatcher() -> Never {
    let wlog = watcherLogPath
    let log = { (s: String) in appendLog(wlog, s) }
    // started from a valid app only (the app repairs or removes a LaunchAgent that isn't): that
    // same app is the one it opens
    let own = ownApp()
    guard let app = own.app else {
        log("not checking: \(Bundle.main.bundlePath) isn't a valid RB Stems Plus (\(own.problem ?? "?")); open RB Stems Plus to repair the watcher")
        exit(0)
    }
    try? FileManager.default.createDirectory(atPath: support, withIntermediateDirectories: true)
    // one watcher at a time (launchd may start it again while it waits); a start that finds the
    // lock taken leaves the again-mark for the running check
    guard takeWatcherLock(watcherLock, again: watcherAgainFile, log: log) else { exit(0) }
    let shell = WatcherShell(app: app, log: log)
    guard shell.start() else {
        safeRemove(watcherLock)
        exit(0)
    }
    // the main run loop: the core's main jobs and the dialog's presentation (until WATCHER_DONE)
    CFRunLoopRun()
    exit(0)
}

private func watcherShellOf(_ ctx: UnsafeMutableRawPointer?) -> WatcherShell { Unmanaged<WatcherShell>.fromOpaque(ctx!).takeUnretainedValue() }

final class WatcherShell {
    let app: String
    let log: (String) -> Void
    private(set) var core: OpaquePointer?
    /// the core's flow thread: one watcher flow
    private let flowQueue = DispatchQueue(label: "rbstemsplus.watcher.flow")
    /// a test's own step runner, dialog and end (shell-test; test builds only); the app's are below
    #if TEST_BUILD
    var runStep: ((core_step, [String], OpaquePointer?) -> Int32)?
    var presentDialog: ((DialogForm) -> DialogAnswer?)?
    var done: () -> Void = { exit(0) }
    #else
    let runStep: ((core_step, [String], OpaquePointer?) -> Int32)? = nil
    let presentDialog: ((DialogForm) -> DialogAnswer?)? = nil
    let done: () -> Void = { exit(0) }
    #endif
    /// the lock and the again-mark (Paths.swift's; a test's own folder)
    var lockPath = watcherLock, againPath = watcherAgainFile
    var gate: Deletable = deletable

    init(app: String, log: @escaping (String) -> Void) {
        self.app = app
        self.log = log
    }

    /// The host table of the watcher role (core.h: post_main, post_work, dialog, step,
    /// close_reply, log).
    func makeHost() -> core_host {
        var h = core_host()
        h.size = UInt32(MemoryLayout<core_host>.size)
        h.ctx = Unmanaged.passUnretained(self).toOpaque()   // the shell lives for the process
        h.facts = { _, _ in }                                // the watcher reads through its steps only
        h.post_main = { _, job in DispatchQueue.main.async { core_run(job) } }
        h.post_work = { ctx, kind, job in
            if kind == CORE_WORK_FLOW { watcherShellOf(ctx).flowQueue.async { core_run(job) } }
            else { DispatchQueue.global(qos: .utility).async { core_run(job) } }
        }
        h.dialog = { ctx, d, r in
            let s = watcherShellOf(ctx)
            return ShellDialogs.dialog(d!, r!) { form in
                if let p = s.presentDialog { return p(form) }
                // a dialog only: no Dock icon, no menu bar; alone, with no app window
                return DialogPresenter.shared.present {
                    NSApplication.shared.setActivationPolicy(.accessory)
                    return CoreSheet(form).run(on: nil)
                }
            }
        }
        h.step = { ctx, kind, args, n, out in
            notOnMain()                                      // the flow thread's (CoreShell.swift)
            let a = (0..<Int(max(0, n))).map { args![$0].map { String(cString: $0) } ?? "" }
            let s = watcherShellOf(ctx)
            if let r = s.runStep { return r(kind, a, out) }
            return s.step(kind, a, out)
        }
        h.close_reply = { ctx, what, _ in
            guard what == CORE_CLOSE_WATCHER_DONE else { return }
            let s = watcherShellOf(ctx)
            DispatchQueue.main.async { s.done() }
        }
        h.log = { ctx, line, n in watcherShellOf(ctx).log(cString(line, n)) }
        return h
    }

    /// core_new in the watcher role, then {"ev":"start"}: its rounds begin on the flow thread.
    /// False: the core refused the host, the config or this build's ids (logged).
    func start() -> Bool {
        guard core_api_version() == CORE_API_VERSION, core_ids_hash() == coreIdsHash else {
            log("not checking: the core's API version or ids hash isn't this build's")
            return false
        }
        var h = makeHost()
        let config = WatcherShell.config(app: app)
        core = config.withCString { core_new(&h, CORE_ROLE_WATCHER, $0, CORE_NTS) }
        guard let c = core else { log("not checking: the core refused to start"); return false }
        let ev = #"{"ev":"start"}"#
        let r = ev.withCString { core_event(c, $0, strlen($0)) }
        if r != Int32(CORE_OK) { log("not checking: the core refused the start (\(r))"); return false }
        return true
    }

    /// The start config: the OS, the version and the paths the prepared playlists' check needs (the
    /// prepare helper's arguments: ShellConfig's, with the app this watcher belongs to).
    static func config(app: String) -> String {
        let paths = ["home": home, "support": support, "app": app, "helper": prepareHelper(app), "cache_default": defaultCacheDir,
                     "log": watcherLogPath, "model": modelDir + "/hdemucs.onnx", "root_ort": rootOrt, "rekordbox": rbApp, "kept": keptDir]
        let p = paths.keys.sorted().map { "\(CoreShell.quote($0)):\(CoreShell.quote(paths[$0]!))" }.joined(separator: ",")
        return "{\"os\":\"mac\",\"version\":\(CoreShell.quote(compiledVersion)),\"paths\":{\(p)}}"
    }

    /// The watcher's steps (core.h "The flows' steps", WATCH_* and REKORDBOX): today's platform
    /// code, nothing decided here. Any worker thread (the flow's).
    func step(_ kind: core_step, _ args: [String], _ out: OpaquePointer?) -> Int32 {
        let o = CoreFactSheet(out: out)
        func arg(_ i: Int) -> String { i < args.count ? args[i] : "" }
        switch kind {
        case CORE_STEP_REKORDBOX:
            o.flag(FACT_REKORDBOX_OPEN, rekordboxOpen())
            o.flag(FACT_PIONEER_UPDATING, pioneerUpdating())
            return 0
        case CORE_STEP_WATCH_LOCK:
            switch arg(0) {
            case "release": safeRemove(lockPath, whole: false, gate)
            case "again": o.flag(FACT_STEP_FLAG, fileType(againPath) != nil)
            case "retake": o.flag(FACT_STEP_FLAG, mkdir(lockPath, 0o700) == 0)
            case "clear_again": safeRemove(againPath, whole: false, gate)
            default: return 1
            }
            return 0
        case CORE_STEP_WATCH_PAUSE:
            sleep(UInt32(max(0, Int(arg(0)) ?? 0)))
            utimes(lockPath, nil)                           // the lock's age says this check is alive
            return 0
        case CORE_STEP_WATCH_BUSY:
            o.flag(FACT_STEP_FLAG, appBusy())
            o.str(FACT_STEP_TEXT, markedAction()?.id ?? "")
            return 0
        case CORE_STEP_WATCH_FACTS:
            let c = chosen(), l = compatibleList(), engine = engineVersion(), rb = rekordboxVersion()
            o.flag(FACT_CHOSEN_MODEL, c.model)
            o.flag(FACT_CHOSEN_CACHE, c.cache)
            o.flag(FACT_STEMS_PLUS_PRESENT, stemsPlusPresent())
            o.flag(FACT_STEMS_CACHE_PRESENT, stemsCachePresent())
            o.str(FACT_REKORDBOX_VERSION, rb)
            o.str(FACT_ENGINE, engine)
            o.flag(FACT_ENGINE_SUPPORTED, engineSupported(engine))
            o.flag(FACT_REKORDBOX_SUPPORTED, rekordboxSupported(rb))
            o.str(FACT_ORT_PREFIX, l.ortVersionPrefix)
            o.str(FACT_STEP_PATH, l.rekordbox.joined(separator: "\n"))
            o.str(FACT_STEP_URL, l.stemsEngine.joined(separator: "\n"))
            o.str(FACT_STEP_TEXT, (try? String(contentsOfFile: dismissedFile, encoding: .utf8)) ?? "")
            o.str(FACT_STEP_TEXT2, versionText(shown: currentShownVersion()))
            return 0
        case CORE_STEP_WATCH_REMEMBER:
            return (try? arg(0).write(toFile: dismissedFile, atomically: true, encoding: .utf8)) != nil ? 0 : 1
        case CORE_STEP_WATCH_REINSTALL:
            // the app does the reinstall: it reads the request at launch, or on the reopen event
            // macOS sends when it is already open; opened once this process has exited (and so let
            // go of the lock): its reinstall changes what the watcher watches
            writeReinstallRequest()
            afterExit(poll: "0.2", "/bin/sleep 0.5; /usr/bin/open -a \"$1\"", [app])
            return 0
        case CORE_STEP_WATCH_KEPT:
            return kept(arg(0), arg(1), arg(2), o)
        case CORE_STEP_WATCH_HELPER:
            // runPrepareHelper, waited for (its own timeout: the helper is stopped then)
            let r = runPrepareHelper(Array(args.dropFirst()), app: app, timeout: TimeInterval(Int(arg(0)) ?? 60))
            o.i64(FACT_STEP_CODE, Int64(r.status))
            o.str(FACT_STEP_TEXT, r.out)
            o.str(FACT_STEP_TEXT2, r.err)
            return 0
        default:
            return 1
        }
    }

    /// WATCH_KEPT's reads and writes (core.h): the prepared playlists' check's platform part.
    func kept(_ op: String, _ a1: String, _ a2: String, _ o: FactSink) -> Int32 {
        switch op {
        case "session":
            o.str(FACT_STEP_TEXT, runTool("/usr/bin/pgrep", ["-f", "rekordbox\\.app/Contents/MacOS/rekordbox( |$)"]).1
                .split(separator: "\n").first.map(String.init) ?? "")
            o.str(FACT_STEP_TEXT2, (try? String(contentsOfFile: keptCheckedFile, encoding: .utf8)) ?? "")
        case "seen":
            try? a1.write(toFile: keptCheckedFile, atomically: true, encoding: .utf8)
        case "sets":
            o.str(FACT_KEPT_SETS, ShellFacts.keptSetsText(keptSets()))
        case "facts":
            ShellFacts.watcherPrepare(o, helper: prepareHelper(app))
        case "noticed":
            if let d = FileManager.default.contents(atPath: keptNoticedFile) { d.withUnsafeBytes { o.bytes(FACT_STEP_TEXT, $0) } }
        case "notice":
            try? Data(a1.utf8).write(to: URL(fileURLWithPath: keptNoticedFile), options: .atomic)
        case "running":
            o.flag(FACT_STEP_FLAG, runTool("/usr/bin/pgrep", ["-f", setRunPattern(a1)]).0 == 0)
        case "notify":
            notify(title: a1, body: a2)
        default:
            return 1
        }
        return 0
    }
}
