// The core's vectors from 1.3.0's Swift (test only): runs the Swift app's own functions (the frozen
// copy core/tests/swift/oracle, built without main.swift) on fixed and generated inputs and writes
// what they return as a C header (core/tests/gen-vectors.sh OUT writes OUT/vectors_swift.h),
// which the core's unit tests compare against (core/tests/vectors.c).
// Nothing here reads or writes the user's files: only a scratch folder under build/.
import AppKit

// MARK: - output

var out = ""
func cstr(_ bytes: [UInt8]) -> String {
    var s = "\""
    for b in bytes {
        if b >= 0x20 && b < 0x7F && b != 0x22 && b != 0x5C && b != 0x3F { s.append(Character(UnicodeScalar(b))) }
        else { s += String(format: "\\%03o", b) }
    }
    return s + "\""
}
func cstr(_ s: String) -> String { cstr(Array(s.utf8)) }
/// a dump's string: bytes under 0x20, "\" and "|" as \xx, so dumps compare as C strings
func esc(_ s: String) -> String { String(decoding: escBytes(s), as: UTF8.self) }
func escBytes(_ s: String) -> [UInt8] {
    var o: [UInt8] = []
    for b in s.utf8 {
        if b < 0x20 || b == 0x5C || b == 0x7C { o += Array(String(format: "\\%02x", b).utf8) } else { o.append(b) }
    }
    return o
}
func d17(_ x: Double) -> String { String(format: "%.17g", x) }

/// one table: rows of string arguments and the wanted result (bytes)
func table(_ name: String, _ rows: [([[UInt8]], [UInt8])]) {
    out += "static const swift_vec \(name)[] = {\n"
    for (args, want) in rows {
        out += "    {{" + args.map { cstr($0) }.joined(separator: ", ") + "}, \(args.count), " + cstr(want) + "},\n"
    }
    out += "};\n\n"
}
func s2b(_ s: String) -> [UInt8] { Array(s.utf8) }

// a fixed pseudo-random sequence (xorshift), so the vectors are the same on every run
var rng: UInt64 = 0x9E3779B97F4A7C15
func rnd(_ n: Int) -> Int { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return Int(rng % UInt64(n)) }

// MARK: - masking (Report.swift)

/// Report.swift redact, word for word, with the home folder and the names given (redact reads
/// the account's own: never this machine's in a committed file)
func redactWith(_ s: String, home h: String, names: [String], keep: Set<String>) -> String {
    var out = maskSecrets(s.replacingOccurrences(of: h, with: "~"), keep: keep)
    for name in names where name.count >= 2 {
        let pattern = "\\b" + NSRegularExpression.escapedPattern(for: name) + "\\b"
        out = out.replacingOccurrences(of: pattern, with: "<user>", options: [.regularExpression, .caseInsensitive])
    }
    return out
}

let maskFixed: [String] = [
    "", "plain text, nothing to mask", "password=hunter2", "Password: \"s3cr3t\"", "api_key = abc123, next", "x -password=1", "ab-password=x",
    "password=<token> and password=Bearer abcdefghij", "token=Basic abcdefghijk", "session_id=\"abc\" cookie:xyz;more",
    "https://x.example.com/a?X-Amz-Signature=abc&b=2&access_token=zzz#frag", "?key=", "?a;token=v", "&code=1234 &x=1",
    "Authorization: Bearer abcdefghijklmnop", "bearer  ABCDEFGH", "Basically abcdefghijk", "xBearer abcdefghij",
    "mail me at john.doe+tag@example.co.uk now", "a@b.c", "a@b.com", "a@b.c0m", "a@b.com.x1", "x@y@b.com", "x@y.z@b.com", "a@b.com.", "a@b-c.d-e.fg",
    "jwt eyJhbGciOiJI.eyJzdWIiOiIx.SflKxwRJSMeKKF2QT4fw ok", "eyJabc.defgh.ijkl", "xeyJabcd.efgh.ij",
    "AKIAIOSFODNN7EXAMPLE", "AKIAIOSFODNN7EXAMPLE1", "id ASIAABCDEFGHIJKLMNOP.", "ghp_" + String(repeating: "a1B2", count: 9), "ghp_" + String(repeating: "a", count: 29),
    "github_pat_" + String(repeating: "x_Y9", count: 9), "xoxb-1234567890-abc", "xoxa-123", "sk-" + String(repeating: "Ab3_", count: 6), "sk-short",
    "-----BEGIN RSA PRIVATE KEY-----\nMIIEow\n-----END RSA PRIVATE KEY-----\nafter", "-----BEGIN PRIVATE KEY-----\nunterminated",
    "-----BEGIN ENCRYPTED PRIVATE KEY-----x-----END ENCRYPTED PRIVATE KEY----- y", "-----BEGIN PUBLIC KEY-----\nabc\n-----END PUBLIC KEY-----",
    "sha 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef end", "commit 0123456789abcdef0123456789abcdef01234567",
    "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef", "ffffffffffffffffffffffffffffffffffffffff", "aB3dE5gH7jK9mN1pQ3sT5vX7zB9dF1hJ",
    "aB3dE5gH7jK9mN1pQ3sT5vX7zB9dF1hJ==", "aB3dE5gH7jK9mN1pQ3sT5vX7zB9dF1hJ===", "aB3dE5gH7jK9mN1pQ3sT5vX7zB9dF1hJ=x",
    "CrashReporterFunctionNameWithWords1234567890", "x=aB3dE5gH7jK9mN1pQ3sT5vX7zB9dF1hJ", "/Users/dj/Music/Track 01.mp3",
    "home /Users/dj and /Users/djx", "DJ Smith played; dj smith; DJSMITH; dj", "Test User, test", "José MÜLLER josé müller",
    "toKen=abc", "paſsword=x", "ſk-" + String(repeating: "a", count: 20), "café password=é", "naïve token: \"v\"", "“password=x”", "pwd:  \"q\"",
    "PASSWD=\"\"", "secret=\"", "access-key: val", "private_key=abc", "sessionid=1;", "x.authorization=foo",
]
let maskFrags = ["password", "token", "Token", "key", "=", ":", " ", "\"", "'", "Bearer ", "Basic ", "eyJ", ".", "@", "a", "Z", "0", "7", "-", "_",
                 "?", "&", ";", "#", "<", ">", "sk-", "ghp_", "AKIA", "xoxb-", "-----BEGIN RSA PRIVATE KEY-----", "-----END RSA PRIVATE KEY-----",
                 "deadbeef", "0123456789", "AbCdEf", "xyzXYZ", "é", "ſ", "K", "\n", "\t", "/Users/dj", "dj", "DJ Smith", "example.com", "com",
                 "secret", "session_id", "cookie", "sig", "code", ",", "<token>", "+", "/", "~", "%", "github_pat_", "aB3", "=="]
var maskRows: [([[UInt8]], [UInt8])] = []
var maskCases = maskFixed
for _ in 0..<1500 {
    var s = ""
    for _ in 0..<(1 + rnd(24)) { s += maskFrags[rnd(maskFrags.count)] }
    if rnd(4) == 0 { s += String(repeating: ["a", "B", "9", "f", "_"][rnd(5)], count: 30 + rnd(10)) }
    maskCases.append(s)
}
let keepSet: Set<String> = ["0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"]
for s in maskCases {
    maskRows.append(([s2b(s)], s2b(redactWith(s, home: "/Users/dj", names: ["DJ Smith", "dj"], keep: keepSet))))
}
for s in ["José MÜLLER josé müller", "Test User, test, TEST", "x", "ab", "a"] {
    maskRows.append(([s2b(s), s2b("names2")], s2b(redactWith(s, home: "/Users/test", names: ["José Müller", "test", "a"], keep: []))))
}
table("SV_MASK", maskRows)

