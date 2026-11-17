// One version for both platforms (Update.swift, Payload.swift): a release's payload_version is
// its VERSION, app.version the version of the last release that changed the Mac app. A carried
// app gets no notice and no swap; the app shows the release's version once a verified
// payload.json names it; a beta's payload.json never counts. Keys and files made here, in the
// temporary folder: the user's payload.json, floor and record are never used.
import Foundation
import CryptoKit

func oneVersionTests() {
    let fm = FileManager.default
    let dir = tmp + "/one-version"
    try! fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
    let key = P256.Signing.PrivateKey()
    let keys = [TrustedKey(name: "release", base64: key.publicKey.derRepresentation.base64EncodedString())].compactMap { $0 }
    func sign(_ d: Data) -> Data { try! key.signature(for: d).derRepresentation }
    let sha = String(repeating: "ab", count: 32), commit = String(repeating: "0", count: 40)
    let windows = """
        "windows": {"app": {"version": "1.5.0", "zip": "rbstemsplus-windows.zip", "sha256": "\(sha)", "admin_sha256": "\(sha)",
                            "stub_sha256": {"x64": "\(sha)", "arm64": "\(sha)"}},
                    "bridge": {"file": "rbstemsplus-bridge-win-x64.dll", "sha256": "\(sha)"},
                    "compatible": {"rekordbox": ["7.2.19"], "ort_sha256": ["\(sha)"], "stems_engine": ["0002"]},
                    "resampler_proved": [{"product_version": "7.2.19", "rekordbox_sha256": "\(sha)"}]}
        """
    /// A final's payload.json as release.sh writes it from 1.4.0: app.commit, bridge.commit and,
    /// with `win`, the windows block (keys no Mac app reads).
    func payload(_ v: String, app: String, win: Bool = true) -> Data {
        Data("""
            {"payload_version": "\(v)", "commit": "\(commit)",
             "app": {"version": "\(app)", "commit": "\(commit)", "zip": "rbstemsplus-app.zip", "sha256": "\(sha)"},
             "bridge": {"file": "libonnxruntime.1.18.0.dylib", "commit": "\(commit)", "sha256": "\(sha)"},
             "model": {"file": "stemsplus-model.onnx", "sha256": "\(sha)", "size": 9}, "previous_models": [],
             "compatible": {"rekordbox": ["7.2.19"], "ort_version_prefix": "1.18.", "stems_engine": ["0002"]}\(win ? ",\n" + windows : "")}

            """.utf8)
    }
    /// A beta's payload.json: no Mac "app" or "bridge" (release.sh refuses a beta with them).
    func betaPayload(_ v: String) -> Data {
        Data("""
            {"payload_version": "\(v)", "commit": "\(commit)",
             "model": {"file": "stemsplus-model.onnx", "sha256": "\(sha)", "size": 9}, "previous_models": [],
            \(windows)}

            """.utf8)
    }

    // MARK: x.y.z: the only versions a payload.json may carry
    for v in ["1.4.0", "1.10.2", "0.0.1", "123456789.0.0"] { check(isReleaseVersion(v), "x.y.z: \(v) is a release version") }
    for v in ["1.5.0-beta.1", "1.5.0-beta", "v1.4.0", "1.4", "1.4.0.1", "1.4.0 ", " 1.4.0", "", "1..0", "1.4.x", "1.4.0\n",
              "\u{0661}.\u{0664}.\u{0660}", "1234567890.0.0", "+1.4.0", "-1.4.0", "1.4.0.", ".1.4.0"] {
        check(!isReleaseVersion(v), "x.y.z: \(v.debugDescription) isn't one")
    }
    check(decodeManifest(payload("1.4.1", app: "1.4.0")) != nil, "a final with a carried app, app.commit, bridge.commit and a windows block is valid")
    check(decodeManifest(payload("1.4.1", app: "1.4.0", win: false)) != nil, "a final without the windows block (Mac releases before 1.5.0) is valid")
    check(decodeManifest(payload("1.5.0-beta.1", app: "1.5.0")) == nil, "a suffixed payload_version is refused (Manifest.valid)")
    check(decodeManifest(payload("1.5.0", app: "1.5.0-beta.1")) == nil, "a suffixed app.version is refused")
    check(decodeManifest(payload("1.5.0-beta.1", app: "1.4.0")) == nil, "a suffixed payload_version with a carried app is refused")
    check(decodeManifest(payload("1.5", app: "1.5")) == nil, "a version that isn't x.y.z (1.5) is refused")
    check(decodeManifest(betaPayload("1.5.0-beta.1")) == nil, "a beta's payload.json (no app, no bridge) is refused")
    check(decodeManifest(betaPayload("1.5.0")) == nil, "a payload.json without app is refused, whatever its version")
    check(downloadBase.hasSuffix("/releases/latest/download/"), "payload.json comes from releases/latest only, which never names a pre-release")

    // MARK: a 1.4.0 app (its own version is the floor) reading the releases after it
    let store = dir + "/store"
    try! fm.createDirectory(atPath: store, withIntermediateDirectories: true)
    let files = PayloadFiles(json: store + "/payload.json", signature: store + "/payload.json.sig", floor: store + "/payload-version",
                             keys: keys, minimum: "1.4.0", shown: store + "/shown-version")
    func accept(_ d: Data) -> Result<Manifest, PayloadRefusal> { acceptManifest(d, signature: sign(d), files) }
    func record() -> String? { fm.contents(atPath: files.shown).map { String(decoding: $0, as: UTF8.self) } }
    check(currentShownVersion(files, own: "1.4.0") == "1.4.0" && record() == nil, "no payload.json yet: the app's own version is shown")

    // a release that carried the Mac app (Windows only): no notice, the release's version shown
    let carried = payload("1.4.1", app: "1.4.0")
    guard case .success(let m1) = accept(carried) else { check(false, "the carried release 1.4.1 (app 1.4.0) is accepted"); return }
    check(payloadFloor(files) == "1.4.1", "the carried release raises the floor to 1.4.1")
    check(!newerThanApp(m1.app.version, own: "1.4.0") && updateNotice(app: m1.app.version, own: "1.4.0") == nil,
          "carried app (payload 1.4.1, app 1.4.0) on app 1.4.0: no notice, no update, so no swap and no App Management prompt")
    check(noteShownVersion(m1, files, own: "1.4.0") == "1.4.1" && record() == "1.4.1\n1.4.0\n", "and it shows 1.4.1, recorded with its app version")
    check(currentShownVersion(files, own: "1.4.0") == "1.4.1", "the watcher, the report and About read 1.4.1 from the record")
    check(versionText(own: "1.4.0", build: "1.4.0", shown: "1.4.1") == "1.4.1 (Mac app 1.4.0, build 1.4.0)",
          "the log and the report: the release's version, then the Mac app's own")
    check(versionText(own: "1.4.0", build: "1.4.0", shown: "1.4.0") == "1.4.0 (1.4.0)", "with nothing else to show: as 1.3.0 writes it")
    check(appLine(version: "1.4.0", build: "1.4.0", shown: "1.4.1", plus: (.on, "a."), cache: (.off, "b.")).hasPrefix("RB Stems Plus: 1.4.1 (Mac app 1.4.0, build 1.4.0); lights:"),
          "the report's app line names both")
    // the saved copy, read again at the next launch, keeps it
    check(savedManifest(files).map { noteShownVersion($0, files, own: "1.4.0") } == "1.4.1", "read back at launch: still 1.4.1")
    // a payload.json that isn't verified moves nothing
    check(noteShownVersion(decodeManifest(payload("1.4.9", app: "1.4.0")), files, own: "1.4.0") == "1.4.1", "an unverified payload.json moves nothing")

    // a release with a new Mac app: the notice names that app; the shown version doesn't step back
    let newApp = payload("1.4.2", app: "1.4.2")
    guard case .success(let m2) = accept(newApp) else { check(false, "the release 1.4.2 (app 1.4.2) is accepted"); return }
    check(updateNotice(app: m2.app.version, own: "1.4.0") == "RB Stems Plus 1.4.2 is available: choose Update RB Stems Plus in the RB Stems Plus menu.",
          "payload 1.4.2 / app 1.4.2 on app 1.4.0: the notice names 1.4.2")
    check(noteShownVersion(m2, files, own: "1.4.0") == "1.4.1" && record() == "1.4.1\n1.4.0\n",
          "fetched, not installed: 1.4.1 is still shown (1.4.2 isn't this app)")

    // betas: refused before the floor moves, so the real final still counts after them
    let floorBefore = payloadFloor(files), jsonBefore = fm.contents(atPath: files.json)
    for (what, d) in [("payload 1.5.0-beta.1 (app 1.5.0-beta.1)", payload("1.5.0-beta.1", app: "1.5.0-beta.1")),
                      ("payload 1.5.0-beta.1 (app 1.4.2)", payload("1.5.0-beta.1", app: "1.4.2")),
                      ("payload 1.5.0 (app 1.5.0-beta.1)", payload("1.5.0", app: "1.5.0-beta.1")),
                      ("a beta's own payload.json (no app)", betaPayload("1.5.0-beta.1")),
                      ("a payload.json without app", betaPayload("1.5.0"))] {
        guard case .failure(.notValid) = accept(d) else { check(false, "\(what), validly signed: refused as not valid"); continue }
        check(payloadFloor(files) == floorBefore && fm.contents(atPath: files.json) == jsonBefore,
              "\(what), validly signed: refused, the floor (\(floorBefore ?? "none")) and the saved copy unchanged")
    }
    check(currentShownVersion(files, own: "1.4.0") == "1.4.1", "the refused betas show nothing")
    guard case .success(let m3) = accept(payload("1.5.0", app: "1.5.0")) else { check(false, "the final 1.5.0 is accepted after the refused beta"); return }
    check(payloadFloor(files) == "1.5.0" && updateNotice(app: m3.app.version, own: "1.4.0") != nil, "the final 1.5.0 is accepted after the refused betas")

    // the sequential vector: carried (shown moves), newer app fetched (stays), replaced (moves)
    var r: ShownVersion? = nil
    r = nextShownVersion(own: "1.4.0", record: r, seen: (payload: "1.4.1", app: "1.4.0"))
    check(r == ShownVersion(shown: "1.4.1", app: "1.4.0") && shownVersion(own: "1.4.0", record: r) == "1.4.1", "sequence: a carried release (1.4.1, app 1.4.0): 1.4.1 shown")
    r = nextShownVersion(own: "1.4.0", record: r, seen: (payload: "1.5.0", app: "1.5.0"))
    check(shownVersion(own: "1.4.0", record: r) == "1.4.1", "sequence: then 1.5.0 (app 1.5.0) fetched, not installed: 1.4.1 still shown")
    check(shownVersion(own: "1.5.0", record: r) == "1.5.0", "sequence: replaced by the 1.5.0 app, before it reads payload.json: its own 1.5.0")
    r = nextShownVersion(own: "1.5.0", record: r, seen: (payload: "1.5.0", app: "1.5.0"))
    check(r == ShownVersion(shown: "1.5.0", app: "1.5.0") && shownVersion(own: "1.5.0", record: r) == "1.5.0", "sequence: the 1.5.0 app reads it: 1.5.0, recorded for app 1.5.0")
    // the same on disk: the 1.5.0 app replaced the 1.4.0 one (whose record says 1.4.1)
    check(currentShownVersion(files, own: "1.5.0") == "1.5.0", "on disk: a record left by the replaced app counts as none")
    check(noteShownVersion(nil, files, own: "1.5.0") == "1.5.0" && record() == nil, "on disk: and is cleared, also with no payload.json to read")
    check(noteShownVersion(savedManifest(files), files, own: "1.5.0") == "1.5.0" && record() == "1.5.0\n1.5.0\n", "on disk: the saved 1.5.0 recorded for app 1.5.0")

    // the install path (Install/Reinstall: fetchManifestChecked, then updateFound): the record
    // moves through updateFound's own step, with no separate call to noteShownVersion
    let installStore = dir + "/install"
    try! fm.createDirectory(atPath: installStore, withIntermediateDirectories: true)
    let installFiles = PayloadFiles(json: installStore + "/payload.json", signature: installStore + "/payload.json.sig",
                                    floor: installStore + "/payload-version", keys: keys, minimum: "1.4.0", shown: installStore + "/shown-version")
    let installed = payload("1.4.1", app: "1.4.0")
    if case .success(let m) = acceptManifest(installed, signature: sign(installed), installFiles) {
        check(currentShownVersion(installFiles, own: "1.4.0") == "1.4.0", "install path: accepted only, nothing shown yet")
        check(manifestSeen(m, installFiles, own: "1.4.0") == nil && currentShownVersion(installFiles, own: "1.4.0") == "1.4.1",
              "install path: payload 1.4.1 carrying app 1.4.0, through updateFound's step: no notice, 1.4.1 shown")
    } else { check(false, "install path: payload 1.4.1 carrying app 1.4.0 is accepted") }
    let announced = payload("1.4.2", app: "1.4.2")
    if case .success(let m) = acceptManifest(announced, signature: sign(announced), installFiles) {
        check(manifestSeen(m, installFiles, own: "1.4.0") != nil && currentShownVersion(installFiles, own: "1.4.0") == "1.4.1",
              "install path: then 1.4.2 (app 1.4.2): the notice, and 1.4.1 still shown")
    } else { check(false, "install path: payload 1.4.2 is accepted") }

    // the rule's edges
    check(nextShownVersion(own: "1.4.0", record: nil, seen: nil) == nil && shownVersion(own: "1.4.0", record: nil) == "1.4.0", "no record: the app's own version")
    let high = ShownVersion(shown: "1.4.3", app: "1.4.0")
    check(nextShownVersion(own: "1.4.0", record: high, seen: (payload: "1.4.1", app: "1.4.0")) == high, "never back: 1.4.1 after 1.4.3 leaves 1.4.3")
    check(nextShownVersion(own: "1.4.0", record: high, seen: (payload: "1.4.4", app: "1.4.0"))?.shown == "1.4.4", "a later carried release moves it on (1.4.4)")
    check(nextShownVersion(own: "1.4.0", record: nil, seen: (payload: "1.3.9", app: "1.4.0")) == nil, "a payload older than the app names nothing to show")
    check(nextShownVersion(own: "1.4.0", record: nil, seen: (payload: "1.4.1-beta.1", app: "1.4.0")) == nil, "a suffixed payload_version is never shown")
    check(nextShownVersion(own: "?", record: nil, seen: (payload: "1.4.1", app: "?")) == nil, "an app without a version of its own records nothing")
    check(ShownVersion(Data("1.4.1\n1.4.0\n".utf8)) == ShownVersion(shown: "1.4.1", app: "1.4.0"), "the record reads back")
    for bad in ["1.4.1\n1.4.0", "1.4.1\n", "1.4.1 1.4.0\n", "1.3.9\n1.4.0\n", "1.4.1-beta.1\n1.4.0\n", "1.4.1\n1.4.0\nx\n", String(repeating: "1", count: 80)] {
        check(ShownVersion(Data(bad.utf8)) == nil, "a record that isn't one is none: \(bad.debugDescription.prefix(30))")
    }
    check(ShownVersion(nil) == nil, "no record file: none")
    // a link at the record's place is replaced, never written through
    try? fm.removeItem(atPath: files.shown)
    let target = dir + "/elsewhere"
    fm.createFile(atPath: target, contents: Data("keep\n".utf8))
    try! fm.createSymbolicLink(atPath: files.shown, withDestinationPath: target)
    check(noteShownVersion(savedManifest(files), files, own: "1.5.0") == "1.5.0" && fileType(files.shown) == S_IFREG
          && fm.contents(atPath: target) == Data("keep\n".utf8), "a link at the record's place is replaced, its target untouched")
    try? fm.removeItem(atPath: files.shown)
    try! fm.createSymbolicLink(atPath: files.shown, withDestinationPath: target)
    check(noteShownVersion(nil, files, own: "1.6.0") == "1.6.0" && fileType(files.shown) == nil && fm.contents(atPath: target) == Data("keep\n".utf8),
          "a link left there by a replaced app is removed, its target untouched")

    // two threads (the update check's fetch on one worker, an action's payload read on another):
    // the record only ever moves up, whichever writes last
    let store2 = dir + "/store2"
    try! fm.createDirectory(atPath: store2, withIntermediateDirectories: true)
    let files2 = PayloadFiles(json: store2 + "/payload.json", signature: store2 + "/payload.json.sig", floor: store2 + "/payload-version",
                              keys: keys, minimum: "1.4.0", shown: store2 + "/shown-version")
    func accept2(_ d: Data) -> Manifest? {
        if case .success(let m) = acceptManifest(d, signature: sign(d), files2) { return m }
        return nil
    }
    guard let low = accept2(payload("1.4.1", app: "1.4.0")), let high = accept2(payload("1.4.3", app: "1.4.0")) else {
        check(false, "two threads: the releases 1.4.1 and 1.4.3 (app 1.4.0) are accepted"); return
    }
    var backwards = 0
    for _ in 0..<300 {
        try? fm.removeItem(atPath: files2.shown)
        DispatchQueue.concurrentPerform(iterations: 2) { i in _ = noteShownVersion(i == 0 ? low : high, files2, own: "1.4.0") }
        if fm.contents(atPath: files2.shown) != Data("1.4.3\n1.4.0\n".utf8) { backwards += 1 }
    }
    check(backwards == 0, "two threads noting 1.4.1 and 1.4.3 at once: 1.4.3 is recorded every time (\(backwards) of 300 went back)")
}
