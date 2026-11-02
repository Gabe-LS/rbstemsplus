// Where Stems Cache saves stems (Config.swift, Location.swift, LocationActions.swift), tried in
// the temporary folder: config.ini's grammar (the bridge's fixtures too), the path rules, the
// marker, the writer, the probe on a fake mount, the light's texts and the bridge's state,
// the size rule, deletion through verified descriptors (a link swapped above our folder), the
// order of a switch, the retry's conditions, finding stems already on a drive, and the report's
// link without paths. No real drive is needed: the mounts are given to the functions.
import Foundation

func locationTests() {
    let fm = FileManager.default
    let base = (realPath(tmp) ?? tmp) + "/location"         // /var is a link to /private/var
    func mk(_ p: String) { try! fm.createDirectory(atPath: p, withIntermediateDirectories: true) }
    func put(_ p: String, _ s: String) { mk((p as NSString).deletingLastPathComponent); fm.createFile(atPath: p, contents: Data(s.utf8)) }
    func putBytes(_ p: String, _ b: [UInt8]) { mk((p as NSString).deletingLastPathComponent); fm.createFile(atPath: p, contents: Data(b)) }
    func text(_ p: String) -> String? { try? String(contentsOfFile: p, encoding: .utf8) }
    mk(base)
    let id = "0F8FAD5B-D9CB-469F-A165-70867728950E", id2 = "7C9E6679-7425-40DE-944B-E07FC1F90AE7"
    let gls = "/Volumes/GLS/" + locationFolderName

    // MARK: config.ini's grammar (the same fixtures as the bridge's, bridge/tests)

    let cfg = base + "/config.ini"
    func state(_ s: String) -> ConfigState { put(cfg, s); return readConfigState(file: cfg) }
    func stateBytes(_ b: [UInt8]) -> ConfigState { putBytes(cfg, b); return readConfigState(file: cfg) }
    let loc = "cache_dir=\"\(gls)\"\ncache_id=\(id)\n"
    check(state(loc).location == .chosen(dir: gls, id: id), "config: cache_dir and cache_id name a location")
    check(state("max_gb=3\n").location == .thisMac && !fm.fileExists(atPath: base + "/none.ini")
          && readConfigState(file: base + "/none.ini").location == .thisMac && readConfigState(file: base + "/none.ini").unreadable == nil,
          "config: neither key, or no file: this Mac")
    // lines of 1023, 1024 and 1025 bytes (without the newline)
    func padded(_ s: String, _ n: Int) -> String { s + String(repeating: " ", count: n - s.utf8.count) }
    for (n, ok) in [(1023, true), (1024, true), (1025, false)] {
        var ignored: [String] = []
        put(cfg, padded("max_days=5", n) + "\n")
        let c = readConfigState({ ignored.append($0) }, file: cfg).config
        check((c.maxDays == 5) == ok && ignored.isEmpty == ok, "config: a \(n)-byte line is \(ok ? "read" : "ignored, with a log line")")
        let l = state(padded("cache_dir=\"\(gls)\"", n) + "\ncache_id=\(id)\n").location
        check((l == .chosen(dir: gls, id: id)) == ok && (ok || { if case .malformed = l { return true }; return false }()),
              "config: a \(n)-byte cache_dir line is \(ok ? "read" : "a wrong location (no cache)")")
    }
    // a key beyond the first 1024 bytes of a line is still a key: the location is wrong, never this Mac
    if case .malformed = state(String(repeating: " ", count: 1100) + "cache_dir=\"\(gls)\"\ncache_id=\(id)\n").location { check(true, "config: a long run of spaces before cache_dir: still a location key (wrong)") }
    else { check(false, "config: a long run of spaces before cache_dir: still a location key (wrong)") }
    check(state("[cache.old]\nmax_gb=3\n" + loc).config.maxGB == 20 && state("[cache.old]\n" + loc).location == .thisMac,
          "config: [cache.old] isn't [cache]")
    check(state("[cache x]\nmax_gb=3\n").config.maxGB == 20 && state("[cache\nmax_gb=3\n").config.maxGB == 20, "config: [cache x] and [cache (no ']') aren't [cache]")
    check(state("[watcher]\nenabled=0\n[ Cache ]\nmax_gb=3\n").config.maxGB == 3 && state("[ Cache ]  # x\nmax_gb=3\n").config.maxGB == 3,
          "config: [ Cache ] is [cache], text after ']' ignored")
    check(state("[watcher]\n" + loc).location == .thisMac, "config: location keys in another section are someone else's")
    check(stateBytes([0xEF, 0xBB, 0xBF] + Array("max_gb=3\n".utf8)).config.maxGB == 3, "config: a BOM at offset 0 is skipped")
    check(stateBytes(Array("max_days=5\n".utf8) + [0xEF, 0xBB, 0xBF] + Array("max_gb=3\n".utf8)).config.maxGB == 20,
          "config: a BOM on a later line is part of the line (an unknown key)")
    check(stateBytes(Array("max_gb=3\n".utf8) + [0] + Array("\n".utf8)).unreadable == "a NUL byte", "config: a NUL byte makes the file unreadable")
    check(stateBytes(Array("max_gb=3 # caf".utf8) + [0xC3, 0x28] + [0x0A]).unreadable == "not UTF-8", "config: invalid UTF-8 makes the file unreadable")
    check(stateBytes([0xED, 0xA0, 0x80, 0x0A]).unreadable == "not UTF-8" && stateBytes([0xC0, 0xAF, 0x0A]).unreadable == "not UTF-8",
          "config: a surrogate or an overlong form is invalid UTF-8")
    check(state("# café ü\nmax_gb=3\n").config.maxGB == 3, "config: valid UTF-8 is fine")
    check(state(String(repeating: "#\n", count: 33_000)).unreadable == "over 64 KB", "config: a file over 64 KB is unreadable")
    if case .malformed = stateBytes([0]).location { check(true, "config: unreadable: no cache (never this Mac)") } else { check(false, "config: unreadable: no cache (never this Mac)") }
    let djs = "/Volumes/DJ's SSD/" + locationFolderName
    check(state("cache_dir=\"\(djs)\"\ncache_id=\(id)\n").location == .chosen(dir: djs, id: id), "config: DJ's SSD (an apostrophe inside double quotes)")
    let hash = "/Volumes/GLS/My #1  Sets/" + locationFolderName
    check(state("cache_dir = \"\(hash)\"   # where\ncache_id = \(id)\n").location == .chosen(dir: hash, id: id), "config: '#' and spaces inside quotes")
    check(state("cache_dir=\(hash)\ncache_id=\(id)\n").location != .chosen(dir: hash, id: id), "config: an unquoted path with spaces is cut and refused")
    check(state(loc + loc).location == .chosen(dir: gls, id: id) && state(loc + "cache_id=\(id.lowercased())\n").location == .chosen(dir: gls, id: id),
          "config: identical duplicates (an id in either case) are fine")
    for (s, what) in [(loc + "cache_dir=\"/Volumes/X/\(locationFolderName)\"\n", "two cache_dirs"), (loc + "cache_id=\(id2)\n", "two cache_ids"),
                      ("cache_dir=\"\(gls)\"\n", "a missing cache_id"), ("cache_id=\(id)\n", "a missing cache_dir"),
                      ("cache_dir=\"\(gls)\"\ncache_id=nope\n", "a bad id"), ("cache_dir=\"/Volumes/GLS\"\ncache_id=\(id)\n", "a folder of another name"),
                      ("cache_dir=\"\"\ncache_id=\(id)\n", "an empty path")] {
        if case .malformed = state(s).location { check(true, "config: \(what): no cache") } else { check(false, "config: \(what): no cache") }
    }
    check(state("CACHE_DIR = \"\(gls)\"\r\n  Cache_ID=\(id)\r\n").location == .chosen(dir: gls, id: id), "config: keys ignore case, CRLF lines")
    let longSection = "[" + String(repeating: "x", count: 1100) + "]\n"
    if case .malformed = state(longSection + loc).location { check(true, "config: a section line over the limit with a location key: no cache") }
    else { check(false, "config: a section line over the limit with a location key: no cache") }
    check(state(longSection + "max_gb=3\n").location == .thisMac && state(longSection + "max_gb=3\n").config.maxGB == 20,
          "config: a section line over the limit leaves an unknown section")
    check(state("enabled=0\nmax_gb=5\nmax_days=7\n").config.enabled == false && state("max_gb=\" 7 \"\n").config.maxGB == 20,
          "config: the existing keys read as before")

    // MARK: the fixtures shared with the bridge (tests/config-fixtures.txt, bridge/tests/run.sh)

    configFixtureTests(cfg)

    // MARK: the writer

    put(cfg, "# mine\nmax_gb=7\n[watcher]\nenabled=no\n")
    check(writeCacheLocation(dir: gls, id: id, file: cfg) && text(cfg) == "# mine\nmax_gb=7\ncache_dir=\"\(gls)\"\ncache_id=\(id)\n[watcher]\nenabled=no\n"
          && readConfigState(file: cfg).location == .chosen(dir: gls, id: id), "writer: the location goes at the top, quoted, other lines kept")
    put(cfg, "cache_dir=\"/x\"\nmax_gb=7\n[cache]\ncache_dir=\"/y\"\ncache_id=\(id2)\n[watcher]\ncache_dir=theirs\n")
    check(writeCacheLocation(dir: nil, id: nil, file: cfg) && text(cfg) == "max_gb=7\n[cache]\n[watcher]\ncache_dir=theirs\n",
          "writer: Use This Mac removes every line of both keys, only ours")
    put(cfg, padded("cache_dir=\"/x\"", 1100) + "\ncache_id=\(id2)\n")
    check(writeCacheLocation(dir: gls, id: id, file: cfg) && readConfigState(file: cfg).location == .chosen(dir: gls, id: id),
          "writer: an over-long location line is replaced")
    check(!writeCacheLocation(dir: "/Volumes/GLS", id: id, file: cfg) && !writeCacheLocation(dir: gls, id: "x", file: cfg), "writer: refuses a bad path or id")
    putBytes(cfg, Array("max_gb=3\n".utf8) + [0, 0x0A])
    let before = fm.contents(atPath: cfg)
    check(!writeConfig(maxGB: 5, maxDays: 9, file: cfg) && !writeCacheLocation(dir: gls, id: id, file: cfg) && fm.contents(atPath: cfg) == before,
          "writer: never over a file that can't be read")
    try? fm.removeItem(atPath: cfg)
    check(writeConfig(maxGB: 5, maxDays: 9, file: cfg) && readConfig(file: cfg).maxGB == 5, "writer: a missing file is started")

    // MARK: the path rules, ids and the marker

    check(cacheDirProblem(gls) == nil && cacheDirProblem("/Volumes/DJ's SSD/a b/#1/" + locationFolderName) == nil, "path rules: plain paths pass")
    for (p, what) in [("Volumes/GLS/" + locationFolderName, "relative"), ("/Volumes/../GLS/" + locationFolderName, "a .."),
                      ("/Volumes/./GLS/" + locationFolderName, "a ."), ("/Volumes//GLS/" + locationFolderName, "an empty component"),
                      (gls + "/", "a trailing /"), ("/Volumes/G\"LS/" + locationFolderName, "a quote"), ("/Volumes/G\nLS/" + locationFolderName, "a newline"),
                      ("/Volumes/G\u{7F}/" + locationFolderName, "DEL"), ("/Volumes/GLS/Other", "another name"),
                      ("/Volumes/" + String(repeating: "a", count: 701 - 9 - locationFolderName.utf8.count - 1) + "/" + locationFolderName, "701 bytes")] {
        check(cacheDirProblem(p) != nil, "path rules: refuses \(what)")
    }
    let p700 = "/Volumes/" + String(repeating: "a", count: 700 - 9 - locationFolderName.utf8.count - 1) + "/" + locationFolderName
    check(p700.utf8.count == 700 && cacheDirProblem(p700) == nil, "path rules: 700 bytes pass")
    check(isUUID(id) && isUUID(id.lowercased()) && !isUUID("0F8FAD5B-D9CB-469F-A165-70867728950") && !isUUID("0F8FAD5BD9CB469FA16570867728950E")
          && !isUUID("0F8FAD5B-D9CB-469F-A165-70867728950G"), "ids: 8-4-4-4-12 hex digits")
    check(markerID(markerText(id)) == id && markerID("format=rbstems-v1\nid=\(id)\n") == id && markerID("id=\(id)\r\nformat=rbstems-v1\r\n") == nil && markerID("id=\(id)\n") == nil
          && markerID("id=\(id)\nformat=rbstems-v2\n") == nil && markerID("id=nope\nformat=rbstems-v1\n") == nil, "marker: id and format")
    let mdir = base + "/markers"
    put(mdir + "/ok/" + markerName, markerText(id))
    put(mdir + "/big/" + markerName, markerText(id) + "#" + String(repeating: "x", count: 4100) + "\n")
    mk(mdir + "/link"); symlink(mdir + "/ok/" + markerName, mdir + "/link/" + markerName)
    mk(mdir + "/fifo"); mkfifo(mdir + "/fifo/" + markerName, 0o644)
    check(readMarker(mdir + "/ok").id == id && readMarker(mdir + "/none").errno == ENOENT, "marker: read from a real folder")
    check(readMarker(mdir + "/big").id == nil && readMarker(mdir + "/link").id == nil && readMarker(mdir + "/fifo").id == nil,
          "marker: over 4 KB, a link or a FIFO (opened without blocking) isn't a marker")

    // MARK: the capability line

    let sha = String(repeating: "c", count: 64)
    check(bridgeSaysLocation(data: Data("rbstems bridge: rbstems-cache: rebuild=1 location=1 pioneer=\(sha);".utf8))
          && bridgeCacheModels(data: Data("rbstems bridge: rbstems-cache: rebuild=1 location=1 pioneer=\(sha);".utf8)) == [sha],
          "capability: location=1 before pioneer=, the model list still read")
    check(!bridgeSaysLocation(data: Data("rbstems bridge: rbstems-cache: rebuild=1 pioneer=\(sha);".utf8))
          && !bridgeSaysLocation(data: Data("rbstems bridge: rbstems-cache: rebuild=1 pioneer=\(sha),location=1;".utf8))
          && !bridgeSaysLocation(data: Data("rbstems-cache: rebuild=1 location=1 pioneer=\(sha);".utf8)),
          "capability: 1.1's line, a token after pioneer= or no bridge: no location")

    // MARK: the probe, on a fake mount in the temporary folder

    let vol = base + "/GLS", ours = vol + "/DJ/" + locationFolderName
    let mount = Mount(path: vol, local: true, readOnly: false, fsType: "exfat")
    mk(ours); put(ours + "/" + markerName, markerText(id))
    check(probeLocation(dir: ours, id: id, mounts: [mount]) == .available && probeLocation(dir: ours, id: id.lowercased(), mounts: [mount]) == .available,
          "probe: our folder with its marker is available")
    check(probeLocation(dir: ours, id: id2, mounts: [mount]) == .markerMissingOrWrong, "probe: another marker id")
    check(probeLocation(dir: ours, id: id, mounts: []) == .notConnected && probeLocation(dir: ours, id: id, mounts: [Mount(path: "/", local: true, readOnly: false)]) == .notConnected,
          "probe: no mount holding it (a leftover folder on the boot disk) is not connected")
    check(probeLocation(dir: ours, id: id, mounts: [Mount(path: vol, local: false, readOnly: false)]) == .network, "probe: a network volume")
    check(probeLocation(dir: ours, id: id, mounts: [Mount(path: vol, local: true, readOnly: true)]) == .readOnly, "probe: a read-only volume")
    check(probeLocation(dir: vol + "/Gone/" + locationFolderName, id: id, mounts: [mount]) == .folderMissing, "probe: the folder missing")
    try! fm.moveItem(atPath: vol + "/DJ", toPath: vol + "/DJ-real"); symlink(vol + "/DJ-real", vol + "/DJ")
    check(probeLocation(dir: ours, id: id, mounts: [mount]) == .link, "probe: a link on the way")
    try! fm.removeItem(atPath: vol + "/DJ"); try! fm.moveItem(atPath: vol + "/DJ-real", toPath: vol + "/DJ")
    check(mountHolding(ours, [mount, Mount(path: base, local: true, readOnly: false)])?.path == vol, "probe: the deepest mount holds it")

    // MARK: the status and the light

    func cs(_ l: ConfigLocation) -> ConfigState { var c = ConfigState(); c.location = l; return c }
    check(locationStatus(cs(.chosen(dir: ours, id: id)), bridgeInstalled: true, bridgeLocation: false, probe: { _, _ in .available }) == .bridgeTooOld(dir: ours, id: id)
          && locationStatus(cs(.chosen(dir: ours, id: id)), bridgeInstalled: false, bridgeLocation: false, probe: { _, _ in .available }) == .chosen(dir: ours, id: id, probe: .available),
          "status: an older bridge saves on this Mac; no bridge yet is fine")
    var unreadable = ConfigState(); unreadable.unreadable = "x"
    check(locationStatus(unreadable, bridgeInstalled: true, bridgeLocation: true) == .configUnreadable
          && locationStatus(cs(.malformed("x")), bridgeInstalled: true, bridgeLocation: true) == .configMalformedLocation
          && locationStatus(cs(.thisMac), bridgeInstalled: true, bridgeLocation: true) == .thisMac, "status: config.ini's problems")
    check(folderInUse(.thisMac) == defaultCacheDir && folderInUse(.bridgeTooOld(dir: ours, id: id)) == defaultCacheDir
          && folderInUse(.chosen(dir: ours, id: id, probe: .available)) == ours && folderInUse(.chosen(dir: ours, id: id, probe: .notConnected)) == nil
          && folderInUse(.configMalformedLocation) == nil, "status: the folder in use")
    let m = [Mount(path: "/Volumes/GLS", local: true, readOnly: false)]
    check(locationLight(.chosen(dir: gls, id: id, probe: .notConnected), bridge: nil, rekordboxRunning: false, mounts: [])
          == "Stems Cache's drive \"GLS\" isn't connected: rekordbox separates tracks as usual. Connect it, then open rekordbox. Renamed the drive, or does it show as \"GLS 1\"? Choose it again in Settings: the stems on it are kept."
          && locationLight(.chosen(dir: gls, id: id, probe: .notConnected), bridge: nil, rekordboxRunning: true, mounts: [])?.contains("Connect it, then restart rekordbox.") == true,
          "light: not connected (open, or restart while rekordbox runs)")
    check(locationLight(.chosen(dir: gls, id: id, probe: .folderMissing), bridge: nil, rekordboxRunning: false, mounts: m) == "Stems Cache's folder on \"GLS\" is missing or was replaced."
          && locationLight(.chosen(dir: gls, id: id, probe: .readOnly), bridge: nil, rekordboxRunning: false, mounts: m) == "\"GLS\" is read-only; macOS may need to repair it in Disk Utility."
          && locationLight(.chosen(dir: gls, id: id, probe: .noPermission), bridge: nil, rekordboxRunning: false, mounts: m)?.contains("Files and Folders › RB Stems Plus") == true
          && locationLight(.chosen(dir: gls, id: id, probe: .link), bridge: nil, rekordboxRunning: false, mounts: m)?.contains("is a link") == true
          && locationLight(.configUnreadable, bridge: nil, rekordboxRunning: false, mounts: m)?.hasPrefix("Stems Cache can't read its settings (config.ini).") == true
          && locationLight(.configMalformedLocation, bridge: nil, rekordboxRunning: false, mounts: m) == "Where Stems Cache saves stems is set wrongly in config.ini. Choose it again in Settings."
          && locationLight(.bridgeTooOld(dir: gls, id: id), bridge: nil, rekordboxRunning: false, mounts: m) == "Stems Cache needs an update to save stems on \"GLS\": it's saving them on this Mac for now. Click Reinstall Stems Cache."
          && locationLight(.chosen(dir: gls, id: id, probe: .available), bridge: nil, rekordboxRunning: false, mounts: m) == nil
          && locationLight(.thisMac, bridge: nil, rekordboxRunning: false, mounts: m) == nil, "light: each state's text")
    let off = BridgeState(pid: getpid(), start: processStartTime(getpid()) ?? -1, location: gls, state: "unavailable", reason: "not-connected")
    check(locationLight(.chosen(dir: gls, id: id, probe: .available), bridge: off, rekordboxRunning: true, mounts: m)
            == "Stems Cache will use \"GLS\" again in a few seconds.",
          "light: reconnected while rekordbox runs: the bridge's next check uses it, no restart")
    check(locationLight(.chosen(dir: gls, id: id, probe: .notConnected), bridge: off, rekordboxRunning: true, mounts: m)
            == "Stems Cache isn't using \"GLS\" right now: the drive isn't connected. Connect it: Stems Cache uses it again by itself.",
          "light: disconnected while rekordbox runs: connect it, no restart")
    for r in ["not-connected", "folder", "marker", "read-only", "not-local", "io-error", "write-failed"] {
        var b = off; b.reason = r
        check(locationLight(.chosen(dir: gls, id: id, probe: .folderMissing), bridge: b, rekordboxRunning: true, mounts: m)?.lowercased().contains("restart") == false,
              "light: the bridge's \(r): no restart (it checks the drive again by itself)")
    }
    var perm = off; perm.reason = "no-permission"
    check(locationLight(.chosen(dir: gls, id: id, probe: .available), bridge: perm, rekordboxRunning: true, mounts: m)?.contains("Files and Folders › rekordbox") == true,
          "light: the bridge without permission points to rekordbox's privacy setting")
    let stateText = "pid=\(off.pid)\nstart=\(off.start)\nlocation=\(gls)\nstate=unavailable\nreason=not-connected\n"
    check(parseBridgeState(stateText) == off && parseBridgeState("pid=1\nstate=available\n") == nil && parseBridgeState("pid=x\nstart=1\nstate=available\n") == nil
          && parseBridgeState("pid=1\nstart=1\nstate=lost\n") == nil, "bridge-state: parsed, malformed ones refused")
    check(liveBridgeState(off, dir: gls) == off && liveBridgeState(off, dir: "/Volumes/X/" + locationFolderName) == nil
          && liveBridgeState(off, dir: gls, startOf: { _ in off.start + 1 }) == nil && liveBridgeState(off, dir: gls, startOf: { _ in nil }) == nil,
          "bridge-state: trusted only for the rekordbox that wrote it (pid and start) and the location in config.ini")
    check(cacheLight(installed: true, plusOn: true, chosen: true, rekordboxModel: true, bridgeModels: [], model: nil, engine: "0002", pausedFloor: 50_000_000_000,
                     location: "X", drive: "GLS") == (.limited, "X")
          && cacheLight(installed: true, plusOn: true, chosen: true, rekordboxModel: true, bridgeModels: [], model: nil, engine: "0002",
                        pausedFloor: 50_000_000_000, drive: "GLS").tip == "Stems Cache stopped saving stems: \"GLS\" has less than 50 GB free."
          && cacheLight(installed: false, plusOn: true, chosen: true, rekordboxModel: true, bridgeModels: [], model: nil, engine: "0002", pausedFloor: nil, location: "X").look == .off,
          "light: the location's text, the floor naming the drive")

    // MARK: the size rule

    let sz = base + "/size", model = String(repeating: "a", count: 64), key = "ab" + String(repeating: "1", count: 62)
    func blocks(_ p: String) -> Int64 { var st = stat(); return lstat(p, &st) == 0 ? Int64(st.st_blocks) * 512 : 0 }
    let counted = ["\(key).flac", "\(key).desc", "._\(key).flac", "._\(key).desc"].map { "\(sz)/\(model)/ab/\($0)" }
    for p in counted { put(p, String(repeating: "x", count: 5000)) }
    for p in ["\(sz)/\(model)/ab/\(key).flac.12.tmp", "\(sz)/model-1.sha256", "\(sz)/\(model)/cd/\(key).flac",
              "\(sz)/\(model)x/ab/\(key).flac", "\(sz)/\(model)/abc/\(key).flac", "\(sz)/\(model)/ab/x.flac", "\(sz)/\(model)/ab/._x", "\(sz)/" + markerName] {
        put(p, String(repeating: "y", count: 9000))
    }
    let want = counted.map(blocks).reduce(0, +)
    check(want > 0 && entriesSize(sz) == want, "size: an entry's files and their companions on the disk, nothing else (\(entriesSize(sz) ?? -1) vs \(want))")
    check(entriesSize(base + "/nothing") == 0 && aboutSize(0) == "" && aboutSize(12_400_000_000).hasPrefix(" (about 11.5 GB")
          && aboutSize(35 * 1_048_576) == " (about 35 MB)", "size: as the texts say it")
    check(StemsSize.measuring.text == " (measuring…)" && StemsSize.failed.text == " (couldn't measure)" && StemsSize.notConnected.text == " (not connected)",
          "size: never 0 while unknown")

    // MARK: deleting through verified descriptors

    func layout(_ dir: String) {
        mk(dir); put(dir + "/" + markerName, markerText(id)); put(dir + "/._" + markerName, "c")
        put(dir + "/\(model)/ab/\(key).flac", "x"); put(dir + "/._\(model)", "c"); put(dir + "/notes.txt", "keep")
    }
    let dv = base + "/DV", dOurs = dv + "/DJ/" + locationFolderName, dMount = [Mount(path: dv, local: true, readOnly: false)]
    layout(dOurs)
    check(removeInLocation(LocationGate(dir: dOurs, id: id2), wholeFolder: false, mounts: dMount).ok == false && fileType(dOurs + "/\(model)/ab/\(key).flac") != nil,
          "delete: another marker id deletes nothing")
    let cleared = removeInLocation(LocationGate(dir: dOurs, id: id), wholeFolder: false, mounts: dMount)
    check(cleared.ok && fileType(dOurs + "/\(model)") == nil && fileType(dOurs + "/._\(model)") == nil && readMarker(dOurs).id == id
          && fileType(dOurs + "/._" + markerName) != nil && fileType(dOurs + "/notes.txt") != nil,
          "Clear: the model folders and \"._\" files go; the folder, the marker and anything else stay")
    // a link swapped in above our folder after it was checked: nothing outside is touched
    let outside = base + "/outside-DJ/" + locationFolderName
    layout(outside); put(outside + "/\(model)/sentinel", "keep")
    check(probeLocation(dir: dOurs, id: id, mounts: dMount) == .available, "delete: checked first")
    try! fm.moveItem(atPath: dv + "/DJ", toPath: dv + "/DJ-real"); symlink(base + "/outside-DJ", dv + "/DJ")
    check(!removeInLocation(LocationGate(dir: dOurs, id: id), wholeFolder: true, mounts: dMount).ok && fileType(outside + "/\(model)/sentinel") != nil
          && fileType(outside + "/" + markerName) != nil, "delete: a link swapped above our folder after the check: the sentinel outside survives")
    try! fm.removeItem(atPath: dv + "/DJ"); try! fm.moveItem(atPath: dv + "/DJ-real", toPath: dv + "/DJ")
    put(dv + "/DJ/._" + locationFolderName, "c"); put(dv + "/DJ/other.txt", "keep"); layout(dOurs)
    mk(dOurs + "/\(model)/ab/inner"); symlink(base + "/outside-DJ", dOurs + "/\(model)/ab/inner/link")
    check(removeInLocation(LocationGate(dir: dOurs, id: id), wholeFolder: true, mounts: dMount).ok && fileType(dOurs) == nil
          && fileType(dv + "/DJ/._" + locationFolderName) == nil && fileType(dv + "/DJ/other.txt") != nil && fileType(outside + "/\(model)/sentinel") != nil,
          "uninstall: our folder and its companion in the parent go, nothing else; a link inside is removed as a link")
    check(!removeInLocation(LocationGate(dir: dOurs, id: id), wholeFolder: true, mounts: []).ok, "delete: not connected: nothing")

    // MARK: the order of a switch: folder, marker, then config.ini; nothing else created

    let parent = base + "/new-parent"
    mk(parent)
    let made = createLocationFolder(in: parent)
    check(made.dir == parent + "/" + locationFolderName && made.id.map(isUUID) == true && readMarker(made.dir!).id == made.id
          && (try? fm.contentsOfDirectory(atPath: made.dir!)) == [markerName] && (try? fm.contentsOfDirectory(atPath: parent)) == [locationFolderName],
          "switch: our folder and its marker only")
    let again = createLocationFolder(in: parent)
    check(again.dir == nil && again.error == "exists" && readMarker(made.dir!).id == made.id, "switch: an existing folder is never taken over, its marker never rewritten")
    removeNewLocationFolder(made.dir!)
    check(fileType(made.dir!) == nil, "switch: a failure before config.ini removes what this run made")
    let made2 = createLocationFolder(in: parent); put(made2.dir! + "/someone.txt", "x")
    removeNewLocationFolder(made2.dir!)
    check(fileType(made2.dir! + "/someone.txt") != nil, "switch: a folder with anything else in it stays")
    // This Mac's stems after the commit: the model folders, never the memos
    let mac = base + "/mac-cache"
    put(mac + "/\(model)/ab/\(key).flac", "x"); put(mac + "/model-12.sha256", "memo"); put(mac + "/other/x", "x")
    let macGate = Deletable(inside: [mac], roots: [], files: [], apps: [])
    check(removeThisMacStems(mac, macGate) && fileType(mac + "/\(model)") == nil && fileType(mac + "/model-12.sha256") != nil && fileType(mac + "/other/x") != nil,
          "switch: this Mac's model folders deleted, the memos kept")

    // MARK: the retry of this Mac's deletion

    let avail = LocationStatus.chosen(dir: gls, id: id, probe: .available)
    check(mayRetryThisMacDeletion(flag: true, status: avail, bridgeLocation: true, rekordboxRunning: false), "retry: flag, available, location=1, rekordbox closed")
    check(!mayRetryThisMacDeletion(flag: false, status: avail, bridgeLocation: true, rekordboxRunning: false)
          && !mayRetryThisMacDeletion(flag: true, status: .bridgeTooOld(dir: gls, id: id), bridgeLocation: false, rekordboxRunning: false)
          && !mayRetryThisMacDeletion(flag: true, status: avail, bridgeLocation: false, rekordboxRunning: false)
          && !mayRetryThisMacDeletion(flag: true, status: avail, bridgeLocation: true, rekordboxRunning: true)
          && !mayRetryThisMacDeletion(flag: true, status: .chosen(dir: gls, id: id, probe: .notConnected), bridgeLocation: true, rekordboxRunning: false)
          && !mayRetryThisMacDeletion(flag: true, status: .configMalformedLocation, bridgeLocation: true, rekordboxRunning: false)
          && !mayRetryThisMacDeletion(flag: true, status: .configUnreadable, bridgeLocation: true, rekordboxRunning: false),
          "retry: never otherwise (an older bridge may be saving on this Mac)")

    // MARK: finding the stems already on a drive

    let d2 = base + "/GLS 2"
    put(d2 + "/DJ/" + locationFolderName + "/" + markerName, markerText(id))
    check(discover(folder: d2 + "/DJ/" + locationFolderName, mount: d2, relative: nil) == .found(dir: d2 + "/DJ/" + locationFolderName, id: id),
          "discover: the chosen folder itself")
    check(discover(folder: d2 + "/DJ", mount: d2, relative: nil) == .found(dir: d2 + "/DJ/" + locationFolderName, id: id), "discover: inside the chosen folder")
    check(discover(folder: d2, mount: d2, relative: relativeOnDrive("/Volumes/GLS/DJ/" + locationFolderName, hint: LocationHint()))
          == .found(dir: d2 + "/DJ/" + locationFolderName, id: id), "discover: the old path on a renamed drive (from cache_dir)")
    check(discover(folder: d2, mount: d2, relative: relativeOnDrive(nil, hint: LocationHint(mount: "/Users/x/mnt", relative: "DJ/" + locationFolderName, ownIDs: [])))
          == .found(dir: d2 + "/DJ/" + locationFolderName, id: id), "discover: the old path from the hint (a mount point outside /Volumes)")
    check(discover(folder: d2, mount: d2, relative: nil) == .none && discover(folder: d2, mount: d2, relative: "../x") == .none, "discover: nothing else is looked at")
    mk(d2 + "/Bare/" + locationFolderName)
    check(discover(folder: d2 + "/Bare", mount: d2, relative: nil) == .notOurs(dir: d2 + "/Bare/" + locationFolderName), "discover: our name without a marker is refused")
    let hfile = base + "/hint"
    let h = hintFor(dir: d2 + "/DJ/" + locationFolderName, mount: d2, old: LocationHint(), newID: id)
    check(writeLocationHint(h, hfile) && readLocationHint(hfile) == h && h.relative == "DJ/" + locationFolderName && h.ownIDs == [id], "hint: written and read back")

    // MARK: where a location may be (3.1.6), by the volume's facts

    let goodFacts = VolumeFacts(mountPoint: vol, name: "GLS", fsType: "exfat", local: true, readOnly: false, network: false,
                                deviceInternal: false, deviceProtocol: "USB", boot: false, timeMachine: false)
    func refusal(_ change: (inout VolumeFacts) -> Void, folder: String = vol + "/DJ") -> LocationRefusal? {
        var f = goodFacts; change(&f); return locationRefusalFor(folder: folder, real: folder, facts: f)
    }
    check(refusal({ _ in }, folder: vol + "/DJ") == .insideOurFolder || true, "where: (setup)")
    try? fm.removeItem(atPath: vol); mk(vol + "/DJ")
    check(refusal({ _ in }) == nil && refusal({ $0.deviceProtocol = "Disk Image" }) == nil && refusal({ $0.fsType = "apfs" }) == nil, "where: an external drive (and a disk image)")
    check(refusal({ $0.deviceInternal = true }) == .internalDisk && refusal({ $0.deviceInternal = nil }) == .internalDisk && refusal({ $0.boot = true }) == .internalDisk,
          "where: internal disks, an unknown device and the boot volume are refused")
    check(refusal({ $0.local = false }) == .network && refusal({ $0.network = true }) == .network, "where: network volumes refused")
    check(refusal({ $0.readOnly = true }) == .readOnly && refusal({ $0.fsType = "ntfs"; $0.readOnly = true }) == .readOnly, "where: read-only (NTFS too)")
    check(refusal({ $0.timeMachine = true }) == .timeMachine && refusal({ $0.fsType = "smbfs" }) == .fileSystem, "where: Time Machine, other file systems")
    check(locationRefusalFor(folder: vol + "/DJ", real: vol + "/Other", facts: goodFacts) == .link, "where: a folder reached through a link")
    mk(vol + "/DJ/" + locationFolderName + "/x")
    check(refusal({ _ in }, folder: vol + "/DJ/" + locationFolderName + "/x") == .insideOurFolder && refusal({ _ in }, folder: vol + "/DJ/" + locationFolderName) == nil,
          "where: inside our folder refused, our folder itself fine")
    put(vol + "/DJ/" + markerName, markerText(id)); mk(vol + "/DJ/sub")
    check(refusal({ _ in }, folder: vol + "/DJ/sub") == .insideOurFolder, "where: a marker above refused")
    check(locationRefusalFor(folder: vol, real: vol, facts: nil) == .unreadable, "where: unreadable volume")
    check(!externalDeviceAllowed(isInternal: true, deviceProtocol: "Thunderbolt") && externalDeviceAllowed(isInternal: false, deviceProtocol: "USB"),
          "where: Q6 by DeviceInternal (Q10 open)")
    check(locationRefusal(.internalDisk, drive: "GLS") == "Choose a folder on an external drive. To save stems on this Mac, use This Mac."
          && locationRefusal(.network, drive: "GLS") == "Choose a drive connected to this Mac, not a network drive."
          && locationRefusal(.readOnly, drive: "GLS") == "\"GLS\" is read-only." && locationRefusal(.timeMachine, drive: "GLS") == "\"GLS\" is a Time Machine backup drive."
          && locationRefusal(.insideOurFolder, drive: "GLS") == "Choose the folder that contains \"RB Stems Plus Cache\", not a folder inside it.",
          "where: the refusal texts")

    // MARK: the dialogs' texts

    let gb182 = Int64(18.2 * 1_073_741_824), gb124 = Int64(12.4 * 1_073_741_824)
    check(switchMessage(drive: "GLS", found: nil, thisMac: gb182, oldDrive: nil)
          == "Stems Cache will save stems on \"GLS\". The 18.2 GB saved on this Mac will be deleted: rekordbox separates those tracks again the next time you load them.",
          "switch: from this Mac to a new folder")
    check(switchMessage(drive: "GLS", found: gb124, thisMac: gb182, oldDrive: nil)
          == "Stems Cache will use the 12.4 GB already saved on \"GLS\". The 18.2 GB saved on this Mac will be deleted: rekordbox separates those tracks again the next time you load them.",
          "switch: stems already on the drive")
    check(switchMessage(drive: "GLS", found: nil, thisMac: 0, oldDrive: nil) == "Stems Cache will save stems on \"GLS\"."
          && switchMessage(drive: "Stick", found: nil, thisMac: 0, oldDrive: "GLS") == "Stems Cache will save stems on \"Stick\". The stems saved on \"GLS\" stay there.",
          "switch: nothing on this Mac to delete; from another drive, its stems stay")
    check(useThisMacMessage(oldDrive: "GLS") == "Stems Cache will save stems on this Mac. The stems saved on \"GLS\" stay there.",
          "Use This Mac: the drive's stems stay")
    for m in [switchMessage(drive: "GLS", found: gb124, thisMac: gb182, oldDrive: "Old"), useThisMacMessage(oldDrive: "GLS")] {
        check(!m.contains("Settings") && !m.contains("Finder") && !m.contains("eject") && !m.contains("another Mac"),
              "the switch dialogs say only what the switch does: \(m.prefix(40))…")
    }
    check(clearLocationMessage(drive: "GLS") == "Deletes all stems saved in this folder on \"GLS\", including any saved there by other Macs or accounts.", "Clear: the shared folder's wording")
    check(uninstallEverythingMessage(othersCache: false, noSavedModel: false, stems: " (about 18.0 GB)", drive: "GLS", driveConnected: true)
            .hasSuffix("RB Stems Plus will be deleted, including all stems saved in its folder on \"GLS\" (about 18.0 GB), also any saved there by other Macs or accounts.")
          && uninstallEverythingMessage(othersCache: false, noSavedModel: false, stems: "", drive: "GLS", driveConnected: false)
            .hasSuffix("Its stems on \"GLS\", which isn't connected, stay: connect it first to delete them too.")
          && uninstallEverythingMessage(othersCache: false, noSavedModel: false, stems: " (1.2 GB)").hasSuffix("including the stems it saved (1.2 GB)."),
          "uninstall: the confirmation names the drive, connected or not")
    check(ownFilesRemovedText("A.", app: .deleted, othersCache: false, cacheLink: false, left: ["B."]) == "A. B.", "uninstall: the final alert says what stayed")
    check(locationRemovalNote(drive: nil, connected: true, size: "") == "" && locationRemovalNote(drive: "GLS", connected: false, size: "").contains("isn't connected"),
          "Remove anyway: names the drive's stems")
    check(locationLabel(cs(.chosen(dir: "/Volumes/GLS/DJ/Sets/" + locationFolderName, id: id)), mounts: m) == "Stems Cache location: GLS › DJ › Sets"
          && locationLabel(cs(.chosen(dir: gls, id: id)), mounts: m) == "Stems Cache location: GLS" && locationLabel(cs(.thisMac)) == "Stems Cache location: This Mac",
          "Settings: the location row's label")

    // MARK: the report's link and title name no drive and no path

    let privateDir = "/Volumes/GLS/Clients/Secret Project/" + locationFolderName
    let line = "2026-11-01 12:00:00  stems on \(privateDir) deleted: FAILED (x); drive \"GLS\"; also /Volumes/Other Disk/a b/c.flac (y)"
    let masked = maskLocation(line, dir: privateDir, mounts: [Mount(path: "/Volumes/GLS", local: true, readOnly: false)])
    check(!masked.contains("GLS") && !masked.contains("Secret") && !masked.contains("Clients") && !masked.contains("Other Disk") && masked.contains("<stems folder>"),
          "report: no drive name or path in the link: \(masked)")
    var snap = LocationSnapshot(); snap.status = .chosen(dir: privateDir, id: id, probe: .notConnected); snap.bridge = off
    check(locationCode(snap) == "notConnected (bridge unavailable/not-connected)", "report: the location as its code")
}

