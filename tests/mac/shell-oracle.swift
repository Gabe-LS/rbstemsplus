// The Swift app's code that the shell's host tests compare the core with on this Mac's live facts
// (tests/mac/shell-test.sh), where it left app/Sources at M7's switch-over: Controller.refresh()'s
// status-line pieces and the reminders' line, the two lights, prepareFacts and the model's background sha256
// (the watcher's prepared playlists' check moved into the core), copied from the frozen oracle (core/tests/swift/oracle:
// app/Sources at 56d7c11 plus freeze-time fixes, the same sources the core's vectors are made from). Test only: never in the app.
import Foundation

/// On a standard account with something of Stems Cache on this Mac (`cacheTraces`), why its
/// uninstall buttons (and, if this account chose it, Reinstall) are off; nil on an administrator
/// account, or with nothing to remove (needsAdminLine then says why it can't be installed).
func standardAccountLine(admin: Bool, cacheTraces: Bool, cacheChosen: Bool) -> String? {
    guard !admin, cacheTraces else { return nil }
    return cacheChosen ? "Reinstalling or removing Stems Cache needs an administrator account."
                       : "Removing Stems Cache needs an administrator account."
}

/// The status line's request when ownBundle is nil.
let moveToApplications = "Move RB Stems Plus to the Applications folder, then open it from there."

/// The status line while the reinstall reminders are off.
let watcherOffLine = "Reinstall reminders are off. Turn on \(watcherName) in System Settings › General › Login Items & Extensions."

/// What preparing depends on, gathered by prepareFacts() (or by the tests).
struct PrepareFacts {
    var appleSilicon = true
    var helperThere = true
    var rekordboxInstalled = true
    var rekordboxOK = true                  // its version is one Stems Cache supports
    var rekordboxVersion = ""
    var cacheInstalled = true
    var bridgeKeeps = true                  // the bridge says kept=1
    var ortThere = true                     // Pioneer's ONNX Runtime, the copy Stems Cache keeps
    var configUnreadable: String?
    var enabled = true
    /// Stems Cache's light, as the Status tab shows it (cacheLight): preparing needs it on
    var cacheLook = LightLook.on
    var cacheTip = ""
    /// PartAnalysisQuality: 0 sound quality, 1 speed, nil unknown
    var quality: Int?
    /// our model is installed (else rekordbox's own runs)
    var plusOn = false
    /// the installed model's checksum (nil: still being worked out)
    var modelSHA: String?
    var ourModels: [String] = [ourModel]
    /// the folder the stems go to now (nil: none usable), and whether it is a chosen drive's
    var cacheFolder: String?
    var chosenDrive = false
    var maxGB = 20.0
}

/// The facts now, for the app (`app`: the bundle the helper is in). The model's checksum is the
/// background one (nil until worked out: `done` runs then). `measured`: where the stems are saved
/// comes from the app's measurement (LocationMonitor); the watcher, which doesn't measure (and
/// mustn't look at a drive while rekordbox opens), passes false and takes config.ini's chosen
/// folder as it is: its --quick check doesn't look into a cache on another drive.
func prepareFacts(app: String = Bundle.main.bundlePath, measured: Bool = true, done: @escaping () -> Void = {}) -> PrepareFacts {
    var f = PrepareFacts()
    f.appleSilicon = appleSilicon
    f.helperThere = FileManager.default.isExecutableFile(atPath: prepareHelper(app))
    f.rekordboxInstalled = rekordboxInstalled()
    f.rekordboxVersion = rekordboxVersion()
    f.rekordboxOK = rekordboxSupported(f.rekordboxVersion)
    f.cacheInstalled = stemsCachePresent()
    f.bridgeKeeps = bridgeSaysKept()
    f.ortThere = FileManager.default.fileExists(atPath: rootOrt)
    let st = readConfigState()
    f.configUnreadable = st.unreadable
    f.enabled = st.config.enabled
    f.maxGB = st.config.maxGB
    f.plusOn = stemsPlusPresent()
    let loc = LocationMonitor.shared.snapshot()
    let own = f.cacheInstalled && !f.plusOn
    f.modelSHA = currentModelSHA(done: done)
    let light = cacheLight(installed: f.cacheInstalled, plusOn: f.plusOn, chosen: chosen().cache, rekordboxModel: st.config.rekordboxModel,
                           bridgeModels: own ? bridgeCacheModels() : nil, model: own ? f.modelSHA : nil,
                           engine: engineVersion(), pausedFloor: f.enabled ? loc.pausedFloor : nil, location: loc.lightTip, drive: loc.drive)
    f.cacheLook = light.look
    f.cacheTip = light.tip
    f.quality = stemsQuality()
    if let m = savedManifest()?.model.sha256 { f.ourModels.append(m) }
    switch st.location {
    case .thisMac: f.cacheFolder = defaultCacheDir
    case .chosen(let dir, _): f.cacheFolder = !measured || loc.status.flatMap(folderInUse) == dir ? dir : nil; f.chosenDrive = true
    case .malformed: f.cacheFolder = nil
    }
    return f
}

/// The checksum of rekordbox's model as it is now, if it was worked out already (else nil, and it
/// is worked out in the background, then `done` runs on the main thread). Remembered per path,
/// size and modification time: the model is about 300 MB, too slow to hash on the main thread.
func currentModelSHA(_ path: String = modelDir + "/hdemucs.onnx", done: @escaping () -> Void) -> String? {
    let a = try? FileManager.default.attributesOfItem(atPath: path)
    guard let size = a?[.size] as? Int else { return nil }
    let key = "\(path) \(size) \((a?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0)"
    modelSHALock.lock(); let known = modelSHAMemo[key], working = modelSHAWorking.contains(key)
    if known == nil && !working { modelSHAWorking.insert(key) }
    modelSHALock.unlock()
    if let k = known { return k }
    if !working {
        DispatchQueue.global(qos: .utility).async {
            let s = sha256(path)
            modelSHALock.lock(); if let s = s { modelSHAMemo[key] = s }; modelSHAWorking.remove(key); modelSHALock.unlock()
            if s != nil { DispatchQueue.main.async(execute: done) }
        }
    }
    return nil
}

private var modelSHAMemo: [String: String] = [:], modelSHAWorking = Set<String>()

private let modelSHALock = NSLock()

// the lights' rules (Report.swift's until the report's lights became the core's)
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
