// Our stems models and Pioneer's: which models are ours (by checksum, from signed facts only),
// saving Pioneer's model before ours replaces it, choosing and putting back the right one, and
// installing ours. Pioneer's models are saved in originals/ as <sha256>.onnx, each with
// <sha256>.engine holding the STEMS Engine version (demucs3_ver.txt) it came with.
// Shared by the app and the tests (which use a temporary folder).
import Foundation

/// Every model a release of RB Stems Plus has installed, compiled in: still known as ours after
/// payload.json names a newer one.
let builtInModels: Set<String> = [ourModel]

/// The checksums of the stems models that are ours: those compiled into this app, and those the
/// saved payload.json names (its model and previous_models). Never a size, a name or a list the
/// user can edit. The one place this is decided: payload.json's part counts only if its
/// signature verified (savedManifest checks it on every read; a payload.json that wasn't
/// checked, signedBy nil, adds nothing).
func ourModelChecksums(_ m: Manifest? = savedManifest()) -> Set<String> {
    var s = builtInModels
    if let m = m, m.signedBy != nil { s.insert(m.model.sha256); s.formUnion(m.previousModels ?? []) }
    return s
}

/// 64 lowercase hex digits.
func isSHA256(_ s: String) -> Bool { s.count == 64 && s.allSatisfy { "0123456789abcdef".contains($0) } }

/// What a model step did: ok or not, and a sentence for the log or the user. noSpace: it failed
/// only for lack of free space (freeing some and trying again can work).
struct Outcome { let ok: Bool; let message: String; var noSpace = false }

/// Said when rekordbox's model is neither ours nor one that can be taken for Pioneer's
/// (Models.mayBePioneers).
let unknownModelMessage = "rekordbox's stems model isn't one RB Stems Plus knows, so it was left as it is."

/// Said when the folder of Pioneer's saved models can't be listed (Models.originalsUnreadable).
let originalsUnreadableMessage = "The saved copies of rekordbox's own stems model can't be read."

/// rekordbox's model folder and our saved originals: what the steps below work on.
/// Models.originalsCheck: what is saved (best first), what verifies, whether anything can't be checked.
typealias OriginalsCheck = (saved: [(sha: String, engine: String)], verified: [String], unknown: Bool)

struct Models {
    let dir: String                 // rekordbox's model folder (hdemucs.onnx, demucs3_ver.txt)
    let originals: String           // Pioneer's saved models
    let ours: Set<String>           // ourModelChecksums()
    var gate: Deletable = deletable
    var log: (String) -> Void = { _ in }
    /// Called right before rekordbox's model is checked again and replaced (the tests change it there).
    var willReplace: () -> Void = {}
    /// How rekordbox's model and its folder are looked up (lstat; the tests make it fail).
    var lookup: (String) -> Lookup = fileLookup
    /// How a saved copy is hashed in originalsCheck (sha256; the tests make a read fail)
    var hashFile: (String) -> String? = { sha256($0) }

    var model: String { dir + "/hdemucs.onnx" }
    var newTemp: String { dir + "/.hdemucs.new" }
    var restoreTemp: String { dir + "/.hdemucs.restore" }
    /// Where 1.0 saved Pioneer's model (one file, without its checksum or engine version).
    var legacy: String { originals + "/hdemucs.onnx" }
    func saved(_ sha: String) -> String { originals + "/\(sha).onnx" }
    func engineFile(_ sha: String) -> String { originals + "/\(sha).engine" }
    var engine: String { engineVersion(in: dir) }

    private func remove(_ p: String) { safeRemove(p, whole: false, gate) }
    private func fail(_ s: String) -> Outcome { log("  \(s)"); return Outcome(ok: false, message: s) }

