// Where Stems Cache saves stems: this Mac (~/Library/Caches/rbstemsplus, the default) or our own
// folder, "RB Stems Plus Cache.noindex", on an external drive the user chose (Settings). Shared
// with the bridge: the folder's name, its marker file, the config.ini keys (Config.swift), the
// path rules, the bridge's state file and how an entry's size is counted. The bridge never
// creates our folder or anything above it; the app creates it, writes its marker once, and
// deletes in it only through descriptors it has verified (removeInLocation).
import DiskArbitration
import Foundation

/// Our folder's name, inside the folder the user chose (.noindex: Spotlight skips it).
let locationFolderName = "RB Stems Plus Cache.noindex"
/// The marker in our folder: "id=<UUID>\nformat=rbstems-v1\n", written once, never rewritten.
let markerName = ".rbstemsplus-cache"
let markerFormat = "rbstems-v1"
let markerMax = 4096
/// The bridge's state while rekordbox runs (written by the bridge, read for the light).
let bridgeStateFile = logDir + "/bridge-state"
/// Where the stems were before: the mount point and the path below it (finding them on a
/// renamed drive), and the marker ids this account created (no sharing note for its own stems).
let locationHintFile = support + "/location-hint"
/// This Mac's stems couldn't all be deleted after a switch to a drive: retried at launch.
let pendingDeletionFile = support + "/pending-deletion"
/// The longest cache_dir the bridge takes (its paths below must still fit).
let cacheDirMax = 700

// MARK: - the path rules (the bridge's too)

/// Why `dir` can't be our folder's path, or nil: absolute, at most 700 bytes, plain components
/// (no empty, "." or ".."), no '"', control character or DEL (config.ini quotes it), ending in
/// our folder's name, below at least one folder.
func cacheDirProblem(_ dir: String) -> String? {
    let b = Array(dir.utf8)
    guard b.first == 0x2F else { return "not an absolute path" }
    guard b.count <= cacheDirMax else { return "over \(cacheDirMax) bytes" }
    if b.contains(where: { $0 < 0x20 || $0 == 0x7F || $0 == 0x22 }) { return "a quote or a control character" }
    guard let c = pathComponents(dir) else { return "an empty, \".\" or \"..\" component" }
    guard c.last == locationFolderName else { return "not a folder named \(locationFolderName)" }
    // never at the root: that is the boot volume, and the bridge refuses it too (location_path_ok)
    guard c.count >= 2 else { return "our folder at the root of the disk" }
    return nil
}

/// An id as the marker and cache_id hold it: 8-4-4-4-12 hex digits (either case).
func isUUID(_ s: String) -> Bool {
    let parts = s.split(separator: "-", omittingEmptySubsequences: false)
    return parts.map(\.count) == [8, 4, 4, 4, 12] && parts.allSatisfy { $0.allSatisfy { $0.isASCII && $0.isHexDigit } }
}

// MARK: - the marker

/// The marker's text for `id`.
func markerText(_ id: String) -> String { "id=\(id)\nformat=\(markerFormat)\n" }

/// The id in a marker's text, if it is a valid marker, exactly as the bridge's marker_ok judges
/// it (the shared fixtures: tests/marker-fixtures.txt): no NUL byte; lines split at "\n" with
/// nothing trimmed (a CR is part of the value); a line starting "id=" with more after it is an
/// id, one starting "format=" with more after it a format (anything else is ignored); every id
/// the same valid id (case ignored), every format exactly ours, and at least one of each.
func markerID(_ text: String) -> String? {
    if text.utf8.contains(0) { return nil }
    var id: String?, format = false
    for line in text.components(separatedBy: "\n") {
        if line.hasPrefix("id="), line.utf8.count > 3 {
            let v = String(line.dropFirst(3))
            guard isUUID(v), id.map({ $0.lowercased() == v.lowercased() }) ?? true else { return nil }
            if id == nil { id = v }
        } else if line.hasPrefix("format="), line.utf8.count > 7 {
            guard line.dropFirst(7) == markerFormat else { return nil }
            format = true
        }
    }
    return format ? id : nil
}