// Masking outside ASCII (review Q7: the core masks at least what 1.3.0 masks): ICU's \b (its \w,
// and the Grapheme_Extend and Cf code points it looks through), full case folding in the
// keywords and the names (ß against ss), the home folder's canonical equivalents. Each row is
// [text, home, names...], all redacted as redact does; no keep set.
var uniRows: [([[UInt8]], [UInt8])] = []
func uniRow(_ s: String, home: String = "/Users/dj", names: [String] = []) {
    precondition(names.count <= 18)
    uniRows.append(([s2b(s), s2b(home)] + names.map(s2b), s2b(redactWith(s, home: home, names: names, keep: []))))
}
// the two differences review Q7 named, and their neighbours
uniRow("ANNA STRASSE was here; anna straße too", names: ["Anna Straße"])
uniRow("x።password=hunter2 y")
uniRow("Anna Strasse, ANNA STRAẞE, anna straſſe, Anna Straße, Anna Strass, Anna Stras, Anna Straßen", names: ["ANNA STRASSE"])
uniRow("Anna Strasse, ANNA STRAẞE, anna straſſe, Anna Straße, Strassen", names: ["Anna Straẞe"])
uniRow("Strass Straß Stras Straßx", names: ["Strass", "Stras"])
uniRow("Straß Strass", names: ["Straß"])
uniRow("ß ss SS ẞ sß", names: ["ss"])
uniRow("Fiona ﬁona FIONA ﬀ ff stop ﬆop ﬅop", names: ["Fiona", "ff", "ſtop"])
uniRow("İx i̇x Ix ix ıx", names: ["İx"])
uniRow("Ix ix ıx İx", names: ["ix"])
uniRow("ΣΊΣΥΦΟΣ σίσυφος σίσυφοσ ςίςυφος", names: ["Σίσυφος"])
uniRow("ΐ ΐ ΐ Ϊ́ ΐx", names: ["aΐ", "ΐx"])
uniRow("Kelvin KELVIN kelvin", names: ["Kelvin"])
uniRow("ǅemal ǄEMAL ǆemal Dželal", names: ["Ǆemal"])
uniRow("ԵՒ եւ և", names: ["եւ"])
// normalisation forms: ICU compares code points (after folding), not canonical equivalents
uniRow("José JOSÉ Jose\u{301} JOSE\u{301}", names: ["José"])
uniRow("José JOSÉ Jose\u{301} JOSE\u{301}", names: ["Jose\u{301}"])
uniRow("Å Å Å å", names: ["xÅ", "Åx"])
uniRow("a xÅ xÅ xÅ Åx", names: ["xÅ", "Åx"])
// how many characters a name has (Swift's name.count >= 2)
uniRow("e\u{301} é ab\u{301} \u{301}a x\u{301}y", names: ["e\u{301}", "ab\u{301}", "\u{301}a", "x\u{301}y"])
uniRow("a\u{200D}b a\u{200D} 👍🏽 ab", names: ["a\u{200D}", "\u{200D}b"])
// the keywords, folded in full
for s in ["paßword=x", "PAẞWORD=x", "paßwd=1", "xpaßwd_y=1", "paßwd_y: \"v\"", "a paßwd b", "pasßword=x", "pasſword=x",
          "acceß_key=v", "acceẞ-key: v", "accesſkey=v", "seßion_id=1", "ſeßionid=1", "SEẞION_ID=1", "seß_id=1",
          "https://x/?seßion=1&a=2", "?paßw=1", "?xpaßwd=1;y=2", "?accesﬆoken=1", "?ﬆ=1", "toKen=1", "tokeN=1",
          "PRİVATE_KEY=1", "prıvate_key=1", "api_ķey=1", "Baſic abcdefghijk", "BEARER abcdefghijk", "password=Baſic xyz",
          "password=<token>", "password=BEARER", "eyJ…", "aſia1234567890ABCDEF", "ſk-" + String(repeating: "a", count: 20),
          "paßword=x paßword=y", "ßpassword=x", "password=ß", "ß=1", "word ßword=1"] {
    uniRow(s)
}
// \b next to Grapheme_Extend and Cf code points, and outside ASCII
for s in [" \u{301}password=x", "\u{301}password=x", "a\u{301}password=x", "x\u{200B}password=x", " \u{200B}password=x",
          "x\u{AD}password=x", "x\u{FEFF}token=1", "\u{200D}token=1", "a\u{200C}token=1", "é\u{301}\u{302}token=1", "።token=1",
          "AKIAIOSFODNN7EXAMPLE\u{301}", "AKIAIOSFODNN7EXAMPLE\u{200B}", "AKIAIOSFODNN7EXAMPLE።", "AKIAIOSFODNN7EXAMPLEé",
          "ghp_" + String(repeating: "a1B2", count: 9) + "\u{301}", "dj\u{301} dj\u{200B} \u{301}dj ádj dj። ⁀dj ‿dj dj‿",
          "Bearer\u{301} abcdefghijk", "\u{301}Bearer abcdefghijk", "x\u{20DD}Basic abcdefghijk", "٣password=1", "_password=1",
          "‿password=1", "⁔token=1", "Ⅻpassword=1", "ⓐpassword=1", "🅰password=1", "𝐀password=1", "〆token=1", "ー token=1"] {
    uniRow(s, names: ["dj"])
}
// the home folder: NSString's search takes a canonically equivalent code point for an ASCII one
uniRow("/Users/\u{212A}im/x and /Users/Kim/y and /Users/kim", home: "/Users/Kim")
uniRow("/Users/a\u{37E}b/x /Users/a;b", home: "/Users/a;b")
uniRow("/Users/a\u{1FEF}b/x", home: "/Users/a`b")
uniRow("/Users/dj/x /Users/djx /Users/dj\u{200D}x /USERS/dj", home: "/Users/dj")
// the home before a code point NSString joins to its last character: not the home (then the later passes see it)
uniRow("/Users/dj\u{301}/x /Users/dj\u{F870}/x /Users/dj\u{1F3FB}/x /Users/dj\u{903}/x", home: "/Users/dj")
uniRow("?/Users/mikey\u{301}=hunter2 &/Users/sigrid\u{301}/x=hunter2", home: "/Users/mikey")
uniRow("&/Users/sigrid\u{301}/x=hunter2", home: "/Users/sigrid")
uniRow("/Users/Jose\u{301} Smith/x", home: "/Users/Jose", names: ["Jose\u{301} Smith", "Jose"])
uniRow("/Users/Andre\u{301} Smith/x", home: "/Users/Andre", names: ["Andre\u{301} Smith", "Andre"])
// a name of "a" and an Apple private-use code point ICU calls Grapheme_Extend but Swift counts as a character
for c in [0xF86F, 0xF870, 0xF87F, 0xF880, 0xF883, 0xF884, 0xF899, 0xF89A, 0xF89F, 0xF8A0] as [UInt32] {
    let nm = "a" + String(Unicode.Scalar(c)!)
    uniRow("x \(nm) y", names: [nm])
}
// every code point where \w, Grapheme_Extend or Cf changes (and the one before), beside a
// keyword, an AWS key and a name: \b on either side of each
func klass(_ v: UInt32) -> Int {
    guard let u = Unicode.Scalar(v) else { return -1 }
    let p = u.properties, g = p.generalCategory
    let word = p.isAlphabetic || [.nonspacingMark, .spacingMark, .enclosingMark, .decimalNumber, .connectorPunctuation].contains(g) || v == 0x200C || v == 0x200D
    return (word ? 1 : 0) | (p.isGraphemeExtend ? 2 : 0) | (g == .format ? 4 : 0)
}
var edges: [Unicode.Scalar] = []
var prevKlass = klass(0x7F)
for v in UInt32(0x80)...0x10FFFF {
    let k = klass(v)
    if k < 0 { continue }
    if k != prevKlass {
        if let b = Unicode.Scalar(v - 1), edges.last != b { edges.append(b) }
        edges.append(Unicode.Scalar(v)!)
    }
    prevKlass = k
}
for start in stride(from: 0, to: edges.count, by: 40) {
    var s = ""
    for c in edges[start..<min(start + 40, edges.count)] {
        s += "a\(c)token=1\n \(c)token=2\nAKIAIOSFODNN7EXAMPLE\(c)\n\(c)dj\(c)\n"
    }
    uniRow(s, names: ["dj"])
}
// every code point case mapping or folding changes: a name made with it against its mappings, and
// names made with its mappings against it
var cased: [Unicode.Scalar] = []
for v in UInt32(0x80)...0x10FFFF {
    if let u = Unicode.Scalar(v), u.properties.changesWhenCaseFolded || u.properties.changesWhenCaseMapped { cased.append(u) }
}
func variants(_ c: Unicode.Scalar) -> [String] {
    let s = String(c)
    return [s, s.lowercased(), s.uppercased(), s.capitalized, s.folding(options: .caseInsensitive, locale: nil)]
}
for start in stride(from: 0, to: cased.count, by: 18) {
    let group = cased[start..<min(start + 18, cased.count)]
    uniRow(group.map { c in variants(c).map { "q\($0)z" }.joined(separator: " ") }.joined(separator: "\n"), names: group.map { "q\($0)z" })
}
for start in stride(from: 0, to: cased.count, by: 9) {
    let group = cased[start..<min(start + 9, cased.count)]
    uniRow(group.map { "q\($0)z" }.joined(separator: " "), names: group.flatMap { c in [String(c).lowercased(), String(c).uppercased()].map { "q\($0)z" } })
}
// random texts from fragments outside ASCII (their own sequence: the other tables' don't move)
var urng: UInt64 = 0x2545F4914F6CDD1D
func urnd(_ n: Int) -> Int { urng ^= urng << 13; urng ^= urng >> 7; urng ^= urng << 17; return Int(urng % UInt64(n)) }
let uniFrags = ["password", "passwd", "pa", "ß", "ẞ", "ſ", "ss", "word", "wd", "session", "seß", "ion", "_id", "acce", "_key", "token", "=", ":",
                " ", "\"", "?", "&", "\u{301}", "\u{200B}", "\u{AD}", "\u{200D}", "።", "é", "e", "Straße", "STRASSE", "strass", "Anna", " ",
                "ﬆ", "ﬁ", "İ", "K", "AKIAIOSFODNN7EXAMPLE", "Bearer ", "abcdefghij", "dj", "DJ", "\n", "x", "-", "٣", "‿", "/Users/dj", "/Users/\u{212A}im"]
