// The window's three tabs, as in macOS's settings windows: Status (the two features and the
// prepared playlists), Prepare (a playlist from rekordbox's library, what preparing it takes,
// the run) and Manage (each feature's actions, then the log: Controller.swift). All three are
// 984 × 560, so the window never jumps between tabs; its title follows the tab. Every button is in the menu bar too (makeMenu, main.swift). The preparing itself is
// Prepare.swift's.
import AppKit
import UserNotifications

let tabSizes: [NSSize] = Array(repeating: NSSize(width: 984, height: 560), count: 3)
enum Tab: Int { case status = 0, prepare, manage }

/// The tabs: a toolbar of three items; the window takes the selected tab's size and name.
final class MainTabs: NSTabViewController {
    weak var window: NSWindow?

    override func tabView(_ tabView: NSTabView, didSelect item: NSTabViewItem?) {
        super.tabView(tabView, didSelect: item)
        guard let item = item, let w = window else { return }
        let i = tabView.indexOfTabViewItem(item)
        w.title = item.label + titleNote
        let size = tabSizes[max(0, min(i, tabSizes.count - 1))]
        let frame = w.frameRect(forContentRect: NSRect(origin: .zero, size: size))
        let old = w.frame
        w.setFrame(NSRect(x: old.minX, y: old.maxY - frame.height, width: frame.width, height: frame.height), display: true, animate: w.isVisible)
    }
}

func label(_ s: String, bold: Bool = false, size: CGFloat = 13, wrap: Bool = false, secondary: Bool = false) -> NSTextField {
    let l = wrap ? NSTextField(wrappingLabelWithString: s) : NSTextField(labelWithString: s)
    l.font = bold ? .boldSystemFont(ofSize: size) : .systemFont(ofSize: size)
    if secondary { l.textColor = .secondaryLabelColor }
    return l
}

func vstack(_ views: [NSView], spacing: CGFloat = 10) -> NSStackView {
    let s = NSStackView(views: views)
    s.orientation = .vertical
    s.alignment = .leading
    s.spacing = spacing
    return s
}

/// A table with fixed-height rows, its columns' titles and widths, inside a scroll view.
func makeTable(_ columns: [(id: String, title: String, width: CGFloat)], owner: NSObject & NSTableViewDataSource & NSTableViewDelegate) -> (NSScrollView, NSTableView) {
    let t = NSTableView()
    for c in columns {
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(c.id))
        col.title = c.title
        col.width = c.width
        t.addTableColumn(col)
    }
    t.rowHeight = 22
    t.usesAlternatingRowBackgroundColors = true
    t.columnAutoresizingStyle = .lastColumnOnlyAutoresizingStyle
    t.dataSource = owner
    t.delegate = owner
    let scroll = NSScrollView()
    scroll.documentView = t
    scroll.hasVerticalScroller = true
    scroll.borderType = .bezelBorder
    return (scroll, t)
}

/// A table cell: its text (cut in the middle when long, whole in its tooltip), and a symbol before it.
func cell(_ table: NSTableView, _ text: String, secondary: Bool = false, symbol: String? = nil) -> NSTableCellView {
    // a real width from the start, and the text's right edge "at most" the cell's: a cell made at
    // zero width can't hold its constraints, AppKit drops the left ones, and the text ends up
    // pinned to the right
    let v = NSTableCellView(frame: NSRect(x: 0, y: 0, width: 300, height: table.rowHeight))
    let l = NSTextField(labelWithString: text)
    l.lineBreakMode = .byTruncatingMiddle
    l.toolTip = text
    if secondary { l.textColor = .secondaryLabelColor }
    l.translatesAutoresizingMaskIntoConstraints = false
    v.addSubview(l)
    v.textField = l
    if let sym = symbol, let image = NSImage(systemSymbolName: sym, accessibilityDescription: nil) {
        let img = NSImageView(image: image)
        img.translatesAutoresizingMaskIntoConstraints = false
        v.addSubview(img)
        v.imageView = img
        img.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 2).isActive = true
        img.centerYAnchor.constraint(equalTo: v.centerYAnchor).isActive = true
        l.leadingAnchor.constraint(equalTo: img.trailingAnchor, constant: 4).isActive = true
    } else {
        l.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 4).isActive = true
    }
    l.trailingAnchor.constraint(lessThanOrEqualTo: v.trailingAnchor, constant: -4).isActive = true
    l.centerYAnchor.constraint(equalTo: v.centerYAnchor).isActive = true
    return v
}

private let dateOnly: DateFormatter = { let f = DateFormatter(); f.dateStyle = .medium; f.timeStyle = .none; return f }()

// MARK: - Status

final class StatusTab: NSViewController, NSTableViewDataSource, NSTableViewDelegate {
    unowned let c: Controller
    let plusTip = label("", wrap: true, secondary: true), cacheTip = label("", wrap: true, secondary: true)
    var sets: [KeptSet] = []
    /// what each set's check found ("Ready", "3 tracks to prepare again"), by name
    var states: [String: String] = [:]
    var checking = Set<String>()
    /// each set's tracks as its check found them in the playlist, by name
    var counts: [String: Int] = [:]
    /// bumped when things may have changed: a check started before says what isn't true now, and stops
    private var gen = 0 { didSet { jobs.forEach { $0.cancel() }; jobs = [] } }
    private var jobs: [HelperJob] = []
    /// What the checks said isn't so now (the app changed something): checks under way stop, and
    /// the next reload checks every set again.
    func forget() { gen += 1; checking.removeAll(); states.removeAll() }
    /// the set whose row shows the run's progress
    private var shownRunning: String?
    var table = NSTableView()
    let againBtn = NSButton(), stopBtn = NSButton()
    let runLine = label("", secondary: true)

    init(_ c: Controller) { self.c = c; super.init(nibName: nil, bundle: nil) }
    required init?(coder: NSCoder) { nil }