/// Reads a marker through an open folder: a regular file (never a link or a FIFO), at most 4 KB.
/// Its id, or the errno (ENOENT: none; EINVAL: not a valid marker).
func readMarker(at dirfd: Int32) -> (id: String?, errno: Int32) {
    let fd = openat(dirfd, markerName, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
    if fd < 0 { return (nil, errno) }
    defer { close(fd) }
    var st = stat()
    guard fstat(fd, &st) == 0, st.st_mode & S_IFMT == S_IFREG, st.st_size <= markerMax else { return (nil, EINVAL) }
    var buf = [UInt8](repeating: 0, count: markerMax + 1)
    let n = read(fd, &buf, buf.count)
    guard n >= 0, n <= markerMax, let id = markerID(String(decoding: buf[0..<n], as: UTF8.self)) else { return (nil, n < 0 ? errno : EINVAL) }
    return (id, 0)
}

/// The marker of the folder at `dir` (opened without following a link), or the errno.
func readMarker(_ dir: String) -> (id: String?, errno: Int32) {
    let fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
    if fd < 0 { return (nil, errno) }
    defer { close(fd) }
    return readMarker(at: fd)
}

// MARK: - volumes

/// A mounted volume as getmntinfo lists it.
struct Mount: Equatable {
    var path: String
    var local: Bool
    var readOnly: Bool
    var fsType: String = ""
    var device: String = ""
}

/// The volumes mounted now, without waiting for a slow one (MNT_NOWAIT).
func currentMounts() -> [Mount] {
    var p: UnsafeMutablePointer<statfs>?
    let n = getmntinfo(&p, MNT_NOWAIT)
    guard n > 0, let list = p else { return [] }
    return (0..<Int(n)).map { i in
        var fs = list[i]
        let path = withUnsafePointer(to: &fs.f_mntonname) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        let type = withUnsafePointer(to: &fs.f_fstypename) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        let dev = withUnsafePointer(to: &fs.f_mntfromname) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        return Mount(path: path, local: fs.f_flags & UInt32(MNT_LOCAL) != 0, readOnly: fs.f_flags & UInt32(MNT_RDONLY) != 0, fsType: type, device: dev)
    }
}

/// The mount that holds `path` among `mounts`: the longest mount point that is a whole-component
/// prefix of it, never "/" (a leftover /Volumes/GLS folder on the boot disk is no connection).
func mountHolding(_ path: String, _ mounts: [Mount]) -> Mount? {
    guard let c = pathComponents(path) else { return nil }
    return mounts.filter { m in
        guard m.path != "/", let mc = pathComponents(m.path) else { return false }
        return mc.count < c.count && Array(c.prefix(mc.count)) == mc
    }.max { $0.path.count < $1.path.count }
}

/// The drive's name as the user knows it: the mount point's last component ("GLS").
func driveName(_ dir: String, mounts: [Mount] = currentMounts()) -> String {
    if let m = mountHolding(dir, mounts) { return (m.path as NSString).lastPathComponent }
    if let c = pathComponents(dir), c.count >= 2, c[0] == "Volumes" { return c[1] }
    return ((dir as NSString).deletingLastPathComponent as NSString).lastPathComponent
}

/// What decides whether a folder may hold our folder (3.1.6), worked out by volumeFacts.
struct VolumeFacts: Equatable {
    var mountPoint: String
    var name: String
    var fsType: String              // statfs's f_fstypename: apfs, hfs, exfat, msdos, ntfs, …
    var local: Bool
    var readOnly: Bool
    var network: Bool               // DiskArbitration's VolumeNetwork
    var deviceInternal: Bool?       // DiskArbitration's DeviceInternal; nil: unknown
    var deviceProtocol: String?     // DiskArbitration's DeviceProtocol (USB, Thunderbolt, Disk Image, …)
    var boot: Bool                  // the same device as / or /System/Volumes/Data
    var timeMachine: Bool
}

/// Q6 (decided by the owner): external drives only, by DiskArbitration's DeviceInternal. Kept in
/// one place: Q10 (Thunderbolt/USB4 enclosures and the SD slot reporting internal) may widen it.
func externalDeviceAllowed(isInternal: Bool?, deviceProtocol: String?) -> Bool { isInternal == false }

/// The file systems our folder may be on: APFS, Mac OS Extended, exFAT and FAT32.
let locationFileSystems: Set<String> = ["apfs", "hfs", "exfat", "msdos"]

/// The facts of the volume holding `path` (realpath'd by the caller), or nil if it can't be read.
func volumeFacts(_ path: String) -> VolumeFacts? {
    var fs = statfs()
    guard statfs(path, &fs) == 0 else { return nil }
    let mount = withUnsafePointer(to: &fs.f_mntonname) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
    let type = withUnsafePointer(to: &fs.f_fstypename) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
    let from = withUnsafePointer(to: &fs.f_mntfromname) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
    var st = stat(), root = stat(), data = stat()
    let dev = stat(path, &st) == 0 ? st.st_dev : -1
    let boot = (stat("/", &root) == 0 && root.st_dev == dev) || (stat("/System/Volumes/Data", &data) == 0 && data.st_dev == dev)
    var isInternal: Bool?, network = false, proto: String?
    if let session = DASessionCreate(kCFAllocatorDefault), from.hasPrefix("/dev/"),
       let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, String(from.dropFirst(5))),
       let desc = DADiskCopyDescription(disk) as? [String: Any] {
        isInternal = desc[kDADiskDescriptionDeviceInternalKey as String] as? Bool
        network = desc[kDADiskDescriptionVolumeNetworkKey as String] as? Bool ?? false
        proto = desc[kDADiskDescriptionDeviceProtocolKey as String] as? String
    }
    let name = (try? URL(fileURLWithPath: mount).resourceValues(forKeys: [.volumeNameKey]))?.volumeName ?? (mount as NSString).lastPathComponent
    let tm = timeMachineMounts().contains(mount) || fileType(mount + "/Backups.backupdb") == S_IFDIR
        || (type == "apfs" && from.hasPrefix("/dev/") && apfsRoles(String(from.dropFirst(5))).contains("Backup"))
    return VolumeFacts(mountPoint: mount, name: name, fsType: type, local: fs.f_flags & UInt32(MNT_LOCAL) != 0,
                       readOnly: fs.f_flags & UInt32(MNT_RDONLY) != 0 || type == "ntfs", network: network || from.hasPrefix("//"),
                       deviceInternal: isInternal, deviceProtocol: proto, boot: boot, timeMachine: tm)
}