for _ in 0..<1500 {
    var s = ""
    for _ in 0..<(1 + urnd(16)) { s += uniFrags[urnd(uniFrags.count)] }
    uniRow(s, names: ["Anna Straße", "dj", "Kim"])
}
table("SV_MASK_UNICODE", uniRows)

// Where the core masks more than 1.3.0 by design (vectors.c pins the C result beside Swift's,
// which must still differ): a name Swift counts as one character (a spacing mark, Hangul jamo)
// that the core's count takes as two.
var moreRows: [([[UInt8]], [UInt8])] = []
for (s, home, names) in [("a कि b", "/Users/dj", ["कि"]), ("a \u{1100}\u{1161} b", "/Users/dj", ["\u{1100}\u{1161}"])] {
    moreRows.append(([s2b(s), s2b(home)] + names.map(s2b), s2b(redactWith(s, home: home, names: names, keep: []))))
}
table("SV_MASK_MORE", moreRows)

// MARK: - the helper's lines (Prepare.swift)

func dumpTrack(_ t: TrackLine) -> String {
    "track|\(esc(t.file))|\(esc(t.result))|\(esc(t.why))|\(esc(t.access))|\(esc(t.folder))|\(t.entries)|\(t.written)|\(t.already)|\(t.missing)|\(t.refused)|\(d17(t.seconds))|\(d17(t.gb))|\(esc(t.name))"
}
func dumpRun(_ r: RunLine?) -> String {
    guard let r = r else { return "nil" }
    switch r {
    case .progress(let f, let files, let c, let cs): return "progress|\(f)|\(files)|\(c)|\(cs)"
    case .track(let t): return dumpTrack(t)
    case .room(let n, let gb): return "room|\(n)|\(d17(gb))"
    case .done(let files, let st, let gb, let err): return "done|\(files)|\(st.map(esc) ?? "nil")|\(d17(gb))|\(err.map(esc) ?? "nil")"
    }
}
func dumpEstimate(_ e: Estimate?) -> String {
    guard let e = e else { return "nil" }
    var s = "est|\(e.refused.map(esc) ?? "nil")|\(e.ready)|\(e.prepare)|\(e.skipped)|\(e.unchecked)|\(e.asking)|\(e.fitFiles)|\(d17(e.timeS))|\(d17(e.needGB))|\(d17(e.keptGB))|\(d17(e.shareGB))|\(d17(e.maxGB))|\(d17(e.maxGBNeeded))|\(e.fits)|\(e.diskOK)|\(e.folders.map(esc).joined(separator: ","))"
    for t in e.tracks { s += "\n" + dumpTrack(t) }
    return s
}

