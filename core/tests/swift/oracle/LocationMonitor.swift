// Where the stems are saved, worked out off the main thread (3.3.11): a drive may be asleep or
// slow, and the light, Settings, the uninstall confirmation and the report must never wait for
// it. The cheap state (config.ini, the probe, the floor, the bridge's state) is worked out again
// on activation, when a volume is mounted, unmounted or renamed, when the bridge rewrites
// bridge-state (it uses a reconnected drive again by itself), and after a location change, and
// published at once; the size (a walk of every entry: seconds on a cold USB drive) on its own
// queue, only after a location change, after Clear, and every 10 minutes, and dropped if the
// folder in use changed meanwhile.
import AppKit

struct LocationSnapshot: Equatable {
    /// nil until the first measurement
    var status: LocationStatus?
    /// the bridge's state, when it speaks for the rekordbox running now
    var bridge: BridgeState?
    var rekordboxRunning = false
    /// the floor of the folder in use when its disk is under it
    var pausedFloor: Int64?
    var size = StemsSize.measuring
    /// the chosen drive's name (nil on this Mac)
    var drive: String?
    /// the app's probe of the chosen folder (also while an older bridge saves on this Mac)
    var probe: LocationProbe?

    /// Whether the chosen folder can be cleared or deleted now.
    var chosenAvailable: Bool { probe == .available }

    /// The chosen folder as it can be deleted now (its path and marker id), or nil.
    var deletableGate: LocationGate? {
        guard probe == .available else { return nil }
        switch status {
        case .chosen(let d, let i, _)?, .bridgeTooOld(let d, let i)?: return LocationGate(dir: d, id: i)
        default: return nil
        }
    }

    /// The light's location text (Location.swift), if the location isn't fine.
    var lightTip: String? { status.flatMap { locationLight($0, bridge: bridge, rekordboxRunning: rekordboxRunning) } }
}

final class LocationMonitor {
    static let shared = LocationMonitor()
    private let queue = DispatchQueue(label: "rbstemsplus.location", qos: .utility)
    private let sizeQueue = DispatchQueue(label: "rbstemsplus.location.size", qos: .utility)
    private let lock = NSLock()
    private var snap = LocationSnapshot()
    private var working = false, again = false, againSize = false
    private var sizeFolder: String?, sizeTime = Date.distantPast, sizeGen = 0
    private var observing = false
    private var stateWatch: DispatchSourceFileSystemObject?
    /// The cheap state as it is now, and an entry walk's size: the app's own, or the tests'.
    private let cheapState: () -> LocationSnapshot
    private let sizeOf: (String) -> Int64?
    /// Run on the main thread when the snapshot changes (the window's refresh).
    var onChange: (() -> Void)?

    init(cheapState: @escaping () -> LocationSnapshot = measureCheapState, sizeOf: @escaping (String) -> Int64? = entriesSize) {
        self.cheapState = cheapState
        self.sizeOf = sizeOf
    }

    func snapshot() -> LocationSnapshot { lock.lock(); defer { lock.unlock() }; return snap }

    /// Works the state out again in the background; the size too when `size`, when the folder in
    /// use changed, or when it is 10 minutes old.
    func update(size: Bool = false) {
        lock.lock()
        if working { again = true; againSize = againSize || size; lock.unlock(); return }
        working = true
        lock.unlock()
        queue.async { self.measure(size: size) }
    }

    /// The size shown is out of date (the folder was just cleared, or the location changed):
    /// "measuring…" until the next walk. Any thread.
    func invalidateSize() {
        lock.lock(); snap.size = .measuring; sizeTime = .distantPast; lock.unlock()
    }

    private func measure(size: Bool) {
        var s = cheapState()
        let folder = s.status.flatMap(folderInUse)
        lock.lock()
        let old = snap, needSize = size || folder != sizeFolder || Date().timeIntervalSince(sizeTime) > 600
        // the old size stands while it is still the size of this folder; otherwise "measuring…"
        s.size = folder == sizeFolder && !size ? old.size : folder == nil ? (s.status?.dir != nil ? .notConnected : .failed) : .measuring
        if folder == nil { sizeFolder = nil; sizeTime = Date() }
        snap = s
        let rerun = again, rerunSize = againSize
        again = false; againSize = false
        working = rerun
        if needSize && folder != nil { sizeGen += 1 }     // an older walk still queued is then dropped
        let gen = sizeGen
        lock.unlock()
        if s != old, let f = onChange { DispatchQueue.main.async(execute: f) }
        if needSize, let f = folder { sizeQueue.async { self.walk(f, gen) } }
        if rerun { measure(size: rerunSize) }
    }

