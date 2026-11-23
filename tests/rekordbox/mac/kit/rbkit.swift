// rbkit: the Mac driver's tools in the VM that need no permission (tests/rekordbox/README.md).
// Built on the host by vm-session.sh stage (xcrun swiftc; vm-session-test.sh builds it too).
//
//   rbkit midi serve DIR        the virtual MIDI device "RBSP Test" (a source rekordbox reads and a
//                               destination it writes its lights to), kept until "quit". DIR holds
//                               midi-ctl (a FIFO: "ID press NOTE TIMES GAPMS" or "ID quit" lines),
//                               midi-ack ("ID ok" / "ID err WHY" lines), midi-in.log (what rekordbox
//                               sent: "MS STATUS D1 D2", MS since 1970, hex bytes) and midi-ready
//                               (written once the endpoints exist: "ready PID")
//   rbkit midi ports            every MIDI source and destination ("source NAME", "destination NAME")
//   rbkit ocr PNG X Y W H [SCALE] [plain]   the text in that region of the screenshot (pixels), read
//                               by macOS's own text recognition (Vision); light text on rekordbox's
//                               dark background is inverted first (not with "plain"); SCALE 1-4
//                               (default 2) enlarges the region before it is read
//   rbkit match ROWS TITLE      the row (0-based index into the file ROWS, one name a line) a title
//                               reads as, or -1 (match-lib.ps1's rule: the closest name within 3 edits
//                               and at least 2 closer than any other row)
//   rbkit vote ROWS TITLE...    the row most readings point at: at least 2 votes and more than any
//                               other row, or -1
//   rbkit norm TEXT             the text as it is compared (lower case, letters and digits only)
//   rbkit ocrtest OUTDIR        Vision's self-test: rekordbox-like light-on-dark texts drawn into
//                               OUTDIR/ocrtest.png, then read back ("read N: TEXT" lines)
// Exit 0 done; 1 failed; 2 bad arguments.
import AppKit
import CoreMIDI
import Foundation
import Vision

let args = CommandLine.arguments
func fail(_ s: String, _ code: Int32 = 1) -> Never { FileHandle.standardError.write(Data(("rbkit: " + s + "\n").utf8)); exit(code) }
func usage() -> Never { fail("usage: rbkit midi serve DIR | midi ports | ocr PNG X Y W H [SCALE] [plain] | match ROWS TITLE | vote ROWS TITLE... | norm TEXT | ocrtest OUTDIR", 2) }

// MARK: - the title matching (match-lib.ps1's rules, as they are)

func norm(_ s: String) -> String { String(s.lowercased().unicodeScalars.filter { ("a"..."z").contains($0) || ("0"..."9").contains($0) }.map(Character.init)) }
func dist(_ a: String, _ b: String) -> Int {
    let x = Array(a), y = Array(b)
    if x.isEmpty { return y.count }
    if y.isEmpty { return x.count }
    var prev = Array(0...y.count), cur = [Int](repeating: 0, count: y.count + 1)
    for i in 1...x.count {
        cur[0] = i
        for j in 1...y.count { cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x[i - 1] == y[j - 1] ? 0 : 1)) }
        swap(&prev, &cur)
    }
    return prev[y.count]
}
func findRow(_ t: String, _ rows: [String]) -> Int {
    let k = norm(t)
    if k.isEmpty || rows.isEmpty { return -1 }
    var ds: [(j: Int, e: Int)] = []
    for (j, r) in rows.enumerated() { ds.append((j: j, e: dist(k, norm(r)))) }
    ds.sort { (a: (j: Int, e: Int), b: (j: Int, e: Int)) -> Bool in a.e != b.e ? a.e < b.e : a.j < b.j }
    if ds[0].e <= 3 && (ds.count < 2 || ds[1].e >= ds[0].e + 2) { return ds[0].j }
    return -1
}
func voteRow(_ ts: [String], _ rows: [String]) -> Int {
    var v: [Int: Int] = [:]
    for t in ts { let j = findRow(t, rows); if j >= 0 { v[j, default: 0] += 1 } }
    let s = v.sorted { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }
    if let top = s.first, top.value >= 2, s.count < 2 || s[1].value < top.value { return top.key }
    return -1
}
func readRows(_ path: String) -> [String] {
    guard let t = try? String(contentsOfFile: path, encoding: .utf8) else { fail("no rows file \(path)") }
    return t.split(separator: "\n", omittingEmptySubsequences: true).map(String.init)
}

// MARK: - text recognition (Vision)