let lineFixed: [String] = [
    "{\"at\":0,\"files\":3,\"chunk\":1,\"chunks\":20}", "{\"file\":\"/Users/dj/Music/a.mp3\",\"result\":\"prepared\",\"written\":12,\"already\":3,\"silent\":0,\"refused\":1,\"how\":\"decoded\"}",
    "{\"file\":\"/Volumes/GLS/b.m4a\",\"result\":\"skipped\",\"why\":\"can't be read\",\"access\":\"asking\",\"folder\":\"Documents\"}",
    "{\"room\":12,\"cache_gb\":18.250}", "{\"done\":3,\"stopped\":\"stopped\",\"runs\":40,\"run_s\":99.5,\"kept\":\"123\",\"until\":1700000000,\"kept_gb\":2.125}",
    "{\"done\":2,\"runs\":1,\"run_s\":1.0,\"kept_error\":\"\",\"manifest_error\":\"disk full\"}", "{\"done\":2,\"kept_error\":\"x\",\"manifest_error\":\"y\"}",
    "{\"done\":1,\"stopped\":5}", "{\"done\":1,\"stopped\":null}", "{\"done\":\"7\"}", "{\"done\":true,\"kept_gb\":true}", "{\"chunk\":null}", "{\"file\":null}",
    "{\"file\":12,\"result\":3.5,\"entries\":\"9\",\"written\":2.9,\"already\":-2.9,\"seconds\":\"1\",\"gb\":false}", "{\"file\":\"x\",\"entries\":1e21,\"missing\":-1e300,\"refused\":9223372036854775807}",
    "{\"file\":\"x\",\"entries\":18446744073709551615}", "{\"file\":\"x\",\"entries\":-9223372036854775808}", "{\"file\":\"x\",\"seconds\":1e-400}",
    "{\"file\":\"x\",\"seconds\":1e400}", "{\"file\":\"x\",\"why\":0.30000000000000004}", "{\"file\":\"x\",\"why\":1234567890123456.7}", "{\"file\":\"x\",\"why\":-0.0}",
    "{\"file\":\"x\",\"why\":5e-324}", "{\"file\":\"x\",\"why\":1E2}", "{\"file\":\"x\",\"why\":true}", "{\"file\":\"x\",\"why\":[1]}", "{\"file\":\"x\",\"why\":{\"a\":1}}",
    "{\"file\":\"a\",\"file\":\"b\"}", "{\"file\":\"a\",}", "{\"file\":\"a\",,}", "{,}", "{}", "{\"file\":\"a\"} ", " {\"file\":\"a\"}", "{\"file\":\"a\"}x", "{\"file\":\"a\"}\r",
    "{\"file\":\"\\u0000x\\n\\t\\\\\\\"\\/\\b\\f\\r\"}", "{\"file\":\"\\ud83d\\ude00\"}", "{\"file\":\"\\ud83d\"}", "{\"file\":\"\\ude00\"}", "{\"file\":\"\\x\"}",
    "{\"file\":\"a\tb\"}", "{\"file\":\"\u{7f}é“x”\"}", "{\"file\":\"/a/b/\"}", "{\"file\":\"/\"}", "{\"file\":\"//\"}", "{\"file\":\"a//\"}", "{\"file\":\"\"}",
    "{\"file\":01}", "{\"file\":1.}", "{\"file\":.5}", "{\"file\":-}", "{\"file\":+1}", "{\"file\":1e}", "{\"file\":tru}", "{\"file\":nul}", "{\"file\":\"x\"", "{\"file\" \"x\"}",
    "{\"\\u0066ile\":\"x\"}", "{\"file\":\"x\",\"result\":\"ready\",\"entries\":40,\"gb\":0.112}", "{\"file\":\"x\",\"result\":\"prepare\",\"seconds\":245.0,\"entries\":22,\"gb\":0.061,\"missing\":3}",
    "{\"estimate\":2,\"ready\":1,\"prepare\":1,\"skipped\":0,\"entries\":62,\"runs\":22,\"time_s\":55,\"need_gb\":0.061,\"kept_gb\":1.250,\"share_gb\":15.000,\"max_gb\":20,\"cache_gb\":3.5,\"free_gb\":100.0,\"fits\":true,\"fit_files\":2,\"max_gb_needed\":1.75,\"disk_ok\":true,\"asking\":0,\"folders\":[\"Documents\",\"Desktop\"]}",
    "{\"refused\":\"5 playlists are kept already\"}", "{\"refused\":5}", "{\"playlist\":\"12\",\"parent\":\"0\",\"name\":\"Friday\",\"kind\":\"playlist\",\"tracks\":31}",
    "{\"playlist\":7,\"name\":\"F\",\"kind\":\"folder\",\"tracks\":\"x\"}", "{\"playlist\":\"\"}", "not json", "", "{", "{\"at\":1}", "{\"chunk\":1,\"file\":\"x\",\"room\":1,\"done\":1}",
    "{\"file\":\"x\",\"room\":1}", "{\"room\":1,\"done\":1}",
    String(repeating: "{\"a\":", count: 511) + "1" + String(repeating: "}", count: 511), String(repeating: "{\"a\":", count: 512) + "1" + String(repeating: "}", count: 512),
    "{\"a\":" + String(repeating: "[", count: 511) + String(repeating: "]", count: 511) + ",\"file\":\"deep\"}",
    "{\"a\":" + String(repeating: "[", count: 512) + String(repeating: "]", count: 512) + ",\"file\":\"deep\"}",
    "{\"a\":" + String(repeating: "[", count: 513) + String(repeating: "]", count: 513) + ",\"file\":\"deep\"}",
    "{\"a\":[1,]}", "{\"a\":[,]}", "{\"a\":[1,,2]}", "{\"a\":[\"x\",]}", "{\"a\":{\"b\":1,}}", "{\"a\":{,}}",
]
let lineKeys = ["file", "result", "why", "entries", "written", "already", "missing", "refused", "seconds", "gb", "chunk", "at", "files", "chunks", "room",
                "cache_gb", "done", "stopped", "kept_gb", "kept_error", "manifest_error", "estimate", "ready", "prepare", "skipped", "fits", "folders", "playlist",
                "name", "kind", "tracks", "parent", "access", "folder"]
let lineVals = ["\"x\"", "\"\"", "\"é\"", "0", "1", "-1", "2.5", "1e3", "true", "false", "null", "[]", "[\"a\"]", "[1]", "{}", "12345678901", "-0", "0.001",
                "\"prepare\"", "\"ready\"", "\"skipped\"", "\"prepared\"", "\"stopped\"", "\"unchecked\"", "\"75% share\"", "\"\\u00e9\""]
var lineCases = lineFixed
for _ in 0..<700 {
    var parts: [String] = []
    for _ in 0..<(1 + rnd(6)) { parts.append("\"\(lineKeys[rnd(lineKeys.count)])\":\(lineVals[rnd(lineVals.count)])") }
    var s = "{" + parts.joined(separator: ",") + "}"
    if rnd(10) == 0 { s.insert(Character([" ", ",", "x", "\t", "}"][rnd(5)]), at: s.index(s.startIndex, offsetBy: rnd(s.count))) }
    lineCases.append(s)
}
var runRows: [([[UInt8]], [UInt8])] = [], plRows: [([[UInt8]], [UInt8])] = [], tlRows: [([[UInt8]], [UInt8])] = []
for s in lineCases {
    runRows.append(([s2b(s)], s2b(dumpRun(parseRunLine(Substring(s))))))
    let p = parsePlaylists(s).first
    plRows.append(([s2b(s)], s2b(p.map { "pl|\(esc($0.id))|\(esc($0.parent))|\(esc($0.name))|\(esc($0.kind))|\($0.tracks)" } ?? "nil")))
    tlRows.append(([s2b(s)], s2b(parseTrackList(s).first.map { esc($0.file) } ?? "nil")))
}
// invalid UTF-8, as PrepareRun reads a line: String(decoding:as:) first
for bytes: [UInt8] in [[0x7B, 0x22, 0x66, 0x69, 0x6C, 0x65, 0x22, 0x3A, 0x22, 0xFF, 0xC3, 0x22, 0x7D], [0x7B, 0x22, 0x66, 0x69, 0x6C, 0x65, 0x22, 0x3A, 0x22, 0xE2, 0x82, 0x41, 0xF0, 0x9F, 0x22, 0x7D],
                       [0x7B, 0x22, 0x66, 0x69, 0x6C, 0x65, 0x22, 0x3A, 0x22, 0xED, 0xA0, 0x80, 0xC0, 0xAF, 0xF4, 0x90, 0x80, 0x80, 0x22, 0x7D], [0xFF, 0x7B, 0x7D]] {
    runRows.append(([bytes], s2b(dumpRun(parseRunLine(Substring(String(decoding: bytes, as: UTF8.self)))))))
}
table("SV_RUN_LINE", runRows)
table("SV_PLAYLIST", plRows)
table("SV_TRACK_LIST", tlRows)