/// The mount points of Time Machine destinations that are connected (tmutil destinationinfo).
func timeMachineMounts() -> Set<String> {
    let r = runTool("/usr/bin/tmutil", ["destinationinfo", "-X"])
    guard r.0 == 0, let plist = try? PropertyListSerialization.propertyList(from: Data(r.1.utf8), format: nil) as? [String: Any],
          let list = plist["Destinations"] as? [[String: Any]] else { return [] }
    return Set(list.compactMap { $0["MountPoint"] as? String })
}

/// An APFS volume's roles ("System", "Data", "Backup": a Time Machine destination, this Mac's or
/// another's) from `diskutil apfs list -plist`, by its device ("disk5s2"); [] if unknown.
func apfsRoles(_ device: String) -> [String] {
    let r = runTool("/usr/sbin/diskutil", ["apfs", "list", "-plist"])
    return r.0 == 0 ? apfsRoles(plist: Data(r.1.utf8), device: device) : []
}

func apfsRoles(plist: Data, device: String) -> [String] {
    guard let d = try? PropertyListSerialization.propertyList(from: plist, format: nil) as? [String: Any],
          let containers = d["Containers"] as? [[String: Any]] else { return [] }
    for c in containers {
        for v in (c["Volumes"] as? [[String: Any]]) ?? [] where v["DeviceIdentifier"] as? String == device {
            return (v["Roles"] as? [String]) ?? []
        }
    }
    return []
}

/// The refusal texts of 3.1.6.
func locationRefusal(_ why: LocationRefusal, drive: String) -> String {
    switch why {
    case .internalDisk: return "Choose a folder on an external drive. To save stems on this Mac, use This Mac."
    case .network: return "Choose a drive connected to this Mac, not a network drive."
    case .readOnly: return "\"\(drive)\" is read-only."
    case .timeMachine: return "\"\(drive)\" is a Time Machine backup drive."
    case .insideOurFolder: return "Choose the folder that contains \"RB Stems Plus Cache\", not a folder inside it."
    case .link: return "A folder on the way to the one you chose is a link. Choose the folder itself."
    case .fileSystem: return "\"\(drive)\" isn't formatted as APFS, Mac OS Extended, exFAT or FAT32."
    case .path: return "That folder's path is too long, or has a character Stems Cache can't use (a double quote or a control character). Choose another folder."
    case .unreadable: return "RB Stems Plus can't read that folder's drive. Choose another folder."
    }
}

enum LocationRefusal: Equatable { case internalDisk, network, readOnly, timeMachine, insideOurFolder, link, fileSystem, path, unreadable }

/// Whether `folder` (as chosen, already its realpath: `real`) may hold our folder, by the facts
/// of its volume. nil: it may.
func locationRefusalFor(folder: String, real: String?, facts: VolumeFacts?) -> LocationRefusal? {
    guard let real = real, let f = facts else { return .unreadable }
    if !f.local || f.network { return .network }
    if f.boot || !externalDeviceAllowed(isInternal: f.deviceInternal, deviceProtocol: f.deviceProtocol) { return .internalDisk }
    if f.timeMachine { return .timeMachine }
    if !locationFileSystems.contains(f.fsType) && f.fsType != "ntfs" { return .fileSystem }
    if f.readOnly { return .readOnly }
    // no link from the mount point down: the folder as chosen is its own realpath
    if pathComponents(folder) != pathComponents(real) { return .link }
    guard let c = pathComponents(real), let mc = pathComponents(f.mountPoint), c.count >= mc.count, Array(c.prefix(mc.count)) == mc else { return .link }
    // not inside our folder, and no marker above (its own marker is fine: it is our folder)
    if c.dropLast().contains(locationFolderName) { return .insideOurFolder }
    var p = f.mountPoint
    for name in c.dropFirst(mc.count).dropLast() {
        p += "/" + name
        if fileType(p + "/" + markerName) != nil { return .insideOurFolder }
    }
    if f.mountPoint != real, fileType(f.mountPoint + "/" + markerName) != nil { return .insideOurFolder }
    let dir = c.last == locationFolderName ? real : real + "/" + locationFolderName
    if cacheDirProblem(dir) != nil { return .path }
    return nil
}

// MARK: - where the stems are now

/// What the app sees of the chosen location (its own probe).
enum LocationProbe: String, Equatable {
    case available, notConnected, folderMissing, markerMissingOrWrong, link, network, readOnly, noPermission
}

/// Probes our folder `dir` whose marker should say `id`: connected (a mount holds it), local,
/// every component from the mount point down a real folder, the marker ours, writable. Uses
/// `mounts` (getmntinfo's list) and no other volume.
func probeLocation(dir: String, id: String, mounts: [Mount] = currentMounts()) -> LocationProbe {
    guard let m = mountHolding(dir, mounts) else { return .notConnected }
    if !m.local { return .network }
    guard let c = pathComponents(dir), let mc = pathComponents(m.path) else { return .folderMissing }
    var p = m.path
    for name in c.dropFirst(mc.count) {
        p += "/" + name
        var st = stat()
        if lstat(p, &st) != 0 {
            let e = errno
            return e == EACCES || e == EPERM ? .noPermission : e == ENOENT || e == ENOTDIR ? .folderMissing : .folderMissing
        }
        switch st.st_mode & S_IFMT {
        case S_IFDIR: continue
        case S_IFLNK: return .link
        default: return .folderMissing
        }
    }
    let marker = readMarker(dir)
    if marker.errno == EACCES || marker.errno == EPERM { return .noPermission }
    guard let found = marker.id, found.lowercased() == id.lowercased() else { return .markerMissingOrWrong }
    if m.readOnly { return .readOnly }
    if access(dir, W_OK) != 0 { let e = errno; return e == EROFS ? .readOnly : .noPermission }
    return .available
}