    override func loadView() {
        // each feature: its light and name, its tip under them, on a rounded panel as tall as its text
        let box = { (icon: NSImageView, name: NSTextField, tip: NSTextField) -> NSStackView in
            name.font = .boldSystemFont(ofSize: 14)
            let head = NSStackView(views: [icon, name])
            let b = vstack([head, tip], spacing: 6)
            b.edgeInsets = NSEdgeInsets(top: 12, left: 12, bottom: 12, right: 12)
            b.wantsLayer = true
            b.layer?.cornerRadius = 8
            b.layer?.backgroundColor = NSColor.quaternaryLabelColor.withAlphaComponent(0.08).cgColor
            b.setContentHuggingPriority(.required, for: .vertical)
            tip.preferredMaxLayoutWidth = 340
            return b
        }
        let features = NSStackView(views: [box(c.plusIcon, c.plusLabel, plusTip), box(c.cacheIcon, c.cacheLabel, cacheTip)])
        features.distribution = .fillEqually
        features.alignment = .top
        features.spacing = 16
        features.setContentHuggingPriority(.required, for: .vertical)
        let (scroll, t) = makeTable([("name", "Playlist", 300), ("tracks", "Tracks", 70), ("until", "Kept until", 130), ("state", "State", 220)], owner: self)
        table = t
        t.target = self
        t.doubleAction = #selector(prepareAgain)
        againBtn.title = "Prepare Again"; againBtn.target = self; againBtn.action = #selector(prepareAgain)
        stopBtn.title = "Stop Keeping…"; stopBtn.target = self; stopBtn.action = #selector(stopKeepingSet)
        let gap = NSView(); gap.setContentHuggingPriority(.init(1), for: .horizontal)
        let buttons = NSStackView(views: [runLine, gap, stopBtn, againBtn])
        let title = label("Prepared playlists", bold: true, size: 14)
        let note = label("Up to \(keptSetsMax) playlists, each kept until its date. Stems Cache leaves their stems alone until then.", secondary: true)
        let root = vstack([features, title, note, scroll, buttons], spacing: 10)
        root.edgeInsets = NSEdgeInsets(top: 16, left: 20, bottom: 16, right: 20)
        for v in [features, scroll, buttons] {
            v.translatesAutoresizingMaskIntoConstraints = false
            v.widthAnchor.constraint(equalTo: root.widthAnchor, constant: -40).isActive = true
        }
        scroll.setContentHuggingPriority(.init(1), for: .vertical)
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 160).isActive = true
        root.frame = NSRect(origin: .zero, size: tabSizes[Tab.status.rawValue])
        view = root
        preferredContentSize = tabSizes[Tab.status.rawValue]
    }

    override func viewWillAppear() { super.viewWillAppear(); reload() }

    /// The sets again, their names, and each one's check (in the background).
    func reload() {
        gen += 1
        let g = gen
        checking.removeAll()
        sets = keptSets()
        defer { table.reloadData() }                // "Checking…" only where no state is known yet
        updateButtons()
        if !sets.isEmpty { c.prepareTab.loadPlaylists() }      // the sets' names, as they are now
        // the number of tracks is the library's: known whatever can't be checked now
        for s in sets {
            DispatchQueue.global(qos: .userInitiated).async {
                let r = runPrepareHelper(["--tracks", s.name], timeout: 60)
                DispatchQueue.main.async {
                    guard g == self.gen else { return }
                    self.counts[s.name] = r.status == 0 ? parseTrackList(r.out).count : nil     // deleted: what the set holds
                    self.table.reloadData()
                }
            }
        }
        let f = prepareFacts()
        guard prepareProblems(f).isEmpty else {
            for s in sets { states[s.name] = "Can't be checked now" }
            return
        }
        for s in sets where c.prepareTab.runningSet != s.name {
            // a "can't be checked" isn't a state of the set: "Checking…" while it is checked now
            if states[s.name] == "Can't be checked now" || states[s.name] == "Couldn't be checked" { states[s.name] = nil }
            checking.insert(s.name)
            let job = HelperJob()
            jobs.append(job)
            let args = prepareBaseArgs(f) + ["--estimate", "--keep", s.name, "--wait-read", "1"]
            DispatchQueue.global(qos: .utility).async {
                let r = checkSet(s.name, args) { runPrepareHelper($0, timeout: 120, job: job) }
                let e = parseEstimate(r.out)
                DispatchQueue.main.async {
                    guard g == self.gen else { return }
                    self.checking.remove(s.name)
                    self.states[s.name] = setState(e)
                    self.table.reloadData()
                }
            }
        }
    }

    /// A check reads the playlists' tracks, which may bring up macOS's privacy prompt: not from a
    /// tab nobody is looking at, and not over rekordbox. Shown, it checks again (viewWillAppear).
    func reloadIfShowing() { if inFront(self), !c.actionRunning { reload() } }

    func updateButtons() {
        let row = table.selectedRow
        let ok = row >= 0 && row < sets.count && !c.prepareTab.isRunning && !c.actionRunning
        againBtn.isEnabled = ok
        stopBtn.isEnabled = row >= 0 && row < sets.count && c.prepareTab.runningSet != sets[row].name
        runLine.stringValue = c.prepareTab.runSummary
        // the row of the set being prepared says what this line says, and stops when the run does
        let now = c.prepareTab.runningSet
        if isViewLoaded, let col = table.tableColumns.firstIndex(where: { $0.identifier.rawValue == "state" }) {
            for name in Set([shownRunning, now].compactMap { $0 }) {
                if let row = sets.firstIndex(where: { $0.name == name }), row < table.numberOfRows {
                    table.reloadData(forRowIndexes: [row], columnIndexes: [col])
                }
            }
        }
        shownRunning = now
    }

    func numberOfRows(in tableView: NSTableView) -> Int { sets.count }
    func tableView(_ tableView: NSTableView, viewFor col: NSTableColumn?, row: Int) -> NSView? {
        let s = sets[row]
        switch col?.identifier.rawValue {
        case "name": return cell(tableView, c.prepareTab.playlistName(s.name))
        case "tracks":
            let n = counts[s.name] ?? s.files
            return cell(tableView, n > 0 ? "\(n)" : "–")
        case "until": return cell(tableView, s.until.map { dateOnly.string(from: $0) } ?? "–")
        default:
            // a state already known stays while it is checked again: it changes only if the check says otherwise
            let st = c.prepareTab.runningSet == s.name ? c.prepareTab.runSummary : states[s.name] ?? (checking.contains(s.name) ? "Checking…" : "Not checked")
            return cell(tableView, st, secondary: true)
        }
    }
    func tableViewSelectionDidChange(_ notification: Notification) { updateButtons() }

    @objc func prepareAgain() {
        let row = table.selectedRow
        guard row >= 0 && row < sets.count else { return }
        c.showTab(.prepare)
        c.prepareTab.select(id: sets[row].name)
    }

    @objc func stopKeepingSet() {
        let row = table.selectedRow
        guard row >= 0 && row < sets.count else { return }
        let s = sets[row], name = c.prepareTab.playlistName(s.name)
        guard c.confirm("Stop keeping “\(name)”", "Stop Keeping",
                        "Its stems stay in Stems Cache like any others, and may be removed when Stems Cache needs room.") else { return }
        c.log("prepare: stopped keeping playlist \(s.name): \(stopKeeping(s.name) ? "ok" : "FAILED")")
        states[s.name] = nil; counts[s.name] = nil
        reload()
    }

    /// The feature lights' tips, set by Controller.refresh.
    func setTips(plus: String, cache: String) { plusTip.stringValue = plus; cacheTip.stringValue = cache }
}

