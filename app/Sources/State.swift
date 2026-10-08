// The small files the app and the watcher share in ~/Library/Application Support/rbstemsplus:
// installed (the user's choice), dismissed (prompts not to show again), request (the watcher
// asks the app to reinstall) and busy (the app is changing things).
import Foundation

// MARK: - installed: what the user chose

func chosen() -> (model: Bool, cache: Bool) {
    let s = (try? String(contentsOfFile: chosenFile, encoding: .utf8)) ?? ""
    return (s.contains("model=1"), s.contains("cache=1"))
}

func setChosen(model: Bool, cache: Bool) {
    try? FileManager.default.createDirectory(atPath: support, withIntermediateDirectories: true)
    try? "model=\(model ? 1 : 0)\ncache=\(cache ? 1 : 0)\n".write(toFile: chosenFile, atomically: true, encoding: .utf8)
}

// MARK: - dismissed: "Don't ask again for this version"

func dismissed() -> Set<String> {
    Set(((try? String(contentsOfFile: dismissedFile, encoding: .utf8)) ?? "").split(separator: "\n").map(String.init))
}

func remember(_ keys: [String]) {
    let all = dismissed().union(keys).sorted().joined(separator: "\n") + "\n"
    try? all.write(toFile: dismissedFile, atomically: true, encoding: .utf8)
}

/// What went missing from what the user chose, as prompt keys (version-specific, so "Don't ask
/// again for this version" asks again after the next change).
func missingKeys() -> [String] {
    let c = chosen()
    var keys: [String] = []
    if c.cache && !stemsCachePresent() { keys.append("cache:\(rekordboxVersion())") }
    if c.model && !stemsPlusPresent() { keys.append("model:\(engineVersion())") }
    return keys.filter { !dismissed().contains($0) }
}

// MARK: - request: the watcher's "Reinstall now"

func writeReinstallRequest(_ path: String = requestFile) {
    try? "reinstall\n".write(toFile: path, atomically: true, encoding: .utf8)
}

/// How long ago the request was written, or nil if there is none. The request stays: the app
/// takes it only once the reinstall starts or is turned down (takeReinstallRequest).
func reinstallRequestAge(_ path: String = requestFile) -> TimeInterval? {
    guard let a = try? FileManager.default.attributesOfItem(atPath: path) else { return nil }
    return (a[.modificationDate] as? Date).map { max(0, -$0.timeIntervalSinceNow) } ?? 0
}

/// Once per request: the file is removed as it is read. Returns how long ago it was written, or
/// nil if there is none.
@discardableResult
func takeReinstallRequest(_ path: String = requestFile, _ gate: Deletable = deletable) -> TimeInterval? {
    guard let age = reinstallRequestAge(path) else { return nil }
    safeRemove(path, whole: false, gate)
    return age
}

// MARK: - the stems Stems Cache saved

/// Their size, e.g. " (1.2 GB)", or "" under 0.1 GB.
func savedStemsSize() -> String {
    // the last line is the total; any line before it is a warning (an unreadable folder)
    let kb = runTool("/usr/bin/du", ["-sk", cacheDir]).1.split(separator: "\n").last?.split(separator: "\t").first.map(String.init) ?? ""
    let gb = (Double(kb) ?? 0) / 1_048_576
    return gb >= 0.1 ? String(format: " (%.1f GB)", gb) : ""
}

/// Whether the saved stems folder is a link. The app doesn't open with one (startupProblem), but
/// one may be made while it runs: then nothing clears or deletes it, nor what it points to.
func cacheIsLink() -> Bool { fileType(cacheDir) == S_IFLNK }
let cacheLinkLine = "~/Library/Caches/rbstemsplus is a link, so RB Stems Plus leaves it and what it points to alone."

// MARK: - free space

/// Free bytes on `path`'s disk as the bridge measures them: as Finder counts them (with
/// purgeable space, which macOS frees on demand), else statfs's smaller count when that fails or
/// says 0. nil if neither answers. `finder` and `fallback`: the two measures (the tests' own).
func freeBytes(_ path: String, finder: (String) -> Int64? = finderFreeBytes, fallback: (String) -> Int64? = statfsFreeBytes) -> Int64? {
    if let b = finder(path), b > 0 { return b }
    return fallback(path)
}

