// Settings… (Cmd+,): a sheet in the app's style with where Stems Cache saves stems (This Mac or
// a folder on an external drive: Change…, Use This Mac; LocationActions.swift), its two limits,
// written to config.ini, and "Clear saved stems" with the size they take there.
import AppKit

extension Controller {
    @objc func showSettings() {
        guard !actionRunning else { return }
        let c = readConfig(log)
        var gb = configNumber(c.maxGB), days = configNumber(c.maxDays)
        while true {
            let st = readConfigState()
            let loc = LocationMonitor.shared.snapshot()           // measured in the background, never here
            let installed = stemsCachePresent(), canChoose = !installed || bridgeSaysLocation()
            // the stems in use (this Mac's folder, also while an older bridge saves there, or the
            // chosen folder when it can be used now) and what Clear clears; "measuring…" while the
            // background measurement is of another folder (just after a change or a Clear)
            let stems = settingsStems(st, loc)
            let size = stems.size, target = stems.clear
            let clear = "Clear Saved Stems" + size
            var message = "Stems Cache uses these settings the next time rekordbox opens."
            if st.unreadable != nil { message = "Stems Cache can't read its settings (config.ini), so nothing can be changed here. See Help › RB Stems Plus Troubleshooting." }
            else if !canChoose { message += " Reinstall Stems Cache to choose where stems are saved." }
            let sheet = Sheet(title: "Settings", message: message,
                              buttons: ["Save", clear, "Cancel"],
                              fields: [("Stems Cache size limit (GB)", gb), ("Remove stems unused for (days)", days)],
                              rows: [(locationLabel(st), ["Change…", "Use This Mac"])])
            let readable = st.unreadable == nil
            sheet.buttonViews[0].isEnabled = readable
            sheet.buttonViews[1].isEnabled = readable && (target == .thisMac ? fileType(defaultCacheDir) == S_IFDIR : target != nil)
            sheet.rowButtons[0].isEnabled = readable && canChoose
            sheet.rowButtons[1].isEnabled = readable && st.location != .thisMac
            let choice = sheet.run(on: window).choice
            gb = sheet.inputs[0].stringValue.trimmingCharacters(in: .whitespaces)
            days = sheet.inputs[1].stringValue.trimmingCharacters(in: .whitespaces)
            switch choice {
            case "Save":
                // a comma works as the decimal mark too
                let g = Double(gb.replacingOccurrences(of: ",", with: ".")), d = Double(days.replacingOccurrences(of: ",", with: "."))
                guard let g = g, maxGBRange.contains(g) else {
                    _ = alert("Size out of range", "Enter a size from 0.1 to 100,000 GB.", ["OK"]); continue
                }
                guard let d = d, maxDaysRange.contains(d) else {
                    _ = alert("Days out of range", "Enter a number of days from 1 to 36,500.", ["OK"]); continue
                }
                if writeConfig(maxGB: g, maxDays: d) { log("settings: max_gb=\(configNumber(g)) max_days=\(configNumber(d))"); prepareChanged() }
                else {
                    log("settings: couldn't write \(configFile)")
                    _ = alert("Settings not saved", "RB Stems Plus couldn't write its settings file. See the log in this window.", ["OK"])
                }
                return
            case "Change…":
                changeLocation()
                continue                                        // back to the settings, with the fields as they were
            case "Use This Mac":
                useThisMac()
                continue
            case clear:
                if case .drive(let g)? = target {
                    let drive = loc.drive ?? driveName(g.dir)
                    guard confirm("Clear saved stems" + size, "Clear", clearLocationMessage(drive: drive)) else { continue }
                    guard preflight() else { continue }        // the bridge works in that folder while rekordbox runs
                    clearLocation(g, drive: drive)
                    return                                      // it runs in the background; the window shows it
                }
                guard confirm("Clear saved stems" + size, "Clear", "Deletes the saved stems. rekordbox separates those tracks again the next time you load them.") else { continue }
                // the bridge works in that folder while rekordbox runs
                guard preflight() else { continue }
                let ok = safeRemove(defaultCacheDir, whole: true)
                log("saved stems cleared\(size): \(ok ? "ok" : cacheIsLink() ? "FAILED (the folder is a link)" : "FAILED")")
                if !ok { _ = alert("Saved stems not cleared", cacheIsLink() ? cacheLinkLine : "Some saved stems couldn't be deleted. See the log in this window.", ["OK"]) }
                prepareChanged()
                LocationMonitor.shared.invalidateSize()
                LocationMonitor.shared.update(size: true)
                continue                                        // back to the settings, now without stems
            default:
                return
            }
        }
    }
}