/// The notification's title when a run ends with the app in the background: "is prepared" only
/// for a run that exited normally with its last line, stopped nowhere, skipped nothing and saved
/// its set, and covered the whole playlist. `finished`: the tracks that reached their end;
/// `partial`: Prepare the First N Tracks.
func runEndTitle(_ name: String, status: Int32, gotDone: Bool, stopped: String?, finished: Int, skipped: Int, error: String?, partial: Bool = false) -> String {
    if status != 0 || !gotDone { return "“\(name)” couldn't be prepared" }
    if stopped == nil && skipped == 0 && error == nil && !partial { return "“\(name)” is prepared" }
    return finished == 0 ? "“\(name)” couldn't be prepared" : "“\(name)” isn't fully prepared"
}

/// The last run in one line, under the next estimate (the block above the buttons keeps three
/// lines: the estimate's two and this one).
func lastRunLine(files: Int, skipped: Int, stopped: String?, error: String? = nil) -> String {
    var s = "Last time: \(files) \(files == 1 ? "track" : "tracks") done"
    if skipped > 0 { s += ", \(skipped) skipped" }
    if let st = stopped { s += st == "stopped" ? ", stopped before the end" : st.contains("75%") ? ", stopped at the 75% share" : ", stopped early (see the log)" }
    if error != nil { s += ", the set couldn't be saved (see the log)" }
    return s + "."
}

/// Whether a tab is what the user is looking at: a check reads the playlists' tracks, which may
/// bring up macOS's privacy prompt, and never over rekordbox.
func inFront(_ tab: NSViewController) -> Bool {
    tab.isViewLoaded && tab.view.window?.isMiniaturized == false && NSApp.isActive
}

/// A set's state from its check.
func setState(_ e: Estimate?) -> String {
    guard let e = e else { return "Couldn't be checked" }
    if let r = e.refused { return r }
    if e.ready + e.prepare + e.skipped + e.unchecked == 0 { return "No tracks in the playlist" }
    if e.prepare == 0 && e.skipped == 0 { return e.unchecked > 0 ? "Ready (\(e.unchecked) not checked)" : "Ready" }
    var parts: [String] = []
    if e.prepare > 0 { parts.append("\(e.prepare) \(e.prepare == 1 ? "track" : "tracks") to prepare") }
    if e.skipped > 0 { parts.append("\(e.skipped) can't be read") }
    return parts.joined(separator: ", ")
}

// MARK: - Prepare

final class PNode: NSObject {
    let p: Playlist
    var children: [PNode] = []
    init(_ p: Playlist) { self.p = p }
}

/// The tree of playlists from --playlists' lines (parents come before their children).
func playlistTree(_ list: [Playlist]) -> [PNode] {
    var byID: [String: PNode] = [:], roots: [PNode] = []
    for p in list {
        let n = PNode(p)
        byID[p.id] = n
        if let parent = byID[p.parent] { parent.children.append(n) } else { roots.append(n) }
    }
    return roots
}

final class PrepareTab: NSViewController, NSOutlineViewDataSource, NSOutlineViewDelegate, NSTableViewDataSource, NSTableViewDelegate {
    unowned let c: Controller
    var playlists: [Playlist] = []
    var roots: [PNode] = []
    var loadError: String?
    let outline = NSOutlineView()
    private var reloadingList = false
    /// The folders the user closed, by id: a reload makes new rows and opens them all.
    private var closedFolders = Set<String>()
    let nameLabel = label("Choose a playlist", bold: true, size: 16)
    let info = label("", wrap: true)
    let until = NSDatePicker()
    let prepareBtn = NSButton(), checkBtn = NSButton(), pauseBtn = NSButton(), stopBtn = NSButton()
    let bar = NSProgressIndicator()
    let runGap = NSView()
    var tracksTable = NSTableView()
    /// the selected playlist, its estimate (nil while it runs or before), the lines shown
    var selected: Playlist?
    var estimate: Estimate?
    var estimating = false
    /// a check of the playlist already shown, out of sight: what it said stays until the new one
    /// comes, and Prepare clicked meanwhile waits for it (`prepareWhenChecked`)
    private var refreshing = false
    private var prepareWhenChecked = false
    /// something this app did changed what the tab shows (a setting, the location, an install):
    /// the next check is a visible one
    var stale = false
    private var gen = 0 { didSet { job?.cancel(); job = nil } }
    private var listGen = 0
    /// the estimate under way
    private var job: HelperJob?
    /// the row after the last track line: lines come in the rows' order
    private var nextRow = 0
    private var outlineDrawn = false
    var lines: [TrackLine] = []
    var problems: [String] = []
    /// the run, if one is going, and what it has said
    var run: PrepareRun?
    var runPlaylist: Playlist?
    var runDone = 0, runSkipped = 0, runFiles = 0
    var runFraction = 0.0
    /// the row of the track the run is separating, if the rows are the run's tracks
    var runRow: Int?
    var runPhase = ""
    /// the last run's summary, shown under the next estimate
    var lastRun = ""
    /// what the run's last line said: whether it came, and why it stopped early
    var runGotDone = false
    var runShort = ""
    var runError: String?
    var runPartial = false
    var runStopped: String?