func loadImage(_ path: String) -> CGImage {
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { fail("can't read the image \(path)") }
    return img
}
/// The region (pixels, from the top left) cut out, inverted unless plain, enlarged by scale.
func region(_ img: CGImage, _ r: CGRect, scale: Int, invert: Bool) -> CGImage {
    let box = r.intersection(CGRect(x: 0, y: 0, width: img.width, height: img.height))
    guard !box.isEmpty, let cut = img.cropping(to: box) else { fail("the region \(r) is outside the \(img.width)x\(img.height) image") }
    let w = Int(box.width) * scale, h = Int(box.height) * scale
    guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { fail("no drawing context") }
    ctx.interpolationQuality = .high
    ctx.draw(cut, in: CGRect(x: 0, y: 0, width: w, height: h))
    if invert {
        ctx.setBlendMode(.difference); ctx.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
    }
    guard let out = ctx.makeImage() else { fail("no image") }
    return out
}
func recognise(_ img: CGImage) -> String {
    let req = VNRecognizeTextRequest()
    req.recognitionLevel = .accurate
    req.usesLanguageCorrection = false   // file names, not words
    req.recognitionLanguages = ["en-US"] // Latin letters only (else "mp3" can come back with a Cyrillic "р")
    do { try VNImageRequestHandler(cgImage: img, options: [:]).perform([req]) } catch { fail("Vision: \(error)") }
    // top to bottom, then left to right, as one line
    let obs = (req.results ?? []).sorted { abs($0.boundingBox.midY - $1.boundingBox.midY) > 0.02 ? $0.boundingBox.midY > $1.boundingBox.midY : $0.boundingBox.minX < $1.boundingBox.minX }
    return obs.compactMap { $0.topCandidates(1).first?.string }.joined(separator: " ")
}

// MARK: - MIDI

func str(_ o: MIDIObjectRef, _ p: CFString) -> String {
    var s: Unmanaged<CFString>?
    return MIDIObjectGetStringProperty(o, p, &s) == noErr ? (s?.takeRetainedValue() as String? ?? "") : ""
}
func ports() {
    for i in 0..<MIDIGetNumberOfSources() { print("source " + str(MIDIGetSource(i), kMIDIPropertyDisplayName)) }
    for i in 0..<MIDIGetNumberOfDestinations() { print("destination " + str(MIDIGetDestination(i), kMIDIPropertyDisplayName)) }
}
func append(_ path: String, _ line: String) {
    let d = Data((line + "\n").utf8)
    if let h = FileHandle(forWritingAtPath: path) { h.seekToEndOfFile(); h.write(d); h.closeFile() }
    else { FileManager.default.createFile(atPath: path, contents: d, attributes: [.posixPermissions: 0o644]) }
}
func nowMs() -> Int64 { Int64(Date().timeIntervalSince1970 * 1000) }

func serve(_ dir: String) -> Never {
    let name = "RBSP Test" as CFString
    var client = MIDIClientRef(), src = MIDIEndpointRef(), dst = MIDIEndpointRef()
    guard MIDIClientCreateWithBlock("RBSP Kit" as CFString, &client, nil) == noErr else { fail("MIDIClientCreate failed") }
    guard MIDISourceCreateWithProtocol(client, name, ._1_0, &src) == noErr else { fail("MIDISourceCreate failed") }
    let inLog = dir + "/midi-in.log"
    let q = DispatchQueue(label: "rbkit.in")
    // what rekordbox sends (its lights): MIDI 1.0 channel voice messages in UMP words (type 2)
    let st = MIDIDestinationCreateWithProtocol(client, name, ._1_0, &dst) { list, _ in
        var lines: [String] = []
        for p in list.unsafeSequence() {
            let n = Int(p.pointee.wordCount)
            withUnsafeBytes(of: p.pointee.words) { raw in
                let w = raw.bindMemory(to: UInt32.self)
                for k in 0..<min(n, w.count) where w[k] >> 28 == 2 {
                    lines.append(String(format: "%lld %02X %02X %02X", nowMs(), (w[k] >> 16) & 0xFF, (w[k] >> 8) & 0x7F, w[k] & 0x7F))
                }
            }
        }
        if !lines.isEmpty { q.async { for l in lines { append(inLog, l) } } }
    }
    guard st == noErr else { fail("MIDIDestinationCreate failed") }
    // fixed unique IDs: rekordbox sees the same device after a restart of this daemon (a collision
    // with another device's ID is refused by CoreMIDI: then the ID it gave is kept)
    _ = MIDIObjectSetIntegerProperty(src, kMIDIPropertyUniqueID, 0x5242_5350)
    _ = MIDIObjectSetIntegerProperty(dst, kMIDIPropertyUniqueID, 0x5242_5351)
    func send(_ bytes: [UInt8]) {
        let w = UInt32(0x2000_0000) | UInt32(bytes[0]) << 16 | UInt32(bytes[1]) << 8 | UInt32(bytes[2])
        var list = MIDIEventList()
        let pkt = MIDIEventListInit(&list, ._1_0)
        _ = MIDIEventListAdd(&list, MemoryLayout<MIDIEventList>.size, pkt, 0, 1, [w])
        _ = MIDIReceivedEventList(src, &list)
    }
    let ctl = dir + "/midi-ctl", ack = dir + "/midi-ack"
    unlink(ctl)
    guard mkfifo(ctl, 0o622) == 0 else { fail("mkfifo \(ctl): \(String(cString: strerror(errno)))") }
    chmod(ctl, 0o622)
    // read-write: the FIFO never reports end of file between writers
    let fd = open(ctl, O_RDWR)
    guard fd >= 0 else { fail("open \(ctl)") }
    try? "ready \(getpid())\n".write(toFile: dir + "/midi-ready", atomically: true, encoding: .utf8)
    let fh = FileHandle(fileDescriptor: fd)
    var buf = Data()
    while true {
        let d = fh.availableData
        if d.isEmpty { usleep(50_000); continue }
        buf.append(d)
        while let nl = buf.firstIndex(of: 0x0A) {
            let line = String(decoding: buf[buf.startIndex..<nl], as: UTF8.self)
            buf.removeSubrange(buf.startIndex...nl)
            let f = line.split(separator: " ").map(String.init)
            guard f.count >= 2 else { continue }
            let id = f[0]
            switch f[1] {
            case "quit":
                append(ack, id + " ok"); unlink(ctl); unlink(dir + "/midi-ready")
                MIDIEndpointDispose(src); MIDIEndpointDispose(dst); exit(0)
            case "press":
                guard f.count == 5, let note = UInt8(f[2]), note < 128, let times = Int(f[3]), times > 0, times <= 200,
                      let gap = UInt32(f[4]), gap <= 5000 else { append(ack, id + " err bad press: " + line); continue }
                // as Pioneer hardware: note on 127, then note on 0 as the release (a note off is
                // held and repeated by rekordbox)
                for k in 0..<times {
                    send([0x90, note, 127]); usleep(60_000); send([0x90, note, 0])
                    if k < times - 1 { usleep(gap * 1000) }
                }
                append(ack, id + " ok")
            default: append(ack, id + " err unknown: " + line)
            }
        }
    }
}

