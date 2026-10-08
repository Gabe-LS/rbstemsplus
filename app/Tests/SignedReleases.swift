// Signed releases (Signing.swift, Payload.swift, scripts/bootstrap.sh, scripts/build.sh), tried
// with cases built to break them. Keys made here (CryptoKit and macOS's own openssl), files in
// the temporary folder: the user's payload.json and the compiled-in keys are never used.
import Foundation
import CryptoKit

func signedReleaseTests() {
    let fm = FileManager.default
    let dir = tmp + "/signing"
    try! fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
    var refusedPayloads: [String] = []
    payloadRefused = { refusedPayloads.append($0) }

    // MARK: the keys: release and backup trusted, another one not
    let releaseKey = P256.Signing.PrivateKey(), backupKey = P256.Signing.PrivateKey(), otherKey = P256.Signing.PrivateKey()
    let b64 = { (k: P256.Signing.PrivateKey) in k.publicKey.derRepresentation.base64EncodedString() }
    let keys = [TrustedKey(name: "release", base64: b64(releaseKey)), TrustedKey(name: "backup", base64: b64(backupKey))].compactMap { $0 }
    check(keys.count == 2, "compiled-in style keys (base64 DER) are read")
    check(TrustedKey(name: "x", base64: "bm90IGEga2V5") == nil && TrustedKey(name: "x", base64: "%%%") == nil, "a key that isn't P-256 DER is refused")
    func sign(_ d: Data, _ k: P256.Signing.PrivateKey) -> Data { try! k.signature(for: d).derRepresentation }

    let sha = String(repeating: "ab", count: 32), modelSha = String(repeating: "cd", count: 32), oldSha = String(repeating: "ef", count: 32)
    func payload(_ v: String, model: String = modelSha) -> Data {
        Data("""
            {"payload_version": "\(v)", "commit": "\(String(repeating: "0", count: 40))",
             "app": {"version": "\(v)", "zip": "rbstemsplus-app.zip", "sha256": "\(sha)"},
             "bridge": {"file": "libonnxruntime.1.18.0.dylib", "sha256": "\(sha)"},
             "model": {"file": "stemsplus-model.onnx", "sha256": "\(model)", "size": 9}, "previous_models": ["\(oldSha)"],
             "compatible": {"rekordbox": ["7.2.19"], "ort_version_prefix": "1.18.", "stems_engine": ["0002"]}}

            """.utf8)
    }
    let p = payload("1.1.0"), good = sign(p, releaseKey)

    // MARK: the signature
    check(signer(of: p, signature: good, keys: keys) == "release", "a signature by the release key is accepted")
    check(signer(of: p, signature: sign(p, backupKey), keys: keys) == "backup", "a signature by the backup key is accepted")
    var flipped = p; flipped[flipped.count / 2] ^= 0x01
    check(signer(of: flipped, signature: good, keys: keys) == nil, "payload.json with one byte flipped is refused")
    var flippedSig = good; flippedSig[flippedSig.count - 3] ^= 0x01
    check(signer(of: p, signature: flippedSig, keys: keys) == nil, "a signature with one byte flipped is refused")
    check(signer(of: p, signature: sign(p, otherKey), keys: keys) == nil, "a signature by another key is refused")
    check(signer(of: p, signature: good.prefix(good.count - 1), keys: keys) == nil, "a truncated signature is refused")
    check(signer(of: p, signature: good.prefix(8), keys: keys) == nil, "a signature cut to 8 bytes is refused")
    check(signer(of: p, signature: Data(), keys: keys) == nil, "an empty signature is refused")
    check(signer(of: p, signature: sign(payload("1.2.0"), releaseKey), keys: keys) == nil, "a signature for another payload.json is refused")
    check(signer(of: p, signature: Data(repeating: 0x30, count: 72), keys: keys) == nil, "garbage as a signature is refused")
    check(signer(of: p, signature: good, keys: []) == nil, "no keys: nothing is accepted")
    check(signer(of: p + Data(" ".utf8), signature: good, keys: keys) == nil, "a byte appended to payload.json is refused")

    // signed by macOS's own openssl (as release.sh does), checked by the app
    let ossl = dir + "/openssl"
    try! fm.createDirectory(atPath: ossl, withIntermediateDirectories: true)
    fm.createFile(atPath: ossl + "/payload.json", contents: p)
    let made = runTool("/usr/bin/openssl", ["ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", ossl + "/key.pem"]).0 == 0
        && runTool("/usr/bin/openssl", ["ec", "-in", ossl + "/key.pem", "-pubout", "-outform", "DER", "-out", ossl + "/pub.der"]).0 == 0
        && runTool("/usr/bin/openssl", ["dgst", "-sha256", "-sign", ossl + "/key.pem", "-out", ossl + "/sig", ossl + "/payload.json"]).0 == 0
    check(made, "openssl made a key and a signature")
    let osslKey = fm.contents(atPath: ossl + "/pub.der").flatMap { TrustedKey(name: "openssl", base64: $0.base64EncodedString()) }
    check(osslKey != nil && signer(of: p, signature: fm.contents(atPath: ossl + "/sig") ?? Data(), keys: keys + [osslKey!]) == "openssl",
          "a signature made by openssl verifies in the app")
    // and the other way: openssl (the bootstrap) checks a signature made with CryptoKit
    fm.createFile(atPath: ossl + "/ck.sig", contents: good)
    fm.createFile(atPath: ossl + "/release.pem", contents: Data(releaseKey.publicKey.pemRepresentation.utf8))
    check(runTool("/usr/bin/openssl", ["dgst", "-sha256", "-verify", ossl + "/release.pem", "-signature", ossl + "/ck.sig", ossl + "/payload.json"]).0 == 0,
          "openssl verifies a signature made with CryptoKit")

    // MARK: the saved payload.json, checked on every read
    let store = dir + "/store"
    try! fm.createDirectory(atPath: store, withIntermediateDirectories: true)
    let files = PayloadFiles(json: store + "/payload.json", signature: store + "/payload.json.sig", floor: store + "/payload-version", keys: keys, minimum: "1.0.0")
    func saved() -> Manifest? { savedManifest(files) }
    func accepted(_ d: Data, _ s: Data) -> PayloadRefusal? {
        switch acceptManifest(d, signature: s, files) { case .success: return nil; case .failure(let why): return why }
    }
    check(saved() == nil && refusedPayloads.isEmpty, "no payload.json saved: none, and nothing to report")
    // 1.0 saved payload.json without a signature: ignored (and fetched again)
    fm.createFile(atPath: files.json, contents: p)
    check(saved() == nil && refusedPayloads.last?.contains("no signature") == true, "an unsigned saved payload.json (1.0's) is ignored, and the log says why")
    check(ourModelChecksums(saved()) == builtInModels, "ours: an unsigned saved payload.json adds nothing")
    // a download that doesn't verify changes nothing
    for (name, sig) in [("another key's", sign(p, otherKey)), ("a truncated", good.prefix(20)), ("an empty", Data())] {
        check(accepted(p, sig) != nil && fm.contents(atPath: files.json) == p && !fm.fileExists(atPath: files.signature) && !fm.fileExists(atPath: files.floor),
              "a download with \(name) signature is refused and saves nothing")
    }
    // a good one
    check(accepted(p, good) == nil, "a signed payload.json is accepted")
    check(fm.contents(atPath: files.json) == p && fm.contents(atPath: files.signature) == good, "it is saved with its signature next to it")
    check(payloadFloor(files) == "1.1.0", "its version is the newest accepted")
    let m = saved()
    check(m?.payloadVersion == "1.1.0" && m?.signedBy == "release", "the saved payload.json verifies when read (release key)")
    check(ourModelChecksums(m) == builtInModels.union([modelSha, oldSha]), "ours: a verified payload.json's model and previous_models count")
    check(ourModelChecksums(decodeManifest(p)) == builtInModels, "ours: the same payload.json, not verified, adds nothing")
    // changed after the download
    fm.createFile(atPath: files.json, contents: p + Data(" ".utf8))
    check(saved() == nil && refusedPayloads.last?.contains("doesn't verify") == true, "a saved payload.json changed after the download is refused on read")
    check(ourModelChecksums(saved()) == builtInModels, "ours: a changed saved payload.json adds nothing")
    fm.createFile(atPath: files.json, contents: payload("1.1.0", model: String(repeating: "12", count: 32)))
    check(saved() == nil, "a saved payload.json naming another model is refused on read")
    fm.createFile(atPath: files.json, contents: p)
    check(saved()?.signedBy == "release", "put back as downloaded, it verifies again")
    try! fm.removeItem(atPath: files.signature)
    check(saved() == nil, "a saved payload.json whose signature is gone is refused")
    fm.createFile(atPath: files.signature, contents: Data())
    check(saved() == nil, "a saved payload.json with an empty signature is refused")
    fm.createFile(atPath: files.signature, contents: sign(payload("1.2.0"), releaseKey))
    check(saved() == nil, "a saved payload.json with another payload's signature is refused")
    fm.createFile(atPath: files.signature, contents: good)
    check(saved() != nil, "with its own signature back, it verifies")
    // reads run concurrently with no shared state to break
    var concurrent = [Bool](repeating: false, count: 8)
    let lock = NSLock()
    DispatchQueue.concurrentPerform(iterations: 8) { i in let ok = savedManifest(files) != nil; lock.lock(); concurrent[i] = ok; lock.unlock() }
    check(!concurrent.contains(false), "concurrent reads all verify")

    // MARK: no going back to an older payload
    let newer = payload("1.2.0")
    check(accepted(newer, sign(newer, backupKey)) == nil && payloadFloor(files) == "1.2.0", "a newer payload.json (backup key) is accepted and raises the newest accepted")
    check(accepted(p, good) == .older("1.1.0", than: "1.2.0"), "an older payload.json, validly signed, is refused")
    check(fm.contents(atPath: files.json) == newer, "and the newer one stays saved")
    check(accepted(newer, sign(newer, releaseKey)) == nil, "the same version again is accepted")
    check(accepted(payload("1.10.0"), sign(payload("1.10.0"), releaseKey)) == nil && payloadFloor(files) == "1.10.0", "1.10.0 is newer than 1.2.0")
    check(accepted(newer, sign(newer, releaseKey)) == .older("1.2.0", than: "1.10.0"), "1.2.0 is older than 1.10.0")
    // the saved pair swapped for an older signed pair (behind the app's back)
    fm.createFile(atPath: files.json, contents: p); fm.createFile(atPath: files.signature, contents: good)
    check(saved() == nil && refusedPayloads.last?.contains("older") == true, "an older signed pair put in place is refused on read")
    // the documented limit: a process of this account can delete the record
    try! fm.removeItem(atPath: files.floor)
    check(saved()?.payloadVersion == "1.1.0", "(without the record of the newest accepted, an older signed one above the app's own version is read again)")
    fm.createFile(atPath: files.floor, contents: Data("not a version!\n".utf8))
    check(payloadFloor(files) == nil, "a record that isn't a version is ignored")

    // MARK: the app's own version is the oldest payload it accepts, also with no record at all
    let version = (try? String(contentsOfFile: fm.currentDirectoryPath + "/VERSION", encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines)
    check(compiledVersion == version && PayloadFiles().minimum == compiledVersion && payloadMinimum == compiledVersion,
          "VERSION (\(version ?? "?")) is compiled in as the oldest payload the app accepts")
    let low = dir + "/minimum"
    try! fm.createDirectory(atPath: low, withIntermediateDirectories: true)
    let floored = PayloadFiles(json: low + "/payload.json", signature: low + "/payload.json.sig", floor: low + "/payload-version", keys: keys, minimum: "1.2.0")
    check(payloadFloor(floored) == nil && acceptedFloor(floored) == "1.2.0", "no payload accepted yet: the app's own version is the floor")
    if case .failure(.older("1.1.0", than: "1.2.0")) = acceptManifest(p, signature: good, floored) {
        check(!fm.fileExists(atPath: floored.json) && !fm.fileExists(atPath: floored.floor), "a signed payload older than the app, never any accepted: refused, nothing saved")
    } else { check(false, "a signed payload older than the app, never any accepted: refused, nothing saved") }
    check((try? acceptManifest(newer, signature: sign(newer, releaseKey), floored).get())?.payloadVersion == "1.2.0", "the app's own version is accepted")
    check((try? acceptManifest(payload("1.10.0"), signature: sign(payload("1.10.0"), releaseKey), floored).get()) != nil && acceptedFloor(floored) == "1.10.0",
          "a newer one is accepted and raises the floor above the app's version")
    fm.createFile(atPath: floored.json, contents: p); fm.createFile(atPath: floored.signature, contents: good)
    try? fm.removeItem(atPath: floored.floor)
    check(savedManifest(floored) == nil && refusedPayloads.last?.contains("older than 1.2.0") == true,
          "an older signed pair put back with the record deleted: still refused (the app's version)")
    check(acceptedFloor(PayloadFiles(floor: low + "/none", minimum: "")) == nil, "(no minimum and no record: no floor)")

    // MARK: the bootstrap's own check (its verify_payload, run alone, with macOS's openssl)
    let bootstrap = fm.currentDirectoryPath + "/scripts/bootstrap.sh"
    let lines = ((try? String(contentsOfFile: bootstrap, encoding: .utf8)) ?? "").components(separatedBy: "\n")
    /// One of the bootstrap's functions, as its text, to run alone.
    func bootstrapFunction(_ name: String) -> String {
        let start = lines.firstIndex(of: name + "() {")
        let end = start.flatMap { s in lines[s...].firstIndex(of: "}") }
        check(start != nil && end != nil, "scripts/bootstrap.sh has \(name)")
        return start.flatMap { s in end.map { lines[s...$0].joined(separator: "\n") } } ?? "false"
    }
    let function = bootstrapFunction("verify_payload")
    check(lines.contains("SIGNING_KEYS=\"\""), "scripts/bootstrap.sh has its one empty SIGNING_KEYS line for build.sh")
    check(lines.filter { $0 == "PAYLOAD_MINIMUM=\"\"" }.count == 1, "scripts/bootstrap.sh has its one empty PAYLOAD_MINIMUM line for build.sh")
    let bs = dir + "/bootstrap"
    try! fm.createDirectory(atPath: bs, withIntermediateDirectories: true)
    func bootstrapAccepts(_ d: Data, _ sig: Data?, _ keys: [String]) -> Bool {
        let work = bs + "/\(UUID().uuidString)"
        try! fm.createDirectory(atPath: work, withIntermediateDirectories: true)
        fm.createFile(atPath: work + "/payload.json", contents: d)
        if let s = sig { fm.createFile(atPath: work + "/payload.json.sig", contents: s) }
        let r = runTool("/bin/bash", ["-c", "set -euo pipefail\n" + function + "\nverify_payload \"$@\"", "rbsp",
                                      work + "/payload.json", work + "/payload.json.sig", work] + keys)
        return r.0 == 0
    }
    let two = [b64(releaseKey), b64(backupKey)]
    check(bootstrapAccepts(p, good, two), "bootstrap: a signature by the release key is accepted")
    check(bootstrapAccepts(p, sign(p, backupKey), two), "bootstrap: a signature by the backup key is accepted")
    check(bootstrapAccepts(p, fm.contents(atPath: ossl + "/sig"), [fm.contents(atPath: ossl + "/pub.der")!.base64EncodedString()]),
          "bootstrap: a signature made by openssl is accepted with its key")
    check(!bootstrapAccepts(flipped, good, two), "bootstrap: payload.json with one byte flipped is refused")
    check(!bootstrapAccepts(p, flippedSig, two), "bootstrap: a signature with one byte flipped is refused")
    check(!bootstrapAccepts(p, sign(p, otherKey), two), "bootstrap: a signature by another key is refused")
    check(!bootstrapAccepts(p, good.prefix(good.count - 1), two), "bootstrap: a truncated signature is refused")
    check(!bootstrapAccepts(p, Data(), two), "bootstrap: an empty signature is refused")
    check(!bootstrapAccepts(p, nil, two), "bootstrap: a missing signature is refused")
    check(!bootstrapAccepts(p, sign(newer, releaseKey), two), "bootstrap: a signature for another payload.json is refused")
    check(!bootstrapAccepts(p, good, []), "bootstrap: no keys, nothing accepted")
    check(!bootstrapAccepts(p, good, ["not a key", "-----BEGIN"]), "bootstrap: keys that aren't keys accept nothing")
    check(!bootstrapAccepts(p, good, [b64(otherKey)]), "bootstrap: only another key: refused")

    // MARK: a key given twice (JSONDecoder takes the first, plutil the last), in the app and the bootstrap
    let plainJSON = bootstrapFunction("plain_json")
    func bootstrapPlain(_ d: Data) -> Bool {
        let file = bs + "/\(UUID().uuidString).json"
        fm.createFile(atPath: file, contents: d)
        return runTool("/bin/bash", ["-c", "set -euo pipefail\n" + plainJSON + "\nplain_json \"$1\"", "rbsp", file]).0 == 0
    }
    let text = String(decoding: p, as: UTF8.self)
    func edited(_ from: String, _ to: String) -> Data {
        check(text.components(separatedBy: from).count == 2, "(the test payload has \(from.debugDescription) once)")
        return Data(text.replacingOccurrences(of: from, with: to).utf8)
    }
    let evilApp = "\"app\": {\"version\": \"9.9.9\", \"zip\": \"evil.zip\", \"sha256\": \"\(oldSha)\"}"
    let refusedJSON: [(String, Data)] = [
        ("\"app\" twice at the top", edited("{\"payload_version\"", "{" + evilApp + ", \"payload_version\"")),
        ("\"app\" twice, the second last", edited("\"previous_models\"", evilApp + ", \"previous_models\"")),
        ("\"payload_version\" twice", edited("\"commit\"", "\"payload_version\": \"9.9.9\", \"commit\"")),
        ("\"file\" twice in bridge", edited("\"bridge\": {\"file\"", "\"bridge\": {\"file\": \"evil.dylib\", \"file\"")),
        ("\"stems_engine\" twice in compatible", edited("\"stems_engine\"", "\"stems_engine\": [\"9999\"], \"stems_engine\"")),
        ("a key twice in an unknown object", edited("\"commit\"", "\"future\": {\"a\": 1, \"a\": 2}, \"commit\"")),
        ("a key twice in an object in an array", edited("\"commit\"", "\"future\": [{\"a\": 1}, {\"b\": [{\"c\": 1, \"c\": 1}]}], \"commit\"")),
        ("\"app\" twice, once written with an escape", edited("\"previous_models\"", evilApp.replacingOccurrences(of: "\"app\"", with: "\"\\u0061pp\"") + ", \"previous_models\"")),
        ("a key with an escape", edited("\"commit\"", "\"fu\\\"ture\": 1, \"commit\"")),
        ("a key outside ASCII", edited("\"commit\"", "\"caf\u{e9}\": 1, \"commit\"")),
        ("an empty key", edited("\"commit\"", "\"\": 1, \"commit\"")),
        ("two objects", p + Data("{}".utf8)),
        ("something after the object", p + Data("x".utf8)),
        ("an array, not an object", Data(("[" + text + "]").utf8)),
        ("an unfinished string", Data("{\"payload_version\": \"1.1.0".utf8)),
        ("an unfinished object", Data(text.dropLast(3).utf8)),
        ("nothing", Data()),
        ("a byte-order mark", Data([0xef, 0xbb, 0xbf]) + p),
    ]
    for (what, d) in refusedJSON {
        check(!plainJSONKeys(d) && decodeManifest(d) == nil, "app: payload.json with \(what) is refused")
        check(!bootstrapPlain(d), "bootstrap: payload.json with \(what) is refused")
    }
    let dupSigned = refusedJSON[0].1
    if case .failure(.notValid) = checkManifest(dupSigned, signature: sign(dupSigned, releaseKey), keys: keys, floor: nil) {
        check(true, "app: a validly signed payload.json with \"app\" twice is still refused")
    } else { check(false, "app: a validly signed payload.json with \"app\" twice is still refused") }
    let acceptedJSON: [(String, Data)] = [
        ("as built", p),
        ("an unknown key (a later release's)", edited("\"commit\"", "\"future\": {\"a\": [1, -2.5e3, true, false, null, {\"b\": \"x\"}]}, \"commit\"")),
        ("escapes in a value", edited("\"commit\": \"", "\"note\": \"a \\\"quoted\\\" \\\\ \\u00e9\", \"commit\": \"")),
        ("the same key in different objects", edited("\"commit\"", "\"future\": [{\"a\": 1}, {\"a\": 2}], \"commit\"")),
        ("other spacing", Data(text.replacingOccurrences(of: "\n", with: "\r\n").replacingOccurrences(of: ": ", with: ":\t").utf8)),
    ]
    for (what, d) in acceptedJSON {
        check(plainJSONKeys(d) && decodeManifest(d)?.payloadVersion == "1.1.0", "app: payload.json with \(what) is accepted")
        check(bootstrapPlain(d), "bootstrap: payload.json with \(what) is accepted")
    }
    check(decodeManifest(edited("\"compatible\": {\"rekordbox\"", "\"compatible\": {\"ortVersionPrefix\": \"9.\", \"rekordbox\""))?.compatible.ortVersionPrefix == "1.18.",
          "app: keys are read exactly as written (\"ortVersionPrefix\" is not \"ort_version_prefix\")")

    // MARK: the bootstrap's own version is the oldest payload it installs
    let olderVersion = bootstrapFunction("older_version")
    func bootstrapOlder(_ a: String, _ b: String) -> Bool {
        runTool("/bin/bash", ["-c", "set -euo pipefail\n" + olderVersion + "\nolder_version \"$1\" \"$2\"", "rbsp", a, b]).0 == 0
    }
    for (a, b, older) in [("1.0.9", "1.0.10", true), ("1.0.10", "1.0.9", false), ("1.1.0", "1.1.0", false), ("1.1.0", "1.2.0", true),
                          ("2.0.0", "1.9.9", false), ("1.1", "1.1.0", false), ("1.1.0", "1.1", false), ("1.0.0", "1.0.1", true),
                          ("08.09.10", "8.9.9", false), ("0.9.9", "1.0.0", true)] {
        check(bootstrapOlder(a, b) == older, "bootstrap: \(a) is \(older ? "" : "not ")older than \(b)")
        // (releases are x.y.z, which build.sh checks; with another number of parts they may differ)
        if a.split(separator: ".").count == b.split(separator: ".").count {
            check(bootstrapOlder(a, b) == olderPayload(a, than: b), "bootstrap and app agree on \(a) and \(b)")
        }
    }
    for bad in ["", "1.0.0 ", "v1.0.0", "1..0", "1.0.0-beta", "a[$(touch \(bs)/ran)]", "1.0.0\n2", "-1"] {
        check(bootstrapOlder(bad, "1.0.0") && bootstrapOlder("1.0.0", bad), "bootstrap: \(bad.debugDescription) isn't a version: refused either way")
    }
    check(!fm.fileExists(atPath: bs + "/ran"), "bootstrap: a version is never run as code")

    // the places it replaces: only a real app folder with our bundle ID
    let isOurs = bootstrapFunction("is_ours")
    func bootstrapOwns(_ path: String) -> Bool {
        runTool("/bin/bash", ["-c", "set -euo pipefail\nBUNDLE_ID=io.github.rbstemsplus.app\n" + isOurs + "\nis_ours \"$1\"", "rbsp", path]).0 == 0
    }
    func app(_ path: String, id: String?) {
        try! fm.createDirectory(atPath: path + "/Contents", withIntermediateDirectories: true)
        if let id = id { try! (["CFBundleIdentifier": id] as NSDictionary).write(to: URL(fileURLWithPath: path + "/Contents/Info.plist")) }
    }
    app(bs + "/ours.app", id: bundleID); app(bs + "/theirs.app", id: "com.example.other"); app(bs + "/bare.app", id: nil)
    symlink(bs + "/ours.app", bs + "/link.app")
    fm.createFile(atPath: bs + "/file.app", contents: Data())
    check(bootstrapOwns(bs + "/ours.app"), "bootstrap: a real app folder with our bundle ID may be replaced")
    check(!bootstrapOwns(bs + "/link.app"), "bootstrap: a link to our app is left alone")
    check(!bootstrapOwns(bs + "/theirs.app"), "bootstrap: another app is left alone")
    check(!bootstrapOwns(bs + "/bare.app"), "bootstrap: a folder without Info.plist is left alone")
    check(!bootstrapOwns(bs + "/file.app"), "bootstrap: a file is left alone")
    check(!bootstrapOwns(bs + "/missing.app"), "bootstrap: nothing there isn't ours")
    // MARK: the busy marker: "<pid>\n<start>\n[<action>\n<label>\n]" (State.swift), its writer by pid and start time
    let busyFunctions = ["started", "check_busy", "mark_busy", "unmark_busy"].map(bootstrapFunction).joined(separator: "\n")
    let marker = bs + "/busy"
    func bootstrapBash(_ code: String, _ args: [String] = []) -> (Int32, String) {
        runTool("/bin/bash", ["-c", "set -euo pipefail\nBUSY=\"$1\"; shift; MARKED=0; OWN_START=; SAVED_BUSY=\nfail() { echo \"$*\"; exit 1; }\n"
                                    + busyFunctions + "\n" + code, "rbsp", marker] + args)
    }
    func bootstrapBusy() -> Bool { let r = bootstrapBash("check_busy"); return r.0 != 0 && r.1.contains("changing rekordbox or updating itself") }
    let myStart = kernelStart(getpid())
    check(myStart != nil && bootstrapBash("started \"$1\"", [String(getpid())]).1 == String(myStart!),
          "bootstrap: a process's start time is the kernel's p_starttime, as the app records it (\(myStart.map(String.init) ?? "?"))")
    check(bootstrapBash("started 99999999").1.isEmpty, "bootstrap: no start time for a process that doesn't exist")
    func writeMarker(_ s: String, age: TimeInterval = 0) {
        fm.createFile(atPath: marker, contents: Data(s.utf8))
        if age > 0 { try! fm.setAttributes([.modificationDate: Date().addingTimeInterval(-age)], ofItemAtPath: marker) }
    }
    try? fm.removeItem(atPath: marker)
    check(!bootstrapBusy(), "bootstrap: no busy marker, it goes on")
    let start = myStart.map(String.init) ?? "0"
    writeMarker("\(getpid())\n\(start)\ninstall\nInstall Demucs v4\n")
    check(bootstrapBusy(), "bootstrap: the app's marker, its writer running: stops")
    writeMarker("\(getpid())\n\(start)\n")
    check(bootstrapBusy(), "bootstrap: the update's marker (pid and start), its writer running: stops")
    writeMarker("\(getpid())\n\((myStart ?? 0) - 100)\ninstall\nInstall Demucs v4\n")
    check(!bootstrapBusy(), "bootstrap: a marker whose pid is now another process's (another start time): goes on")
    writeMarker("\(getpid())\ninstall\nInstall Stems Plus\n")
    check(bootstrapBusy(), "bootstrap: 1.0's marker (no start time), its writer running: stops")
    writeMarker("\(getpid())\n", age: Date().timeIntervalSince1970 - Double(myStart ?? 0) + 3600)
    check(!bootstrapBusy(), "bootstrap: 1.0's marker written before its pid's process started (the pid used again): goes on")
    let gone = Process(); gone.executableURL = URL(fileURLWithPath: "/usr/bin/true"); try! gone.run(); gone.waitUntilExit()
    writeMarker("\(gone.processIdentifier)\n\(start)\ninstall\n")
    check(!bootstrapBusy(), "bootstrap: a marker left by a process that has gone: goes on")
    writeMarker("not a pid\n")
    check(!bootstrapBusy(), "bootstrap: a marker without a pid: goes on")
    // the bootstrap's own marker while it replaces the app, and what was there before afterwards
    let during = bs + "/busy-during"
    func markRun(_ before: String?, replaceWith: String? = nil) -> (pid: String, during: String, after: String?) {
        try? fm.removeItem(atPath: marker); try? fm.removeItem(atPath: during)
        if let b = before { fm.createFile(atPath: marker, contents: Data(b.utf8)) }
        let r = bootstrapBash("mark_busy; cp \"$BUSY\" \"$1\"; [ -z \"${2-}\" ] || printf '%s' \"$2\" > \"$BUSY\"; unmark_busy; printf '%s' \"$$\"",
                              [during] + (replaceWith.map { [$0] } ?? []))
        return (r.1, (try? String(contentsOfFile: during, encoding: .utf8)) ?? "", try? String(contentsOfFile: marker, encoding: .utf8))
    }
    func liveMarker(_ text: String, pid: String) -> Bool {
        let l = text.components(separatedBy: "\n")
        return l.count >= 3 && l[0] == pid && Int(l[1]).map { abs($0 - Int(Date().timeIntervalSince1970)) < 120 } == true
    }
    var mk = markRun(nil)
    check(liveMarker(mk.during, pid: mk.pid) && mk.during.components(separatedBy: "\n").count == 3 && mk.after == nil,
          "bootstrap: its marker is its pid and start time while it works, and is removed afterwards")
    let leftAction = "\(gone.processIdentifier)\n12345\nuninstall-all\nUninstall RB Stems Plus completely\n"
    mk = markRun(leftAction)
    check(liveMarker(mk.during, pid: mk.pid) && mk.during.hasSuffix("\nuninstall-all\nUninstall RB Stems Plus completely\n") && mk.after == leftAction,
          "bootstrap: an action the app left half-done stays in its marker, and the app's marker is put back exactly")
    let oldAction = "\(gone.processIdentifier)\ninstall\nInstall Stems Plus\n"
    mk = markRun(oldAction)
    check(mk.during.hasSuffix("\ninstall\nInstall Stems Plus\n") && mk.after == oldAction, "bootstrap: the same with 1.0's marker")
    mk = markRun(nil, replaceWith: "4242\n1\nreinstall\nReinstall\n")
    check(mk.after == "4242\n1\nreinstall\nReinstall\n", "bootstrap: a marker someone else wrote meanwhile is left alone")

    // MARK: a release is built only from a commit (build.sh, in a scratch repository)
    let repo = dir + "/repo"
    try! fm.createDirectory(atPath: repo + "/scripts", withIntermediateDirectories: true)
    try! fm.copyItem(atPath: fm.currentDirectoryPath + "/scripts/build.sh", toPath: repo + "/scripts/build.sh")
    fm.createFile(atPath: repo + "/VERSION", contents: Data("9.9.9\n".utf8))
    func git(_ a: [String]) -> Bool { runTool("/usr/bin/git", ["-C", repo, "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"] + a).0 == 0 }
    check(git(["init", "-q"]) && git(["add", "-A"]) && git(["commit", "-q", "-m", "x"]), "a scratch repository with build.sh")
    func build(_ args: [String] = []) -> (Int32, String) { runTool("/bin/bash", [repo + "/scripts/build.sh"] + args) }
    var r = build()
    check(r.0 != 0 && r.1.contains("keys/release.pub.pem is missing"), "a clean tree gets past the commit check (then stops: no keys): \(r.1)")
    fm.createFile(atPath: repo + "/untracked", contents: Data())
    r = build()
    check(r.0 != 0 && r.1.contains("uncommitted or untracked"), "an untracked file stops a release")
    try! fm.removeItem(atPath: repo + "/untracked")
    fm.createFile(atPath: repo + "/VERSION", contents: Data("9.9.8\n".utf8))
    r = build()
    check(r.0 != 0 && r.1.contains("uncommitted or untracked"), "an uncommitted change stops a release")
    check(git(["add", "-A"]), "staged")
    r = build()
    check(r.0 != 0 && r.1.contains("uncommitted or untracked"), "a staged, uncommitted change stops a release")
    // (runTool's children get a clean environment: the variable goes through env)
    r = runTool("/usr/bin/env", ["RBSTEMSPLUS_TEST_KEY=" + dir + "/x.pem", "/bin/bash", repo + "/scripts/build.sh"])
    check(r.0 != 0 && r.1.contains("RBSTEMSPLUS_TEST_KEY"), "RBSTEMSPLUS_TEST_KEY in the environment stops a release")

    // MARK: releases are built here, never by GitHub; scripts/release.sh stops before the network,
    // a key or a prompt when something here is wrong
    check(!fm.fileExists(atPath: fm.currentDirectoryPath + "/.github/workflows/release.yml"), "no GitHub workflow builds or uploads a release")
    try! fm.copyItem(atPath: fm.currentDirectoryPath + "/scripts/release.sh", toPath: repo + "/scripts/release.sh")
    func release(_ args: [String]) -> (Int32, String) { runTool("/bin/bash", [repo + "/scripts/release.sh"] + args) }
    r = release([])
    check(r.0 == 2 && r.1.contains("usage"), "release.sh without a tag: usage")
    r = release(["9.9.8"])
    check(r.0 != 0 && r.1.contains("vX.Y.Z"), "release.sh: a tag that isn't vX.Y.Z is refused")
    r = release(["v9.9.8", "--skip-bridge-rebuild"])
    check(r.0 == 2 && r.1.contains("usage"), "release.sh: an unknown option is refused")
    r = release(["v9.9.8"])
    check(r.0 != 0 && r.1.contains("uncommitted or untracked"), "release.sh: an uncommitted tree is refused: \(r.1)")
    check(git(["add", "-A"]) && git(["commit", "-q", "-m", "y"]), "(committed)")
    r = release(["v9.9.7"])
    check(r.0 != 0 && r.1.contains("VERSION says 9.9.8"), "release.sh: a tag that isn't VERSION is refused")
    let keyFile = dir + "/not-a-real-key.pem"
    r = release(["v9.9.8", "--key", keyFile])
    check(r.0 != 0 && r.1.contains("no signing key"), "release.sh: no key file: refused")
    fm.createFile(atPath: keyFile, contents: Data("not a key\n".utf8), attributes: [.posixPermissions: 0o644])
    r = release(["v9.9.8", "--key", keyFile])
    check(r.0 != 0 && r.1.contains("chmod 600"), "release.sh: a key file others can read: refused")
    chmod(keyFile, 0o600)
    r = release(["v9.9.8", "--key", keyFile])
    check(r.0 != 0 && r.1.contains("in Terminal"), "release.sh: not in Terminal: refused before the network, the build or a prompt")
    check(runTool("/usr/bin/git", ["-C", repo, "tag", "-l"]).1.isEmpty, "release.sh: no tag made by any of these")
}

/// When process `pid` started, in whole seconds since 1970, from the kernel (kinfo_proc's
/// p_starttime): what the busy marker records. nil if there is no such process.
private func kernelStart(_ pid: pid_t) -> Int? {
    var info = kinfo_proc()
    var size = MemoryLayout<kinfo_proc>.stride
    var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, pid]
    guard sysctl(&mib, u_int(mib.count), &info, &size, nil, 0) == 0, size > 0 else { return nil }
    return Int(info.kp_proc.p_starttime.tv_sec)
}