/// tests/config-fixtures.txt's content escapes decoded to bytes: \n \r \t \\ \xHH and
/// \*COUNT.C (the character C, COUNT times).
func fixtureBytes(_ s: String) -> [UInt8]? {
    let b = Array(s.utf8)
    var out: [UInt8] = [], i = 0
    while i < b.count {
        guard b[i] == 0x5C else { out.append(b[i]); i += 1; continue }
        guard i + 1 < b.count else { return nil }
        switch b[i + 1] {
        case UInt8(ascii: "n"): out.append(0x0A); i += 2
        case UInt8(ascii: "r"): out.append(0x0D); i += 2
        case UInt8(ascii: "t"): out.append(0x09); i += 2
        case 0x5C: out.append(0x5C); i += 2
        case UInt8(ascii: "x"):
            guard i + 3 < b.count, let v = UInt8(String(decoding: b[(i + 2)...(i + 3)], as: UTF8.self), radix: 16) else { return nil }
            out.append(v); i += 4
        case UInt8(ascii: "*"):
            var j = i + 2
            while j < b.count, b[j] >= 0x30, b[j] <= 0x39 { j += 1 }
            guard j + 1 < b.count, b[j] == UInt8(ascii: "."), let n = Int(String(decoding: b[(i + 2)..<j], as: UTF8.self)) else { return nil }
            out += [UInt8](repeating: b[j + 1], count: n); i = j + 2
        default: return nil
        }
    }
    return out
}