    init(_ c: Controller) { self.c = c; super.init(nibName: nil, bundle: nil) }
    required init?(coder: NSCoder) { nil }

    var isRunning: Bool { run?.running == true }
    var runningSet: String? { isRunning ? runPlaylist?.id : nil }
    var runSummary: String {
        guard isRunning, let p = runPlaylist else { return "" }
        return "\(run?.paused == true ? "Paused" : "Preparing") “\(p.name)”: \(Int(runFraction * 100))%"
    }

    override func loadView() {
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("p"))
        col.title = "rekordbox's playlists"
        col.width = 270
        col.resizingMask = .autoresizingMask
        outline.addTableColumn(col)
        outline.outlineTableColumn = col
        outline.columnAutoresizingStyle = .firstColumnOnlyAutoresizingStyle
        outline.autoresizesOutlineColumn = false
        outline.headerView = nil
        outline.rowHeight = 22
        outline.dataSource = self
        outline.delegate = self
        let left = NSScrollView()
        left.documentView = outline
        left.hasVerticalScroller = true
        left.borderType = .bezelBorder
        until.datePickerStyle = .textFieldAndStepper
        until.datePickerElements = .yearMonthDay
        until.dateValue = Date().addingTimeInterval(Double(keepDaysDefault) * 86400)
        until.minDate = Date().addingTimeInterval(86400)
        until.maxDate = Date().addingTimeInterval(365 * 86400)
        let keepRow = NSStackView(views: [label("Keep the stems until"), until])
        for (b, t, a) in [(prepareBtn, "Prepare", #selector(prepareAction)), (checkBtn, "Check Again", #selector(checkAgain)),
                          (pauseBtn, "Pause", #selector(pauseAction)), (stopBtn, "Stop", #selector(stopAction))] {
            b.title = t; b.target = self; b.action = a; b.bezelStyle = .rounded
        }
        prepareBtn.keyEquivalent = "\r"
        // one row: Check Again and Prepare, or, while a run goes, its progress bar, Pause and Stop
        // in their place (the same height: nothing below moves)
        bar.style = .bar
        bar.isIndeterminate = false
        bar.minValue = 0; bar.maxValue = 1
        bar.setContentHuggingPriority(.init(1), for: .horizontal)
        runGap.setContentHuggingPriority(.init(1), for: .horizontal)
        let buttons = NSStackView(views: [checkBtn, bar, runGap, pauseBtn, stopBtn, prepareBtn])
        let (scroll, t) = makeTable([("track", "Track", 240), ("result", "Result", 90), ("why", "Note", 280)], owner: self)
        tracksTable = t
        let right = vstack([nameLabel, info, keepRow, buttons, scroll], spacing: 10)
        for v in [info, buttons, scroll] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            v.widthAnchor.constraint(equalTo: right.widthAnchor).isActive = true
        }
        info.preferredMaxLayoutWidth = 620
        // the summary and its note (one block) keep three lines, also while the playlist is
        // checked, so the rows below don't move when the estimate comes
        let line = ceil(NSFont.systemFont(ofSize: 13).boundingRectForFont.height)
        info.heightAnchor.constraint(greaterThanOrEqualToConstant: 3 * line).isActive = true
        scroll.setContentHuggingPriority(.init(1), for: .vertical)
        let root = NSStackView(views: [left, right])
        root.alignment = .top
        root.spacing = 16
        root.edgeInsets = NSEdgeInsets(top: 16, left: 20, bottom: 16, right: 20)
        left.translatesAutoresizingMaskIntoConstraints = false
        left.widthAnchor.constraint(equalToConstant: 300).isActive = true
        left.heightAnchor.constraint(equalTo: root.heightAnchor, constant: -32).isActive = true
        right.heightAnchor.constraint(equalTo: root.heightAnchor, constant: -32).isActive = true
        right.translatesAutoresizingMaskIntoConstraints = false
        right.widthAnchor.constraint(equalTo: root.widthAnchor, constant: -(300 + 16 + 40)).isActive = true
        root.frame = NSRect(origin: .zero, size: tabSizes[Tab.prepare.rawValue])
        view = root
        preferredContentSize = tabSizes[Tab.prepare.rawValue]
        update()
    }

    override func viewWillAppear() {
        super.viewWillAppear()
        if !outlineDrawn && !roots.isEmpty { drawOutline() }        // read before the tab was first shown
        reloadList(quiet: !stale)
        stale = false
    }

    func playlistName(_ id: String) -> String { playlists.first { $0.id == id }?.name ?? "Playlist \(id)" }

    /// rekordbox's playlists, read by the helper (in the background).
    func loadPlaylists(then: (() -> Void)? = nil) {
        problems = prepareProblems(prepareFacts { [weak self] in
            guard let self = self else { return }
            self.check(results: inFront(self))
        })
        guard FileManager.default.isExecutableFile(atPath: prepareHelper()), appleSilicon else { estimating = false; refreshing = false; prepareWhenChecked = false; tracksTable.reloadData(); update(); return }
        listGen += 1
        let g = listGen
        DispatchQueue.global(qos: .userInitiated).async {
            let r = runPrepareHelper(["--playlists"], timeout: 60)
            DispatchQueue.main.async {
                // only the last read says what the library holds now
                if g == self.listGen { self.show(r) }
                then?()
            }
        }
    }