/// Finder's count: VolumeAvailableCapacityForImportantUsage.
func finderFreeBytes(_ path: String) -> Int64? {
    (try? URL(fileURLWithPath: path).resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage
}

/// statfs's count: the blocks available to this account.
func statfsFreeBytes(_ path: String) -> Int64? {
    var fs = statfs()
    return statfs(path, &fs) == 0 ? Int64(fs.f_bavail) * Int64(fs.f_bsize) : nil
}

/// The size of `path`'s disk as statfs counts it (f_blocks * f_bsize, as the bridge does); nil if
/// it can't be read.
func diskBytes(_ path: String) -> Int64? {
    var fs = statfs()
    return statfs(path, &fs) == 0 && fs.f_blocks > 0 ? Int64(fs.f_blocks) * Int64(fs.f_bsize) : nil
}

/// The most Stems Cache's floor can be: 50 GB as Finder counts them (10^9 bytes).
let cacheFreeFloorMax: Int64 = 50_000_000_000

/// Stems Cache saves no stems while its disk has less than this free (free_floor in
/// bridge/rbstems_bridge.c, the same formula): 10% of the disk's size (`disk`, in bytes), at most
/// 50 GB; 50 GB when the size is unknown. Not a setting.
func cacheFreeFloor(disk: Int64?) -> Int64 {
    guard let d = disk, d > 0 else { return cacheFreeFloorMax }
    return min(cacheFreeFloorMax, d / 10)
}

/// A floor in whole GB (10^9 bytes), rounded, for the texts: 25.6 GB is 26.
func cacheFloorGB(_ floor: Int64) -> Int64 { (floor + 500_000_000) / 1_000_000_000 }

/// The folder whose disk the bridge measures: the cache folder, or the nearest folder above it
/// while there is none yet.
func cacheVolumePath() -> String {
    var p = cacheDir
    while p.count > 1 && !FileManager.default.fileExists(atPath: p) { p = (p as NSString).deletingLastPathComponent }
    return p
}

/// Stems Cache's floor if it has paused saving for lack of space (`free` under the floor for a
/// disk of `disk` bytes), else nil; nil too if the free space is unknown.
func cachePausedFloor(free: Int64?, disk: Int64?) -> Int64? {
    let floor = cacheFreeFloor(disk: disk)
    return free.map { $0 < floor } == true ? floor : nil
}

/// The same, measured on the cache's disk as the bridge measures it.
func cachePausedFloor() -> Int64? {
    let path = cacheVolumePath()
    return cachePausedFloor(free: freeBytes(path), disk: diskBytes(path))
}

/// The Stems Cache light's tip while it is installed and paused under `floor` (Controller.swift, cacheLight).
func cachePausedTip(_ floor: Int64) -> String { "Stems Cache stopped saving stems: your disk has less than \(cacheFloorGB(floor)) GB free." }
/// Said when installing or reinstalling Stems Cache while it would be paused under `floor`.
func cachePausedAtInstall(_ floor: Int64) -> String { "Stems Cache will stop saving stems while your disk has less than \(cacheFloorGB(floor)) GB free." }

// MARK: - busy: the app is half-way through a change

/// An action as the marker records it: what to run again ("install", "reinstall",
/// "reinstall-missing", "uninstall-plus", "uninstall-cache", "uninstall-all") and its label.
struct Action { let id: String; let label: String }

/// The marker keeps the watcher from asking while the app changes things, and the install
/// command from replacing the app meanwhile. Its lines: the writer's pid, that process's start
/// time (seconds since 1970, the kernel's p_starttime: processStartTime), then the action's id and
/// label (the app only; the update script writes the first two). So if the app quits half-way
/// (e.g. "Quit & Reopen" at App Management) the next launch can offer to finish the action, and a
/// pid used again by another process later never counts as the writer. nil removes it.
func setBusyMarker(_ action: Action?, at path: String = busyFile) {
    if let a = action {
        try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        let pid = getpid()
        try? "\(pid)\n\(processStartTime(pid) ?? 0)\n\(a.id)\n\(a.label)\n".write(toFile: path, atomically: true, encoding: .utf8)
    } else { safeRemove(path) }
}

/// When process `pid` started, in whole seconds since 1970 (sysctl KERN_PROC_PID, p_starttime;
/// what ps -o lstart= shows), or nil if there is no such process. Any account's processes.
func processStartTime(_ pid: pid_t) -> Int? {
    guard pid > 0 else { return nil }
    var info = kinfo_proc()
    var size = MemoryLayout<kinfo_proc>.stride
    var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, pid]
    guard sysctl(&mib, u_int(mib.count), &info, &size, nil, 0) == 0, size > 0 else { return nil }
    return Int(info.kp_proc.p_starttime.tv_sec)
}