/// Every fixture of tests/config-fixtures.txt, read by the app's reader: the values and the
/// location it expects (the bridge's run.sh checks the same file).
func configFixtureTests(_ cfg: String) {
    let file = FileManager.default.currentDirectoryPath + "/tests/config-fixtures.txt"
    guard let text = try? String(contentsOfFile: file, encoding: .utf8) else { check(false, "config fixtures: \(file) can't be read"); return }
    var n = 0
    for line in text.components(separatedBy: "\n") where !line.isEmpty && !line.hasPrefix("#") {
        let f = line.components(separatedBy: "\t")
        guard f.count == 2 || f.count == 3, let bytes = fixtureBytes(f.count == 3 ? f[2] : "") else { check(false, "config fixture: can't parse \(line.prefix(60))"); continue }
        n += 1
        FileManager.default.createFile(atPath: cfg, contents: Data(bytes))
        let st = readConfigState(file: cfg)
        var want: [String: String] = [:]
        for kv in f[1].split(separator: " ") { if let eq = kv.firstIndex(of: "=") { want[String(kv[..<eq])] = String(kv[kv.index(after: eq)...]) } }
        var got: String
        switch st.location {
        case _ where st.unreadable != nil: got = "unreadable"
        case .thisMac: got = "thismac"
        case .chosen: got = "chosen"
        case .malformed: got = "malformed"
        }
        var ok = got == want["location"]
        if got != "unreadable" {
            if let e = want["enabled"] { ok = ok && (st.config.enabled ? "1" : "0") == e }
            if let g = want["max_gb"].flatMap(Double.init) { ok = ok && st.config.maxGB == g }
            if let d = want["max_days"].flatMap(Double.init) { ok = ok && st.config.maxDays == d }
            if let d = want["dir"] {
                let dir = fixtureBytes(d).map { String(decoding: $0, as: UTF8.self).replacingOccurrences(of: "%20", with: " ") }
                ok = ok && st.location.chosenDir == dir
            }
        }
        check(ok, "config fixture \(f[0]): want \(f[1].prefix(80)); got location=\(got) enabled=\(st.config.enabled ? 1 : 0) max_gb=\(configNumber(st.config.maxGB)) max_days=\(configNumber(st.config.maxDays))")
    }
    check(n >= 40, "config fixtures: \(n) read")
}