    /// The size of `folder` (a walk), published only if no newer measurement started meanwhile
    /// and that folder is still the one in use.
    private func walk(_ folder: String, _ gen: Int) {
        lock.lock(); let stale = gen != sizeGen; lock.unlock()
        if stale { return }                             // a newer measurement queued its own walk
        let size: StemsSize = sizeOf(folder).map { .measured($0) } ?? .failed
        lock.lock()
        let current = snap.status.flatMap(folderInUse) == folder
        if current { snap.size = size; sizeFolder = folder; sizeTime = Date() }
        lock.unlock()
        if current, let f = onChange { DispatchQueue.main.async(execute: f) }
    }

    /// From the app's start: mounts, unmounts and renames, and the 10-minute size.
    func startObserving() {
        guard !observing else { return }
        observing = true
        let nc = NSWorkspace.shared.notificationCenter
        for n in [NSWorkspace.didMountNotification, NSWorkspace.didUnmountNotification, NSWorkspace.didRenameVolumeNotification] {
            nc.addObserver(forName: n, object: nil, queue: .main) { [weak self] _ in self?.update() }
        }
        Timer.scheduledTimer(withTimeInterval: 600, repeats: true) { [weak self] _ in self?.update(size: true) }
        watchBridgeState()
        update(size: true)
    }

    /// bridge-state is replaced by a rename in the logs folder: a change in that folder's entries
    /// (not the logs' appends) measures again. The folder gone (an uninstall): watched no more.
    /// Main thread.
    private func watchBridgeState() {
        guard stateWatch == nil else { return }
        let fd = open(logDir, O_EVTONLY | O_DIRECTORY)
        guard fd >= 0 else { return }
        let s = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .delete, .rename], queue: .main)
        s.setEventHandler { [weak self, weak s] in
            if let e = s?.data, !e.isDisjoint(with: [.delete, .rename]) { s?.cancel(); self?.stateWatch = nil }
            self?.update()
        }
        s.setCancelHandler { close(fd) }
        stateWatch = s
        s.resume()
    }

    /// Waits (off the main thread) until a measurement started now is done: for actions that
    /// need the current state, not the cached one.
    func current(size: Bool = false) -> LocationSnapshot {
        let fresh = state(readConfigState())
        var s = snapshot()
        s.status = fresh.status; s.drive = fresh.drive; s.probe = fresh.probe
        let status = fresh.status!
        if size {
            let folder = folderInUse(status)
            s.size = folder.map { f in sizeOf(f).map { .measured($0) } ?? .failed } ?? (status.dir != nil ? .notConnected : .failed)
        }
        return s
    }
}

/// The cheap state, as the app measures it: config.ini, the probe of a chosen folder, whether
/// rekordbox runs and the bridge's state for it, the floor of the folder in use.
func measureCheapState() -> LocationSnapshot {
    let cfg = readConfigState()
    var s = state(cfg)
    s.rekordboxRunning = rekordboxOpen()
    s.bridge = s.rekordboxRunning ? liveBridgeState(readBridgeState(), dir: cfg.location.chosenDir ?? "") : nil
    s.pausedFloor = s.status.flatMap(folderInUse).flatMap { cachePausedFloor($0) }
    return s
}

/// The status, drive and probe from config.ini as it is now (the probe runs once).
private func state(_ cfg: ConfigState) -> LocationSnapshot {
    let installed = stemsCachePresent()
    var s = LocationSnapshot()
    if case .chosen(let d, let i) = cfg.location, cfg.unreadable == nil { s.probe = probeLocation(dir: d, id: i); s.drive = driveName(d) }
    let p = s.probe
    s.status = locationStatus(cfg, bridgeInstalled: installed, bridgeLocation: installed && bridgeSaysLocation(), probe: { _, _ in p ?? .notConnected })
    return s
}

extension ConfigLocation {
    var chosenDir: String? { if case .chosen(let d, _) = self { return d }; return nil }
    var chosenID: String? { if case .chosen(_, let i) = self { return i }; return nil }
}