/// A busy marker as read: the writer's pid, its start time (nil in the old format, which had
/// none), the action (the app's only) and when the file was written.
struct BusyMarker { let pid: pid_t; let start: Int?; let action: Action?; let written: Date? }

/// The marker at `path`, if there is one. The old format (1.0, and updates started by 1.0) has
/// no start time: its second line, if any, is the action's id.
func readBusyMarker(_ path: String = busyFile) -> BusyMarker? {
    guard let s = try? String(contentsOfFile: path, encoding: .utf8) else { return nil }
    let lines = s.split(separator: "\n", omittingEmptySubsequences: false).map { $0.trimmingCharacters(in: .whitespaces) }
    guard let pid = pid_t(lines[0]), pid > 0 else { return nil }
    let start = lines.count >= 2 && !lines[1].isEmpty && lines[1].allSatisfy({ $0.isASCII && $0.isNumber }) ? Int(lines[1]) : nil
    let rest = Array(lines.dropFirst(start == nil ? 1 : 2))
    let action = rest.count >= 2 && !rest[0].isEmpty ? Action(id: rest[0], label: rest[1]) : nil
    let written = (try? FileManager.default.attributesOfItem(atPath: path))?[.modificationDate] as? Date
    return BusyMarker(pid: pid, start: start, action: action, written: written)
}

/// Whether the process that wrote the marker still runs: a process with its pid that started
/// when the marker says. A marker in the old format names no start time: its writer is the
/// process with that pid that had started by the time the file was written (it was running
/// then, and no other process can have its pid at the same time). A process that got the pid
/// later, also after a restart, started after the file was written, so it doesn't count.
/// `startOf`: processStartTime (the tests' own).
func markerLive(_ m: BusyMarker, startOf: (pid_t) -> Int? = processStartTime) -> Bool {
    guard let started = startOf(m.pid) else { return false }
    if let s = m.start { return started == s }
    guard let w = m.written else { return false }
    return started <= Int(w.timeIntervalSince1970.rounded(.down))
}

/// Whether the app (or its update) is changing things: the marker's writer still runs. A
/// marker left by an app that quit stays for the app's next launch.
func appBusy(_ path: String = busyFile) -> Bool {
    readBusyMarker(path).map { markerLive($0) } ?? false
}

/// The action an earlier run of the app left half-done (its process is gone), or nil. Waits a
/// moment for a process macOS is still quitting ("Quit & Reopen" starts the new one at once).
/// A marker of this very process is no leftover; one with this pid but another start time is.
func leftoverAction(_ path: String = busyFile, wait: Int = 20) -> Action? {
    guard let m = readBusyMarker(path) else { return nil }
    var waited = 0
    while markerLive(m) {
        if m.pid == getpid() || waited >= wait { return nil }      // this run's own, or still running after 2 s: really busy
        usleep(100_000); waited += 1
    }
    return m.action
}

/// The action in the busy marker, whether or not the process that wrote it still runs.
func markedAction() -> Action? { readBusyMarker()?.action }

// MARK: - app.lock: one copy of the app at a time

enum LockResult: Equatable { case held(Int32), busy, failed(String) }

/// Takes an exclusive lock on `path` (created if missing), waiting up to `wait` seconds while
/// another process holds it (macOS's "Quit & Reopen" starts the new copy before the old one has
/// quit). O_NOFOLLOW: a link there is refused. O_CLOEXEC: no child process inherits it, so
/// nothing the app started can keep it once the app is gone. Held until the descriptor is
/// closed or the process ends, however it ends.
func takeLock(_ path: String, wait: TimeInterval) -> LockResult {
    let fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0o600)
    guard fd >= 0 else { return .failed(String(cString: strerror(errno))) }
    var st = stat()
    guard fstat(fd, &st) == 0, st.st_mode & S_IFMT == S_IFREG else { close(fd); return .failed("not a file") }
    let deadline = Date().addingTimeInterval(wait)
    while flock(fd, LOCK_EX | LOCK_NB) != 0 {
        let e = errno
        guard e == EWOULDBLOCK || e == EINTR else { close(fd); return .failed(String(cString: strerror(e))) }
        guard Date() < deadline else { close(fd); return .busy }
        usleep(100_000)
    }
    return .held(fd)
}