// estimates: whole outputs (lines joined with "\n")
let estFixed: [[String]] = [
    [], ["{\"file\":\"a\",\"result\":\"ready\"}"], ["{\"estimate\":0}"],
    ["{\"file\":\"/m/a.mp3\",\"result\":\"ready\",\"entries\":40,\"gb\":0.112}", "{\"file\":\"/m/b.mp3\",\"result\":\"prepare\",\"seconds\":245.0,\"entries\":22,\"gb\":0.061}",
     "{\"file\":\"/m/c.mp3\",\"result\":\"prepare\",\"seconds\":3700.4,\"entries\":22,\"gb\":0.061,\"missing\":2}", "{\"file\":\"/m/d.mp3\",\"result\":\"skipped\",\"why\":\"gone\"}",
     "{\"estimate\":4,\"ready\":1,\"prepare\":2,\"skipped\":1,\"time_s\":3946,\"need_gb\":0.122,\"kept_gb\":1.250,\"share_gb\":15.000,\"max_gb\":20,\"fits\":true,\"fit_files\":4,\"max_gb_needed\":1.372,\"disk_ok\":true,\"asking\":0,\"folders\":[]}"],
    ["{\"file\":\"a\",\"result\":\"prepare\",\"seconds\":10}", "{\"refused\":\"no\"}", "{\"estimate\":1,\"ready\":9}"],
    ["{\"estimate\":1,\"ready\":1}", "{\"estimate\":2,\"prepare\":3,\"max_gb\":20.5,\"share_gb\":15.375,\"need_gb\":30.5,\"kept_gb\":0,\"max_gb_needed\":41.04,\"fit_files\":1,\"time_s\":59.9,\"fits\":false,\"disk_ok\":false}"],
    ["{\"estimate\":3,\"ready\":0,\"prepare\":0,\"skipped\":2,\"unchecked\":1,\"folders\":[\"Documents\",3]}"],
    ["{\"estimate\":3,\"ready\":2,\"prepare\":0,\"skipped\":0,\"unchecked\":1,\"share_gb\":0.5,\"max_gb\":0.6666666666666666,\"kept_gb\":0.0004}"],
    ["{\"estimate\":3,\"ready\":0,\"prepare\":1,\"skipped\":0,\"time_s\":30,\"need_gb\":0.9999,\"share_gb\":0.99951171875,\"max_gb\":1.3,\"max_gb_needed\":1.33,\"fit_files\":0}",
     "{\"file\":\"z\",\"result\":\"prepare\",\"seconds\":59.4}"],
]
var estCases = estFixed
let estLines = lineCases.filter { $0.contains("estimate") || $0.contains("file") || $0.contains("refused") }
for _ in 0..<200 { estCases.append((0..<(1 + rnd(5))).map { _ in estLines[rnd(estLines.count)] }) }
var estRows: [([[UInt8]], [UInt8])] = [], sumRows: [([[UInt8]], [UInt8])] = [], fitRows: [([[UInt8]], [UInt8])] = []
var stateRows: [([[UInt8]], [UInt8])] = [], noticeRows: [([[UInt8]], [UInt8])] = []
for c in estCases {
    let text = c.joined(separator: "\n")
    let e = parseEstimate(text)
    estRows.append(([s2b(text)], s2b(dumpEstimate(e))))
    stateRows.append(([s2b(text)], s2b(setState(e))))
    if let e = e {
        sumRows.append(([s2b(text)], s2b(estimateSummary(e))))
        for free in [0.0, 5.0, 1000.0] {
            let d = doesntFitChoices(e, freeGB: free)
            fitRows.append(([s2b(text), s2b(d17(free))], s2b(d.message + "|" + d.buttons.joined(separator: "|") + "|" + (d.raiseTo.map(d17) ?? "nil"))))
        }
        noticeRows.append(([s2b(text)], s2b(keptSetNotice(e) ?? "nil")))
    }
}
table("SV_ESTIMATE", estRows)
table("SV_SET_STATE", stateRows)
table("SV_EST_SUMMARY", sumRows)
table("SV_DOESNT_FIT", fitRows)
table("SV_KEPT_NOTICE", noticeRows)

// MARK: - words

let doubles: [Double] = [0, -0.0, 0.1, 0.5, 0.999, 1, 1.05, 1.25, 2.675, 20, 20.5, 37.3, 59.4, 59.9, 60, 89.9, 90, 3599, 3570, 3600, 5432.1, 86399, 1e-5, 1e-4, 0.0001234,
                         1.0 / 3, 123456789.125, 1e15 + 0.5, 9007199254740992, 9007199254740994, 1e16, 1.5e16, 12345678901234568, -0.5, -20.5, 0.00048828125,
                         0.0009765625, 1023.9999, 0.95, 0.94999, 4.35, 0.05, 2.5e-3, 100.0, 4e17]  // beyond Int, Swift traps (durationText, configNumber)
var numRows: [([[UInt8]], [UInt8])] = []
for x in doubles {
    numRows.append(([s2b(d17(x))], s2b(durationText(x) + "|" + sizeText(x) + "|" + configNumber(x) + "|" + String(x))))
}
table("SV_NUMBERS", numRows)

var byteRows: [([[UInt8]], [UInt8])] = []
for b: Int64 in [0, -1, 1, 1_048_575, 1_048_576, 3_000_000, 107_374_182, 107_374_183, 1_073_741_824, 13_314_398_617, 1 << 40] {
    byteRows.append(([s2b(String(b))], s2b(aboutSize(b) + "|" + sizeWords(b) + "|" + StemsSize.measured(b).text + "|" + StemsSize.measuring.text + StemsSize.failed.text + StemsSize.notConnected.text)))
}
table("SV_BYTES", byteRows)

var floorRows: [([[UInt8]], [UInt8])] = []
for (free, disk): (Int64?, Int64?) in [(nil, nil), (0, nil), (10, 100), (49_999_999_999, nil), (50_000_000_000, nil), (9, 100), (1_000_000_000, 499_999_999_999),
                                        (25_600_000_000, 256_000_000_000), (25_400_000_000, 254_000_000_001), (1, 0), (1, -5)] {
    let floor = cachePausedFloor(free: free, disk: disk)
    let tipFloor = cacheFreeFloor(disk: disk)
    floorRows.append(([s2b(free.map(String.init) ?? "nil"), s2b(disk.map(String.init) ?? "nil")],
                      s2b("\(tipFloor)|\(floor.map(String.init) ?? "nil")|\(cacheFloorGB(tipFloor))|" + cachePausedTip(tipFloor) + "|" + cachePausedTip(tipFloor, drive: "GLS")
                          + "|" + cachePausedAtInstall(tipFloor) + "|" + cachePausedAtInstall(tipFloor, drive: "My \"Drive\""))))
}
table("SV_FLOOR", floorRows)

// tracks' rows: result, note
var trackRows: [([[UInt8]], [UInt8])] = []
for r in ["", "ready", "prepare", "skipped", "prepared", "stopped", "unchecked", "weird"] {
    for (w, a, rf, miss, why, sec) in [(0, 0, 0, 0, "", 10.0), (0, 0, 2, 0, "", 70.0), (3, 0, 0, 0, "", 3599.0), (0, 4, 0, 2, "", 0.0), (2, 2, 1, 0, "", 0.0), (1, 0, 0, 0, "gone", 5.0)] {
        let t = TrackLine(file: "/m/x.mp3", result: r, why: why, entries: 1, written: w, already: a, missing: miss, refused: rf, seconds: sec, gb: 0.1)
        var note = t.why
        if note.isEmpty && t.result == "prepare" && t.missing > 0 { note = "\(t.missing) of its stems were removed from the cache" }
        if note.isEmpty && t.result == "prepare" { note = durationText(t.seconds) }
        if note.isEmpty && t.result == "prepared" { note = preparedNote(t) }
        trackRows.append(([s2b(r), s2b(String(w)), s2b(String(a)), s2b(String(rf)), s2b(String(miss)), s2b(why), s2b(d17(sec))],
                          s2b(resultText(t, checking: true) + "|" + resultText(t, checking: false) + "|" + preparedNote(t) + "|" + note)))
    }
}
table("SV_TRACK_ROW", trackRows)