    /// What --playlists said. The list on the left is redrawn only when it changed.
    private func show(_ r: (status: Int32, out: String, err: String)) {
        let list = r.status == 0 ? parsePlaylists(r.out) : playlists
        loadError = r.status == 0 ? nil : r.err.isEmpty ? "rekordbox's library can't be read." : r.err
        if list != playlists {
            playlists = list
            roots = playlistTree(list)
            if isViewLoaded { drawOutline() }
        } else if isViewLoaded && !outlineDrawn && !roots.isEmpty { drawOutline() }
        if isRunning, let id = runPlaylist?.id, let now = list.first(where: { $0.id == id }) { runPlaylist = now }
        if let id = selected?.id {
            if let now = list.first(where: { $0.id == id }) { selected = now } else { choose(nil) }    // renamed, or deleted
        }
        reloadingList = true
        selectRow(selected?.id, open: false)
        reloadingList = false
        c.statusTab.table.reloadData()
        update()
    }

    /// The list on the left from `roots`: every folder open, except those the user closed.
    private func drawOutline() {
        reloadingList = true
        outline.reloadData()
        outline.expandItem(nil, expandChildren: true)
        closeFolders(in: roots)
        reloadingList = false
        outlineDrawn = true
    }

    /// Closes the folders the user closed, except one the selected playlist is in, whose row stays
    /// visible. Returns whether the selected playlist is among `nodes` or below them.
    @discardableResult private func closeFolders(in nodes: [PNode]) -> Bool {
        var holds = false
        for n in nodes {
            let inside = closeFolders(in: n.children)
            if inside { closedFolders.remove(n.p.id) }
            else if closedFolders.contains(n.p.id) { outline.collapseItem(n) }
            if inside || n.p.id == selected?.id { holds = true }
        }
        return holds
    }

    /// Selects playlist `id` (Status' Prepare Again) and checks it.
    func select(id: String) {
        guard !isRunning else { return }
        let go = {
            guard let p = self.playlists.first(where: { $0.id == id }) else { return }
            self.selectRow(id)
            self.choose(p)
        }
        if playlists.isEmpty { loadPlaylists(then: go) } else { go() }
    }

    /// Selects playlist `id`'s row, if it is there; with `open`, opening the folders it is in. Main
    /// thread.
    private func selectRow(_ id: String?, open: Bool = true) {
        guard let id = id else { return }
        func path(_ nodes: [PNode]) -> [PNode]? {
            for n in nodes {
                if n.p.id == id { return [n] }
                if let below = path(n.children) { return [n] + below }
            }
            return nil
        }
        guard let chain = path(roots), let node = chain.last else { return }
        for folder in chain.dropLast() where open && !outline.isItemExpanded(folder) {
            outline.expandItem(folder)
            closedFolders.remove(folder.p.id)
        }
        let row = outline.row(forItem: node)
        // already there: the list stays where the user scrolled it
        if row >= 0, row != outline.selectedRow { outline.selectRowIndexes([row], byExtendingSelection: false); outline.scrollRowToVisible(row) }
    }

    func choose(_ p: Playlist?) {
        guard !isRunning else { return }
        if p?.id != selected?.id { lastRun = ""; lines = [] }
        selected = p
        estimate = nil
        recheck()
    }

    @objc func recheck() { check(results: true) }

    /// The checks again, the selected playlist's tracks from the library, then, with `results` and
    /// nothing in the way, what each one needs, filling the rows as it comes. Reading the library
    /// looks at no track, so the tracks are always shown; `results` reads them, which may bring up
    /// macOS's privacy prompt: not for a tab out of sight.
    private func check(results: Bool, quiet: Bool = false) {
        let facts = prepareFacts { [weak self] in self?.update() }
        problems = prepareProblems(facts)
        gen += 1
        let g = gen
        if isRunning { estimating = false; refreshing = false; prepareWhenChecked = false; update(); return }
        guard let p = selected, p.canPrepare else {
            estimating = false; estimate = nil; lines = []
            tracksTable.reloadData()
            update()
            return
        }
        let checks = results && problems.isEmpty
        if quiet && !results && problems.isEmpty && estimate != nil {
            // left the tab or the app before the playlists came: what is shown stays, and coming
            // back checks again
            refreshing = false; prepareWhenChecked = false; estimating = false
            tracksTable.reloadData()
            update()
            return
        }
        // `quiet`: the playlist shown is checked again in the background; what it shows changes
        // only where the check says something else
        let silent = quiet && checks && estimate != nil
        refreshing = silent
        if !silent {
            prepareWhenChecked = false
            estimating = checks
            if !checks { estimate = nil }
            lines = notChecked(lines)
            tracksTable.reloadData()
        }
        nextRow = 0
        update()
        let args = prepareBaseArgs(facts) + ["--estimate", "--playlist", p.id, "--until", untilArg()]
        DispatchQueue.global(qos: .userInitiated).async {
            let list = runPrepareHelper(["--tracks", p.id], timeout: 60)
            DispatchQueue.main.async {
                guard g == self.gen else { return }             // moved on: no check of this one
                if list.status == 0 {
                    let now = parseTrackList(list.out)
                    if silent {                                 // the rows shown stay for tracks still there
                        let shown = Dictionary(self.lines.map { (Array($0.file.utf8), $0) }, uniquingKeysWith: { a, _ in a })
                        let rows = now.map { shown[Array($0.file.utf8)] ?? $0 }
                        if rows != self.lines { self.lines = rows; self.tracksTable.reloadData() }
                    } else { self.lines = now; self.tracksTable.reloadData() }
                }
                else if !checks {                               // an estimate would say why itself
                    self.lines = []
                    self.problems.append(list.err.isEmpty ? "rekordbox's library can't be read." : list.err)
                    self.tracksTable.reloadData()
                    self.update()
                }
                guard checks else { return }
                if inFront(self) { self.runEstimate(args, g) }  // left the tab or the app meanwhile: no read now
                else if silent {                                // checked on coming back
                    self.refreshing = false; self.prepareWhenChecked = false; self.estimating = false
                    self.tracksTable.reloadData()
                    self.update()
                }
                else { self.estimating = false; self.estimate = nil; self.tracksTable.reloadData(); self.update() }
            }
        }
    }

