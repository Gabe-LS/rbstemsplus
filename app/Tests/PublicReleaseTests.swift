// scripts/release.sh's public copy, end to end without the network: a scratch development
// repository (with a fake scripts/build.sh), bare repositories as both origins (the public one
// named .../Gabe-LS/rbstemsplus.git, as release.sh expects), a fake gh that keeps the release in
// a folder, a test signing key without a passphrase, and a terminal (a pty) to type "yes" in.
// Nothing here touches GitHub, the real repositories or the user's git configuration.
import Foundation

func publicReleaseTests() {
    let fm = FileManager.default
    let dir = tmp + "/public-release"
    let dev = dir + "/dev", devOrigin = dir + "/dev-origin.git"
    let publicBare = dir + "/Gabe-LS/rbstemsplus.git", publicClone = dir + "/RB Stems Plus (public)"
    let ghState = dir + "/gh", bin = dir + "/bin"
    for d in [dev + "/scripts", dev + "/keys", dev + "/docs/release-notes", dir + "/Gabe-LS", ghState, bin] {
        try! fm.createDirectory(atPath: d, withIntermediateDirectories: true)
    }
    // git sees only this configuration: no global identity, signing or hooks
    let gitConfig = dir + "/gitconfig"
    fm.createFile(atPath: gitConfig, contents: Data("[init]\n\tdefaultBranch = main\n[advice]\n\tdetachedHead = false\n".utf8))
    let env = ["PATH": bin + ":/usr/bin:/bin:/usr/sbin:/sbin", "HOME": dir, "TMPDIR": dir, "LANG": "en_US.UTF-8",
               "GIT_CONFIG_GLOBAL": gitConfig, "GIT_CONFIG_NOSYSTEM": "1", "FAKE_GH": ghState, "FAKE_PUBLIC": publicBare]
    func run(_ exe: String, _ args: [String]) -> (Int32, String) {
        runTool("/usr/bin/env", env.map { "\($0.key)=\($0.value)" } + [exe] + args)
    }
    func git(_ repo: String, _ args: String...) -> (Int32, String) { run("/usr/bin/git", ["-C", repo] + args) }
    func write(_ path: String, _ text: String, mode: Int = 0o644) {
        fm.createFile(atPath: path, contents: Data(text.utf8), attributes: [.posixPermissions: mode])
    }

    // the development repository
    try! fm.copyItem(atPath: fm.currentDirectoryPath + "/scripts/release.sh", toPath: dev + "/scripts/release.sh")
    write(dev + "/scripts/build.sh", """
        #!/bin/bash
        # a fake build: every asset, with the commit it was built from in payload.json
        set -euo pipefail
        root="$(cd "$(dirname "$0")/.." && pwd)"
        [ -z "$(git -C "$root" status --porcelain)" ] || { echo "fake build: not clean" >&2; exit 1; }
        commit="$(git -C "$root" rev-parse HEAD)"; v="$(tr -d '[:space:]' < "$root/VERSION")"
        d="$root/dist"; mkdir -p "$d"
        for f in rbstemsplus-app.zip libonnxruntime.1.18.0.dylib stemsplus-model.onnx bootstrap.sh; do echo "$f $commit" > "$d/$f"; done
        cp "$root/NOTICE" "$root/LICENSE" "$d/"
        printf '{"payload_version": "%s", "commit": "%s", "app": {"version": "%s"}}\\n' "$v" "$commit" "$v" > "$d/payload.json"
        (cd "$d" && shasum -a 256 rbstemsplus-app.zip libonnxruntime.1.18.0.dylib stemsplus-model.onnx bootstrap.sh NOTICE LICENSE payload.json > SHA256SUMS)
        echo "fake build of $commit"

        """, mode: 0o755)
    write(dev + "/VERSION", "9.9.8\n")
    write(dev + "/NOTICE", "RB Stems Plus\nCopyright (c) 2026 Gabe-LS.\n")
    write(dev + "/LICENSE", "MIT License\n")
    write(dev + "/README.md", "# RB Stems Plus\n\nThe design is private.\n")
    write(dev + "/.gitignore", "build/\ndist/\n")
    write(dev + "/.publicignore", "# private\ndocs/PRIVATE-NOTES.md\ndocs/internal\n")
    write(dev + "/docs/PRIVATE-NOTES.md", "the plan\n")
    try! fm.createDirectory(atPath: dev + "/docs/internal", withIntermediateDirectories: true)
    write(dev + "/docs/internal/vm.md", "the VM\n")
    write(dev + "/docs/public.md", "hello\n")
    write(dev + "/docs/release-notes/9.9.8.md", "The notes of 9.9.8.\n")
    let key = dir + "/release-key.pem"
    check(run("/usr/bin/openssl", ["ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", key]).0 == 0
          && run("/usr/bin/openssl", ["ec", "-in", key, "-pubout", "-out", dev + "/keys/release.pub.pem"]).0 == 0
          && run("/usr/bin/openssl", ["ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", dir + "/backup.pem"]).0 == 0
          && run("/usr/bin/openssl", ["ec", "-in", dir + "/backup.pem", "-pubout", "-out", dev + "/keys/backup.pub.pem"]).0 == 0,
          "public release: test signing keys")
    chmod(key, 0o600)
    check(git(dir, "init", "-q", "--bare", devOrigin).0 == 0 && git(dir, "init", "-q", "--bare", publicBare).0 == 0
          && git(dev, "init", "-q").0 == 0 && git(dev, "config", "user.name", "Dev Person").0 == 0
          && git(dev, "config", "user.email", "dev.person@example.org").0 == 0 && git(dev, "add", "-A").0 == 0
          && git(dev, "commit", "-q", "-m", "first").0 == 0 && git(dev, "remote", "add", "origin", devOrigin).0 == 0
          && git(dev, "push", "-q", "origin", "main").0 == 0,
          "public release: a development repository and two bare origins")
    let release = dev + "/scripts/release.sh"
    func dryRun() -> (Int32, String) { run("/bin/bash", [release, "v9.9.8", "--dry-run"]) }
    func publicRefs() -> String { git(publicBare, "for-each-ref").1 }

    // the fake gh: one release, kept in a folder; create needs the tag on the public origin
    write(bin + "/gh", """
        #!/bin/bash
        st="$FAKE_GH"
        echo "gh $*" >> "$st/calls"
        case "$1 $2" in
          "auth status") exit 0 ;;
          "release view")
            [ -d "$st/release" ] || { echo "release not found" >&2; exit 1; }
            case "$*" in
              *isDraft*) echo true ;;
              *assets*) for f in "$st/release"/*; do echo "$(basename "$f") $(stat -f %z "$f")"; done ;;
            esac ;;
          "release create")
            shift 2; tag="$1"; shift
            git -C "$FAKE_PUBLIC" rev-parse -q --verify "refs/tags/$tag" > /dev/null || { echo "fake gh: no tag $tag on the public origin" >&2; exit 1; }
            mkdir "$st/release" || exit 1
            while [ $# -gt 0 ]; do
              case "$1" in
                --repo|--title|--notes|--notes-file) echo "$1 $2" >> "$st/args"; shift 2 ;;
                --*) echo "$1" >> "$st/args"; shift ;;
                *) cp "$1" "$st/release/"; shift ;;
              esac
            done ;;
          "release download")
            shift 3; out=""; pats=()
            while [ $# -gt 0 ]; do case "$1" in --dir) out="$2"; shift 2 ;; --pattern) pats+=("$2"); shift 2 ;; *) shift 2 ;; esac; done
            mkdir -p "$out"; for p in "${pats[@]}"; do cp "$st/release/$p" "$out/"; done ;;
          *) echo "fake gh: unexpected: $*" >&2; exit 1 ;;
        esac

        """, mode: 0o755)

    // MARK: the dry run: refusals before anything is made
    var r = dryRun()
    check(r.0 != 0 && r.1.contains("rbstemsplus.publicEmail isn't set"), "public release: refused without the public email: \(r.1)")
    _ = git(dev, "config", "rbstemsplus.publicEmail", "dev.person@example.org")
    r = dryRun()
    check(r.0 != 0 && r.1.contains("noreply"), "public release: refused with an email that isn't Gabe-LS's noreply address")
    let email = "12345678+Gabe-LS@users.noreply.github.com"
    _ = git(dev, "config", "rbstemsplus.publicEmail", email)
    r = dryRun()
    check(r.0 != 0 && r.1.contains("no public clone at \(publicClone)"), "public release: the default public clone is the sibling folder: \(r.1)")
    check(git(dir, "clone", "-q", publicBare, publicClone).0 == 0, "public release: an empty public clone")
    r = dryRun()
    check(r.0 == 0 && r.1.contains("left out: .publicignore docs/PRIVATE-NOTES.md docs/internal") && r.1.contains("new: docs/public.md")
          && !r.1.contains("new: docs/PRIVATE-NOTES.md") && !r.1.contains("new: docs/internal") && r.1.contains("Gabe-LS <\(email)>"),
          "public release: the dry run lists what would be published, without the private paths: \(r.1)")
    check(publicRefs().isEmpty && git(publicClone, "rev-parse", "-q", "--verify", "HEAD").0 != 0, "public release: the dry run changes nothing")

    write(dev + "/README.md", "# RB Stems Plus\n\nThe design is in docs/PRIVATE-NOTES.md.\n")
    _ = git(dev, "commit", "-q", "-am", "mention")
    r = dryRun()
    check(r.0 != 0 && r.1.contains("mention a private file") && r.1.contains("README.md"), "public release: a mention of a private file is refused: \(r.1)")
    write(dev + "/README.md", "# RB Stems Plus\n\nSee PRIVATE-NOTES for the plan.\n")
    _ = git(dev, "commit", "-q", "-am", "mention without the extension")
    r = dryRun()
    check(r.0 != 0 && r.1.contains("mention a private file"), "public release: a mention by the capitals name alone is refused too")
    write(dev + "/README.md", "# RB Stems Plus\n\nWritten by Dev Person.\n")
    _ = git(dev, "commit", "-q", "-am", "name")
    r = dryRun()
    check(r.0 != 0 && r.1.contains("developer's own name or email") && r.1.contains("README.md"), "public release: the developer's git name is refused: \(r.1)")
    write(dev + "/README.md", "# RB Stems Plus\n\nMail DEV.PERSON@example.org.\n")
    _ = git(dev, "commit", "-q", "-am", "email")
    r = dryRun()
    check(r.0 != 0 && r.1.contains("developer's own name or email"), "public release: the developer's git email is refused, in any case")
    write(dev + "/.publicignore", "docs/PRIVATE-NOTES.md\ndocs/internal\ndocs/gone.md\n")
    write(dev + "/README.md", "# RB Stems Plus\n\nBy Gabe-LS.\n")
    _ = git(dev, "commit", "-q", "-am", "a missing private path")
    r = dryRun()
    check(r.0 != 0 && r.1.contains("docs/gone.md, which isn't in"), "public release: a .publicignore path missing from the commit is refused")
    write(dev + "/.publicignore", "docs/PRIVATE-NOTES.md\ndocs/internal\n")
    _ = git(dev, "commit", "-q", "-am", "fixed")
    _ = git(dev, "push", "-q", "origin", "main")
    r = dryRun()
    check(r.0 == 0, "public release: a clean tree passes the dry run: \(r.1)")

    // MARK: the release, in a terminal, with the fake gh
    r = runInTerminal(["/bin/bash", release, "v9.9.8", "--key", key], env: env, answer: "no\n")
    check(r.0 != 0 && r.1.contains("stopped: nothing was pushed or uploaded"), "public release: anything but yes stops before pushing: \(r.1.suffix(300))")
    check(publicRefs().isEmpty && !fm.fileExists(atPath: ghState + "/release"), "public release: no push, no draft")
    let unpushed = git(publicClone, "rev-parse", "HEAD").1
    r = runInTerminal(["/bin/bash", release, "v9.9.8", "--key", key], env: env, answer: "yes\n")
    check(r.0 == 0 && r.1.contains("Draft v9.9.8 is on Gabe-LS/rbstemsplus"), "public release: the release goes through: \(r.1.suffix(600))")
    let pubCommit = git(publicBare, "rev-parse", "main").1
    check(pubCommit == unpushed && r.1.contains("from an earlier run"), "public release: the earlier run's unpushed commit is reused, not made twice")
    check(git(publicBare, "log", "-1", "--format=%an <%ae>|%cn <%ce>|%B", "main").1 == "Gabe-LS <\(email)>|Gabe-LS <\(email)>|RB Stems Plus 9.9.8",
          "public release: one commit by Gabe-LS, named after the version: \(git(publicBare, "log", "-1", "--format=%an <%ae>|%cn <%ce>|%B", "main").1)")
    check(git(publicBare, "rev-list", "--count", "main").1 == "1", "public release: the public history is the release alone")
    let files = git(publicBare, "ls-tree", "-r", "--name-only", "main").1.split(separator: "\n").map(String.init)
    check(files.contains("docs/public.md") && files.contains("scripts/release.sh") && !files.contains(".publicignore")
          && !files.contains { $0.hasPrefix("docs/internal") || $0.contains("PRIVATE-NOTES") }, "public release: no private path is published: \(files)")
    check(git(publicBare, "rev-parse", "v9.9.8^{commit}").1 == pubCommit
          && git(publicBare, "for-each-ref", "--format=%(objecttype) %(taggername) %(taggeremail)", "refs/tags/v9.9.8").1 == "tag Gabe-LS <\(email)>",
          "public release: an annotated tag v9.9.8 by Gabe-LS on the public commit")
    check(git(devOrigin, "rev-parse", "v9.9.8^{commit}").1 == git(dev, "rev-parse", "HEAD").1, "public release: the development tag is on the development commit")
    let payload = (try? String(contentsOfFile: ghState + "/release/payload.json", encoding: .utf8)) ?? ""
    check(payload.contains("\"commit\": \"\(pubCommit)\""), "public release: the draft's payload.json names the public commit: \(payload)")
    let uploaded = ((try? fm.contentsOfDirectory(atPath: ghState + "/release")) ?? []).sorted()
    check(uploaded.count == 9 && uploaded.contains("payload.json.sig"), "public release: the draft has the nine files: \(uploaded)")
    check(run("/usr/bin/openssl", ["dgst", "-sha256", "-verify", dev + "/keys/release.pub.pem", "-signature",
                                   ghState + "/release/payload.json.sig", ghState + "/release/payload.json"]).0 == 0,
          "public release: the draft's payload.json is signed with the release key")
    let args = (try? String(contentsOfFile: ghState + "/args", encoding: .utf8)) ?? ""
    check(args.contains("--draft") && args.contains("--repo Gabe-LS/rbstemsplus") && args.contains("RB Stems Plus (public)/docs/release-notes/9.9.8.md\n") && args.contains("--notes-file /"),
          "public release: a draft on the public repository, with the version's notes: \(args)")
    r = runInTerminal(["/bin/bash", release, "v9.9.8", "--key", key], env: env, answer: "yes\n")
    check(r.0 != 0 && r.1.contains("GitHub has a release v9.9.8 already"), "public release: a release that exists is never replaced")

    // MARK: the public clone must be the public repository's, clean and in step
    _ = git(publicClone, "remote", "set-url", "origin", dir + "/Gabe-LS/rbstemsplus-dev.git")
    r = dryRun()
    check(r.0 != 0 && r.1.contains("not Gabe-LS/rbstemsplus"), "public release: a clone of another repository is refused")
    _ = git(publicClone, "remote", "set-url", "origin", publicBare)
    write(publicClone + "/stray.txt", "x")
    r = dryRun()
    check(r.0 != 0 && r.1.contains("uncommitted or untracked"), "public release: a public clone with stray files is refused")
    try? fm.removeItem(atPath: publicClone + "/stray.txt")
    _ = git(dev, "remote", "set-url", "origin", publicBare)
    r = dryRun()
    check(r.0 != 0 && r.1.contains("run release.sh in the private development repository"), "public release: run from the public repository: refused")
    _ = git(dev, "remote", "set-url", "origin", devOrigin)
}

