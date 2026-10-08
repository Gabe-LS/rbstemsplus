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

/// rekordbox's model folder and our saved originals: what the steps below work on.
struct Models {
    let dir: String                 // rekordbox's model folder (hdemucs.onnx, demucs3_ver.txt)
    let originals: String           // Pioneer's saved models
    let ours: Set<String>           // ourModelChecksums()
    var gate: Deletable = deletable
    var log: (String) -> Void = { _ in }
    /// Called right before rekordbox's model is checked again and replaced (the tests change it there).
    var willReplace: () -> Void = {}

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

    /// Whether originals/<sha>.onnx is there and has that checksum.
    func savedIsGood(_ sha: String) -> Bool { fileType(saved(sha)) == S_IFREG && sha256(saved(sha)) == sha }

    /// Pioneer's saved models that verify (their checksums, best first): what "Remove RB Stems
    /// Plus anyway" must keep.
    func verifiedOriginals() -> [String] { savedOriginals().map(\.sha).filter(savedIsGood) }

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
    func savedOriginals() -> [(sha: String, engine: String)] {
        let current = engine
        var list: [(sha: String, engine: String, date: Date)] = []
        for name in (try? FileManager.default.contentsOfDirectory(atPath: originals)) ?? [] where name.hasSuffix(".onnx") {
            let sha = String(name.dropLast(5))
            guard isSHA256(sha), !ours.contains(sha), fileType(saved(sha)) == S_IFREG else { continue }
            let e = ((try? String(contentsOfFile: engineFile(sha), encoding: .utf8)) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            let date = (try? FileManager.default.attributesOfItem(atPath: saved(sha)))?[.modificationDate] as? Date ?? .distantPast
            list.append((sha, e, date))
        }
        func rank(_ e: String) -> Int { !current.isEmpty && e == current ? 0 : (e.isEmpty ? 2 : 1) }
        return list.sorted { a, b in
            if rank(a.engine) != rank(b.engine) { return rank(a.engine) < rank(b.engine) }
            if a.engine != b.engine { return a.engine.compare(b.engine, options: .numeric) == .orderedDescending }
            return a.date > b.date
        }.map { ($0.sha, $0.engine) }
    }

    // MARK: putting it back

    /// Puts Pioneer's model back over ours, or where rekordbox's model is missing (if its folder
    /// is there). A model that can be taken for Pioneer's (mayBePioneers: rekordbox put its own
    /// back) stays, and so does one this app doesn't know, but that is a failure: the caller
    /// must not delete the saved originals then. Copied to .hdemucs.restore, checked; rekordbox's
    /// model hashed again (it must not have changed); renamed over, checked again. The temporary
    /// copy is removed on failure.
    func restore() -> Outcome {
        guard fileType(dir) == S_IFDIR else { return done("rekordbox's model folder isn't there: nothing to put back.") }
        let present = fileType(model) != nil
        let live = present ? sha256(model) : nil
        if present && live == nil { return fail("rekordbox's stems model can't be read.") }
        let candidates = savedOriginals()
        if let l = live, !ours.contains(l) {
            guard mayBePioneers(l, candidates) else { return fail(unknownModelMessage) }
            return done("Pioneer's stems model is already in place.")
        }
        guard !candidates.isEmpty else {
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
                guard (fileType(model) != nil ? sha256(model) : nil) == live else {
                    remove(restoreTemp); return fail("rekordbox's stems model changed meanwhile.")
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

    private func done(_ s: String) -> Outcome { log("restore Pioneer's model: \(s)"); return Outcome(ok: true, message: s) }

    // MARK: installing ours

    /// Puts our model (`source`, checksum `sha`) in rekordbox's place. rekordbox's model is
    /// replaced only if it is ours, or once Pioneer's is saved and verified; it is hashed again
    /// right before the rename and must not have changed. The copy is checked before the rename.
    /// On failure rekordbox's model is as it was.
    func installOurs(from source: String, sha: String) -> Outcome {
        let present = fileType(model) != nil
        if present && fileType(model) != S_IFREG { return fail("rekordbox's stems model isn't a file.") }
        let live = present ? sha256(model) : nil
        if present && live == nil { return fail("rekordbox's stems model can't be read.") }
        if let l = live, !ours.contains(l) {
            guard mayBePioneers(l) else { return fail(unknownModelMessage) }
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
            guard (fileType(model) != nil ? sha256(model) : nil) == live else {
                remove(newTemp); return fail("rekordbox's stems model changed meanwhile.")
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
    /// ours-<sha>.onnx and never used. Returns true in that case if rekordbox has our model:
    /// then there is no saved copy of Pioneer's to put back, and the user is told.
    func migrateLegacy() -> Bool {
        guard fileType(legacy) == S_IFREG else { return false }
        guard let sha = sha256(legacy) else { log("1.0's saved model can't be read: left as it is"); return false }
        let live = fileType(model) != nil ? sha256(model) : nil
        if ours.contains(sha) {
            let aside = originals + "/ours-\(sha).onnx"
            if renamex_np(legacy, aside, UInt32(RENAME_EXCL)) != 0 { remove(legacy) }   // the same file is already set aside
            log("1.0's saved model is RB Stems Plus's own (\(sha.prefix(12))): set aside, never put back")
            return (live.map(ours.contains) ?? false) && savedOriginals().isEmpty
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
    guard fileType(m.model) != nil else { return "rekordbox has no stems model." }
    guard let sha = sha256(m.model) else { return "rekordbox's stems model can't be read." }
    return m.ours.contains(sha) ? "rekordbox still has the Demucs v4 model." : "rekordbox's stems model isn't one RB Stems Plus knows."
}
