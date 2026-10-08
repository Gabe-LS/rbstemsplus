// Settings… (Cmd+,): a sheet in the app's style with Stems Cache's two limits, written to
// config.ini, and "Clear saved stems" with the size they take.
import AppKit

extension Controller {
    @objc func showSettings() {
        guard !actionRunning else { return }
        let c = readConfig(log)
        var gb = configNumber(c.maxGB), days = configNumber(c.maxDays)
        while true {
            let size = savedStemsSize()
            let clear = "Clear Saved Stems" + size
            let sheet = Sheet(title: "Settings", message: "Stems Cache uses these settings the next time rekordbox opens.",
                              buttons: ["Save", clear, "Cancel"],
                              fields: [("Stems Cache size limit (GB)", gb), ("Remove stems unused for (days)", days)])
            sheet.buttonViews[1].isEnabled = FileManager.default.fileExists(atPath: cacheDir)
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
                if writeConfig(maxGB: g, maxDays: d) { log("settings: max_gb=\(configNumber(g)) max_days=\(configNumber(d))") }
                else {
                    log("settings: couldn't write \(configFile)")
                    _ = alert("Settings not saved", "RB Stems Plus couldn't write its settings file. See the log in this window.", ["OK"])
                }
                return
            case clear:
                guard confirm("Clear saved stems" + size, "Clear", "Deletes the saved stems. rekordbox separates those tracks again the next time you load them.") else { continue }
                // the bridge works in that folder while rekordbox runs
                guard preflight() else { continue }
                let ok = safeRemove(cacheDir, whole: true)
                log("saved stems cleared\(size): \(ok ? "ok" : cacheIsLink() ? "FAILED (the folder is a link)" : "FAILED")")
                if !ok { _ = alert("Saved stems not cleared", cacheIsLink() ? cacheLinkLine : "Some saved stems couldn't be deleted. See the log in this window.", ["OK"]) }
                continue                                        // back to the settings, now without stems
            default:
                return
            }
        }
    }
}