/// The location as the app shows it (3.3.1): this Mac, a chosen folder and what the app's probe,
/// the installed bridge and its state say, or config.ini's problem.
enum LocationStatus: Equatable {
    case thisMac
    case chosen(dir: String, id: String, probe: LocationProbe)
    /// chosen, but the installed bridge lacks location=1: it saves on this Mac for now
    case bridgeTooOld(dir: String, id: String)
    case configUnreadable
    case configMalformedLocation

    var dir: String? {
        switch self {
        case .chosen(let d, _, _), .bridgeTooOld(let d, _): return d
        default: return nil
        }
    }
    /// The state's name for the report's link (no path, no drive name).
    var code: String {
        switch self {
        case .thisMac: return "thisMac"
        case .chosen(_, _, let p): return p.rawValue
        case .bridgeTooOld: return "bridgeTooOld"
        case .configUnreadable: return "configUnreadable"
        case .configMalformedLocation: return "configMalformedLocation"
        }
    }
}

/// The status from config.ini (`cfg`), whether a bridge is installed and says location=1, and
/// the probe (only run for a chosen location).
func locationStatus(_ cfg: ConfigState, bridgeInstalled: Bool, bridgeLocation: Bool,
                    probe: (String, String) -> LocationProbe = { probeLocation(dir: $0, id: $1) }) -> LocationStatus {
    if cfg.unreadable != nil { return .configUnreadable }
    switch cfg.location {
    case .thisMac: return .thisMac
    case .malformed: return .configMalformedLocation
    case .chosen(let dir, let id):
        if bridgeInstalled && !bridgeLocation { return .bridgeTooOld(dir: dir, id: id) }
        return .chosen(dir: dir, id: id, probe: probe(dir, id))
    }
}

/// Where the stems are saved for real now: this Mac's folder, or the chosen one; nil when
/// nothing is saved (a chosen location the bridge can't use, config.ini unreadable or wrong).
func folderInUse(_ s: LocationStatus) -> String? {
    switch s {
    case .thisMac, .bridgeTooOld: return defaultCacheDir
    case .chosen(let d, _, .available): return d
    default: return nil
    }
}

// MARK: - the bridge's state while rekordbox runs

struct BridgeState: Equatable {
    var pid: pid_t
    var start: Int
    var location: String
    var state: String               // pending, available, unavailable
    var reason: String
}

func parseBridgeState(_ text: String) -> BridgeState? {
    var v: [String: String] = [:]
    for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
        guard let eq = line.firstIndex(of: "=") else { continue }
        v[String(line[..<eq])] = String(line[line.index(after: eq)...])
    }
    guard let pid = v["pid"].flatMap({ pid_t($0) }), pid > 0, let start = v["start"].flatMap({ Int($0) }),
          let state = v["state"], ["pending", "available", "unavailable"].contains(state) else { return nil }
    return BridgeState(pid: pid, start: start, location: v["location"] ?? "", state: state, reason: v["reason"] ?? "")
}

func readBridgeState(_ path: String = bridgeStateFile) -> BridgeState? {
    guard let d = FileManager.default.contents(atPath: path), d.count <= 8192 else { return nil }
    return parseBridgeState(String(decoding: d, as: UTF8.self))
}

/// The bridge's state if it speaks for the rekordbox running now (its pid and start time) and
/// for the location config.ini names now (`dir`; "" for this Mac); else nil (the app's probe).
func liveBridgeState(_ s: BridgeState?, dir: String, startOf: (pid_t) -> Int? = processStartTime) -> BridgeState? {
    guard let s = s, s.location == dir, startOf(s.pid) == s.start else { return nil }
    return s
}

// MARK: - the light's texts

