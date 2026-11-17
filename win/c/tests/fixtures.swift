// The shared payload and signature fixtures (app.md §7, W1): today's Swift (app/Sources, built
// without main.swift) runs the SignedReleases cases and more, and writes what Signing.swift and
// Payload.swift decide into a text file that win/c/tests reads, so the Windows copy must decide
// the same on the same bytes. Keys are made here each run (CryptoKit, and macOS's own openssl
// for one): nothing reads the user's payload.json or the compiled-in keys; files go only to the
// scratch folder given (under build/).
//
// One record per line, fields separated by one space; every string field is hex (its UTF-8
// bytes), "-" for nil and "=" for an empty one:
//   key NAME B64                      a key the cases name (NAME, B64 hex)
//   keytext DESC TEXT OK              TrustedKey(name:base64: TEXT) != nil
//   sig DESC DATA SIG KEYS SIGNER     signer(of: DATA, signature: SIG, keys: KEYS) (KEYS: names joined by ",")
//   json DESC DATA PLAIN VERSION MODELFILE MODELSHA MODELSIZE PREVIOUS
//                                     plainJSONKeys(DATA), decodeManifest(DATA)?: its payloadVersion,
//                                     model (size in decimal) and previousModels joined by ","
//                                     ("-" each when it doesn't decode; PREVIOUS "-" when nil)
//   check DESC DATA SIG KEYS FLOOR R  checkManifest: ok:NAME, noSignature, badSignature, notValid, older
//   release S OK                      isReleaseVersion(S)
//   older A B OK                      olderPayload(A, than: B), for x.y.z pairs
//   floor TEXT RESULT                 payloadFloor() of a record holding TEXT
//   accepted SAVED MINIMUM RESULT     acceptedFloor() with that record and minimum
//   shownrec DATA SHOWN APP           ShownVersion(DATA): its shown and app ("-" when nil)
//   shown OWN RS RA SP SA S A         nextShownVersion(own: OWN, record: (RS, RA), seen: (SP, SA)): (S, A)
//   failure TEXT KNOWN                updateFailureReason(TEXT) != nil (the oracle's copy, below)
//   configw TEXT OP READABLE AFTER    config.ini holding TEXT ("-": none), writeConfigValues with
//                                     configOps[OP]: whether it was readable, the file after ("-":
//                                     not written)
//   configsets TEXT KEY OK            configSets(KEY) of config.ini holding TEXT
import Foundation
import CryptoKit

var out = "# win/c/tests/fixtures.swift's output (gen-fixtures.sh): what today's Swift decides. Do not edit.\n"
func hex(_ d: Data) -> String { d.isEmpty ? "=" : d.map { String(format: "%02x", $0) }.joined() }
func hex(_ s: String?) -> String { s.map { hex(Data($0.utf8)) } ?? "-" }
func hex(_ d: Data?) -> String { d.map { hex($0) } ?? "-" }
/// a file's bytes, written in place (Foundation's atomic write goes through a folder outside build/)
func put(_ path: String, _ d: Data) { try! d.write(to: URL(fileURLWithPath: path)) }
func line(_ f: [String]) { out += f.joined(separator: " ") + "\n" }

precondition(CommandLine.arguments.count == 2, "usage: gen SCRATCH")
let scratch = CommandLine.arguments[1]   // a scratch folder under build/ (gen-fixtures.sh)
try! FileManager.default.createDirectory(atPath: scratch, withIntermediateDirectories: true)

// MARK: keys

let releaseKey = P256.Signing.PrivateKey(), backupKey = P256.Signing.PrivateKey(), otherKey = P256.Signing.PrivateKey()
let b64 = { (k: P256.Signing.PrivateKey) in k.publicKey.derRepresentation.base64EncodedString() }
var keyByName: [String: TrustedKey] = [:]
func addKey(_ name: String, _ text: String) {
    keyByName[name] = TrustedKey(name: name, base64: text)!
    line(["key", hex(name), hex(text)])
}
addKey("release", b64(releaseKey)); addKey("backup", b64(backupKey)); addKey("other", b64(otherKey))

// a key and a signature made by openssl, as release.sh signs
let ossl = scratch + "/openssl"
try! FileManager.default.createDirectory(atPath: ossl, withIntermediateDirectories: true)
let made = runTool("/usr/bin/openssl", ["ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", ossl + "/key.pem"]).0 == 0
    && runTool("/usr/bin/openssl", ["ec", "-in", ossl + "/key.pem", "-pubout", "-outform", "DER", "-out", ossl + "/pub.der"]).0 == 0
precondition(made, "openssl made no key")
addKey("openssl", FileManager.default.contents(atPath: ossl + "/pub.der")!.base64EncodedString())

func keys(_ names: [String]) -> [TrustedKey] { names.map { keyByName[$0]! } }
func sign(_ d: Data, _ k: P256.Signing.PrivateKey) -> Data { try! k.signature(for: d).derRepresentation }
func osslSign(_ d: Data) -> Data {
    put(ossl + "/in", d)
    let r = runTool("/usr/bin/openssl", ["dgst", "-sha256", "-sign", ossl + "/key.pem", "-out", ossl + "/sig", ossl + "/in"])
    precondition(r.0 == 0, "openssl didn't sign: \(r.1)")
    return FileManager.default.contents(atPath: ossl + "/sig")!
}

// MARK: TrustedKey from its text