    /// The estimate behind check, in the background: its tracks' rows as they come, then all of it.
    private func runEstimate(_ args: [String], _ g: Int) {
        let job = HelperJob()
        self.job = job
        DispatchQueue.global(qos: .userInitiated).async {
            let r = runPrepareHelper(args, timeout: 300, job: job) { t in
                DispatchQueue.main.async {
                    guard g == self.gen else { return }
                    let before = self.lines
                    let row = place(t, in: &self.lines, hint: self.nextRow)
                    self.nextRow = row + 1
                    if self.lines == before { return }          // the same as shown
                    if row < self.tracksTable.numberOfRows {
                        self.tracksTable.reloadData(forRowIndexes: [row], columnIndexes: IndexSet(integersIn: 0..<self.tracksTable.numberOfColumns))
                    } else { self.tracksTable.reloadData() }
                }
            }
            let e = parseEstimate(r.out)
            DispatchQueue.main.async {
                guard g == self.gen else { return }
                let wasChecking = self.estimating || self.refreshing     // rows saying "Checking…"
                self.estimating = false
                self.refreshing = false
                self.estimate = e
                let rows = e.flatMap { $0.refused == nil ? $0.tracks : nil } ?? notChecked(self.lines)
                if rows != self.lines || wasChecking { self.lines = rows; self.tracksTable.reloadData() }
                if e == nil { self.problems = [r.err.isEmpty ? "The playlist couldn't be checked." : r.err] }
                self.update()
                // a Prepare clicked meanwhile: only where the user still is, and only if the new
                // estimate leaves the button on
                if self.prepareWhenChecked {
                    self.prepareWhenChecked = false
                    if self.prepareBtn.isEnabled, inFront(self), NSApp.modalWindow == nil, self.view.window?.attachedSheet == nil { self.prepareAction() }
                }
            }
        }
    }

    @objc func checkAgain() { reloadList() }

    /// The playlists again (one may be new or renamed in rekordbox), then the check; "Checking"
    /// meanwhile, so Prepare doesn't act on the last estimate. `quiet` (coming back to the tab or
    /// the app): a playlist already checked keeps what it shows until the new check says otherwise.
    func reloadList(quiet: Bool = false) {
        guard !isRunning else { loadPlaylists(); recheck(); return }
        let shown = selected?.id
        let silent = quiet && !estimating && estimate != nil && shown != nil && problems.isEmpty
        gen += 1                                        // a check under way is of what was true before
        if silent { refreshing = true }
        else {
            prepareWhenChecked = false
            estimating = selected?.canPrepare == true && problems.isEmpty
            lines = notChecked(lines)
            tracksTable.reloadData()
            update()
        }
        loadPlaylists { [weak self] in
            guard let self = self else { return }
            self.check(results: inFront(self), quiet: silent && self.selected?.id == shown)
        }
    }

    func untilArg() -> String {
        var cal = Calendar.current
        cal.timeZone = .current
        let end = cal.date(bySettingHour: 23, minute: 59, second: 0, of: until.dateValue) ?? until.dateValue
        return String(Int(max(end.timeIntervalSince1970, Date().timeIntervalSince1970 + 3600)))
    }

    /// Every label and button as things are now.
    func update() {
        guard isViewLoaded else { return }
        let running = isRunning
        if let p = runPlaylist, running { nameLabel.stringValue = p.name }
        else { nameLabel.stringValue = selected?.name ?? (loadError == nil ? "Choose a playlist" : "rekordbox's playlists") }
        var text = "", sub = ""
        if let err = loadError { text = err }
        else if !problems.isEmpty { text = problems.joined(separator: "\n") }
        else if running { text = runSummary + "\n" + runPhase }
        else if let p = selected, !p.canPrepare {
            text = p.kind == "folder" ? "A folder: choose one of its playlists." : p.kind == "intelligent" ? "Intelligent playlists can't be prepared: their tracks change as you add music." : "This list can't be prepared."
        } else if selected != nil && estimating { text = "Checking the playlist…" }
        else if let e = estimate {
            if let r = e.refused { text = r }
            else {
                text = estimateSummary(e)
                if !e.diskOK { text += " The disk doesn't have room for them." }
                if e.asking > 0 { sub = "macOS is asking whether RB Stems Plus may read some tracks: answer its prompt, then click Check Again." }
                else if !lastRun.isEmpty { sub = lastRun }
            }
        } else if selected == nil { text = "Choose a playlist on the left to see what preparing it takes. Its stems are kept until the date below, then Stems Cache treats them like any others." }
        else if !runPhase.isEmpty { text = runPhase }
        let body = NSMutableAttributedString(string: text, attributes: [.font: NSFont.systemFont(ofSize: 13), .foregroundColor: NSColor.labelColor])
        if !sub.isEmpty {
            body.append(NSAttributedString(string: "\n" + sub, attributes: [.font: NSFont.systemFont(ofSize: 13), .foregroundColor: NSColor.secondaryLabelColor]))
        }
        info.attributedStringValue = body
        let ready = !running && !c.actionRunning && problems.isEmpty && loadError == nil && selected?.canPrepare == true && !estimating
        prepareBtn.isEnabled = ready && estimate != nil && estimate?.refused == nil && (estimate?.prepare ?? 0) + (estimate?.ready ?? 0) > 0
        prepareBtn.title = (estimate?.prepare ?? 1) == 0 && estimate?.ready ?? 0 > 0 ? "Keep" : "Prepare"
        checkBtn.isEnabled = !running && selected != nil
        until.isEnabled = !running
        pauseBtn.isHidden = !running; stopBtn.isHidden = !running; bar.isHidden = !running
        checkBtn.isHidden = running; prepareBtn.isHidden = running; runGap.isHidden = running
        pauseBtn.title = run?.paused == true ? "Resume" : "Pause"
        bar.doubleValue = runFraction
        outline.isEnabled = !running
        c.statusTab.updateButtons()
    }