/// The Stems Cache light for the location, or nil when the location is fine (3.3.6). `bridge`:
/// liveBridgeState; `rekordboxRunning`: says "restart" rather than "open". The bridge checks an
/// unavailable drive again every 10 seconds and uses it once it passes: no restart for that.
func locationLight(_ s: LocationStatus, bridge: BridgeState?, rekordboxRunning: Bool, mounts: [Mount] = currentMounts()) -> String? {
    let verb = rekordboxRunning ? "restart" : "open"
    // The bridge's state, except: "disabled" (Stems Cache off in config.ini, or a real library
    // it doesn't hook: it never touched the drive, nothing to restart for), and "config" while
    // config.ini is still wrong (only fixing it helps, as the app's own text says).
    let configWrong = s == .configUnreadable || s == .configMalformedLocation
    if let b = bridge, b.state != "available", b.reason != "disabled", !(b.reason == "config" && configWrong) {
        let drive = b.location.isEmpty ? "this Mac" : "\"\(driveName(b.location, mounts: mounts))\""
        if b.state == "pending" { return "Stems Cache is still checking \(drive)." }
        // connected again: the bridge's next check (within 10 seconds) uses it
        if b.reason == "not-connected", case .chosen(_, _, .available) = s { return "Stems Cache will use \(drive) again in a few seconds." }
        let start = "Stems Cache isn't using \(b.location.isEmpty ? "this Mac" : drive) right now: "
        switch b.reason {
        case "not-connected": return start + "the drive isn't connected. Connect it: Stems Cache uses it again by itself."
        case "folder", "marker": return start + "its folder on the drive is missing or was replaced. If this stays, choose the drive again in Settings."
        case "read-only": return start + "the drive is read-only; macOS may need to repair it in Disk Utility."
        case "no-permission": return start + "rekordbox isn't allowed to use the drive. Allow it in System Settings › Privacy & Security › Files and Folders › rekordbox, then restart rekordbox."
        case "not-local": return start + "the drive isn't connected to this Mac."
        case "config": return "Stems Cache isn't saving stems in this rekordbox session: its settings (config.ini) couldn't be read when rekordbox opened. Restart rekordbox."
        default: return start + "reading or writing the drive failed. If it happens again, check the drive in Disk Utility."
        }
    }
    switch s {
    case .thisMac: return nil
    case .configUnreadable: return "Stems Cache can't read its settings (config.ini). See Help › RB Stems Plus Troubleshooting."
    case .configMalformedLocation: return "Where Stems Cache saves stems is set wrongly in config.ini. Choose it again in Settings."
    case .bridgeTooOld(let dir, _):
        return "Stems Cache needs an update to save stems on \"\(driveName(dir, mounts: mounts))\": it's saving them on this Mac for now. Click Reinstall Stems Cache."
    case .chosen(let dir, _, let p):
        let d = "\"\(driveName(dir, mounts: mounts))\""
        switch p {
        case .available: return nil
        case .notConnected:
            // the running bridge hasn't noticed yet (it looks at the drive when it needs it): no restart either
            let connect = bridge?.state == "available" ? "Connect it: Stems Cache uses it again by itself." : "Connect it, then \(verb) rekordbox."
            return "Stems Cache's drive \(d) isn't connected: rekordbox separates tracks as usual. \(connect) Renamed the drive, or does it show as \"\(driveName(dir, mounts: mounts)) 1\"? Choose it again in Settings: the stems on it are kept."
        case .folderMissing, .markerMissingOrWrong: return "Stems Cache's folder on \(d) is missing or was replaced."
        case .link: return "Stems Cache isn't saving: a folder on the way to Stems Cache's folder on \(d) is a link."
        case .network: return "Stems Cache isn't saving: \(d) is a network drive. Choose a drive connected to this Mac in Settings."
        case .readOnly: return "\(d) is read-only; macOS may need to repair it in Disk Utility."
        case .noPermission: return "RB Stems Plus isn't allowed to use \(d). Allow it in System Settings › Privacy & Security › Files and Folders › RB Stems Plus."
        }
    }
}

// MARK: - how much the saved stems take

/// The size of the stems in `folder` as the bridge counts them for its limit: per entry the space
/// its files take on the disk (st_blocks), `<key>.flac` and `.desc` and their "._" companions,
/// under `<64 hex>/<2 hex>/`. Folders, the marker, memos and temporary files aren't counted
/// ("about"). nil if the folder can't be read; 0 if it isn't there.
func entriesSize(_ folder: String) -> Int64? {
    guard fileType(folder) != nil else { return 0 }
    guard fileType(folder) == S_IFDIR, let models = dirNames(folder) else { return nil }
    var total: Int64 = 0
    for m in models where m.count == 64 && isLowerHex(m) && fileType(folder + "/" + m) == S_IFDIR {
        for x in dirNames(folder + "/" + m) ?? [] where x.count == 2 && isLowerHex(x) {
            let dir = folder + "/" + m + "/" + x
            guard fileType(dir) == S_IFDIR else { continue }
            for name in dirNames(dir) ?? [] where countsInEntrySize(name, prefix: x) {
                var st = stat()
                if lstat(dir + "/" + name, &st) == 0 && st.st_mode & S_IFMT == S_IFREG { total += Int64(st.st_blocks) * 512 }
            }
        }
    }
    return total
}

/// The names in a folder, "._" files included (FileManager's contentsOfDirectory leaves those
/// out); nil if it can't be read.
func dirNames(_ path: String) -> [String]? {
    let fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
    guard fd >= 0 else { return nil }
    defer { close(fd) }
    return listing(fd)
}

/// Whether a file in prefix folder `prefix` counts in an entry's size: <key>.flac, <key>.desc, or
/// either's "._" companion, the key 64 lowercase hex starting with the prefix.
func countsInEntrySize(_ name: String, prefix: String) -> Bool {
    let n = name.hasPrefix("._") ? String(name.dropFirst(2)) : name
    guard n.count == 69, n.hasSuffix(".flac") || n.hasSuffix(".desc") else { return false }
    let key = String(n.prefix(64))
    return isLowerHex(key) && key.hasPrefix(prefix)
}

func isLowerHex(_ s: String) -> Bool { s.utf8.allSatisfy { ($0 >= 0x30 && $0 <= 0x39) || ($0 >= 0x61 && $0 <= 0x66) } }