/// Runs a command in a new terminal (a pty: [ -t 0 ] and [ -t 1 ] hold), types `answer` once
/// "Type yes to go on:" shows, and returns its exit code and everything it printed.
func runInTerminal(_ args: [String], env: [String: String], answer: String) -> (Int32, String) {
    var master: Int32 = 0, slave: Int32 = 0
    guard openpty(&master, &slave, nil, nil, nil) == 0 else { return (-1, "openpty failed") }
    let p = Process()
    p.executableURL = URL(fileURLWithPath: args[0])
    p.arguments = Array(args.dropFirst())
    p.environment = env
    let s = FileHandle(fileDescriptor: slave, closeOnDealloc: false)
    p.standardInput = s; p.standardOutput = s; p.standardError = s
    do { try p.run() } catch { close(master); close(slave); return (-1, "\(error)") }
    close(slave)
    var out = Data(), answered = false
    var buf = [UInt8](repeating: 0, count: 4096)
    while true {
        let n = read(master, &buf, buf.count)
        if n <= 0 { break }                              // EIO: every holder of the terminal has exited
        out.append(buf, count: n)
        if !answered, String(decoding: out, as: UTF8.self).contains("Type yes to go on:") {
            _ = answer.withCString { write(master, $0, strlen($0)) }
            answered = true
        }
    }
    p.waitUntilExit()
    close(master)
    return (p.terminationStatus, String(decoding: out, as: UTF8.self).replacingOccurrences(of: "\r\n", with: "\n"))
}