    // MARK: the run

    @objc func prepareAction() {
        guard let p = selected, let e = estimate, !isRunning, !c.actionRunning else { return }
        guard loadError == nil, e.refused != nil || e.prepare + e.ready > 0 else { return }
        if refreshing {                                 // the check under way says what to do: shown
            prepareWhenChecked = true
            estimating = true
            tracksTable.reloadData()
            update()
            return
        }
        if let r = e.refused { _ = c.alert("Can't prepare “\(p.name)”", r, ["OK"]); return }
        var first: Int? = nil
        var facts = prepareFacts()
        // the limit or the location may have changed since the estimate: never a run on what it said
        guard prepareProblems(facts).isEmpty, facts.cacheFolder != nil, abs(facts.maxGB - e.maxGB) <= 0.001 * facts.maxGB else { recheck(); return }
        if !e.fits {
            let free = max(0, (Double(freeBytes(facts.cacheFolder ?? defaultCacheDir) ?? 0) - Double(1 << 30)) / Double(1 << 30))
            let d = doesntFitChoices(e, freeGB: free)
            let choice = c.alert("“\(p.name)” doesn't fit", d.message, d.buttons)
            if choice.hasPrefix("Prepare the First") {
                first = e.fitFiles
            } else if choice.hasPrefix("Raise"), let r = d.raiseTo {
                let days = readConfig().maxDays
                guard writeConfig(maxGB: r, maxDays: days) else { _ = c.alert("Limit not raised", "RB Stems Plus couldn't write its settings file. See the log.", ["OK"]); return }
                c.log("settings: max_gb=\(configNumber(r)) (raised to prepare playlist \(p.id))")
                facts = prepareFacts()
            } else { return }
        } else if !e.diskOK {
            guard c.alert("Not enough free space", "The disk Stems Cache saves on doesn't have room for \(sizeText(e.needGB)) more. Prepare anyway? It stops when the disk gets low.", ["Prepare", "Cancel"]) == "Prepare" else { return }
        }
        // the app may have been left during a sheet, and come back to a new check
        guard !estimating, !refreshing, estimate == e else { recheck(); return }
        startRun(p, prepareBaseArgs(facts) + runArgs(p.id, until: untilArg(), first: first), partial: first != nil)
    }

    func startRun(_ p: Playlist, _ args: [String], partial: Bool = false) {
        guard !c.actionRunning, !isRunning else { return }      // an install or uninstall changes what it reads
        let r = PrepareRun(playlist: p.id)
        runGotDone = false
        runStopped = nil
        runError = nil
        runPartial = partial
        gen += 1                                    // a check under way isn't this run's list
        estimating = false
        refreshing = false
        prepareWhenChecked = false
        nextRow = 0
        run = r
        runPlaylist = p
        runDone = 0; runSkipped = 0; runFiles = max(1, estimate?.tracks.count ?? 1); runFraction = 0
        runPhase = "Starting…"
        runRow = nil
        tracksTable.reloadData()
        c.log("prepare: playlist \(p.id), kept until \(dateOnly.string(from: until.dateValue))")
        let ok = r.start(args, line: { [weak self] l in self?.runLine(l) }, ended: { [weak self] status, err in self?.runEnded(status, err) })
        if !ok { run = nil; runPhase = "The helper couldn't start."; c.log("prepare: the helper couldn't start") }
        update()
        c.statusTab.reloadIfShowing()
    }

    func runLine(_ l: RunLine) {
        switch l {
        case .progress(let f, let files, let chunk, let chunks):
            runFiles = max(1, files)
            runFraction = (Double(f) + Double(chunk) / Double(max(1, chunks))) / Double(runFiles)
            runPhase = "Track \(f + 1) of \(files)."                 // how far: the bar
            // the rows are the run's tracks in the same order, unless the list changed under it
            let row = (runPartial || files == lines.count) && f < lines.count ? f : nil
            if row != runRow {
                runRow = row
                tracksTable.reloadData()
                if let row = row { tracksTable.scrollRowToVisible(row) }
            }
        case .track(let t):
            let row = place(t, in: &lines, hint: nextRow)
            nextRow = row + 1
            if t.result == "skipped" { runSkipped += 1; c.log("prepare: a track skipped: \(noPaths(t.why))") }
            else if t.result == "prepared" { runDone += 1 }     // a track cut short isn't done
            tracksTable.reloadData()
            tracksTable.scrollRowToVisible(row)
        case .room(let n, let gb):
            c.log("prepare: made room: \(n) stems removed, the cache now \(String(format: "%.2f", gb)) GB")
        case .done(_, let stopped, let gb, let err):
            runGotDone = true
            runStopped = stopped
            runError = err
            runShort = lastRunLine(files: runDone, skipped: runSkipped, stopped: stopped, error: err)
            var s = "\(runDone) \(runDone == 1 ? "track" : "tracks") done"
            if runSkipped > 0 { s += ", \(runSkipped) skipped" }
            s += ". Prepared playlists use \(sizeText(gb))."
            if let st = stopped { s += st == "stopped" ? " Stopped: the tracks not done yet aren't prepared." : " Stopped: \(st)." }
            if let e = err { s += " \(e)" }
            runPhase = s
            c.log("prepare: playlist \(runPlaylist?.id ?? ""): \(noPaths(s))")
        }
        update()
    }