// MARK: - Vision's self-test

func ocrtest(_ dir: String) {
    let texts = ["RBSP Tests (20 Tracks)", "01-mp3-cbr320", "15-wav-f32-96k"]
    let w = 900, h = 60 * texts.count
    guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { fail("no drawing context") }
    // rekordbox's look: light grey text on a near-black background
    ctx.setFillColor(CGColor(red: 0.08, green: 0.08, blue: 0.09, alpha: 1)); ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
    let ns = NSGraphicsContext(cgContext: ctx, flipped: false)
    NSGraphicsContext.saveGraphicsState(); NSGraphicsContext.current = ns
    for (i, t) in texts.enumerated() {
        let a: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 26), .foregroundColor: NSColor(white: 0.85, alpha: 1)]
        NSAttributedString(string: t, attributes: a).draw(at: NSPoint(x: 12, y: h - 60 * (i + 1) + 14))
    }
    NSGraphicsContext.restoreGraphicsState()
    guard let img = ctx.makeImage() else { fail("no image") }
    let url = URL(fileURLWithPath: dir + "/ocrtest.png")
    guard let d = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else { fail("can't write \(url.path)") }
    CGImageDestinationAddImage(d, img, nil)
    guard CGImageDestinationFinalize(d) else { fail("can't write \(url.path)") }
    for i in texts.indices { print("read \(i): " + recognise(region(img, CGRect(x: 0, y: 60 * i, width: w, height: 60), scale: 2, invert: true))) }
}

// MARK: - main

guard args.count >= 2 else { usage() }
switch args[1] {
case "midi":
    guard args.count >= 3 else { usage() }
    if args[2] == "ports" { ports() } else if args[2] == "serve", args.count == 4 { serve(args[3]) } else { usage() }
case "ocr":
    guard args.count >= 7, let x = Int(args[3]), let y = Int(args[4]), let w = Int(args[5]), let h = Int(args[6]) else { usage() }
    let scale = args.count >= 8 ? Int(args[7]) ?? 0 : 2
    guard (1...4).contains(scale) else { usage() }
    let plain = args.count >= 9 && args[8] == "plain"
    print(recognise(region(loadImage(args[2]), CGRect(x: x, y: y, width: w, height: h), scale: scale, invert: !plain)))
case "match":
    guard args.count == 4 else { usage() }
    print(findRow(args[3], readRows(args[2])))
case "vote":
    guard args.count >= 3 else { usage() }
    print(voteRow(Array(args.dropFirst(3)), readRows(args[2])))
case "norm":
    guard args.count == 3 else { usage() }
    print(norm(args[2]))
case "ocrtest":
    guard args.count == 3 else { usage() }
    ocrtest(args[2])
default: usage()
}