    /// rekordbox's model as it is now: its checksum, or nil when nothing is there (ENOENT only).
    /// readable false when it can't be looked up or hashed, or isn't a file (a link included, as
    /// the core's hash_at): never taken for "nothing there", so a model is never replaced, nor put
    /// back over, unread.
    func current() -> (readable: Bool, sha: String?) {
        switch lookup(model) {
        case .missing: return (true, nil)
        case .unreadable: return (false, nil)
        case .type(S_IFREG): let s = sha256(model); return (s != nil, s)
        case .type: return (false, nil)                          // a link or anything but a file: never hashed through
        }
    }

    // MARK: saving Pioneer's model

    /// Saves rekordbox's model as Pioneer's original. `sha`: its checksum, taken by the caller
    /// just before (the copy must have it). Never one of ours, never over a saved file: copied to
    /// a .part file, checked, then renamed into place only if nothing is there.
    func saveOriginal(_ sha: String) -> Outcome {
        guard isSHA256(sha), !ours.contains(sha) else {
            return fail("rekordbox's stems model is the Demucs v4 model, so it wasn't saved as rekordbox's own.")
        }
        if savedIsGood(sha) { log("save Pioneer's model: already saved (\(sha.prefix(12)))"); return Outcome(ok: true, message: "") }
        try? FileManager.default.createDirectory(atPath: originals, withIntermediateDirectories: true)
        let e = engine
        let part = saved(sha) + ".part"
        remove(part)                                            // left by a run that was stopped
        let err = copyFile(model, part)
        guard err == nil, sha256(part) == sha, syncFile(part) else {
            remove(part)
            log("save Pioneer's model: FAILED \(err ?? "the copy didn't match")")
            return Outcome(ok: false, message: err?.contains("No space") == true ? "There isn't enough free space to save a copy of rekordbox's own stems model."
                                                                                  : "A copy of rekordbox's own stems model couldn't be saved.")
        }
        // the engine version before the model: a saved model never appears without it
        if !e.isEmpty && fileType(engineFile(sha)) == nil { writeAtomically(Data((e + "\n").utf8), to: engineFile(sha)) }
        guard place(part, as: sha) else { remove(part); return fail("A copy of rekordbox's own stems model couldn't be saved.") }
        log("save Pioneer's model: saved (\(sha.prefix(12)), STEMS Engine \(e.isEmpty ? "unknown" : e))")
        return Outcome(ok: true, message: "")
    }

    /// Whether the originals folder can't be listed (any error but ENOENT): then which of
    /// Pioneer's models are saved isn't known, and is never taken for "none saved".
    var originalsUnreadable: Bool {
        guard let d = opendir(originals) else { return errno != ENOENT }
        closedir(d)
        return false
    }

    /// Whether a saved model of Pioneer's may be there that can't be checked: the originals
    /// folder can't be listed, or a <sha>.onnx in it (not ours) can't be looked up or opened.
    /// Such a copy is "unknown", never "none": restore and install stop, and "Remove RB Stems
    /// Plus anyway" keeps the folder.
    var originalsUnknown: Bool { originalsCheck(hashing: false).unknown }

    /// One look at the originals folder, from one scan (review MP1): Pioneer's saved models (best
    /// first, as savedOriginals), those that verify (hashing: each saved copy hashed once; a
    /// regular file that can't be hashed is a read error, so "unknown", not a mismatch), and
    /// whether anything there can't be checked. Every decision about what is saved, what is put
    /// back and what "anyway" keeps takes one of these, never two scans that could disagree.
    func originalsCheck(hashing: Bool = true) -> OriginalsCheck {
        let s = scan()
        var unknown = s.unknown, verified: [String] = []
        let list = ranked(s.list)
        if hashing {
            for o in list {
                switch hashFile(saved(o.sha)) {
                case nil: unknown = true                    // a read error: unknown, never "bad"
                case o.sha?: verified.append(o.sha)
                default: break                              // damaged: it doesn't verify
                }
            }
        }
        return (list, verified, unknown)
    }

    /// Whether removeOwnFolders(keepOriginals: true) keeps the originals folder: it is a real
    /// folder, or its lookup fails (not ENOENT: then nothing in support is deleted). A link or a
    /// file there goes with the rest.
    var originalsStays: Bool {
        switch fileLookup(originals) {
        case .type(S_IFDIR), .unreadable: return true
        case .type, .missing: return false
        }
    }

    /// Whether originals/<sha>.onnx is there and has that checksum.
    func savedIsGood(_ sha: String) -> Bool { fileType(saved(sha)) == S_IFREG && sha256(saved(sha)) == sha }

    /// Pioneer's saved models that verify (their checksums, best first): what "Remove RB Stems
    /// Plus anyway" must keep. Only in a real originals folder, never through a link (scan).
    func verifiedOriginals() -> [String] { originalsCheck().verified }

    /// Whether a model that isn't ours (checksum `sha`) can be taken for Pioneer's: when no
    /// original of Pioneer's is saved, when it is one of them, or when rekordbox's STEMS Engine
    /// version is known and differs from every saved one's (a new STEMS Engine brought it).
    /// Otherwise it may be a newer export of the Demucs v4 model this app doesn't know (its payload.json
    /// missing or refused): it is never saved as Pioneer's, nor taken for Pioneer's put back.
    func mayBePioneers(_ sha: String, _ saved: [(sha: String, engine: String)]? = nil) -> Bool {
        let saved = saved ?? savedOriginals()
        if saved.isEmpty || saved.contains(where: { $0.sha == sha }) { return true }
        let e = engine
        return !e.isEmpty && saved.allSatisfy { !$0.engine.isEmpty && $0.engine != e }
    }

    /// Renames a checked file to originals/<sha>.onnx, never over another file. A saved copy
    /// there that doesn't match its name is set aside first (<sha>.onnx.damaged, never used).
    private func place(_ file: String, as sha: String) -> Bool {
        let dest = saved(sha)
        if fileType(dest) != nil && !savedIsGood(sha) {
            log("saved model \(sha.prefix(12)) is damaged: set aside")
            remove(dest + ".damaged")
            _ = rename(dest, dest + ".damaged")
        }
        return renamex_np(file, dest, UInt32(RENAME_EXCL)) == 0
    }

    // MARK: which one to put back

    /// Pioneer's saved models, best first: the one saved for the current STEMS Engine version,
    /// then the highest known engine version, then those whose version isn't known; the most
    /// recently saved first among equals. Only files named <sha256>.onnx (never a .part or one
    /// set aside) and never one of ours.
    func savedOriginals() -> [(sha: String, engine: String)] { originalsCheck(hashing: false).saved }

    /// scan()'s list in savedOriginals' order
    private func ranked(_ list: [(sha: String, engine: String, date: Date)]) -> [(sha: String, engine: String)] {
        let current = engine
        func rank(_ e: String) -> Int { !current.isEmpty && e == current ? 0 : (e.isEmpty ? 2 : 1) }
        return list.sorted { a, b in
            if rank(a.engine) != rank(b.engine) { return rank(a.engine) < rank(b.engine) }
            if a.engine != b.engine { return a.engine.compare(b.engine, options: .numeric) == .orderedDescending }
            return a.date > b.date
        }.map { ($0.sha, $0.engine) }
    }

    /// One pass over the originals folder: the saved models that can be read (unsorted, with
    /// their engine version and date), and whether a <sha>.onnx that isn't ours can't be checked
    /// (its lookup fails with anything but ENOENT, or it is a file that can't be opened). Such a
    /// copy isn't listed (a good one next to it can still be put back) but makes `unknown` true.
    private func scan() -> (list: [(sha: String, engine: String, date: Date)], unknown: Bool) {
        var list: [(sha: String, engine: String, date: Date)] = [], unknown = false
        // only a real folder is read: through a link (or with a file there) nothing counts as
        // saved, as removeOwnFolders takes it with the rest; a lookup error is "unknown"
        switch lookup(originals) {   // through self.lookup, as each saved copy below (MP1)
        case .type(S_IFDIR): break
        case .unreadable: return ([], true)
        case .type, .missing: return ([], false)
        }
        // listed with readdir (FileManager's listing also looks each entry up, and fails as a
        // whole in a folder that can be read but not searched): a listing error is "unknown"
        guard let d = opendir(originals) else { return ([], errno != ENOENT) }
        var names: [String] = []
        while true {
            errno = 0
            guard let e = readdir(d) else { if errno != 0 { unknown = true }; break }
            let n = withUnsafePointer(to: &e.pointee.d_name) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
            if n != "." && n != ".." { names.append(n) }
        }
        closedir(d)
        // 1.0's saved model (originals/hdemucs.onnx) still there: migrateLegacy couldn't move it,
        // so it may be Pioneer's only copy (review MP2)
        if lookup(legacy) != .missing { unknown = true }
        for name in names where name.hasSuffix(".onnx") {
            let sha = String(name.dropLast(5))
            guard isSHA256(sha), !ours.contains(sha) else { continue }
            switch lookup(saved(sha)) {   // through self.lookup: the tests fail one lookup once
            case .type(S_IFREG):
                let fd = open(saved(sha), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
                guard fd >= 0 else { unknown = true; continue }
                close(fd)
            case .unreadable: unknown = true; continue
            case .type, .missing: continue
            }
            let e = ((try? String(contentsOfFile: engineFile(sha), encoding: .utf8)) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            let date = (try? FileManager.default.attributesOfItem(atPath: saved(sha)))?[.modificationDate] as? Date ?? .distantPast
            list.append((sha, e, date))
        }
        return (list, unknown)
    }

    // MARK: putting it back

    /// Puts Pioneer's model back over ours, or where rekordbox's model is missing (if its folder
    /// is there). A model that can be taken for Pioneer's (mayBePioneers: rekordbox put its own
    /// back) stays, and so does one this app doesn't know, but that is a failure: the caller
    /// must not delete the saved originals then. Copied to .hdemucs.restore, checked; rekordbox's
    /// model hashed again (it must not have changed); renamed over, checked again. The temporary
    /// copy is removed on failure.
    func restore() -> Outcome {
        switch folder() {
        case .there: break
        case .notThere: return done("rekordbox's model folder isn't there: nothing to put back.")
        case .notChecked(let why): return fail(why)
        }
        let now = current()
        guard now.readable else { return fail("rekordbox's stems model can't be read.") }
        let live = now.sha, present = live != nil
        let check = originalsCheck(hashing: false), candidates = check.saved   // one scan for every decision below
        if let l = live, !ours.contains(l) {
            // a saved copy that can't be checked may say this model isn't Pioneer's
            guard !check.unknown else { return fail(originalsUnreadableMessage) }
            guard mayBePioneers(l, candidates) else { return fail(unknownModelMessage) }
            return done("Pioneer's stems model is already in place.")
        }
        guard !candidates.isEmpty else {
            if check.unknown { return fail(originalsUnreadableMessage) }
            if !present { return done("rekordbox has no stems model and none was saved: left as it is.") }
            return fail("There's no saved copy of rekordbox's own stems model on this Mac.")
        }
        // the copy and the rename: quitting waits for them (Shell.swift)
        return critical.run { () -> Outcome in
            for o in candidates {
                remove(restoreTemp)                                     // left by a run that was stopped
                if let err = copyFile(saved(o.sha), restoreTemp) {
                    remove(restoreTemp)
                    if err.contains("No space") {
                        log("  \(err)")
                        return Outcome(ok: false, message: "There isn't enough free space to put rekordbox's own stems model back.", noSpace: true)
                    }
                    return fail("rekordbox's own stems model couldn't be put back: \(err).")
                }
                guard sha256(restoreTemp) == o.sha else {
                    remove(restoreTemp)
                    log("  saved model \(o.sha.prefix(12)) didn't verify: trying the next one")
                    continue
                }
                guard syncFile(restoreTemp) else {
                    let full = errno == ENOSPC
                    remove(restoreTemp)
                    log("  the copy of saved model \(o.sha.prefix(12)) couldn't be written to the disk")
                    return full ? Outcome(ok: false, message: "There isn't enough free space to put rekordbox's own stems model back.", noSpace: true)
                                : fail("rekordbox's own stems model couldn't be put back.")
                }
                willReplace()
                let again = current()
                guard again.readable, again.sha == live else {
                    remove(restoreTemp); return fail(again.readable ? "rekordbox's stems model changed meanwhile." : "rekordbox's stems model can't be read.")
                }
                guard rename(restoreTemp, model) == 0 else {
                    let err = String(cString: strerror(errno))
                    remove(restoreTemp)
                    return fail("rekordbox's own stems model couldn't be put back: \(err).")
                }
                guard sha256(model) == o.sha else { return fail("rekordbox's stems model didn't verify after it was put back.") }
                return done("Pioneer's stems model is back (\(o.sha.prefix(12)), STEMS Engine \(o.engine.isEmpty ? "unknown" : o.engine)).")
            }
            return fail("The saved copy of rekordbox's own stems model is damaged.")
        }
    }

    enum Folder: Equatable { case there, notThere, notChecked(String) }

    /// rekordbox's model folder: a real folder, or not there (nothing at all, ENOTDIR, or not a
    /// folder). A link (ELOOP on Windows) or any other lookup error is neither: the model behind
    /// it isn't verifiably not ours, so restore fails and the uninstall keeps the originals.
    func folder() -> Folder {
        switch lookup(dir) {
        case .type(S_IFDIR): return .there
        case .type(S_IFLNK): return .notChecked("rekordbox's model folder is a link, so its stems model wasn't checked.")
        case .type, .missing, .unreadable(ENOTDIR): return .notThere
        case .unreadable: return .notChecked("rekordbox's model folder can't be read.")
        }
    }

    private func done(_ s: String) -> Outcome { log("restore Pioneer's model: \(s)"); return Outcome(ok: true, message: s) }

    // MARK: installing ours

    /// Puts our model (`source`, checksum `sha`) in rekordbox's place. rekordbox's model is
    /// replaced only if it is ours, or once Pioneer's is saved and verified; it is hashed again
    /// right before the rename and must not have changed. The copy is checked before the rename.
    /// On failure rekordbox's model is as it was.
    func installOurs(from source: String, sha: String) -> Outcome {
        // a linked or unreadable model folder: refused, as restore refuses it (a folder that isn't
        // there fails at the copy, as before)
        if case .notChecked(let why) = folder() { return fail(why) }
        let found = lookup(model)
        if case .unreadable = found { return fail("rekordbox's stems model can't be read.") }
        let present = found != .missing
        if present && found != .type(S_IFREG) { return fail("rekordbox's stems model isn't a file.") }
        let live = present ? sha256(model) : nil
        if present && live == nil { return fail("rekordbox's stems model can't be read.") }
        if let l = live, !ours.contains(l) {
            let check = originalsCheck(hashing: false)   // one scan for both decisions
            guard !check.unknown else { return fail(originalsUnreadableMessage) }
            guard mayBePioneers(l, check.saved) else { return fail(unknownModelMessage) }
            let s = saveOriginal(l)
            guard s.ok else { return s }
            guard savedIsGood(l) else { return fail("A copy of rekordbox's own stems model couldn't be saved.") }
        } else if !present {
            log("Pioneer's model: none to save")
        }
        // the copy and the rename: quitting waits for them (Shell.swift)
        return critical.run { () -> Outcome in
            remove(newTemp)                                             // left by a run that was stopped
            if let err = copyFile(source, newTemp) {
                remove(newTemp)
                return fail(err.contains("No space") ? "There isn't enough free space to copy the Demucs v4 model." : "The Demucs v4 model couldn't be copied: \(err).")
            }
            guard sha256(newTemp) == sha, syncFile(newTemp) else { remove(newTemp); return fail("The Demucs v4 model didn't copy correctly.") }
            willReplace()
            let again = current()
            guard again.readable, again.sha == live else {
                remove(newTemp); return fail(again.readable ? "rekordbox's stems model changed meanwhile." : "rekordbox's stems model can't be read.")
            }
            guard rename(newTemp, model) == 0 else {
                let err = String(cString: strerror(errno))
                remove(newTemp); return fail("The Demucs v4 model couldn't be put in place: \(err).")
            }
            log("Demucs v4: installed (\(sha.prefix(12)))")
            return Outcome(ok: true, message: "")
        }
    }

    // MARK: 1.0's layout

    /// At launch: 1.0's originals/hdemucs.onnx moves to the new layout if it is Pioneer's (its
    /// engine version is known only if rekordbox's model is ours or the same now: no STEMS
    /// Engine download since). If it is ours (saved by mistake), it is set aside as
    /// ours-<sha>.onnx and never used. Returns true in that case if rekordbox has our model and
    /// no saved copy of Pioneer's is there, none that can't be checked either (review MQ3): then
    /// there is nothing to put back, and the user is told.
    func migrateLegacy() -> Bool {
        guard fileType(legacy) == S_IFREG else { return false }
        guard let sha = sha256(legacy) else { log("1.0's saved model can't be read: left as it is"); return false }
        let live = current().sha
        if ours.contains(sha) {
            let aside = originals + "/ours-\(sha).onnx"
            if renamex_np(legacy, aside, UInt32(RENAME_EXCL)) != 0 { remove(legacy) }   // the same file is already set aside
            log("1.0's saved model is RB Stems Plus's own (\(sha.prefix(12))): set aside, never put back")
            let o = originalsCheck(hashing: false)   // a copy that can't be checked may be Pioneer's (review MQ3)
            return (live.map(ours.contains) ?? false) && o.saved.isEmpty && !o.unknown
        }
        if savedIsGood(sha) { remove(legacy); log("1.0's saved model: already in the new layout"); return false }
        let e = engine
        if !e.isEmpty, live == sha || live.map(ours.contains) == true, fileType(engineFile(sha)) == nil {
            writeAtomically(Data((e + "\n").utf8), to: engineFile(sha))
        }
        log("1.0's saved model (\(sha.prefix(12))): \(place(legacy, as: sha) ? "moved to the new layout" : "COULDN'T BE MOVED")")
        return false
    }
}

/// rekordbox's model folder and this account's saved originals, with the current list of ours.
func installedModels(_ log: @escaping (String) -> Void) -> Models {
    Models(dir: modelDir, originals: originalsDir, ours: ourModelChecksums(), log: log)
}

/// Flushes a file to the disk before it is renamed into place. False if it can't be opened.
func syncFile(_ path: String) -> Bool {
    let fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
    guard fd >= 0 else { return false }
    defer { close(fd) }
    return fsync(fd) == 0
}

/// What rekordbox's stems model is when RB Stems Plus is removed without putting Pioneer's back
/// ("Remove RB Stems Plus anyway"): one sentence.
func modelLeftBehind(_ m: Models) -> String {
    switch m.folder() {
    case .notChecked: return "rekordbox's stems model can't be read."
    case .notThere: return "rekordbox has no stems model."
    case .there: break
    }
    let now = m.current()
    guard now.readable else { return "rekordbox's stems model can't be read." }
    guard let sha = now.sha else { return "rekordbox has no stems model." }
    return m.ours.contains(sha) ? "rekordbox still has the Demucs v4 model." : "rekordbox's stems model isn't one RB Stems Plus knows."
}
