// Running commands and writing log files: shared by the app and the watcher.
// A command is a program and its arguments, never a shell string built from values: the only
// shell bodies are constant scripts (the root scripts, the after-exit steps) that get every
// value as an argument ($1…).
import Foundation

/// The account's home folder from the password database. Not HOME, and not Foundation's (which
/// honours CFFIXED_USER_HOME): another process could set either. "" if there is none.
func accountHome() -> String {
    guard let pw = getpwuid(getuid()), let dir = pw.pointee.pw_dir else { return "" }
    return String(cString: dir)
}

/// The only environment a child process gets. Anything inherited could change what a child runs
/// or trusts: BASH_ENV runs a file in every bash, PERL5LIB loads code into perl scripts such as
/// shasum, DYLD_* inject libraries, SSL_CERT_FILE, CURL_CA_BUNDLE and *_PROXY redirect curl.
let childEnvironment: [String: String] = {
    var env = ["PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "HOME": accountHome()]
    // the account's temporary folder as the system knows it, not an inherited TMPDIR
    var buf = [CChar](repeating: 0, count: Int(PATH_MAX))
    if confstr(_CS_DARWIN_USER_TEMP_DIR, &buf, buf.count) > 0 { env["TMPDIR"] = String(cString: buf) }
    if let lang = ProcessInfo.processInfo.environment["LANG"] { env["LANG"] = lang }
    return env
}()

/// A child process: a program by its absolute path, its arguments, the clean environment.
func child(_ exe: String, _ args: [String]) -> Process {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: exe)
    p.arguments = args
    p.environment = childEnvironment
    return p
}

/// The steps quitting waits for: a root script running, or our model being copied into or
/// renamed in rekordbox's folder. Seconds each. Not the password or App Management prompts:
/// nothing has changed then, and the busy marker lets the next launch finish the action.
final class CriticalWork {
    private let lock = NSLock()
    private var count = 0
    private var waiting: [() -> Void] = []

    var running: Bool { lock.lock(); defer { lock.unlock() }; return count > 0 }

    func run<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock(); count += 1; lock.unlock()
        defer {
            lock.lock(); count -= 1
            let ready = count == 0 ? waiting : []
            if count == 0 { waiting = [] }
            lock.unlock()
            ready.forEach { $0() }
        }
        return try body()
    }

    /// Calls `then` once: as soon as no step runs (at once if none does), or after `timeout`
    /// seconds at the latest, so a logout or shutdown is never held up for longer. Any thread.
    func whenIdle(timeout: TimeInterval, _ then: @escaping () -> Void) {
        let once = NSLock()
        var called = false
        let call = { once.lock(); let first = !called; called = true; once.unlock(); if first { then() } }
        lock.lock()
        if count == 0 { lock.unlock(); call(); return }
        waiting.append(call)
        lock.unlock()
        DispatchQueue.global().asyncAfter(deadline: .now() + timeout, execute: call)
    }
}

let critical = CriticalWork()

/// Set before the app quits by itself (the end of Uninstall RB Stems Plus completely, an
/// update): that quit waits for nothing.
var ownQuit = false

/// Whether a quit may go ahead now: the app's own, or nothing critical is running.
func quitNow(own: Bool = ownQuit, _ c: CriticalWork = critical) -> Bool { own || !c.running }

/// A root script: sudo running bash (Admin.swift). runTool counts it as critical while it runs.
func isRootScript(_ exe: String, _ args: [String]) -> Bool { exe == "/usr/bin/sudo" && args.contains("/bin/bash") }

/// Runs a program and waits: its exit code and its output (stdout and stderr together, trimmed).
/// `input` goes to its stdin, written alongside (a large input can't block on a full output
/// pipe; a program that exits without reading it is no error). Without it, stdin is /dev/null.
/// A root script is critical (quitting waits for it) while it runs.
@discardableResult
func runTool(_ exe: String, _ args: [String], input: Data? = nil) -> (Int32, String) {
    isRootScript(exe, args) ? critical.run { runToolNow(exe, args, input: input) } : runToolNow(exe, args, input: input)
}

private func runToolNow(_ exe: String, _ args: [String], input: Data?) -> (Int32, String) {
    let p = child(exe, args)
    let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
    let inPipe = Pipe()
    p.standardInput = input == nil ? FileHandle.nullDevice : inPipe
    do { try p.run() } catch { return (-1, "\(error)") }
    let writing = DispatchGroup()
    if let d = input {
        let w = inPipe.fileHandleForWriting
        _ = fcntl(w.fileDescriptor, F_SETNOSIGPIPE, 1)
        DispatchQueue.global().async(group: writing) { try? w.write(contentsOf: d); try? w.close() }
    }
    let data = pipe.fileHandleForReading.readDataToEndOfFile(); p.waitUntilExit()
    writing.wait()
    return (p.terminationStatus, String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))
}

/// Copies a file's bytes, modes and dates (like cp -p) to `to`, which must not exist: a file or
/// link already there is an error, never followed or replaced. A partial copy stays for the
/// caller to remove. Returns why it failed, or nil.
func copyFile(_ from: String, _ to: String) -> String? {
    let flags = copyfile_flags_t(COPYFILE_DATA | COPYFILE_STAT | COPYFILE_EXCL | COPYFILE_NOFOLLOW_DST)
    if copyfile(from, to, nil, flags) == 0 { return nil }
    let e = errno
    return e == ENOSPC ? "No space left on the disk" : String(cString: strerror(e))
}

/// Adds a line to a log, creating it (and its folder) if needed. O_NOFOLLOW: a link planted at
/// the log's place is refused, never written through. O_APPEND: each line goes at the end.
func appendLog(_ path: String, _ s: String) {
    let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH:mm:ss"
    let line = Data("\(f.string(from: Date()))  \(s)\n".utf8)
    try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
    let fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0o644)
    guard fd >= 0 else { return }
    defer { close(fd) }
    _ = line.withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
}

/// afterExit's first lines: wait while process $1 runs, checking every $2 seconds.
let afterExitWait = "while /bin/kill -0 \"$1\" 2>/dev/null; do /bin/sleep \"$2\"; done; shift 2\n"

/// Runs a constant shell body once this process has exited (e.g. opening or deleting the app),
/// with `args` as $1…. Detached: it outlives this process.
func afterExit(poll: String, _ then: String, _ args: [String]) {
    let p = child("/bin/sh", ["-c", afterExitWait + then, "rbsp", String(ProcessInfo.processInfo.processIdentifier), poll] + args)
    p.standardInput = FileHandle.nullDevice
    try? p.run()
}