let rk = b64(releaseKey)
for (desc, text) in [("the release key", rk), ("not a key (base64)", "bm90IGEga2V5"), ("not base64", "%%%"), ("empty", ""),
                     ("a newline inside", String(rk.prefix(20)) + "\n" + String(rk.dropFirst(20))),
                     ("a space at the end", rk + " "), ("padding dropped", rk.replacingOccurrences(of: "=", with: "")),
                     ("one character short", String(rk.dropLast())), ("one character more", rk + "A"),
                     ("the raw x9.63 point", releaseKey.publicKey.x963Representation.base64EncodedString()),
                     ("the raw point without 04", releaseKey.publicKey.rawRepresentation.base64EncodedString()),
                     ("PEM with its lines", releaseKey.publicKey.pemRepresentation),
                     ("URL-safe alphabet", rk.replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_")),
                     ("the DER with a byte flipped in the OID", { var d = releaseKey.publicKey.derRepresentation; d[8] ^= 1; return d.base64EncodedString() }()),
                     ("the DER with a byte appended", (releaseKey.publicKey.derRepresentation + Data([0])).base64EncodedString())] {
    line(["keytext", hex(desc), hex(text), TrustedKey(name: "x", base64: text) != nil ? "1" : "0"])
}

// MARK: payloads, as build.sh writes them (SignedReleases.swift's payload())

let sha = String(repeating: "ab", count: 32), modelSha = String(repeating: "cd", count: 32), oldSha = String(repeating: "ef", count: 32)
func payload(_ v: String, model: String = modelSha, size: String = "9", extra: String = "") -> Data {
    Data("""
        {"payload_version": "\(v)", "commit": "\(String(repeating: "0", count: 40))",
         "app": {"version": "\(v)", "zip": "rbstemsplus-app.zip", "sha256": "\(sha)"},
         "bridge": {"file": "libonnxruntime.1.18.0.dylib", "sha256": "\(sha)"},
         "model": {"file": "stemsplus-model.onnx", "sha256": "\(model)", "size": \(size)}, "previous_models": ["\(oldSha)"],
         "compatible": {"rekordbox": ["7.2.19"], "ort_version_prefix": "1.18.", "stems_engine": ["0002"]}\(extra)}

        """.utf8)
}
let p = payload("1.1.0"), good = sign(p, releaseKey)

// MARK: signatures

func sigCase(_ desc: String, _ data: Data, _ sig: Data?, _ names: [String]) {
    let who = sig.flatMap { signer(of: data, signature: $0, keys: keys(names)) }
    line(["sig", hex(desc), hex(data), hex(sig), hex(names.joined(separator: ",")), hex(who)])
}
let two = ["release", "backup"]
sigCase("by the release key", p, good, two)
sigCase("by the backup key", p, sign(p, backupKey), two)
var flipped = p; flipped[flipped.count / 2] ^= 0x01
sigCase("payload.json with one byte flipped", flipped, good, two)
var flippedSig = good; flippedSig[flippedSig.count - 3] ^= 0x01
sigCase("a signature with one byte flipped", p, flippedSig, two)
sigCase("by another key", p, sign(p, otherKey), two)
sigCase("truncated by one byte", p, good.prefix(good.count - 1), two)
sigCase("cut to 8 bytes", p, good.prefix(8), two)
sigCase("empty", p, Data(), two)
sigCase("for another payload.json", p, sign(payload("1.2.0"), releaseKey), two)
sigCase("garbage (0x30 x 72)", p, Data(repeating: 0x30, count: 72), two)
sigCase("no keys", p, good, [])
sigCase("a byte appended to payload.json", p + Data(" ".utf8), good, two)
sigCase("made by openssl, its key trusted", p, osslSign(p), two + ["openssl"])
sigCase("made by openssl, its key not trusted", p, osslSign(p), two)
sigCase("the backup key listed first", p, good, ["backup", "release"])
sigCase("an empty payload.json", Data(), sign(Data(), releaseKey), two)

// DER built by hand from a good signature's r and s
let order: [UInt8] = [0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                      0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51]
func sub(_ a: [UInt8], _ b: [UInt8]) -> [UInt8] {          // a - b, 32 bytes, a >= b
    var r = [UInt8](repeating: 0, count: 32), borrow = 0
    for i in stride(from: 31, through: 0, by: -1) {
        var d = Int(a[i]) - Int(b[i]) - borrow
        borrow = d < 0 ? 1 : 0
        if d < 0 { d += 256 }
        r[i] = UInt8(d)
    }
    return r
}
func int(_ v: [UInt8]) -> [UInt8] {                       // a minimal DER INTEGER
    var b = Array(v.drop { $0 == 0 })
    if b.isEmpty { b = [0] }
    if b[0] & 0x80 != 0 { b.insert(0, at: 0) }
    return [0x02, UInt8(b.count)] + b
}
func seq(_ parts: [UInt8]...) -> Data { let body = parts.flatMap { $0 }; return Data([0x30, UInt8(body.count)] + body) }
let raw = try! P256.Signing.ECDSASignature(derRepresentation: good).rawRepresentation
let r = [UInt8](raw.prefix(32)), s = [UInt8](raw.suffix(32))
sigCase("rebuilt from r and s (as made)", p, seq(int(r), int(s)), two)
sigCase("high S (n - s)", p, seq(int(r), int(sub(order, s))), two)
sigCase("r and s swapped", p, seq(int(s), int(r)), two)
sigCase("raw r||s, not DER", p, raw, two)
sigCase("a byte after the sequence", p, good + Data([0]), two)
sigCase("the sequence's length in long form", p, Data([0x30, 0x81, good[1]]) + good.dropFirst(2), two)
sigCase("r with a needless leading zero", p, seq([0x02, UInt8(int(r)[1] + 1), 0] + int(r).dropFirst(2), int(s)), two)
sigCase("s with a needless leading zero", p, seq(int(r), [0x02, UInt8(int(s)[1] + 1), 0] + int(s).dropFirst(2)), two)
sigCase("r = 0", p, seq(int([0]), int(s)), two)
sigCase("s = 0", p, seq(int(r), int([0])), two)
sigCase("r = n", p, seq(int(order), int(s)), two)
sigCase("s = n", p, seq(int(r), int(order)), two)
sigCase("r negative (sign byte dropped)", p, seq([0x02, 0x20] + Array(repeating: 0x80, count: 32), int(s)), two)
sigCase("r of 33 bytes, not a sign byte", p, seq([0x02, 0x21, 0x01] + r, int(s)), two)
sigCase("a third integer", p, seq(int(r), int(s), int([1])), two)
sigCase("a BIT STRING for s", p, seq(int(r), [0x03] + int(s).dropFirst()), two)
sigCase("the sequence's length one too long", p, Data([0x30, good[1] + 1]) + good.dropFirst(2) + Data([0]), two)
sigCase("the sequence's length one too short", p, Data([0x30, good[1] - 1]) + good.dropFirst(2), two)
sigCase("a SET, not a SEQUENCE", p, Data([0x31]) + good.dropFirst(), two)
// many signatures: short r and s (under 32 bytes) and 33-byte ones (a sign byte) come up by chance
for i in 0..<400 {
    let d = Data("payload \(i) \(UUID().uuidString)".utf8)
    let k = [releaseKey, backupKey, otherKey][i % 3]
    let sg = sign(d, k)
    sigCase("random \(i), \(sg.count) bytes", d, sg, two)
    if i % 4 == 0 {   // its high-S twin
        let rw = try! P256.Signing.ECDSASignature(derRepresentation: sg).rawRepresentation
        sigCase("random \(i), high S", d, seq(int([UInt8](rw.prefix(32))), int(sub(order, [UInt8](rw.suffix(32))))), two)
    }
}

// MARK: payload.json's keys and values (plainJSONKeys, decodeManifest)

let text = String(decoding: p, as: UTF8.self)
func edited(_ from: String, _ to: String) -> Data {
    precondition(text.components(separatedBy: from).count == 2, "the test payload has \(from) once")
    return Data(text.replacingOccurrences(of: from, with: to).utf8)
}
func jsonCase(_ desc: String, _ d: Data) {
    let m = decodeManifest(d)
    line(["json", hex(desc), hex(d), plainJSONKeys(d) ? "1" : "0", hex(m?.payloadVersion), hex(m?.model.file), hex(m?.model.sha256),
          hex(m.map { String($0.model.size ?? 0) }), hex(m?.previousModels.map { $0.joined(separator: ",") })])
}
let evilApp = "\"app\": {\"version\": \"9.9.9\", \"zip\": \"evil.zip\", \"sha256\": \"\(oldSha)\"}"
let jsonCases: [(String, Data)] = [
    ("as built", p),
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
    ("a key with a tab", edited("\"commit\"", "\"a\tb\": 1, \"commit\"")),
    ("a key with DEL", edited("\"commit\"", "\"a\u{7f}b\": 1, \"commit\"")),
    ("an empty key", edited("\"commit\"", "\"\": 1, \"commit\"")),
    ("two objects", p + Data("{}".utf8)),
    ("something after the object", p + Data("x".utf8)),
    ("an array, not an object", Data(("[" + text + "]").utf8)),
    ("an unfinished string", Data("{\"payload_version\": \"1.1.0".utf8)),
    ("an unfinished object", Data(text.dropLast(3).utf8)),
    ("nothing", Data()),
    ("a byte-order mark", Data([0xef, 0xbb, 0xbf]) + p),
    ("a trailing comma in an object", edited("\"stems_engine\": [\"0002\"]}", "\"stems_engine\": [\"0002\"],}")),
    ("a trailing comma in an array", edited("[\"0002\"]", "[\"0002\",]")),
    ("an unknown key (a later release's)", edited("\"commit\"", "\"future\": {\"a\": [1, -2.5e3, true, false, null, {\"b\": \"x\"}]}, \"commit\"")),
    ("escapes in a value", edited("\"commit\": \"", "\"note\": \"a \\\"quoted\\\" \\\\ \\u00e9 \\ud83c\\udfb5\", \"commit\": \"")),
    ("the same key in different objects", edited("\"commit\"", "\"future\": [{\"a\": 1}, {\"a\": 2}], \"commit\"")),
    ("other spacing", Data(text.replacingOccurrences(of: "\n", with: "\r\n").replacingOccurrences(of: ": ", with: ":\t").utf8)),
    ("no spacing at all", Data(text.replacingOccurrences(of: "\n", with: "").replacingOccurrences(of: ": ", with: ":").replacingOccurrences(of: ", ", with: ",").utf8)),
    ("keys read exactly (ortVersionPrefix is not ort_version_prefix)", edited("\"compatible\": {\"rekordbox\"", "\"compatible\": {\"ortVersionPrefix\": \"9.\", \"rekordbox\"")),
    ("ort_version_prefix missing", edited("\"ort_version_prefix\": \"1.18.\", ", "")),
    ("compatible missing", edited(",\n \"compatible\": {\"rekordbox\": [\"7.2.19\"], \"ort_version_prefix\": \"1.18.\", \"stems_engine\": [\"0002\"]}", "")),
    ("app missing", edited("\n \"app\": {\"version\": \"1.1.0\", \"zip\": \"rbstemsplus-app.zip\", \"sha256\": \"\(sha)\"},", "")),
    ("bridge missing", edited("\n \"bridge\": {\"file\": \"libonnxruntime.1.18.0.dylib\", \"sha256\": \"\(sha)\"},", "")),
    ("model missing", edited("\n \"model\": {\"file\": \"stemsplus-model.onnx\", \"sha256\": \"\(modelSha)\", \"size\": 9},", "")),
    ("app, bridge and compatible missing", edited(",\n \"compatible\": {\"rekordbox\": [\"7.2.19\"], \"ort_version_prefix\": \"1.18.\", \"stems_engine\": [\"0002\"]}", "")
        .replacingSub("\n \"app\": {\"version\": \"1.1.0\", \"zip\": \"rbstemsplus-app.zip\", \"sha256\": \"\(sha)\"},", "")
        .replacingSub("\n \"bridge\": {\"file\": \"libonnxruntime.1.18.0.dylib\", \"sha256\": \"\(sha)\"},", "")),
    ("previous_models missing", edited(", \"previous_models\": [\"\(oldSha)\"]", "")),
    ("previous_models null", edited("[\"\(oldSha)\"]", "null")),
    ("previous_models empty", edited("[\"\(oldSha)\"]", "[]")),
    ("previous_models not a checksum", edited("[\"\(oldSha)\"]", "[\"abc\"]")),
    ("previous_models a number", edited("[\"\(oldSha)\"]", "[1]")),
    ("previous_models a string, not a list", edited("[\"\(oldSha)\"]", "\"\(oldSha)\"")),
    ("previous_models upper-case hex", edited("[\"\(oldSha)\"]", "[\"\(oldSha.uppercased())\"]")),
    ("a model checksum of 63 digits", edited("\"sha256\": \"\(modelSha)\"", "\"sha256\": \"\(modelSha.dropLast())\"")),
    ("a model checksum of 65 digits", edited("\"sha256\": \"\(modelSha)\"", "\"sha256\": \"\(modelSha)c\"")),
    ("model size 0", payload("1.1.0", size: "0")),
    ("model size -1", payload("1.1.0", size: "-1")),
    ("model size 9.0", payload("1.1.0", size: "9.0")),
    ("model size 9.5", payload("1.1.0", size: "9.5")),
    ("model size 9e0", payload("1.1.0", size: "9e0")),
    ("model size 1E3", payload("1.1.0", size: "1E3")),
    ("model size 9468852530000000000e-10", payload("1.1.0", size: "9468852530000000000e-10")),
    ("model size \"9\"", payload("1.1.0", size: "\"9\"")),
    ("model size null", payload("1.1.0", size: "null")),
    ("model size true", payload("1.1.0", size: "true")),
    ("model size 2^63 - 1", payload("1.1.0", size: "9223372036854775807")),
    ("model size 2^63", payload("1.1.0", size: "9223372036854775808")),
    ("model size 09", payload("1.1.0", size: "09")),
    ("model size +9", payload("1.1.0", size: "+9")),
    ("bridge size given", edited("\"bridge\": {\"file\": \"libonnxruntime.1.18.0.dylib\", \"sha256\": \"\(sha)\"}", "\"bridge\": {\"file\": \"libonnxruntime.1.18.0.dylib\", \"sha256\": \"\(sha)\", \"size\": 5}")),
    ("bridge size a string", edited("\"bridge\": {\"file\": \"libonnxruntime.1.18.0.dylib\", \"sha256\": \"\(sha)\"}", "\"bridge\": {\"file\": \"libonnxruntime.1.18.0.dylib\", \"sha256\": \"\(sha)\", \"size\": \"5\"}")),
    ("payload_version with a suffix", payload("1.5.0-beta.1")),
    ("payload_version with a v", payload("v1.1.0")),
    ("payload_version of two parts", payload("1.1")),
    ("payload_version of four parts", payload("1.1.0.0")),
    ("payload_version with a ten-digit part", payload("1.1.1234567890")),
    ("payload_version with leading zeros", payload("01.01.00")),
    ("payload_version a number", edited("\"payload_version\": \"1.1.0\"", "\"payload_version\": 1.1")),
    ("app.version with a suffix", edited("\"version\": \"1.1.0\"", "\"version\": \"1.1.0-beta.1\"")),
    ("app.version newer than payload_version", edited("\"version\": \"1.1.0\"", "\"version\": \"1.2.0\"")),
    ("app.zip with a slash", edited("rbstemsplus-app.zip", "../app.zip")),
    ("app.zip \"..\"", edited("rbstemsplus-app.zip", "..")),
    ("app.zip \".\"", edited("rbstemsplus-app.zip", ".")),
    ("app.zip with a space", edited("rbstemsplus-app.zip", "rbstemsplus app.zip")),
    ("app.zip empty", edited("rbstemsplus-app.zip", "")),
    ("app.zip outside ASCII", edited("rbstemsplus-app.zip", "rbstemsplus-\u{e9}.zip")),
    ("bridge.file with a backslash escape", edited("libonnxruntime.1.18.0.dylib", "a\\\\b.dylib")),
    ("model.file with an underscore", edited("stemsplus-model.onnx", "stemsplus_model.onnx")),
    ("app.sha256 upper-case", edited("\"app\": {\"version\": \"1.1.0\", \"zip\": \"rbstemsplus-app.zip\", \"sha256\": \"\(sha)\"}", "\"app\": {\"version\": \"1.1.0\", \"zip\": \"rbstemsplus-app.zip\", \"sha256\": \"\(sha.uppercased())\"}")),
    ("compatible.rekordbox a string", edited("\"rekordbox\": [\"7.2.19\"]", "\"rekordbox\": \"7.2.19\"")),
    ("compatible.rekordbox with a number", edited("\"rekordbox\": [\"7.2.19\"]", "\"rekordbox\": [\"7.2.19\", 7]")),
    ("compatible.rekordbox empty", edited("\"rekordbox\": [\"7.2.19\"]", "\"rekordbox\": []")),
    ("compatible.rekordbox odd strings", edited("\"rekordbox\": [\"7.2.19\"]", "\"rekordbox\": [\"7.2 19\", \"\", \"a/b\"]")),
    ("an unknown key holding a deep array (12)", edited("\"commit\"", "\"future\": " + String(repeating: "[", count: 12) + String(repeating: "]", count: 12) + ", \"commit\"")),
    ("a windows block (release.md 1.2)", payload("1.1.0", extra: windowsBlock("1.1.0"))),
    ("a windows block, its app's version with a suffix", payload("1.1.0", extra: windowsBlock("1.1.0-beta.2"))),
    ("a windows block that is a string", payload("1.1.0", extra: ", \"windows\": \"x\"")),
    ("a windows block with a key twice", payload("1.1.0", extra: windowsBlock("1.1.0").replacingOccurrences(of: "\"zip\"", with: "\"zip\": \"a\", \"zip\""))),
]
func windowsBlock(_ appVersion: String) -> String {
    let s = { (c: String) in String(repeating: c, count: 64) }
    return """
        ,
         "windows": {
          "app": {"version": "\(appVersion)", "zip": "rbstemsplus-windows.zip", "sha256": "\(s("1"))", "admin_sha256": "\(s("2"))",
                   "stub_sha256": {"x64": "\(s("3"))", "arm64": "\(s("4"))"}},
          "bridge": {"file": "rbstemsplus-bridge-win-x64.dll", "sha256": "\(s("5"))"},
          "compatible": {"rekordbox": ["7.2.19"], "ort_sha256": ["\(s("6"))"], "stems_engine": ["0002"]},
          "resampler_proved": [{"product_version": "7.2.19", "rekordbox_sha256": "\(s("7"))"}]
         }
        """
}
extension Data {
    func replacingSub(_ a: String, _ b: String) -> Data {
        let t = String(decoding: self, as: UTF8.self)
        precondition(t.components(separatedBy: a).count == 2, "the payload has \(a) once")
        return Data(t.replacingOccurrences(of: a, with: b).utf8)
    }
}
for (desc, d) in jsonCases { jsonCase(desc, d) }

// MARK: checkManifest: the signature, the decode and the floor, in order

func checkCase(_ desc: String, _ d: Data, _ sig: Data?, _ names: [String], _ floor: String?) {
    let res: String
    switch checkManifest(d, signature: sig, keys: keys(names), floor: floor) {
    case .success(let m): res = "ok:" + (m.signedBy ?? "?")
    case .failure(.noSignature): res = "noSignature"
    case .failure(.badSignature): res = "badSignature"
    case .failure(.notValid): res = "notValid"
    case .failure(.older): res = "older"
    case .failure(.notSaved): res = "notSaved"
    }
    line(["check", hex(desc), hex(d), hex(sig), hex(names.joined(separator: ",")), hex(floor), hex(res)])
}
checkCase("signed, no floor", p, good, two, nil)
checkCase("no signature", p, nil, two, nil)
checkCase("an empty signature", p, Data(), two, nil)
checkCase("another key's signature", p, sign(p, otherKey), two, nil)
checkCase("a truncated signature", p, good.prefix(20), two, nil)
let twiceApp = jsonCases[1].1
checkCase("validly signed, \"app\" twice", twiceApp, sign(twiceApp, releaseKey), two, nil)
checkCase("not JSON, validly signed", Data("hello".utf8), sign(Data("hello".utf8), releaseKey), two, nil)
checkCase("not JSON, not signed", Data("hello".utf8), Data([0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01]), two, nil)
for (v, f) in [("1.1.0", "1.1.0"), ("1.1.0", "1.2.0"), ("1.2.0", "1.1.0"), ("1.10.0", "1.2.0"), ("1.2.0", "1.10.0"),
               ("1.0.9", "1.0.10"), ("2.0.0", "1.99.99"), ("1.1.0", "1.0.0"), ("1.1.0", "08.09.10")] {
    let d = payload(v)
    checkCase("payload \(v), floor \(f)", d, sign(d, backupKey), two, f)
}
let suffixed = payload("1.5.0-beta.1")
checkCase("a suffixed payload, signed", suffixed, sign(suffixed, releaseKey), two, nil)
let win = payload("1.1.0", extra: windowsBlock("1.1.0"))
checkCase("with a windows block, signed", win, sign(win, releaseKey), two, "1.0.0")

// MARK: versions and the floor

for v in ["1.4.0", "1.10.2", "0.0.0", "123456789.1.1", "1234567890.1.1", "1.5.0-beta.1", "v1.4.0", "1.4", "1.4.0.0", "1..0", "",
          "1.4.0 ", " 1.4.0", "1.4.a", "-1.4.0", "1.4.0\n", "08.09.10", "\u{0661}.4.0", "1.4.0-beta"] {
    line(["release", hex(v), isReleaseVersion(v) ? "1" : "0"])
}
for (a, b) in [("1.0.9", "1.0.10"), ("1.0.10", "1.0.9"), ("1.1.0", "1.1.0"), ("1.1.0", "1.2.0"), ("2.0.0", "1.9.9"),
               ("1.0.0", "1.0.1"), ("08.09.10", "8.9.9"), ("8.9.9", "08.09.10"), ("0.9.9", "1.0.0"), ("10.0.0", "9.99.99"),
               ("1.2.3", "1.2.30"), ("999999999.0.0", "1.0.0")] {
    line(["older", hex(a), hex(b), olderPayload(a, than: b) ? "1" : "0"])
}
let rec = scratch + "/payload-version"
for t in ["1.2.0\n", "1.2.0", "  1.2.0  \n", "1.2.0\r\n", "\t1.2.0\t", "not a version!\n", "", "\n", "1.2.0-beta.1\n", "a/b\n",
          "1.2.0 1.3.0\n", String(repeating: "1", count: 63), String(repeating: "1", count: 64), "caf\u{e9}\n"] {
    put(rec, Data(t.utf8))
    line(["floor", hex(t), hex(payloadFloor(PayloadFiles(floor: rec)))])
}
for (saved, minimum) in [(nil, "1.2.0"), ("1.1.0", "1.2.0"), ("1.10.0", "1.2.0"), ("1.2.0", ""), (nil, ""), ("1.2.0", "1.2.0"), ("2.0.0", "1.9.9")] {
    if let s = saved { put(rec, Data((s + "\n").utf8)) } else { try? FileManager.default.removeItem(atPath: rec) }
    line(["accepted", hex(saved), hex(minimum), hex(acceptedFloor(PayloadFiles(floor: rec, minimum: minimum)))])
}

// MARK: the version shown and the update's result (Update.swift; update.c's copy)

for t in ["1.4.1\n1.4.0\n", "1.4.0\n1.4.0\n", "1.4.0\n1.4.1\n", "1.4.1\n1.4.0", "1.4.1\n1.4.0\n\n", "1.4.1\r\n1.4.0\n", "\n1.4.0\n", "1.4.1\nv1.4.0\n",
          "1.10.0\n1.9.0\n", "1.5.0-beta.1\n1.4.0\n", String(repeating: "1", count: 60) + "\n1.0.0\n", "1.4.1\n1.4.0\nx", ""] {
    let r = ShownVersion(Data(t.utf8))
    line(["shownrec", hex(t), hex(r?.shown), hex(r?.app)])
}
let versions = ["1.4.0", "1.4.1", "1.4.10", "1.4.9", "1.3.0", "x"]
for own in ["1.4.0", "1.4.9", "v1"] {
    for rec in [nil, ("1.4.0", "1.4.0"), ("1.4.1", "1.4.0"), ("1.4.10", "1.4.0"), ("1.4.9", "1.4.9"), ("1.3.0", "1.3.0")] as [(String, String)?] {
        var seens: [(String, String)?] = [nil]
        for p in versions { for a in ["1.4.0", "1.4.9", "1.5.0"] { seens.append((p, a)) } }
        for seen in seens {
            let before = rec.map { ShownVersion(shown: $0.0, app: $0.1) }
            let after = nextShownVersion(own: own, record: before, seen: seen.map { (payload: $0.0, app: $0.1) })
            line(["shown", hex(own), hex(rec?.0), hex(rec?.1), hex(seen?.0), hex(seen?.1), hex(after?.shown), hex(after?.app)])
        }
    }
}
// updateFailureReason left app/Sources at the switch-over (3cd20789; the core's update_failure_reason
// decides now): this is the frozen oracle's copy (core/tests/swift/oracle/Update.swift), which the
// core's vectors hold the core to, so the Windows parse still answers as the Mac does.
func updateFailureReason(_ result: String) -> String? {
    let words = result.split(whereSeparator: { $0.isWhitespace })
    guard words.count == 2, words[0] == "failed" else { return nil }
    switch words[1] {
    case "app": return "RB Stems Plus was moved or changed while it was updating."
    case "download": return "The download wasn't a valid copy of RB Stems Plus."
    case "staging": return "Something else is in the way next to RB Stems Plus (\(updateStagingName))."
    case "copy": return "The new version couldn't be copied next to RB Stems Plus."
    case "copy-check": return "The copy of the new version didn't check out."
    case "sign": return "The new version couldn't be signed."
    case "swap": return "The new version couldn't take this one's place."
    default: return nil
    }
}
for r in ["failed app", "failed download", "failed staging", "failed copy", "failed copy-check", "failed sign", "failed swap", "failed",
          "failed nothing", " failed\tswap\n", "failed swap now", "FAILED swap", "", "hello", "failed  copy"] {
    line(["failure", hex(r), updateFailureReason(r) == nil ? "0" : "1"])
}

// MARK: config.ini's writer and configSets (Config.swift; native_config.c's copy)

let configTexts: [String?] = [
    nil, "", "\n", "max_gb=5\n", "max_gb=5", "\u{FEFF}max_gb=5\n", "\u{FEFF}[cache]\nx=1\n", "# c\n[watcher]\nenabled=1\n",
    "[cache]\nmax_gb=2\nmax_days=3\n[other]\nmax_gb=9\n", "MAX_GB = 7 # x\r\n", "a\n\n", "max_gb=1\nmax_gb=2\n", "  [ Cache ]\nrekordbox_model=0\n",
    "[cache\nmax_gb=1\n", "max_gb=" + String(repeating: "x", count: 1100) + "\n", "\tmax_days\t=\t9\nrekordbox_model=1\n[a]\n",
    "cache_dir=\"/Volumes/X/Stems\"\ncache_id=0E8A1D84-0C4B-4F6B-9E4B-2D5E1C1C9A11\n",
    "[" + String(repeating: "s", count: 1100) + "]\ncache_dir=\"/a\"\n[cache]\nmax_gb=3\n", "x=1\u{0}\n", "max_gb=\u{FF}\n",
    String(repeating: "#\n", count: 32768),   // CFG_MAX exactly: an insert goes over it (review S42)
    // review S45's boundaries: lines of CFG_LINE_MAX (1024) bytes and one more, with a BOM before,
    // a section that long, a lone CR, a blank line of spaces, CRLF with a section, a BOM alone,
    // combining marks beside line breaks, the location in and outside [cache], one byte under CFG_MAX
    "max_gb=" + String(repeating: "1", count: 1017) + "\n", "max_gb=" + String(repeating: "1", count: 1018) + "\n",
    "\u{FEFF}max_gb=" + String(repeating: "1", count: 1017) + "\nmax_days=2\n",
    "[" + String(repeating: "s", count: 1022) + "]\ncache_dir=\"/a\"\n", "[" + String(repeating: "s", count: 1023) + "]\ncache_id=X\n",
    "max_gb=5\rmax_days=3\n", "   \t \nmax_gb=5\n", "[cache]\r\nmax_gb=5\r\n[other]\r\nmax_gb=6\r\n", "\u{FEFF}", "\n\n",
    "max_gb=5\n\u{0301}max_days=3\n", "x=\u{0301}\n\u{0301}\n[cache]\u{0301}\nmax_gb=1\n",
    "cache_dir=\"/a\"\n[cache]\ncache_id=0E8A1D84-0C4B-4F6B-9E4B-2D5E1C1C9A11\n[other]\ncache_dir=\"/b\"\ncache_id=Y\n",
    String(repeating: "#\n", count: 32767) + "#",
]
let configOps: [(values: [(key: String, value: String)], remove: [String])] = [
    ([("max_gb", "1.5"), ("max_days", "30")], []), ([("rekordbox_model", "1")], []),
    ([("cache_dir", "\"/Volumes/X/Stems\""), ("cache_id", "0E8A1D84-0C4B-4F6B-9E4B-2D5E1C1C9A11")], []), ([], ["cache_dir", "cache_id"]),
]
let configPath = scratch + "/config.ini"
var configUnwritten = 0   // readable, but nothing written: Foundation's atomic write refused (a sandbox)
for t in configTexts {
    for (i, op) in configOps.enumerated() {
        try? FileManager.default.removeItem(atPath: configPath)
        var raw = t.map { Data($0.utf8) }
        if t == "max_gb=\u{FF}\n" { raw = Data([0x6d, 0x61, 0x78, 0x5f, 0x67, 0x62, 0x3d, 0xff, 0x0a]) }   // invalid UTF-8
        if let r = raw { put(configPath, r) }
        let readable = readConfigBytes(configPath).unreadable == nil
        let ok = writeConfigValues(op.values, remove: op.remove, file: configPath)
        let after = ok ? FileManager.default.contents(atPath: configPath) : nil
        if readable && after == nil { configUnwritten += 1 }
        line(["configw", hex(raw), String(i), readable ? "1" : "0", hex(after)])
        if i == 0 {
            for key in ["max_gb", "rekordbox_model", "cache_dir", "x"] {
                if let r = raw { put(configPath, r) } else { try? FileManager.default.removeItem(atPath: configPath) }
                line(["configsets", hex(raw), hex(key), configSets(key, file: configPath) ? "1" : "0"])
            }
        }
    }
}

// MARK: Models.swift's decisions (models.c's copy): every case built in a folder of its own

let mfm = FileManager.default
let mOursData = Data("our model".utf8), mA = Data("Pioneer's model A".utf8), mB = Data("Pioneer's model B".utf8),
    mC = Data("Pioneer's model C".utf8), mX = Data("someone else's model".utf8), mBroken = Data("broken".utf8)
let mOurs = sha256(data: mOursData), shaA = sha256(data: mA), shaB = sha256(data: mB), shaC = sha256(data: mC)
// Swift's saveOriginal writes the engine file atomically, through a folder a sandbox refuses: then no model cases
let mProbe = scratch + "/atomic-probe"
let mCanWrite = (try? "x".write(toFile: mProbe, atomically: true, encoding: .utf8)) != nil
try? mfm.removeItem(atPath: mProbe)
if !mCanWrite { configUnwritten += 1 }
func mListing(_ d: String) -> String {
    guard fileType(d) == S_IFDIR else { return "-" }
    return ((try? mfm.contentsOfDirectory(atPath: d)) ?? []).sorted().map { n in
        let p = d + "/" + n
        switch fileType(p) {
        case S_IFREG: return n + "=" + hex(mfm.contents(atPath: p) ?? Data())
        case S_IFLNK: return n + "@"
        default: return n + "/"
        }
    }.joined(separator: ";")
}
// an original's content standing for "<sha>.onnx is a link to a file holding model A" or "is a folder"
let mAsLink = Data("(a link to model A)".utf8), mAsDir = Data("(a folder)".utf8)
// the originals sets: (name, content, engine (nil: no .engine file), mtime)
let mSets: [[(Data, String, String?, Int)]] = [
    [], [(mA, shaA, "0002", 1000)], [(mA, shaA, "0002", 1000), (mB, shaB, "0003", 1001)],
    [(mA, shaA, "0002", 1000), (mBroken, shaB, "0003", 1001)], [(mA, shaA, nil, 1000)],
    [(mA, shaA, "0002", 1000), (mC, shaC, "0002", 2000)], [], [(mBroken, shaA, "0002", 1000)],
    [(mOursData, mOurs, nil, 1000), (mA, shaA, "0002", 1001)],
    [(mA, shaA, "0002", 2000), (mC, shaC, "0002", 1000)],    // newest first, not name order (review MT4)
    [(mA, shaA, nil, 2000), (mB, shaB, "0002", 1000)],       // a known engine before an unknown one (MT7)
    [(mA, shaA, "2", 1000), (mB, shaB, "10", 900)],          // engines compared as numbers: 10 above 2 (MT8)
    [(mAsLink, shaA, "0002", 1000), (mB, shaB, "0003", 1001)],   // <sha>.onnx a link (MT10)
    [(mAsDir, shaA, "0002", 1000), (mB, shaB, "0003", 1001)],    // <sha>.onnx a folder (MT10)
]
let mModels: [String] = ["none", "ours", "A", "B", "X", "link", "nodir"]
let mEngines: [String?] = [nil, "0002", "0003"]
let mOps = ["install", "installbad", "restore", "left", "info", "order"]
var mCase = 0
for (si, set) in mSets.enumerated() where mCanWrite {
    for model in mModels {
        for engine in mEngines {
            for op in mOps {
                mCase += 1
                let root = scratch + "/m\(mCase)", rbm = root + "/rb", orig = root + "/originals"
                try? mfm.removeItem(atPath: root)
                try! mfm.createDirectory(atPath: root, withIntermediateDirectories: true)
                var spec = ["ours \(mOurs)"]
                let srcData = op == "installbad" ? mBroken : mOursData
                put(root + "/src.onnx", srcData)
                spec.append("src \(hex(srcData)) \(mOurs)")
                if model == "nodir" { spec.append("nodir") } else { try! mfm.createDirectory(atPath: rbm, withIntermediateDirectories: true) }
                if let e = engine, model != "nodir" { put(rbm + "/demucs3_ver.txt", Data((e + "\n").utf8)); spec.append("d demucs3_ver.txt \(hex(Data((e + "\n").utf8))) 1000") }
                let content: Data? = ["ours": mOursData, "A": mA, "B": mB, "X": mX][model]
                if let c = content { put(rbm + "/hdemucs.onnx", c); spec.append("d hdemucs.onnx \(hex(c)) 1000") }
                if model == "link" {
                    put(root + "/elsewhere.onnx", mA)
                    try! mfm.createSymbolicLink(atPath: rbm + "/hdemucs.onnx", withDestinationPath: root + "/elsewhere.onnx")
                    spec.append("link hdemucs.onnx \(hex(mA))")
                }
                if si == 6 { spec.append("noorig") } else {
                    try! mfm.createDirectory(atPath: orig, withIntermediateDirectories: true)
                    for (data, sha, e, t) in set {
                        if data == mAsLink {
                            put(root + "/elsewhere-original.onnx", mA)
                            try! mfm.createSymbolicLink(atPath: orig + "/\(sha).onnx", withDestinationPath: root + "/elsewhere-original.onnx")
                            spec.append("olink \(sha).onnx \(hex(mA))")
                        } else if data == mAsDir {
                            try! mfm.createDirectory(atPath: orig + "/\(sha).onnx", withIntermediateDirectories: false)
                            spec.append("odir \(sha).onnx")
                        } else {
                            put(orig + "/\(sha).onnx", data)
                            try! mfm.setAttributes([.modificationDate: Date(timeIntervalSince1970: TimeInterval(t))], ofItemAtPath: orig + "/\(sha).onnx")
                            spec.append("o \(sha).onnx \(hex(data)) \(t)")
                        }
                        if let e = e {
                            put(orig + "/\(sha).engine", Data((e + "\n").utf8))
                            spec.append("o \(sha).engine \(hex(Data((e + "\n").utf8))) \(t)")
                        }
                    }
                }
                spec.append("op \(op)")
                let gate = Deletable(inside: [orig], roots: [], files: [rbm + "/.hdemucs.new", rbm + "/.hdemucs.restore"], apps: [])
                let m = Models(dir: rbm, originals: orig, ours: [mOurs], gate: gate, log: { _ in })
                var ok = "1", msg = "", extra = ""
                switch op {
                case "install", "installbad":
                    let r = m.installOurs(from: root + "/src.onnx", sha: mOurs); ok = r.ok ? "1" : "0"; msg = r.message
                case "restore": let r = m.restore(); ok = r.ok ? "1" : "0"; msg = r.message
                case "left": extra = modelLeftBehind(m)
                case "info": extra = (m.savedOriginals().isEmpty ? "0" : "1") + (m.verifiedOriginals().isEmpty ? "0" : "1")
                default: extra = m.savedOriginals().map(\.sha).joined(separator: ",")
                }
                line(["modelcase", hex(spec.joined(separator: "\n")), ok, hex(msg), hex(mListing(rbm) + "|" + mListing(orig)), hex(extra)])
                try? mfm.removeItem(atPath: root)
            }
        }
    }
}

// gen-fixtures.sh marks the fixtures partial (made again at the next run) when this is above 0
out += "# config writes not made: \(configUnwritten)\n"

FileHandle.standardOutput.write(Data(out.utf8))