/// A size for the texts: " (about 12.4 GB)", " (about 35 MB)", "" for none.
func aboutSize(_ bytes: Int64) -> String {
    if bytes <= 0 { return "" }
    let gb = Double(bytes) / 1_073_741_824
    return gb >= 0.1 ? String(format: " (about %.1f GB)", gb) : " (about \(max(1, bytes / 1_048_576)) MB)"
}

/// The saved stems' size as Settings and the report show it.
enum StemsSize: Equatable {
    case measuring, measured(Int64), failed, notConnected
    var text: String {
        switch self {
        case .measuring: return " (measuring…)"
        case .measured(let b): return aboutSize(b)
        case .failed: return " (couldn't measure)"
        case .notConnected: return " (not connected)"
        }
    }
}

// MARK: - the discovery hint

struct LocationHint: Equatable {
    var mount: String = ""          // the drive's mount point when the location was chosen
    var relative: String = ""       // our folder's path below it
    var ownIDs: [String] = []       // the marker ids this account created
}

func readLocationHint(_ path: String = locationHintFile) -> LocationHint {
    var h = LocationHint()
    for line in ((try? String(contentsOfFile: path, encoding: .utf8)) ?? "").split(separator: "\n") {
        guard let eq = line.firstIndex(of: "=") else { continue }
        let k = line[..<eq], v = String(line[line.index(after: eq)...])
        if k == "mount" { h.mount = v } else if k == "relative" { h.relative = v } else if k == "own", isUUID(v) { h.ownIDs.append(v) }
    }
    return h
}

@discardableResult
func writeLocationHint(_ h: LocationHint, _ path: String = locationHintFile) -> Bool {
    let text = "mount=\(h.mount)\nrelative=\(h.relative)\n" + Set(h.ownIDs.map { $0.uppercased() }).sorted().map { "own=\($0)\n" }.joined()
    try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
    return writeAtomically(Data(text.utf8), to: path)
}

/// The old location's path below its drive, for finding it on a renamed drive: from the hint
/// recorded when it was chosen, else by taking "/Volumes/<name>" off config.ini's `dir` (nil).
func relativeOnDrive(_ dir: String?, hint: LocationHint) -> String? {
    if !hint.relative.isEmpty { return hint.relative }
    guard let d = dir, let c = pathComponents(d), c.count >= 3, c[0] == "Volumes" else { return nil }
    return c.dropFirst(2).joined(separator: "/")
}

/// The hint for our folder `dir` on the drive mounted at `mount`, keeping the ids of `old`.
func hintFor(dir: String, mount: String, old: LocationHint, newID: String?) -> LocationHint {
    var h = old
    h.mount = mount
    h.relative = pathComponents(dir).map { c in c.dropFirst(pathComponents(mount)?.count ?? 0).joined(separator: "/") } ?? ""
    if let id = newID { h.ownIDs.append(id) }
    return h
}

/// What choosing `folder` (on the drive mounted at `mount`) finds, in order: the folder itself
/// when it is our folder, our folder inside it, our folder at the old location's path on this
/// drive (`relative`). Only that drive is looked at.
enum Discovery: Equatable {
    case none                                   // nothing there: a new, empty folder
    case found(dir: String, id: String)         // our folder with a valid marker
    case notOurs(dir: String)                   // a folder of our name without a valid marker
}

func discover(folder: String, mount: String, relative: String?) -> Discovery {
    var candidates = [folder.hasSuffix("/" + locationFolderName) ? folder : folder + "/" + locationFolderName]
    if let r = relative, pathComponents("/" + r) != nil { candidates.append(mount + "/" + r) }
    for (i, c) in candidates.enumerated() {
        guard fileType(c) != nil else { continue }
        // a candidate from the old path is validated like any choice: a real folder below the mount
        if i > 0, pathComponents(c).map({ realpath(c) == c && $0.contains(locationFolderName) }) != true { continue }
        if fileType(c) == S_IFDIR, let id = readMarker(c).id { return .found(dir: c, id: id) }
        if i == 0 { return .notOurs(dir: c) }
    }
    return .none
}

private func realpath(_ p: String) -> String? {
    guard let r = Darwin.realpath(p, nil) else { return nil }
    defer { free(r) }
    return String(cString: r)
}

func realPath(_ p: String) -> String? { realpath(p) }

// MARK: - creating our folder

/// Creates our folder in `parent` and its marker, in that order (3.3.3), nothing else; on any
/// failure removes what it created. The new id, or why not.
func createLocationFolder(in parent: String) -> (dir: String?, id: String?, error: String?) {
    let dir = parent.hasSuffix("/" + locationFolderName) ? parent : parent + "/" + locationFolderName
    if mkdir(dir, 0o755) != 0 { return (nil, nil, errno == EEXIST ? "exists" : String(cString: strerror(errno))) }
    let id = UUID().uuidString
    guard writeAtomically(Data(markerText(id).utf8), to: dir + "/" + markerName) else {
        let why = String(cString: strerror(errno))
        rmdir(dir)                                                     // only an empty folder goes
        return (nil, nil, "the marker couldn't be written (\(why))")
    }
    return (dir, id, nil)
}

/// Removes our folder this run created (before config.ini named it): its marker and the folder,
/// nothing else (a folder with anything else in it stays).
func removeNewLocationFolder(_ dir: String) {
    let gate = Deletable(inside: [], roots: [], files: [dir + "/" + markerName], apps: [])
    safeRemove(dir + "/" + markerName, whole: false, gate)
    rmdir(dir)
}