/// The code review's fixes (2026-11-02): what the uninstall may delete on a drive, the marker
/// last, the background measurement publishing before the size walk, This Mac's leftovers after
/// an older bridge, the writer over a section line over the limit and with a BOM, the marker
/// fixtures shared with the bridge, the light for the bridge's "disabled" and "config", Settings'
/// sizes, the Time Machine role.
func locationFixTests() {
    let fm = FileManager.default
    let base = (realPath(tmp) ?? tmp) + "/location-fixes"
    func mk(_ p: String) { try! fm.createDirectory(atPath: p, withIntermediateDirectories: true) }
    func put(_ p: String, _ s: String) { mk((p as NSString).deletingLastPathComponent); fm.createFile(atPath: p, contents: Data(s.utf8)) }
    mk(base)
    let id = "0F8FAD5B-D9CB-469F-A165-70867728950E", id2 = "7C9E6679-7425-40DE-944B-E07FC1F90AE7"
    let gls = "/Volumes/GLS/" + locationFolderName, model = String(repeating: "a", count: 64), key = "ab" + String(repeating: "1", count: 62)

    // the uninstall deletes only the folder its confirmation named, found again just before
    let g = LocationGate(dir: gls, id: id)
    check(!driveDeletionAllowed(confirmed: nil, fresh: g), "uninstall: a drive the confirmation didn't name (connected since, or no confirmation) stays")
    check(!driveDeletionAllowed(confirmed: g, fresh: nil), "uninstall: the named drive gone since: nothing to delete")
    check(driveDeletionAllowed(confirmed: g, fresh: LocationGate(dir: gls, id: id.lowercased())), "uninstall: the named folder, found again: deleted")
    check(!driveDeletionAllowed(confirmed: g, fresh: LocationGate(dir: gls, id: id2))
          && !driveDeletionAllowed(confirmed: g, fresh: LocationGate(dir: "/Volumes/GLS 2/" + locationFolderName, id: id)),
          "uninstall: another folder or marker than the one named: kept")
    var snap = LocationSnapshot(); snap.status = .chosen(dir: gls, id: id, probe: .available); snap.probe = .available
    check(snap.deletableGate == g, "uninstall: the gate of an available chosen folder")
    snap.probe = .notConnected; snap.status = .chosen(dir: gls, id: id, probe: .notConnected)
    check(snap.deletableGate == nil, "uninstall: no gate for a folder not connected")
    snap.status = .bridgeTooOld(dir: gls, id: id); snap.probe = .available
    check(snap.deletableGate == g, "uninstall: an older bridge's chosen folder, connected: its gate")
    check(uninstallEverythingMessage(othersCache: false, noSavedModel: false, stems: "", drive: "GLS", driveConnected: true).contains("on \"GLS\"")
          && uninstallEverythingMessage(othersCache: false, noSavedModel: false, stems: "", drive: "GLS", driveConnected: true).contains("other Macs or accounts"),
          "uninstall: the confirmation of a resolved, available drive names it and other Macs or accounts")

    // the complete uninstall's deletion: the marker goes last, only if everything else went
    let dv = base + "/DV", ours = dv + "/DJ/" + locationFolderName, mounts = [Mount(path: dv, local: true, readOnly: false)]
    put(ours + "/" + markerName, markerText(id)); put(ours + "/._" + markerName, "c")
    put(ours + "/\(model)/ab/\(key).flac", "x"); put(ours + "/\(model)/cd/x.flac", "y")
    chmod(ours + "/\(model)/ab", 0o500)                         // its entry can't be deleted
    let r = removeInLocation(LocationGate(dir: ours, id: id), wholeFolder: true, mounts: mounts)
    chmod(ours + "/\(model)/ab", 0o755)
    check(!r.ok && readMarker(ours).id == id && fileType(ours + "/._" + markerName) != nil && fileType(ours + "/\(model)/ab/\(key).flac") != nil,
          "uninstall: an entry that can't be deleted leaves the folder with its marker (found again later)")
    check(removeInLocation(LocationGate(dir: ours, id: id), wholeFolder: true, mounts: mounts).ok && fileType(ours) == nil, "uninstall: then the whole folder, marker last")

    // the background measurement: the cheap state first, the size after its walk; a walk of a
    // folder no longer in use is dropped
    var folder = "/Volumes/A/" + locationFolderName
    let lock = NSLock()
    func cheap() -> LocationSnapshot {
        lock.lock(); let f = folder; lock.unlock()
        var s = LocationSnapshot(); s.status = .chosen(dir: f, id: id, probe: .available); s.probe = .available; return s
    }
    let mon = LocationMonitor(cheapState: cheap, sizeOf: { f in usleep(1_000_000); return Int64(f.utf8.count) })
    mon.update(size: true)
    usleep(300_000)
    let early = mon.snapshot()
    check(early.status == .chosen(dir: folder, id: id, probe: .available) && early.size == .measuring,
          "monitor: the state is published before the size walk ends (\(early.size))")
    lock.lock(); folder = "/Volumes/Bee/" + locationFolderName; lock.unlock()
    mon.update()                                                  // the folder changed during the walk
    var waited = 0
    while mon.snapshot().size != .measured(Int64(folder.utf8.count)) && waited < 50 { usleep(100_000); waited += 1 }
    check(mon.snapshot().size == .measured(Int64(folder.utf8.count)) && mon.snapshot().status?.dir == folder,
          "monitor: the old folder's walk is dropped, the new folder's size published")
    mon.invalidateSize()
    check(mon.snapshot().size == .measuring, "monitor: a size invalidated (after Clear) says measuring")

    // This Mac's leftovers from an older bridge (the owner's decision: deleted at launch, guarded)
    let avail = LocationStatus.chosen(dir: gls, id: id, probe: .available)
    check(thisMacNeedsCleanup(status: avail, bridgeLocation: true, modelFolders: true), "leftovers: a drive in use, location=1, model folders on this Mac: cleaned up")
    check(!thisMacNeedsCleanup(status: avail, bridgeLocation: false, modelFolders: true)
          && !thisMacNeedsCleanup(status: .bridgeTooOld(dir: gls, id: id), bridgeLocation: false, modelFolders: true)
          && !thisMacNeedsCleanup(status: .chosen(dir: gls, id: id, probe: .notConnected), bridgeLocation: true, modelFolders: true)
          && !thisMacNeedsCleanup(status: .thisMac, bridgeLocation: true, modelFolders: true)
          && !thisMacNeedsCleanup(status: avail, bridgeLocation: true, modelFolders: false),
          "leftovers: never with an older bridge, on this Mac, a drive not in use, or nothing there")
    let mac = base + "/mac"
    put(mac + "/model-1.sha256", "memo")
    check(!thisMacHasModelFolders(mac), "leftovers: memos aren't stems")
    mk(mac + "/" + model)
    check(thisMacHasModelFolders(mac), "leftovers: a model folder is")
    check(thisMacDeletionNow(cachePresent: false, bridgeLocation: false) && thisMacDeletionNow(cachePresent: true, bridgeLocation: true)
          && !thisMacDeletionNow(cachePresent: true, bridgeLocation: false), "switch: this Mac's stems wait while an older Stems Cache saves there")

    // Settings' size and Clear
    func cfg(_ l: ConfigLocation) -> ConfigState { var c = ConfigState(); c.location = l; return c }
    var s = LocationSnapshot(); s.status = .bridgeTooOld(dir: gls, id: id); s.size = .measured(5_000_000_000)
    check(settingsStems(cfg(.chosen(dir: gls, id: id)), s) == (aboutSize(5_000_000_000), ClearTarget.thisMac),
          "Settings: while an older bridge saves on this Mac, its size and Clear are this Mac's")
    s.status = .chosen(dir: gls, id: id, probe: .available)
    check(settingsStems(cfg(.chosen(dir: gls, id: id)), s) == (aboutSize(5_000_000_000), ClearTarget.drive(g)), "Settings: the drive in use")
    check(settingsStems(cfg(.thisMac), s) == (StemsSize.measuring.text, ClearTarget.thisMac)
          && settingsStems(cfg(.chosen(dir: "/Volumes/X/" + locationFolderName, id: id)), s) == (StemsSize.measuring.text, nil),
          "Settings: a measurement of another folder (just after a change) says measuring, never its size")
    s.status = .chosen(dir: gls, id: id, probe: .notConnected)
    check(settingsStems(cfg(.chosen(dir: gls, id: id)), s) == (StemsSize.notConnected.text, nil), "Settings: the drive not connected")
    var bad = ConfigState(); bad.unreadable = "x"
    check(settingsStems(bad, s).clear == nil, "Settings: nothing with config.ini unreadable")

    // the writer: a section line over the limit, a BOM
    let c = base + "/config.ini"
    func fixture(_ name: String) -> [UInt8] {
        let text = (try? String(contentsOfFile: fm.currentDirectoryPath + "/tests/config-fixtures.txt", encoding: .utf8)) ?? ""
        let line = text.components(separatedBy: "\n").first { $0.hasPrefix(name + "\t") } ?? ""
        let f = line.components(separatedBy: "\t")
        return f.count == 3 ? fixtureBytes(f[2]) ?? [] : []
    }
    for name in ["long-section-with-location", "long-section-location-elsewhere"] {
        fm.createFile(atPath: c, contents: Data(fixture(name)))
        if case .malformed = readConfigState(file: c).location {} else { check(false, "writer: fixture \(name) starts malformed") }
        check(writeCacheLocation(dir: gls, id: id, file: c) && readConfigState(file: c).location == .chosen(dir: gls, id: id),
              "writer: Change… over \(name): the location written is the one read")
        fm.createFile(atPath: c, contents: Data(fixture(name)))
        check(writeCacheLocation(dir: nil, id: nil, file: c) && readConfigState(file: c).location == .thisMac, "writer: Use This Mac over \(name): this Mac")
    }
    fm.createFile(atPath: c, contents: Data(fixture("long-section-with-location")))
    _ = writeCacheLocation(dir: gls, id: id, file: c)
    check(readConfigState(file: c).config.enabled, "writer: the keys under the replaced section line stay someone else's (enabled=0 there isn't ours)")
    fm.createFile(atPath: c, contents: Data([0xEF, 0xBB, 0xBF] + Array("[watcher]\nenabled=0\n".utf8)))
    let wrote = writeConfigValues([("max_gb", "5")], file: c)
    let out = fm.contents(atPath: c).map { [UInt8]($0) } ?? []
    check(wrote && out.starts(with: [0xEF, 0xBB, 0xBF] + Array("max_gb=5\n[watcher]".utf8))
          && readConfigState(file: c).config.enabled && readConfigState(file: c).config.maxGB == 5,
          "writer: a BOM stays at offset 0 when a key goes before the first section")
    check(cacheDirProblem("/" + locationFolderName) != nil, "path rules: never at the root (the bridge's rule)")

    // the marker: the fixtures shared with the bridge
    let file = fm.currentDirectoryPath + "/tests/marker-fixtures.txt", want = "0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F"
    var n = 0
    for line in ((try? String(contentsOfFile: file, encoding: .utf8)) ?? "").components(separatedBy: "\n") where !line.isEmpty && !line.hasPrefix("#") {
        let f = line.components(separatedBy: "\t")
        guard f.count == 2 || f.count == 3, let bytes = fixtureBytes(f.count == 3 ? f[2] : "") else { check(false, "marker fixture: can't parse \(line.prefix(60))"); continue }
        n += 1
        let d = base + "/marker-\(n)"
        mk(d); fm.createFile(atPath: d + "/" + markerName, contents: Data(bytes))
        let valid = readMarker(d).id?.lowercased() == want.lowercased()
        check(valid == (f[1] == "valid"), "marker fixture \(f[0]): want \(f[1]), got \(valid ? "valid" : "invalid")")
    }
    check(n >= 15, "marker fixtures: \(n) read")

    // the light: the bridge's "disabled" and "config"
    let m = [Mount(path: "/Volumes/GLS", local: true, readOnly: false)]
    let me = BridgeState(pid: getpid(), start: processStartTime(getpid()) ?? -1, location: "", state: "unavailable", reason: "disabled")
    check(locationLight(.thisMac, bridge: me, rekordboxRunning: true, mounts: m) == nil
          && locationLight(.chosen(dir: gls, id: id, probe: .available), bridge: BridgeState(pid: me.pid, start: me.start, location: gls, state: "unavailable", reason: "disabled"), rekordboxRunning: true, mounts: m) == nil,
          "light: Stems Cache off (disabled): no restart text")
    var cfgReason = me; cfgReason.reason = "config"
    check(locationLight(.configMalformedLocation, bridge: cfgReason, rekordboxRunning: true, mounts: m) == "Where Stems Cache saves stems is set wrongly in config.ini. Choose it again in Settings."
          && locationLight(.configUnreadable, bridge: cfgReason, rekordboxRunning: true, mounts: m)?.hasPrefix("Stems Cache can't read its settings") == true,
          "light: config.ini still wrong: the app's own text, not restart")
    check(locationLight(.thisMac, bridge: cfgReason, rekordboxRunning: true, mounts: m)?.contains("Restart rekordbox") == true,
          "light: config.ini fixed while rekordbox runs: restart")

    // Time Machine by the APFS role
    let plist = """
    <?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>Containers</key><array><dict><key>Volumes</key><array>
    <dict><key>DeviceIdentifier</key><string>disk5s1</string><key>Roles</key><array><string>Backup</string></array></dict>
    <dict><key>DeviceIdentifier</key><string>disk5s2</string><key>Roles</key><array/></dict>
    </array></dict></array></dict></plist>
    """
    check(apfsRoles(plist: Data(plist.utf8), device: "disk5s1") == ["Backup"] && apfsRoles(plist: Data(plist.utf8), device: "disk5s2") == []
          && apfsRoles(plist: Data(plist.utf8), device: "disk9s1") == [] && apfsRoles(plist: Data("x".utf8), device: "disk5s1") == [],
          "where: an APFS volume's roles (Backup: Time Machine), by device")
}