    func runEnded(_ status: Int32, _ err: String) {
        if status != 0 && !err.isEmpty { runPhase = err; c.log("prepare: the helper stopped: \(noPaths(err))") }
        else if !runGotDone { runPhase = "The preparing stopped unexpectedly. Click Check Again, then Prepare."; c.log("prepare: the helper ended without its last line (status \(status))") }

        run = nil
        runFraction = 0
        runRow = nil
        let p = runPlaylist
        runPlaylist = nil
        lastRun = runGotDone ? runShort : runPhase
        if let p = p { c.statusTab.states[p.id] = nil }     // what it said before the run isn't so now
        // what is ready now; the run's summary stays below it. Not over rekordbox: a new track may
        // bring up a privacy prompt. Coming back to the app checks.
        if inFront(self) { reloadList() } else { estimate = nil; tracksTable.reloadData(); update() }    // the rows: what the run did
        c.statusTab.reloadIfShowing()
        if NSApp.isActive == false, let p = p {
            notify(title: runEndTitle(p.name, status: status, gotDone: runGotDone, stopped: runStopped, finished: runDone, skipped: runSkipped, error: runError, partial: runPartial), body: runPhase)
        }
    }

    @objc func pauseAction() {
        guard let r = run else { return }
        if r.paused { r.resume(); c.log("prepare: resumed") } else { r.pause(); c.log("prepare: paused") }
        update()
    }

    @objc func stopAction() { run?.stop(); runPhase = "Stopping…"; c.log("prepare: stop asked"); update() }

    /// Stops a run and waits for it to write its set (Quit, actions that change rekordbox). True
    /// once no run is going.
    func stopAndWait(_ seconds: TimeInterval = 30) -> Bool {
        guard let r = run, r.running else { return true }
        r.stop()
        let end = Date().addingTimeInterval(seconds)
        while r.running && Date() < end { RunLoop.current.run(until: Date().addingTimeInterval(0.1)) }
        return !r.running
    }

    // MARK: the outline and the tracks

    func outlineView(_ o: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int { (item as? PNode)?.children.count ?? roots.count }
    func outlineView(_ o: NSOutlineView, child index: Int, ofItem item: Any?) -> Any { (item as? PNode)?.children[index] ?? roots[index] }
    func outlineView(_ o: NSOutlineView, isItemExpandable item: Any) -> Bool { !((item as? PNode)?.children.isEmpty ?? true) }
    func outlineView(_ o: NSOutlineView, viewFor col: NSTableColumn?, item: Any) -> NSView? {
        guard let n = item as? PNode else { return nil }
        let symbol = n.p.kind == "folder" ? "folder" : n.p.kind == "intelligent" ? "gearshape" : "music.note.list"
        return cell(o, n.p.canPrepare ? "\(n.p.name)  (\(n.p.tracks))" : n.p.name, secondary: !n.p.canPrepare, symbol: symbol)
    }
    func outlineViewItemDidCollapse(_ notification: Notification) {
        if !reloadingList, let n = notification.userInfo?["NSObject"] as? PNode { closedFolders.insert(n.p.id) }
    }
    func outlineViewItemDidExpand(_ notification: Notification) {
        if !reloadingList, let n = notification.userInfo?["NSObject"] as? PNode { closedFolders.remove(n.p.id) }
    }
    func outlineViewSelectionDidChange(_ notification: Notification) {
        guard !reloadingList else { return }
        let n = outline.item(atRow: outline.selectedRow) as? PNode
        if n?.p.id != selected?.id { choose(n?.p) }
    }

    func numberOfRows(in tableView: NSTableView) -> Int { lines.count }
    func tableView(_ tableView: NSTableView, viewFor col: NSTableColumn?, row: Int) -> NSView? {
        let t = lines[row]
        switch col?.identifier.rawValue {
        case "track": return cell(tableView, t.name)
        case "result":
            if isRunning, row == runRow, t.result == "prepare" { return cell(tableView, "Preparing…") }
            return cell(tableView, resultText(t, checking: (estimating || refreshing && estimate?.refused == nil) && !isRunning))
        default:
            var w = t.why
            if w.isEmpty && t.result == "prepare" && t.missing > 0 { w = "\(t.missing) of its stems were removed from the cache" }
            if w.isEmpty && t.result == "prepare" { w = durationText(t.seconds) }
            // whether it was separated now: the number of chunks (~11 s each) says nothing to the user
            if w.isEmpty && t.result == "prepared" { w = preparedNote(t) }
            return cell(tableView, w, secondary: true)
        }
    }
}

/// A notification (Notification Center), asking macOS for permission the first time: the app's
/// when a run ends while it is in the background, the watcher's when a prepared playlist isn't
/// ready as rekordbox opens. Never a dialog.
func notify(title: String, body: String) {
    let center = UNUserNotificationCenter.current()
    center.requestAuthorization(options: [.alert, .sound]) { ok, _ in
        guard ok else { return }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        center.add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
    }
}

// MARK: - the controller's tab and menu actions

extension Controller {
    func showTab(_ t: Tab) { tabs.selectedTabViewItemIndex = t.rawValue }
    @objc func showStatusTab() { showTab(.status) }
    @objc func showPrepareTab() { showTab(.prepare) }
    @objc func showManageTab() { showTab(.manage) }
    @objc func preparePlaylistMenu() { showTab(.prepare); window.makeFirstResponder(prepareTab.outline) }
    @objc func checkAgainMenu() {
        // the tab checks as it comes to the front
        let shown = tabs.selectedTabViewItemIndex == Tab.prepare.rawValue
        showTab(.prepare)
        if shown { prepareTab.checkAgain() }
    }
    @objc func pauseMenu() { prepareTab.pauseAction() }
    @objc func stopMenu() { prepareTab.stopAction() }
    @objc func showLogMenu() { showTab(.manage); text.scrollToEndOfDocument(nil) }
}
