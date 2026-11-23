// Preparing playlists' platform reads (Prepare.swift), with fake data: the bridge's kept=1,
// rekordbox's quality setting, the kept sets on disk and the run a set's check skips. The checks
// before preparing, the helper's lines and the words are the core's since M7's switch-over
// (core/tests/vectors.c against the frozen Swift, core/tests/flows_test.c).
import Foundation

func prepareTests() {
    // MARK: the bridge's line and rekordbox's setting
    let line = { (s: String) in Data(("xx rbstems bridge: yy rbstems-cache: " + s + "; zz").utf8) }
    check(bridgeSaysKept(data: line("rebuild=1 location=1 kept=1 pioneer=\(ourModel)")), "kept=1 before pioneer=: keeps")
    check(!bridgeSaysKept(data: line("rebuild=1 location=1 pioneer=\(ourModel)")), "no kept=1: doesn't keep")
    check(!bridgeSaysKept(data: line("rebuild=1 pioneer=kept=1")), "kept=1 after pioneer= isn't a token")
    check(!bridgeSaysKept(data: Data("rbstems-cache: kept=1;".utf8)), "without the bridge's own text: no")
    try? FileManager.default.createDirectory(atPath: tmp + "/prep", withIntermediateDirectories: true)
    let settings = tmp + "/prep/rekordbox3.settings"
    try? "<PROPERTIES><VALUE name=\"PartAnalysis\" val=\"1\"/><VALUE name=\"PartAnalysisQuality\" val=\"1\"/></PROPERTIES>".write(toFile: settings, atomically: true, encoding: .utf8)
    check(stemsQuality(settings) == 1, "PartAnalysisQuality read (not the setting named like it)")
    check(stemsQuality(tmp + "/prep/none") == nil, "no settings file: unknown")
    let qualityCases: [(String, Int?, String)] = [
        (#"<VALUE val="1" name="PartAnalysisQuality"/><VALUE name="Other" val="0"/>"#, 1, "reversed attributes use their own value"),
        (#"<VALUE name="PartAnalysisQuality"/><VALUE name="Other" val="1"/>"#, nil, "a missing value stays unknown"),
        (#"<VALUE name="PartAnalysisQuality" val="0""#, nil, "an unterminated element stays unknown"),
        (#"<VALUE othername="PartAnalysisQuality" val="1"/>"#, nil, "a lookalike attribute is ignored"),
        ("<VALUE name=\"PartAnalysisQuality\"" + String(repeating: " ", count: 65) + "val=\"1\"/>", 1, "attributes more than 64 bytes apart")
    ]
    for (xml, expected, label) in qualityCases {
        try? xml.write(toFile: settings, atomically: true, encoding: .utf8)
        check(stemsQuality(settings) == expected, "stemsQuality: \(label)")
    }

    // MARK: the kept sets on disk
    let kd = tmp + "/prep/kept"
    try? FileManager.default.createDirectory(atPath: kd, withIntermediateDirectories: true)
    let entry = ourModel + " " + String(repeating: "a", count: 64)
    let now = Date()
    try? "rbstemsplus-kept 1\nuntil \(Int(now.timeIntervalSince1970) + 3600)\n\(entry)\n\(entry)\n".write(toFile: kd + "/123.kept", atomically: true, encoding: .utf8)
    try? "rbstemsplus-manifest 1\nhelper 1\nmodel \(ourModel)\nplaylist 123\nrate 0 0 4\nfile 1 2 1 0 /m/a.flac\nfile 1 2 1 0 /m/b.flac\n".write(toFile: kd + "/123.manifest", atomically: true, encoding: .utf8)
    try? "rbstemsplus-kept 1\nuntil \(Int(now.timeIntervalSince1970) - 10)\n".write(toFile: kd + "/old.kept", atomically: true, encoding: .utf8)
    try? "nonsense".write(toFile: kd + "/bad.kept", atomically: true, encoding: .utf8)
    try? "x".write(toFile: kd + "/.hidden.kept", atomically: true, encoding: .utf8)
    try? "x".write(toFile: kd + "/no space.kept", atomically: true, encoding: .utf8)
    let sets = keptSets(kd, now: now)
    check(sets.map { $0.name } == ["123", "bad"] && sets[0].entries == 2 && sets[0].files == 2 && sets[0].until != nil && sets[1].until == nil,
          "kept sets: past their date, hidden and badly named ones left out; one the bridge can't read still listed: \(sets)")
    let past = Int(now.timeIntervalSince1970) - 10, future = past + 3600
    let malformedSets = [
        ("\(past)\n\(entry)\nG\(entry.dropFirst())\n", "expired date with a non-hex entry"),
        ("\(future)\n\(entry.replacingOccurrences(of: " ", with: ":"))\n", "entry with the wrong separator"),
        ("\(future)\n\(entry)", "entry without its final newline"),
        ("0\(future)\n", "date with a leading zero"),
        ("\(future).5\n", "fractional date"),
        ("\(future)", "date without its final newline"),
        ("1000000000000\n", "date longer than twelve digits")
    ]
    for (body, label) in malformedSets {
        try? ("rbstemsplus-kept 1\nuntil " + body).write(toFile: kd + "/broken.kept", atomically: true, encoding: .utf8)
        let broken = keptSets(kd, now: now).first { $0.name == "broken" }
        check(broken == KeptSet(name: "broken"), "kept sets: blank date for \(label)")
    }
    check(stopKeeping("123", dir: kd) && fileType(kd + "/123.kept") == nil && fileType(kd + "/123.manifest") == nil, "stop keeping: the set and its manifest go")
    check(!stopKeeping("../x", dir: kd), "stop keeping: a name with a slash is refused")
    // a set that can't be looked up (its folder can't be searched) isn't reported stopped (review SR5)
    try? "rbstemsplus-kept 1\nuntil \(Int(now.timeIntervalSince1970) + 3600)\n\(entry)\n".write(toFile: kd + "/124.kept", atomically: true, encoding: .utf8)
    chmod(kd, 0o600)
    let stoppedUnseen = stopKeeping("124", dir: kd)
    chmod(kd, 0o755)
    check(!stoppedUnseen && fileType(kd + "/124.kept") != nil, "stop keeping: a set that can't be looked up isn't reported gone")
    // ... nor one whose file can't be deleted (its folder isn't writable)
    chmod(kd, 0o555)
    let stoppedKept = stopKeeping("124", dir: kd)
    chmod(kd, 0o755)
    check(!stoppedKept && fileType(kd + "/124.kept") != nil, "stop keeping: a set that can't be deleted isn't reported gone")
    try? FileManager.default.removeItem(atPath: kd + "/124.kept")
    check(keptNameOK("133477215") && keptNameOK("a-b_C") && !keptNameOK("") && !keptNameOK("a b") && !keptNameOK(String(repeating: "a", count: 65)), "set names as the helper allows them")

    // MARK: the run a set's check skips (the watcher's prepared playlists' check is the core's
    // since M7's switch-over: core/tests/flows_test.c t_kept; its words: core/tests/vectors.c)
    check(setRunPattern("42") == "rbsp-prepare .*--until [^ ]+ --keep 42( |$)", "the run a set's check looks for")
}
