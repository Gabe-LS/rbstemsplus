// Create Report (Report.swift), tried with fake data: the masking of secrets in logs and the
// summary (checksums the report needs stay readable), the last problem from the logs, the
// summary's short lines, and the issue's link (its layout and its length).
import Foundation

func reportTests() {
    // MARK: masking
    let sha = "0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772"      // the model's
    let commit = "4f89581c0ffee0d1e2c3b4a5968778695a4b3c2d"
    let unknownSha = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
    let summary = "model in rekordbox: \(sha) (308572524 bytes)\npayload.json: commit \(commit)\n"
    let keep = knownChecksums(summary)
    check(keep == [sha, commit], "the summary's checksums and commit are known: \(keep)")
    check(redact(summary, keep: keep) == summary, "redact keeps the summary's checksums readable")
    check(redact("ONNX Runtime: \(sha.uppercased())", keep: keep).contains(sha.uppercased()), "a known checksum in capitals stays too")
    check(redact("downloaded file has \(unknownSha)", keep: keep) == "downloaded file has <token>", "a 64-hex value the report didn't print is masked")
    check(redact("short ids stay: 0cc50d877629, abc123", keep: keep) == "short ids stay: 0cc50d877629, abc123", "short checksum prefixes stay")

    let cases: [(String, String, String)] = [
        ("signed in as dj.fake@example.com today", "signed in as <email> today", "an email address"),
        ("contact: First.Last+tag@mail.example.co.uk.", "contact: <email>.", "an email address with a subdomain"),
        ("Authorization: Bearer abcDEF123456ghiJKL789", "Authorization: Bearer <token>", "a bearer token"),
        ("jwt eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0In0.c2lnbmF0dXJl here", "jwt <token> here", "a JWT"),
        ("key AKIAIOSFODNN7EXAMPLE used", "key <token> used", "an AWS access key ID"),
        ("aws_secret_access_key=wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "aws_secret_access_key=<token>", "an AWS secret key"),
        ("token ghp_aBcDeFgHiJkLmNoPqRsTuVwXyZ0123456789", "token <token>", "a GitHub token"),
        ("api_key: sk-ant-api03-FAKEfake0123456789abcdef", "api_key: <token>", "an API key setting"),
        ("password=hunter22 next", "password=<token> next", "a password setting"),
        ("GET https://objects.example.com/a.zip?X-Amz-Credential=AKIDFAKE%2F2026&X-Amz-Signature=deadbeef&response-content-type=zip",
         "GET https://objects.example.com/a.zip?X-Amz-Credential=<token>&X-Amz-Signature=<token>&response-content-type=zip",
         "a URL's credential and signature parameters (not the others)"),
        ("https://example.com/cb?code=abc123&state=ok&session_id=s3ss10n", "https://example.com/cb?code=<token>&state=ok&session_id=<token>",
         "a URL's code and session parameters"),
        ("cookie=Ab3dEf9hIj2lMn5pQr8tUv1xYz4bCd7fGh0jKl", "cookie=<token>", "a cookie"),
        ("blob Zm9vYmFyQmF6UXV4MTIzNDU2Nzg5MGFiY2RlZg== end", "blob <token> end", "a long base64 run"),
        ("-----BEGIN PRIVATE KEY-----\nMIGHAgEAMBMGByqGSM49\n-----END PRIVATE KEY-----\nafter", "<private key>\nafter", "a private key"),
    ]
    for (input, want, what) in cases {
        let got = redact(input, keep: keep)
        check(got == want, "redact masks \(what): \(got)")
    }
    // what must stay as it is: the app's own log lines, UUIDs, code names in crash reports
    let stays = [
        "2026-10-07 10:00:00  admin run (install), attempt 1",
        "  FAILED: rekordbox's folder is missing | exit 3",
        "the password sheet: wrong password",
        "Incident Identifier: E621E1F8-C36C-495A-93FC-0C247A3E6E5F",
        "slice_uuid 4c4c44e1-5555-3144-a1b2-c3d4e5f60718",
        "$s13RB_Stems_Plus10ControllerC12createReportyyFTo + 120",
        "__CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE0_PERFORM_FUNCTION__",
        "Version 26.0 (Build 25A354), arm64, Mac15,3",
        "model 0cc50d877629, bridge 4f89581c0ffe, ONNX Runtime 1.18.0",
    ]
    for line in stays { check(redact(line, keep: keep) == line, "redact leaves alone: \(line)") }
    check(redact(home + "/Library/Logs/rbstemsplus/app.log") == "~/Library/Logs/rbstemsplus/app.log", "the home folder becomes ~")

    // MARK: the last problem: the last 3 hours, repeats collapsed, fixed ones left out
    let now = Date()
    let f = DateFormatter(); f.locale = Locale(identifier: "en_US_POSIX"); f.dateFormat = "yyyy-MM-dd HH:mm:ss"
    let hm = DateFormatter(); hm.locale = Locale(identifier: "en_US_POSIX"); hm.dateFormat = "HH:mm"
    func at(_ ago: TimeInterval) -> String { f.string(from: now.addingTimeInterval(-ago)) }
    func log(_ lines: [(TimeInterval, String)]) -> String { lines.map { "\(at($0.0))  \($0.1)" }.joined(separator: "\n") + "\n" }

    var p = lastProblem(appLog: "", bridgeLog: "", now: now)
    check(p.title == nil && p.text.isEmpty, "no logs: no last problem")
    p = lastProblem(appLog: log([(4 * 3600, "download failed: nothing changed")]), bridgeLog: "", now: now)
    check(p.title == nil && p.text.isEmpty, "a failure 4 hours old is left out")
    p = lastProblem(appLog: log([(3600, "download failed: nothing changed"), (3000, "state: rekordbox 7.2.19")]), bridgeLog: "", now: now)
    check(p.title == "download failed: nothing changed" && p.text == "Last problem:\n\(at(3600))  download failed: nothing changed",
          "a failure 1 hour old is in, with its date and time: \(p.text)")
    p = lastProblem(appLog: "download failed: nothing changed\n10:00 refused\n", bridgeLog: "", now: now)
    check(p.title == nil, "lines without a date are skipped")
    p = lastProblem(appLog: log([(600, "started: RB Stems Plus 1.0.0"), (500, "state: fine")]), bridgeLog: log([(400, "bridge loaded (x)")]), now: now)
    check(p.title == nil && p.text.isEmpty, "nothing failed: no block, the title stays plain")

    let failedRun: [(TimeInterval, String)] = [(1000, "admin run (install), attempt 1"), (990, "  FAILED: Operation not permitted")]
    p = lastProblem(appLog: log(failedRun + [(900, "admin run (install), attempt 2"), (890, "  FAILED: Operation not permitted"),
                                             (800, "admin run (install), attempt 3"), (790, "  FAILED: Operation not permitted")]), bridgeLog: "", now: now)
    check(p.title == "admin run (install) failed", "a failed admin run's title is its name: \(p.title ?? "nil")")
    check(p.text == "Last problem:\n\(at(990))  admin run (install) FAILED: Operation not permitted (3 times, last at \(hm.string(from: now.addingTimeInterval(-790))))",
          "three identical failures are one line with the count: \(p.text)")
    p = lastProblem(appLog: log(failedRun + [(900, "admin run (install), attempt 2"), (890, "  ok: installed")]), bridgeLog: "", now: now)
    check(p.title == nil && p.text.isEmpty, "a failure then a success of the same run: no block")
    p = lastProblem(appLog: log(failedRun + [(900, "admin run (uninstall), attempt 1"), (890, "  ok: removed")]), bridgeLog: "", now: now)
    check(p.title == "admin run (install) failed", "another run's success doesn't hide a failure")
    p = lastProblem(appLog: log(failedRun + [(900, "admin run (install), attempt 1"), (890, "  ok: installed"),
                                             (800, "admin run (install), attempt 1"), (790, "  FAILED: rekordbox's folder is missing")]), bridgeLog: "", now: now)
    check(p.title == "admin run (install) failed" && p.text == "Last problem:\n\(at(790))  admin run (install) FAILED: rekordbox's folder is missing",
          "fail, success, fail: only the last failure: \(p.text)")
    p = lastProblem(appLog: log([(700, "Demucs v4 not installed: the STEMS Engine isn't downloaded"), (600, "Demucs v4: installed (0cc50d877629)")]), bridgeLog: "", now: now)
    check(p.title == nil, "Demucs v4 not installed, then installed: no block")
    p = lastProblem(appLog: log([(700, "Demucs v4 not installed: the STEMS Engine isn't downloaded")]), bridgeLog: "", now: now)
    check(p.title == "Demucs v4 not installed: the STEMS Engine isn't downloaded", "Demucs v4 not installed is a problem")
    p = lastProblem(appLog: log([(700, "Stems Plus not installed: the STEMS Engine isn't downloaded"), (600, "Demucs v4: installed (0cc50d877629)")]), bridgeLog: "", now: now)
    check(p.title == nil, "1.1.0's Stems Plus not installed, then installed after the update: no block")
    p = lastProblem(appLog: log([(700, "Stems Plus not installed: the STEMS Engine isn't downloaded")]), bridgeLog: "", now: now)
    check(p.title == "Stems Plus not installed: the STEMS Engine isn't downloaded", "1.1.0's Stems Plus not installed is a problem")
    p = lastProblem(appLog: log([(700, "Stems Plus not installed: the STEMS Engine isn't downloaded"), (600, "Stems Plus: installed (0cc50d877629)")]), bridgeLog: "", now: now)
    check(p.title == nil, "1.1.0's Stems Plus not installed, then 1.1.0's installed: no block")
    p = lastProblem(appLog: log([(700, "Stems Cache not installed: rekordbox 7.3.0 isn't supported")]), bridgeLog: "", now: now)
    check(p.title == "Stems Cache not installed: rekordbox 7.3.0 isn't supported", "Stems Cache not installed is a problem")
    p = lastProblem(appLog: "", bridgeLog: log([(500, "ERROR no real ONNX Runtime library: rekordbox's analysis will fail; reinstall or uninstall"),
                                                (400, "bridge loaded (real library x)")]), now: now)
    check(p.title == nil, "a bridge loading error, then the bridge loaded: no block")
    p = lastProblem(appLog: "", bridgeLog: log([(500, "ERROR writing /x/y.flac"), (400, "bridge loaded (real library x)"), (300, "hit  abc")]), now: now)
    check(p.title == "Stems Cache: writing /x/y.flac" && p.text.hasPrefix("Last problem:\nbridge.log:\n\(at(500))  ERROR writing"),
          "another bridge error stays, with its own title: \(p.text)")
    p = lastProblem(appLog: "", bridgeLog: log((0..<6).map { (TimeInterval(600 - $0 * 10), "ERROR writing /x/\($0).flac") }), now: now)
    check(p.text.components(separatedBy: "\n").count == 5 && p.text.contains("5.flac") && !p.text.contains("2.flac"), "the bridge's newest 3 errors only")
    p = lastProblem(appLog: log((0..<8).map { (TimeInterval(900 - $0 * 10), "step \($0) failed") }), bridgeLog: "", now: now)
    check(p.text.components(separatedBy: "\n").count == 6 && p.text.contains("step 7 failed") && !p.text.contains("step 2 failed"),
          "app.log's newest 5 problems only")
    check(lastProblem(appLog: log([(60, "report: couldn't save it in ~/Desktop")]), bridgeLog: "", now: now).title == nil, "the report's own lines aren't problems")
    p = lastProblem(appLog: log([(60, String(repeating: "x", count: 100) + " failed")]), bridgeLog: "", now: now)
    check((p.title?.count ?? 0) <= 60 && p.title?.hasSuffix("…") == true, "a long title is cut to 60 characters")
    p = lastProblem(appLog: log([(60, "download failed for dj@example.com with token=abcdef0123456789")]), bridgeLog: "", now: now)
    let masked = redact(p.text)
    check(masked.contains("<email>") && masked.contains("token=<token>") && !masked.contains("example.com"), "the last problem is masked like the rest: \(masked)")

    // MARK: the summary's short lines
    check(appLine(version: "1.0.0", build: "7", plus: (.on, "Demucs v4 is on."), cache: (.limited, "Stems Cache stopped saving stems."))
          == "RB Stems Plus: 1.0.0 (7); lights: Demucs v4 green (Demucs v4 is on.), Stems Cache yellow (Stems Cache stopped saving stems.)",
          "the app line: version, build and the two lights")
    check(lightColour(.off) == "red", "an off light is red")
    check(macLine(os: "Version 26.0 (Build 25A354)", model: "Mac15,3", chip: "Apple M3", arch: "arm64", memory: 16 * 1_073_741_824, language: "en_IT")
          == "macOS: Version 26.0 (Build 25A354); Mac: Mac15,3, Apple M3, arm64, 16 GB memory; language: en_IT", "the Mac line")
    check(rekordboxLine(version: "7.2.19", running: true, rosetta: false, engine: "0002") == "rekordbox: 7.2.19, running: yes, native; STEMS Engine: 0002",
          "the rekordbox line, running natively")
    check(rekordboxLine(version: "7.2.19", running: true, rosetta: true, engine: "0002").contains("running: yes, under Rosetta"), "under Rosetta")
    check(rekordboxLine(version: nil, running: false, rosetta: nil, engine: "") == "rekordbox: not installed, running: no; STEMS Engine: not downloaded",
          "rekordbox not installed")
    check(translated(getpid()) == false, "this test isn't under Rosetta (P_TRANSLATED read from the kernel)")
    check(installedLine(plus: true, cache: false, chosenModel: true, chosenCache: true, modelSha: sha, modelSize: 308_572_524)
          == "installed: Demucs v4 yes, Stems Cache no; chosen: Demucs v4 yes, Stems Cache yes; model in rekordbox: 0cc50d877629 (308572524 bytes)",
          "the installed line, with the model's checksum prefix")
    let reportLines = [appLine(version: "1.0.0", build: "7", plus: plusLight(installed: true), cache: (.off, "Stems Cache is off.")),
                       installedLine(plus: true, cache: true, chosenModel: true, chosenCache: true, modelSha: sha, modelSize: nil)]
    check(reportLines.allSatisfy { $0.range(of: "(?<!RB )Stems Plus", options: .regularExpression) == nil && $0.contains("Demucs v4") },
          "the report names the model feature Demucs v4: \(reportLines)")
    check(diskLine(free: 120_500_000_000, disk: 994_700_000_000, floor: cacheFreeFloor(disk: 994_700_000_000))
          == "disk: 120.5 GB free of 994.7 GB; Stems Cache's floor: 50 GB", "the disk line")
    check(diskLine(free: 20_000_000_000, disk: 256_000_000_000, floor: cacheFreeFloor(disk: 256_000_000_000))
          == "disk: 20.0 GB free of 256.0 GB; Stems Cache's floor: 26 GB, under it: saving paused", "the disk line under the floor")
    check(watcherLine(plistThere: true, loaded: false, disabled: true, payload: "1.0.0", lastCheck: 3 * 3600 + 100)
          == "watcher: installed, turned off in Login Items; payload: 1.0.0; last update check: 3 h ago", "the watcher line, turned off")
    check(watcherLine(plistThere: false, loaded: false, disabled: nil, payload: nil, lastCheck: nil)
          == "watcher: not installed; payload: none; last update check: never", "the watcher line, nothing")
    check(signatureLine(valid: true, verifyOutput: "", signer: ["Identifier=com.pioneerdj.rekordboxdj", "TeamIdentifier=ABCDE12345"])
          == "rekordbox signature: valid; team ABCDE12345", "the signature line")
    check(signatureLine(valid: false, verifyOutput: "/Applications/rekordbox 7/rekordbox.app: a sealed resource is missing or invalid\nmore", signer: [])
          == "rekordbox signature: invalid: /Applications/rekordbox 7/rekordbox.app: a sealed resource is missing or invalid; team none", "an invalid signature")

    // MARK: the issue's link
    let facts = [appLine(version: "1.0.0", build: "7", plus: (.on, "Demucs v4 is on."), cache: (.off, "Stems Cache is off.")),
                 macLine(os: "Version 26.0 (Build 25A354)", model: "Mac15,3", chip: "Apple M3", arch: "arm64", memory: 17_179_869_184, language: "en_IT"),
                 rekordboxLine(version: "7.2.19", running: false, rosetta: nil, engine: "0002"),
                 installedLine(plus: true, cache: false, chosenModel: true, chosenCache: false, modelSha: sha, modelSize: 308_572_524),
                 diskLine(free: 1, disk: 2, floor: 3)]
    func body(_ u: URL?) -> String {
        URLComponents(url: u!, resolvingAgainstBaseURL: false)?.queryItems?.first { $0.name == "body" }?.value ?? ""
    }
    var u = issueURL(title: "Problem report", details: issueDetails(problem: "", facts: facts))
    let b = body(u)
    check(b.hasPrefix("### What happened?\n\n\n\n### Technical details\nPlease also drag in the report zip that RB Stems Plus just showed in Finder (it has the full logs).\n\n```\n"),
          "the user's text comes first, then the technical details: \(b.prefix(200))")
    check(b.hasSuffix(facts.joined(separator: "\n") + "\n```"), "the facts in the code block, in order")
    check(!b.contains("expect"), "no \"what did you expect\"")
    check(URLComponents(url: u!, resolvingAgainstBaseURL: false)?.queryItems?.first { $0.name == "title" }?.value == "Problem report", "the plain title")
    check(u!.absoluteString.hasPrefix(issuesURL + "?"), "the link opens a new issue")
    u = issueURL(title: "Problem report: a+b", details: "1+1")
    check(body(u).contains("1+1") && u!.absoluteString.contains("%2B"), "a + arrives as a +")

    // the worst case: a long last problem in characters that take 9 each in the link, and long facts
    let wide = String(repeating: "é—", count: 2000)
    let hugeProblem = "Last problem:\n" + (0..<9).map { _ in "\(at(60))  \(wide)" }.joined(separator: "\n")
    let longFacts = facts + (0..<30).map { "extra \($0): " + wide }
    let worst = issueDetails(problem: hugeProblem, facts: longFacts.map { $0 + " " + wide })
    u = issueURL(title: "Problem report: " + wide, details: worst)
    let wb = body(u)
    check(u!.absoluteString.utf8.count <= issueURLLimit, "the worst case link is at most \(issueURLLimit) bytes: \(u!.absoluteString.utf8.count)")
    check(wb.contains("Last problem:"), "the last problem is in the worst case link")
    for (i, fact) in facts.prefix(4).enumerated() {
        check(wb.contains(String(fact.prefix(40))), "the worst case link still has fact \(i + 1): \(fact.prefix(40))")
    }
    check(!wb.contains("extra 29"), "the facts at the bottom are the ones left out")
    // an ordinary long report: everything fits
    let ordinary = issueDetails(problem: lastProblem(appLog: log((0..<8).map { (TimeInterval(900 - $0 * 10), "step \($0) failed: " + String(repeating: "detail ", count: 40)) }),
                                                     bridgeLog: "", now: now).text, facts: facts)
    u = issueURL(title: "Problem report: step 7 failed", details: ordinary)
    check(u!.absoluteString.utf8.count <= issueURLLimit && body(u).contains(facts.last!), "an ordinary long report fits whole")
}