/// Time Machine skips our folder on APFS and Mac OS Extended (on exFAT and FAT32 the flag would
/// write a "._" file into the chosen folder).
func excludeFromBackup(_ dir: String, fsType: String) {
    guard fsType == "apfs" || fsType == "hfs" else { return }
    var u = URL(fileURLWithPath: dir)
    var v = URLResourceValues(); v.isExcludedFromBackup = true
    try? u.setResourceValues(v)
}

/// Whether this account can write our folder and its model folders (an owners-on drive of
/// another account says no): access, then a test file created and deleted.
func locationWritable(_ dir: String) -> Bool {
    guard access(dir, W_OK) == 0 else { return false }
    for m in (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? [] where m.count == 64 && isLowerHex(m) {
        if access(dir + "/" + m, W_OK) != 0 { return false }
    }
    let test = dir + "/.rbstemsplus-write-test-\(getpid())"
    let fd = open(test, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o644)
    guard fd >= 0 else { return false }
    close(fd)
    unlink(test)
    return true
}

// MARK: - deleting in a chosen location, through verified descriptors

/// The gate of one action in a chosen location (3.3.7): our folder as the action found it.
struct LocationGate: Equatable { let dir: String; let id: String }

/// Deletes in our folder `gate.dir`: opens every folder from its drive's mount point down with
/// O_NOFOLLOW (each on the mount's device), checks the marker's id through that descriptor, then
/// deletes relative to those descriptors, never by path. `wholeFolder` false (Clear Saved
/// Stems): only the model folders and the "._" files in it; the folder and its marker stay. true
/// (the complete uninstall): everything in it, the folder, and its "._" companion in the parent.
/// `progress` gets the number of files deleted so far. Returns whether all went, and why not.
@discardableResult
func removeInLocation(_ gate: LocationGate, wholeFolder: Bool, mounts: [Mount] = currentMounts(),
                      progress: (Int) -> Void = { _ in }) -> (ok: Bool, why: String?) {
    guard cacheDirProblem(gate.dir) == nil, let m = mountHolding(gate.dir, mounts),
          let c = pathComponents(gate.dir), let mc = pathComponents(m.path) else { return (false, "not connected") }
    var fds: [Int32] = []
    defer { fds.forEach { close($0) } }
    let top = open(m.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
    guard top >= 0 else { return (false, "the drive can't be opened") }
    fds.append(top)
    var st = stat()
    guard fstat(top, &st) == 0 else { return (false, "the drive can't be read") }
    let dev = st.st_dev
    for name in c.dropFirst(mc.count) {
        let fd = openat(fds.last!, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { return (false, "\(name): \(String(cString: strerror(errno)))") }
        fds.append(fd)
        guard fstat(fd, &st) == 0, st.st_dev == dev else { return (false, "\(name) is on another drive") }
    }
    let ours = fds.last!, parent = fds[fds.count - 2]
    guard readMarker(at: ours).id?.lowercased() == gate.id.lowercased() else { return (false, "the folder's marker isn't the one expected") }
    var count = 0, ok = true
    for name in listing(ours) {
        // the marker (and its companion) go last, and only once everything else went: a folder
        // left half-deleted keeps its identity, so Clear, the uninstall and other Macs still find it
        if name == markerName || name == "._" + markerName { continue }
        let keep = !wholeFolder && !(name.count == 64 && isLowerHex(name)) && !name.hasPrefix("._")
        if keep { continue }
        ok = removeEntry(ours, name, dev: dev, count: &count, progress: progress) && ok
    }
    guard wholeFolder else { return (ok, ok ? nil : "some files couldn't be deleted") }
    guard ok else { return (false, "some files couldn't be deleted") }
    if unlinkat(ours, "._" + markerName, 0) != 0 && errno != ENOENT { return (false, "some files couldn't be deleted") }
    guard unlinkat(ours, markerName, 0) == 0, unlinkat(parent, locationFolderName, AT_REMOVEDIR) == 0 else { return (false, "some files couldn't be deleted") }
    // the "._" companion exFAT and FAT32 wrote next to our folder, if the drive didn't remove it
    let companion = "._" + locationFolderName
    if fstatat(parent, companion, &st, AT_SYMLINK_NOFOLLOW) == 0 && st.st_mode & S_IFMT == S_IFREG { unlinkat(parent, companion, 0) }
    return (true, nil)
}

/// The names in an open folder (not "." or "..").
func listing(_ fd: Int32) -> [String] {
    let d = dup(fd)
    guard d >= 0, let dir = fdopendir(d) else { if d >= 0 { close(d) }; return [] }
    defer { closedir(dir) }
    var names: [String] = []
    while let e = readdir(dir) {
        let name = withUnsafePointer(to: &e.pointee.d_name) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        if name != "." && name != ".." { names.append(name) }
    }
    return names
}

/// Deletes `name` in the open folder `at`: a folder (on device `dev`, opened without following a
/// link) with everything in it, anything else (a link included) itself.
private func removeEntry(_ at: Int32, _ name: String, dev: dev_t, count: inout Int, progress: (Int) -> Void) -> Bool {
    var st = stat()
    guard fstatat(at, name, &st, AT_SYMLINK_NOFOLLOW) == 0 else { return errno == ENOENT }
    if st.st_mode & S_IFMT == S_IFDIR {
        let fd = openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { return false }
        defer { close(fd) }
        var inner = stat()
        guard fstat(fd, &inner) == 0, inner.st_dev == dev, inner.st_ino == st.st_ino else { return false }
        var ok = true
        for n in listing(fd) { ok = removeEntry(fd, n, dev: dev, count: &count, progress: progress) && ok }
        return ok && unlinkat(at, name, AT_REMOVEDIR) == 0
    }
    guard unlinkat(at, name, 0) == 0 || errno == ENOENT else { return false }
    count += 1
    if count % 200 == 0 { progress(count) }
    return true
}

/// This Mac's stems (the model folders in ~/Library/Caches/rbstemsplus), deleted through the
/// static gate after a switch to a drive; the model-hash memos stay. Whether all went.
@discardableResult
func removeThisMacStems(_ folder: String = defaultCacheDir, _ gate: Deletable = deletable) -> Bool {
    var ok = true
    for name in (try? FileManager.default.contentsOfDirectory(atPath: folder)) ?? [] where name.count == 64 && isLowerHex(name) {
        ok = safeRemove(folder + "/" + name, whole: false, gate) && ok
    }
    return ok
}

/// Whether the retry of This Mac's deletion may run at launch (3.3.3): the flag is there, the
/// chosen location is available, the installed bridge says location=1 and rekordbox is closed.
/// Never with an older bridge or a config.ini it can't use: then a 1.1 bridge may be saving on
/// this Mac by design.
func mayRetryThisMacDeletion(flag: Bool, status: LocationStatus, bridgeLocation: Bool, rekordboxRunning: Bool) -> Bool {
    guard flag, bridgeLocation, !rekordboxRunning, case .chosen(_, _, .available) = status else { return false }
    return true
}

/// Whether This Mac's model folders (`modelFolders`: there are some in ~/Library/Caches/rbstemsplus)
/// are leftovers to delete at launch, though no switch left the flag: the location is a drive in
/// use and the installed bridge says location=1, so nothing saves on this Mac any more. They come
/// from a time an older bridge saved there (another account's 1.1 app reinstalled it: bridgeTooOld);
/// the owner decided they go automatically, as at a switch, under the retry's conditions.
func thisMacNeedsCleanup(status: LocationStatus, bridgeLocation: Bool, modelFolders: Bool) -> Bool {
    guard bridgeLocation, modelFolders, case .chosen(_, _, .available) = status else { return false }
    return true
}

/// Whether ~/Library/Caches/rbstemsplus holds model folders (64 lowercase hex digits).
func thisMacHasModelFolders(_ folder: String = defaultCacheDir) -> Bool {
    ((try? FileManager.default.contentsOfDirectory(atPath: folder)) ?? []).contains { $0.count == 64 && isLowerHex($0) }
}

/// Whether a switch to a drive may delete This Mac's stems at once: no Stems Cache is installed,
/// or the installed one says location=1. An older one (reinstalled meanwhile from another
/// account) still saves there: then the pending flag leaves it to the guarded launch retry.
func thisMacDeletionNow(cachePresent: Bool, bridgeLocation: Bool) -> Bool { !cachePresent || bridgeLocation }

/// Whether the complete uninstall (or Remove anyway) may delete the chosen folder: only the one
/// the user was told about in the confirmation (`confirmed`, nil when it said nothing would be
/// deleted on a drive), and only if the probe just before deleting finds that same folder and
/// marker id in use (`fresh`). A drive connected after the confirmation, or another folder now,
/// is left, and the final alert says where.
func driveDeletionAllowed(confirmed: LocationGate?, fresh: LocationGate?) -> Bool {
    guard let c = confirmed, let f = fresh else { return false }
    return c.dir == f.dir && c.id.lowercased() == f.id.lowercased()
}

/// What Clear Saved Stems clears: this Mac's folder, or the chosen folder (its gate).
enum ClearTarget: Equatable { case thisMac, drive(LocationGate) }

/// Settings' size of the stems in use and what Clear would clear, from config.ini as it is now
/// (`cfg`) and the background measurement (`snap`). A snapshot of another folder than the one
/// config.ini names now (just after Change…, Use This Mac or Clear) says "measuring…" and clears
/// nothing, never an old size or "not connected". While an older bridge saves on this Mac
/// (bridgeTooOld), the size and Clear are this Mac's.
func settingsStems(_ cfg: ConfigState, _ snap: LocationSnapshot) -> (size: String, clear: ClearTarget?) {
    if cfg.unreadable != nil { return ("", nil) }
    switch cfg.location {
    case .thisMac:
        return snap.status == .thisMac ? (snap.size.text, .thisMac) : (StemsSize.measuring.text, .thisMac)
    case .malformed:
        return ("", .thisMac)                       // nothing is saved; this Mac's old stems may be cleared
    case .chosen(let dir, let id):
        switch snap.status {
        case .bridgeTooOld(let d, _)? where d == dir: return (snap.size.text, .thisMac)
        case .chosen(let d, let i, .available)? where d == dir && i.lowercased() == id.lowercased():
            return (snap.size.text, .drive(LocationGate(dir: d, id: i)))
        case .chosen(let d, _, _)? where d == dir: return (StemsSize.notConnected.text, nil)
        default: return (StemsSize.measuring.text, nil)
        }
    }
}