var runTextRows: [([[UInt8]], [UInt8])] = []
for (files, skipped, stopped, err) in [(0, 0, nil, nil), (1, 0, nil, nil), (2, 1, "stopped", nil), (3, 0, "past the 75% share", "x"), (5, 2, "the disk is full", nil)] as [(Int, Int, String?, String?)] {
    for status: Int32 in [0, 1] {
        for gotDone in [false, true] {
            for partial in [false, true] {
                runTextRows.append(([s2b(String(files)), s2b(String(skipped)), s2b(stopped ?? "nil"), s2b(err ?? "nil"), s2b(String(status)), s2b(gotDone ? "1" : "0"), s2b(partial ? "1" : "0")],
                                    s2b(lastRunLine(files: files, skipped: skipped, stopped: stopped, error: err) + "|"
                                        + runEndTitle("Fri “day”", status: status, gotDone: gotDone, stopped: stopped, finished: files, skipped: skipped, error: err, partial: partial))))
            }
        }
    }
}
table("SV_RUN_TEXTS", runTextRows)

// MARK: - lights and the location (Controller.swift, Location.swift)

var lightRows: [([[UInt8]], [UInt8])] = []
for installed in [false, true] {
    for plusOn in [false, true] {
        for chosen in [false, true] {
            for rbModel in [false, true] {
                for models in [nil, [String](), ["aaa", "bbb"]] as [[String]?] {
                    for model in [nil, "aaa", "ccc"] as [String?] {
                        for engine in ["", "0002"] {
                            for paused in [nil, Int64(26_000_000_000)] as [Int64?] {
                                for loc in [nil, "Stems Cache is still checking this Mac."] as [String?] {
                                    for drive in [nil, "GLS"] as [String?] {
                                        let l = cacheLight(installed: installed, plusOn: plusOn, chosen: chosen, rekordboxModel: rbModel, bridgeModels: models, model: model,
                                                           engine: engine, pausedFloor: paused, location: loc, drive: drive)
                                        let args = [installed, plusOn, chosen, rbModel].map { s2b($0 ? "1" : "0") }
                                            + [s2b(models.map { $0.joined(separator: ",") } ?? "nil"), s2b(model ?? "nil"), s2b(engine), s2b(paused.map(String.init) ?? "nil"),
                                               s2b(loc ?? "nil"), s2b(drive ?? "nil")]
                                        lightRows.append((args, s2b("\(l.look)|\(l.tip)|\(lightDrawing(l.look).description)")))
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
lightRows.append(([s2b("plus")], s2b(plusLight(installed: true).tip + "|" + plusLight(installed: false).tip + "|\(plusLight(installed: true).look)|\(plusLight(installed: false).look)")))
table("SV_CACHE_LIGHT", lightRows)

let mounts = [Mount(path: "/", local: true, readOnly: false), Mount(path: "/Volumes/GLS", local: true, readOnly: false), Mount(path: "/Volumes/My \"Drive\"", local: true, readOnly: false)]
var locRows: [([[UInt8]], [UInt8])] = []
let statuses: [(String, LocationStatus)] = [("thisMac", .thisMac), ("unreadable", .configUnreadable), ("malformed", .configMalformedLocation),
                                            ("old|/Volumes/GLS/RB Stems Plus Cache.noindex", .bridgeTooOld(dir: "/Volumes/GLS/RB Stems Plus Cache.noindex", id: "x"))]
    + ["available", "notConnected", "folderMissing", "markerMissingOrWrong", "link", "network", "readOnly", "noPermission"].map { p in
        ("chosen|/Volumes/My \"Drive\"/DJ/RB Stems Plus Cache.noindex|" + p, .chosen(dir: "/Volumes/My \"Drive\"/DJ/RB Stems Plus Cache.noindex", id: "x", probe: LocationProbe(rawValue: p)!))
    }
let bridges: [BridgeState?] = [nil] + ["pending", "available", "unavailable"].flatMap { st in
    ["", "disabled", "config", "not-connected", "folder", "marker", "read-only", "no-permission", "not-local", "io"].flatMap { r in
        ["", "/Volumes/GLS/RB Stems Plus Cache.noindex"].map { BridgeState(pid: 1, start: 2, location: $0, state: st, reason: r) }
    }
}
for (name, s) in statuses {
    for b in bridges {
        for running in [false, true] {
            let t = locationLight(s, bridge: b, rekordboxRunning: running, mounts: mounts) ?? "nil"
            locRows.append(([s2b(name), s2b(b.map { "\($0.state)|\($0.reason)|\($0.location)" } ?? "nil"), s2b(running ? "1" : "0")], s2b(t)))
        }
    }
}
table("SV_LOCATION_LIGHT", locRows)

// MARK: - prepareProblems, standardAccountLine, shortVersion, listed (Prepare.swift, Compat.swift)

var probRows: [([[UInt8]], [UInt8])] = []
for _ in 0..<600 {
    var f = PrepareFacts()
    f.appleSilicon = rnd(12) != 0; f.helperThere = rnd(12) != 0; f.rekordboxInstalled = rnd(12) != 0; f.cacheInstalled = rnd(10) != 0
    f.bridgeKeeps = rnd(4) != 0; f.ortThere = rnd(4) != 0; f.rekordboxOK = rnd(3) != 0; f.rekordboxVersion = ["7.2.19.0342", "", "7..2", "7.2"][rnd(4)]
    f.configUnreadable = rnd(5) == 0 ? "a quote" : nil; f.enabled = rnd(4) != 0
    f.cacheLook = [LightLook.on, .limited, .off][rnd(3)]
    f.cacheTip = ["", "Stems Cache is off.", "Stems Cache is turned off in config.ini (enabled=0).", "Reinstall Stems Cache: this installation can't keep prepared playlists."][rnd(4)]
    f.quality = [nil, 0, 1][rnd(3)]; f.plusOn = rnd(2) == 0; f.modelSHA = [nil, ourModel, "abc"][rnd(3)]
    if rnd(3) == 0 { f.ourModels.append("abc") }
    f.cacheFolder = rnd(3) == 0 ? nil : "/x"
    let args: [[UInt8]] = [f.appleSilicon, f.helperThere, f.rekordboxInstalled, f.cacheInstalled, f.bridgeKeeps, f.ortThere, f.rekordboxOK, f.enabled, f.plusOn,
                           f.cacheFolder != nil].map { s2b($0 ? "1" : "0") }
        + [s2b(f.rekordboxVersion), s2b(f.configUnreadable ?? "nil"), s2b("\(f.cacheLook)"), s2b(f.cacheTip), s2b(f.quality.map(String.init) ?? "nil"),
           s2b(f.modelSHA ?? "nil"), s2b(f.ourModels.joined(separator: ","))]
    probRows.append((args, s2b(prepareProblems(f).joined(separator: "\n"))))
}
table("SV_PREPARE_PROBLEMS", probRows)

var compatRows: [([[UInt8]], [UInt8])] = []
for v in ["7.2.19.0342", "7.2.19", "7.2", "", "7..2.", ".7.2.19.1", "7.2.190", "8"] {
    let l = listed(v, in: ["7.2.19", "7.2.18"]) ? "1" : "0"
    compatRows.append(([s2b(v)], s2b(shortVersion(v) + "|" + l)))
}
for (a, t, c) in [(true, true, true), (false, false, true), (false, true, true), (false, true, false)] {
    compatRows.append(([s2b("std"), s2b(a ? "1" : "0"), s2b(t ? "1" : "0"), s2b(c ? "1" : "0")], s2b(standardAccountLine(admin: a, cacheTraces: t, cacheChosen: c) ?? "nil")))
}
table("SV_COMPAT", compatRows)

// MARK: - the watcher (Watcher.swift, Actions.swift)

var wdRows: [([[UInt8]], [UInt8])] = []
for mo in [false, true] { for co in [false, true] { for eok in [false, true] { for rok in [false, true] { for pw in [false, true] {
    let d = watcherDialog(modelOff: mo, cacheOff: co, engine: "0003", rekordbox: "7.3.0", engineOK: eok, rekordboxOK: rok, plusWorks: pw)
    wdRows.append(([mo, co, eok, rok, pw].map { s2b($0 ? "1" : "0") }, s2b(d.title + "|" + d.message + "|" + d.buttons.joined(separator: "|"))))
} } } } }
table("SV_WATCHER_DIALOG", wdRows)

/// watcherRounds and watcherSettle with scripted answers: the calls and the log, in order
var roundRows: [([[UInt8]], [UInt8])] = []
for script in ["1a1", "1a0", "0", "1a1a1a1", "1a1a1x", "1a1r0", "1a1a1a1a1", "10", "1a0a"] {
    var answers = Array(script), trace = ""
    func next() -> Character { answers.isEmpty ? "0" : answers.removeFirst() }
    let n = watcherRounds(max: 3, check: { trace += "C"; return next() == "1" }, release: { trace += "R" },
                          again: { trace += "A"; let c = next(); return c == "1" || c == "a" || c == "x" || c == "r" },
                          retake: { trace += "T"; return next() != "0" }, log: { trace += "[\($0)]" })
    roundRows.append(([s2b(script)], s2b("\(n)|" + trace)))
}
table("SV_WATCHER_ROUNDS", roundRows)

var settleRows: [([[UInt8]], [UInt8])] = []
for script in ["u0b0o0h0", "b1b1b0o0h0", "u1o1u0b0o0h0", "o1o1o0u0b0o0h0", "h1", "b1" + String(repeating: "b1", count: 125) + "b1h0o0", "u0b0o0h1", "u1o0u0b1b0b0o0h0"] {
    var answers: [String] = stride(from: 0, to: script.count, by: 2).map { i in String(Array(script)[i..<i + 2]) }, trace = ""
    func ask(_ k: Character) -> Bool {
        // the next scripted answer for this question kind, else false
        if let i = answers.firstIndex(where: { $0.first == k }) { let a = answers.remove(at: i); return a.last == "1" }
        return false
    }
    let r = watcherSettle(updating: { trace += "u"; return ask("u") }, appBusy: { trace += "b"; return ask("b") }, rekordboxOpen: { trace += "o"; return ask("o") },
                          pause: { trace += "p\($0)" }, halfUninstalled: { trace += "h"; return ask("h") ? "uninstall-all" : nil },
                          inSession: { trace += "s" }, log: { trace += "[\($0)]" })
    settleRows.append(([s2b(script)], s2b("\(r)|" + trace)))
}
table("SV_WATCHER_SETTLE", settleRows)

var miscRows: [([[UInt8]], [UInt8])] = []
for (out, label) in [("\t\"com.x\" => disabled\n", "com.x"), ("\"com.x\" => true", "com.x"), ("\"com.x\" =>   enabled", "com.x"), ("\"com.xy\" => disabled", "com.x"),
                     ("x\n  \"com.x\"   =>  disabled  \n", "com.x"), ("", "com.x"), ("\"com.x\"=> disabled", "com.x"), ("\"com.x\" => disabled\n\"com.x\" => enabled", "com.x")] {
    miscRows.append(([s2b("launchd"), s2b(out), s2b(label)], s2b(launchdDisabled(out, label: label) ? "1" : "0")))
}
for p in [false, true] { for l in [false, true] { for d in [false, true] {
    miscRows.append(([s2b("off"), s2b(p ? "1" : "0"), s2b(l ? "1" : "0"), s2b(d ? "1" : "0")], s2b(watcherOff(plistThere: p, loaded: l, disabled: d) ? "1" : "0")))
} } }
for name in ["123", "ab_C-9"] { miscRows.append(([s2b("pattern"), s2b(name)], s2b(setRunPattern(name)))) }
// the launch retry of This Mac's deletion (Location.swift): every input, the status as .chosen(_, _, .available)
// or one representative of each other case
let tmStatuses: [(String, LocationStatus)] = [("avail", .chosen(dir: "/Volumes/GLS/C", id: "A", probe: .available)),
    ("gone", .chosen(dir: "/Volumes/GLS/C", id: "A", probe: .notConnected)), ("thismac", .thisMac),
    ("tooold", .bridgeTooOld(dir: "/Volumes/GLS/C", id: "A")), ("unreadable", .configUnreadable), ("malformed", .configMalformedLocation)]
for (name, st) in tmStatuses { for bl in [false, true] {
    for mf in [false, true] {
        miscRows.append(([s2b("tmcleanup"), s2b(name), s2b(bl ? "1" : "0"), s2b(mf ? "1" : "0")],
                         s2b(thisMacNeedsCleanup(status: st, bridgeLocation: bl, modelFolders: mf) ? "1" : "0")))
    }
    for fl in [false, true] { for rr in [false, true] {
        miscRows.append(([s2b("tmretry"), s2b(fl ? "1" : "0"), s2b(name), s2b(bl ? "1" : "0"), s2b(rr ? "1" : "0")],
                         s2b(mayRetryThisMacDeletion(flag: fl, status: st, bridgeLocation: bl, rekordboxRunning: rr) ? "1" : "0")))
    } }
} }
table("SV_WATCHER_MISC", miscRows)

// MARK: - the state files (State.swift, Location.swift)

let tmp = FileManager.default.currentDirectoryPath + "/build/core/vectors-tmp"
try? FileManager.default.createDirectory(atPath: tmp, withIntermediateDirectories: true)
var markerRows: [([[UInt8]], [UInt8])] = []
let markerTexts: [[UInt8]] = ["", "123", "123\n", "0\n5\n", "-5\n", "+7\n", " 42 \n1700000000\ninstall\nInstall Stems Cache\n", "42\n1700000000\n", "42\ninstall\nlabel\n",
                              "42\n99999999999999999999\ninstall\nlabel", "42\r\n17\r\ninstall\r\nlabel", "42\n17\r\ninstall\nlabel\n", "42\n\u{a0}17\u{3000}\n\tid\t\nlab el\n",
                              "42\n17\n\nlabel\n", "42\n17\nid\n", "2147483647\n", "2147483648\n", "42\n-17\nid\nl\n", "42\n١٧\nid\nl\n", "42x\n"].map { s2b($0) }
    + [[0x34, 0x32, 0x0A, 0xFF, 0x0A]]
for (i, bytes) in markerTexts.enumerated() {
    let p = tmp + "/busy-\(i)"
    try! Data(bytes).write(to: URL(fileURLWithPath: p))          // not atomic: no temporary folder elsewhere
    let m = readBusyMarker(p)
    let d = m.map { "\($0.pid)|\($0.start.map(String.init) ?? "nil")|\($0.action.map { esc($0.id) + "|" + esc($0.label) } ?? "nil|nil")" } ?? "nil"
    var live = ""
    if let m = m {
        for started in [nil, 1_699_999_999, 1_700_000_000, 1_700_000_001] as [Int?] {
            let w = BusyMarker(pid: m.pid, start: m.start, action: m.action, written: Date(timeIntervalSince1970: 1_700_000_000.7))
            let nw = BusyMarker(pid: m.pid, start: m.start, action: m.action, written: nil)
            live += (markerLive(w, startOf: { _ in started }) ? "1" : "0") + (markerLive(nw, startOf: { _ in started }) ? "1" : "0")
        }
    }
    markerRows.append(([bytes], s2b(d + "|" + live)))
}
table("SV_BUSY_MARKER", markerRows)

var bridgeRows: [([[UInt8]], [UInt8])] = []
let bridgeTexts: [[UInt8]] = ["", "pid=1\nstart=2\nstate=available\n", "pid=1\nstart=2\nstate=pending\nlocation=/Volumes/GLS/x\nreason=not-connected",
                              "pid=0\nstart=2\nstate=available", "pid=1\nstart=x\nstate=available", "pid=1\nstart=2\nstate=other", "pid=1\nstart=2\nstate=available\nstate=nope",
                              "start=2\npid=3\nstate=unavailable\nreason=a=b\nlocation=", "pid=1\r\nstart=2\nstate=available", "pid=1\nstart=-2\nstate=available\n=x\nnoeq",
                              "pid=+1\nstart=+2\nstate=available\nlocation=/V/é"].map { s2b($0) }
    + [[0x70, 0x69, 0x64, 0x3D, 0x31, 0x0A, 0x73, 0x74, 0x61, 0x72, 0x74, 0x3D, 0x32, 0x0A, 0x73, 0x74, 0x61, 0x74, 0x65, 0x3D, 0x61, 0x76, 0x61, 0x69, 0x6C, 0x61, 0x62, 0x6C, 0x65, 0x0A,
        0x6C, 0x6F, 0x63, 0x61, 0x74, 0x69, 0x6F, 0x6E, 0x3D, 0xFF, 0x0A]]
for bytes in bridgeTexts {
    let b = parseBridgeState(String(decoding: bytes, as: UTF8.self))
    let live = b.map { s in [("", 2), (s.location, 2), (s.location, 3)].map { pair in liveBridgeState(s, dir: pair.0, startOf: { _ in pair.1 }) != nil ? "1" : "0" }.joined() } ?? ""
    bridgeRows.append(([bytes], s2b(b.map { "\($0.pid)|\($0.start)|\(esc($0.location))|\(esc($0.state))|\(esc($0.reason))" } ?? "nil") + s2b("|" + live)))
}
table("SV_BRIDGE_STATE", bridgeRows)
try? FileManager.default.removeItem(atPath: tmp)

// MARK: - the action dialogs' texts (Actions.swift, LocationActions.swift, Location.swift, Update.swift, Admin.swift)

var dlgRows: [([[UInt8]], [UInt8])] = []
for label in ["Install Demucs v4", "Install Stems Cache", "Reinstall Demucs v4 + Stems Cache", "Reinstall Stems Cache", "Reinstall Demucs v4"] {
    for reinstall in [false, true] { for paused in [nil, Int64(26_000_000_000)] as [Int64?] { for drive in [nil, "GLS"] as [String?] {
        dlgRows.append(([s2b("install"), s2b(label), s2b(reinstall ? "1" : "0"), s2b(paused.map(String.init) ?? "nil"), s2b(drive ?? "nil")],
                        s2b(installNote(label, reinstall: reinstall, paused: paused, drive: drive))))
    } } }
}
for label in ["Uninstall Demucs v4", "Uninstall Stems Cache"] { dlgRows.append(([s2b("uninstall"), s2b(label)], s2b(uninstallNote(label)))) }
for oc in [false, true] { for ns in [false, true] { for stems in ["", " (1.2 GB)"] { for drive in [nil, "GLS"] as [String?] { for conn in [false, true] {
    dlgRows.append(([s2b("all"), s2b(oc ? "1" : "0"), s2b(ns ? "1" : "0"), s2b(stems), s2b(drive ?? "nil"), s2b(conn ? "1" : "0")],
                    s2b(uninstallEverythingMessage(othersCache: oc, noSavedModel: ns, stems: stems, drive: drive, driveConnected: conn))))
} } } } }
for (fi, fate) in [AppFate.deleted, .otherAccount, .cantDelete].enumerated() { for oc in [false, true] { for link in [false, true] { for left in [[], ["x.", "y."]] as [[String]] {
    dlgRows.append(([s2b("fate"), s2b(String(fi)), s2b(oc ? "1" : "0"), s2b(link ? "1" : "0"), s2b(left.joined(separator: ","))],
                    s2b(ownFilesRemovedText("About.", app: fate, othersCache: oc, cacheLink: link, left: left))))
} } } }
for drive in [nil, "GLS"] as [String?] { for conn in [false, true] { for size in ["", " (about 1.0 GB)"] {
    dlgRows.append(([s2b("removal"), s2b(drive ?? "nil"), s2b(conn ? "1" : "0"), s2b(size)], s2b(locationRemovalNote(drive: drive, connected: conn, size: size))))
} } }
for found in [nil, 0, 2_000_000_000] as [Int64?] { for thisMac: Int64 in [0, 50_000_000] { for old in [nil, "Old"] as [String?] {
    dlgRows.append(([s2b("switch"), s2b(found.map(String.init) ?? "nil"), s2b(String(thisMac)), s2b(old ?? "nil")],
                    s2b(switchMessage(drive: "GLS", found: found, thisMac: thisMac, oldDrive: old))))
} } }
for old in [nil, "Old"] as [String?] { dlgRows.append(([s2b("thismac"), s2b(old ?? "nil")], s2b(useThisMacMessage(oldDrive: old) + "|" + clearLocationMessage(drive: "GLS")))) }
for (i, r) in [LocationRefusal.internalDisk, .network, .readOnly, .timeMachine, .insideOurFolder, .link, .fileSystem, .path, .unreadable].enumerated() {
    dlgRows.append(([s2b("refusal"), s2b(String(i))], s2b(locationRefusal(r, drive: "My \"Drive\""))))
}
for result in ["failed app", "failed  sign\n", "failed", "failed x", "failed app extra", "Failed app", "failed\u{a0}swap", "failed copy-check\r\n", "ok",
               "failed staging", "failed download", "failed copy", "\nfailed\u{2028}swap\t", ""] {
    dlgRows.append(([s2b("update"), s2b(result)], s2b(updateFailedMessage(updateFailureReason(result)))))
}
for code: Int32 in [-2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12] {
    let p = Controller.rootProblem(code)
    let cr = cacheFailureReason(.failed(code))
    dlgRows.append(([s2b("root"), s2b(String(code))], s2b((p.map { $0.title + "|" + $0.body } ?? "nil") + "|" + cr)))
}
dlgRows.append(([s2b("removal-ok")], s2b(cacheFailureReason(.removed) + "|" + cacheFailureReason(.keptRootFolder))))
for name in ["install", "reinstall", "remove-root-folder", "uninstall", "x"] {
    let t = Controller.rootRunTexts(name)
    dlgRows.append(([s2b("runtexts"), s2b(name)], s2b(t.status + "|" + t.failedTitle)))
}
table("SV_DIALOG_TEXTS", dlgRows)

print("/* Generated by core/tests/gen-vectors.sh from today's Swift (app/Sources at the commit that")
print(" * wrote it): core/tests/swift/vectors.swift. Do not edit. */")
print("typedef struct { const char *a[20]; int n; const char *want; } swift_vec;\n")
print(out, terminator: "")
