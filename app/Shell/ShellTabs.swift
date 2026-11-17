// The AppKit shell over the shared C core (the private Windows plan's mac-core §3.4, §4 PrepareView.swift,
// M7): the Status and Prepare tabs over the snapshot's rows. Their views are today's
// (PrepareView.swift's loadView, the same layout, the same property names, so the probe's field
// ids stay: status.table, prepare.outline, prepare.tracksTable, …); their data sources read the
// rows the shell holds (every count and cell from the same snapshot), and every user action goes
// to the core as an event. What stays here is view-handle state only: the outline's echo guard
// (reloadingList), its expansion handles, scroll and first responder (§3.4).
//
// Rows (core.h Tables): a new list_gen reloads the table; otherwise the changed rows are reloaded
// in place and appended rows are added. Every build (the app is this shell).
import AppKit

/// Applies one table's delta to an NSTableView (or outline): true when it reloaded everything.
@discardableResult
func applyRows(_ t: NSTableView, _ snap: OpaquePointer, _ d: OpaquePointer?, _ table: core_table) -> Bool {
    let rows = Int(core_rows(snap, table))
    guard let d = d, core_delta_reload(d, table) == 0 else { t.reloadData(); return true }
    let old = Int(core_delta_old_rows(d, table))
    // rows into an empty table: one reload, as 1.3.0 drew a new list (5,000 inserted rows cost more)
    if old == 0 && rows > 0 { t.reloadData(); return true }
    var n: Int32 = 0
    if let ch = core_delta_changed(d, table, &n), n > 0 {
        var set = IndexSet()
        for i in 0..<Int(n) { set.insert(Int(ch[i])) }
        t.reloadData(forRowIndexes: set, columnIndexes: IndexSet(integersIn: 0..<t.numberOfColumns))
    }
    if rows > old { t.insertRows(at: IndexSet(integersIn: old..<rows), withAnimation: []) }
    return false
}

/// A snapshot cell's text.
func cellStr(_ s: OpaquePointer?, _ col: core_column, _ row: Int) -> String {
    var n = 0
    return cString(core_cell_str(s, col, Int32(row), &n), n)
}

// MARK: - Status

final class ShellStatusTab: NSViewController, NSTableViewDataSource, NSTableViewDelegate {
    unowned let shell: CoreShell
    let plusTip = label("", wrap: true, secondary: true), cacheTip = label("", wrap: true, secondary: true)
    var table = NSTableView()
    let againBtn = NSButton(), stopBtn = NSButton()
    let runLine = label("", secondary: true)
    /// the echo guard: a selection the core made by render sends no select back
    private var applying = false

    init(_ s: CoreShell) { shell = s; super.init(nibName: nil, bundle: nil) }
    required init?(coder: NSCoder) { nil }

    override func loadView() {
        let c = shell.controller
        // StatusTab.loadView's layout, unchanged
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
        t.doubleAction = #selector(doubleClicked)
        againBtn.title = "Prepare Again"
        stopBtn.title = "Stop Keeping…"
        shell.button(againBtn, ClickID.prepareAgain)
        shell.button(stopBtn, ClickID.stopKeeping)
        // off until the first render gives their fields (never AppKit's default on)
        againBtn.isEnabled = false
        stopBtn.isEnabled = false
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
        if let s = shell.held { render(s, nil) }
    }

    @objc private func doubleClicked() { if table.clickedRow >= 0 { shell.clicked(againBtn) } }

    /// The lights' tips (S_APP, the lights' fields).
    func setTips(plus: String, cache: String) {
        if plusTip.stringValue != plus { plusTip.stringValue = plus }
        if cacheTip.stringValue != cache { cacheTip.stringValue = cache }
    }

    /// The S_STATUS section: the sets' rows, the selection, Prepare Again, Stop Keeping… and the
    /// run's line beside them (StatusTab.updateButtons' views).
    func render(_ s: OpaquePointer, _ d: OpaquePointer?) {
        guard isViewLoaded else { return }
        applyRows(table, s, d, T_STATUS_SETS)
        let sel = Int(core_i64(s, F_STATUS_SELECTED_ROW))
        if table.selectedRow != sel {
            applying = true
            if sel >= 0 && sel < table.numberOfRows { table.selectRowIndexes([sel], byExtendingSelection: false) } else { table.deselectAll(nil) }
            applying = false
        }
        againBtn.isEnabled = core_flag(s, F_STATUS_AGAIN_ENABLED) != 0
        stopBtn.isEnabled = core_flag(s, F_STATUS_STOP_ENABLED) != 0
        let line = snapStr(s, F_STATUS_RUN_LINE)
        if runLine.stringValue != line { runLine.stringValue = line }
    }

    func numberOfRows(in tableView: NSTableView) -> Int { shell.held.map { Int(core_rows($0, T_STATUS_SETS)) } ?? 0 }
    func tableView(_ tableView: NSTableView, viewFor col: NSTableColumn?, row: Int) -> NSView? {
        let h = shell.held
        switch col?.identifier.rawValue {
        case "name": return cell(tableView, cellStr(h, C_SET_NAME, row))
        case "tracks": return cell(tableView, cellStr(h, C_SET_TRACKS, row))
        case "until": return cell(tableView, cellStr(h, C_SET_UNTIL, row))
        default: return cell(tableView, cellStr(h, C_SET_STATE, row), secondary: true)
        }
    }
    func tableViewSelectionDidChange(_ notification: Notification) {
        guard !applying, let h = shell.held else { return }
        shell.send("{\"ev\":\"select\",\"table\":\"T_STATUS_SETS\",\"row\":\(table.selectedRow),\"gen\":\(core_list_gen(h, T_STATUS_SETS))}")
    }
}

// MARK: - Prepare

/// One outline row (rows never move within a list_gen, so the row index is its identity).
final class OutlineItem: NSObject {
    let row: Int
    var children: [OutlineItem] = []
    init(_ r: Int) { row = r }
}

final class ShellPrepareTab: NSViewController, NSOutlineViewDataSource, NSOutlineViewDelegate, NSTableViewDataSource, NSTableViewDelegate {
    unowned let shell: CoreShell
    let outline = NSOutlineView()
    /// the outline's echo guard: expansion and selection the shell sets send nothing back
    private var reloadingList = false
    private var roots: [OutlineItem] = []
    private var items: [OutlineItem] = []                // by row
    private var shownSelection = -1                      // the outline row selected now (the core's, or the user's just sent)
    let nameLabel = label("Choose a playlist", bold: true, size: 16)
    let info = label("", wrap: true)
    let until = NSDatePicker()
    let prepareBtn = NSButton(), checkBtn = NSButton(), pauseBtn = NSButton(), stopBtn = NSButton()
    let bar = NSProgressIndicator()
    let runGap = NSView()
    var tracksTable = NSTableView()
    private var scrollSeq: Int64 = 0, focusSeq: Int64 = 0, outlineScrollSeq: Int64 = 0
    /// the day the date picker's range was set from (yyyy-MM-dd)
    private var boundsDay = ""
    /// the summary and its note as drawn (the colours split there)
    private var shownInfo: (text: String, sub: String) = ("", "")

    init(_ s: CoreShell) { shell = s; super.init(nibName: nil, bundle: nil) }
    required init?(coder: NSCoder) { nil }

    override func loadView() {
        // PrepareTab.loadView's layout, unchanged
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
        until.target = self
        until.action = #selector(untilChanged)
        let keepRow = NSStackView(views: [label("Keep the stems until"), until])
        for (b, t, id) in [(prepareBtn, "Prepare", ClickID.prepare), (checkBtn, "Check Again", ClickID.checkAgain),
                           (pauseBtn, "Pause", ClickID.pause), (stopBtn, "Stop", ClickID.stop)] {
            b.title = t; b.bezelStyle = .rounded
            shell.button(b, id)
        }
        prepareBtn.keyEquivalent = "\r"
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
        // between runs: Check Again and Prepare; the run's bar, Pause and Stop hidden until the
        // first render says otherwise (F_PREP_RUNNING)
        for v in [bar, runGap, pauseBtn, stopBtn] as [NSView] { v.isHidden = true }
        if let s = shell.held { render(s, nil) }
    }

    @objc private func untilChanged() {
        shell.send("{\"ev\":\"until\",\"date\":\"\(ShellPrepareTab.day.string(from: until.dateValue))\"}")
    }

    /// The "Keep the stems until" date as the core holds it (F_PREP_UNTIL): YYYY-MM-DD, local time.
    static let day: DateFormatter = {
        let f = DateFormatter()
        f.calendar = Calendar(identifier: .gregorian)
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = .current
        f.dateFormat = "yyyy-MM-dd"
        return f
    }()

    /// The S_PREPARE section: PrepareTab.update()'s views, every value the core's.
    func render(_ s: OpaquePointer, _ d: OpaquePointer?) {
        guard isViewLoaded else { return }
        renderOutline(s, d)
        applyRows(tracksTable, s, d, T_PREP_TRACKS)
        let name = snapStr(s, F_PREP_NAME)
        if !name.isEmpty, nameLabel.stringValue != name { nameLabel.stringValue = name }
        // the summary in the label colour, its note after a line break in the secondary colour
        let text = snapStr(s, F_PREP_INFO), sub = snapStr(s, F_PREP_INFO_SUB)
        if shownInfo.text != text || shownInfo.sub != sub {
            shownInfo = (text, sub)
            let font = NSFont.systemFont(ofSize: 13)
            let body = NSMutableAttributedString(string: text, attributes: [.font: font, .foregroundColor: NSColor.labelColor])
            if !sub.isEmpty { body.append(NSAttributedString(string: "\n" + sub, attributes: [.font: font, .foregroundColor: NSColor.secondaryLabelColor])) }
            info.attributedStringValue = body
        }
        let running = core_flag(s, F_PREP_RUNNING) != 0
        prepareBtn.isEnabled = core_flag(s, F_PREP_PREPARE_ENABLED) != 0
        let title = snapStr(s, F_PREP_PREPARE_TITLE)
        if !title.isEmpty, prepareBtn.title != title { prepareBtn.title = title }
        checkBtn.isEnabled = core_flag(s, F_PREP_CHECK_ENABLED) != 0
        until.isEnabled = core_flag(s, F_PREP_UNTIL_ENABLED) != 0
        // the picker's range from today, as the core checks it (SO3: a tab left open past midnight)
        let today = ShellPrepareTab.day.string(from: Date())
        if boundsDay != today {
            boundsDay = today
            until.minDate = Date().addingTimeInterval(86400)
            until.maxDate = Date().addingTimeInterval(365 * 86400)
        }
        let date = snapStr(s, F_PREP_UNTIL)
        if !date.isEmpty, ShellPrepareTab.day.string(from: until.dateValue) != date, let d = ShellPrepareTab.day.date(from: date) {
            until.dateValue = d.addingTimeInterval(12 * 3600)       // its noon: the same day whatever the picker's bounds
        }
        // one row: Check Again and Prepare, or, while a run goes, its bar, Pause and Stop in their place
        for v in [pauseBtn, stopBtn, bar] as [NSView] where v.isHidden == running { v.isHidden = !running }
        for v in [checkBtn, prepareBtn, runGap] as [NSView] where v.isHidden != running { v.isHidden = running }
        let pause = snapStr(s, F_PREP_PAUSE_TITLE)
        if !pause.isEmpty, pauseBtn.title != pause { pauseBtn.title = pause }
        bar.doubleValue = core_f64(s, F_PREP_PROGRESS)
        outline.isEnabled = core_flag(s, F_PREP_OUTLINE_ENABLED) != 0
        applyOneShots(s)
    }

    /// The section's one-shot requests (the tracks' scroll, the outline's focus and scroll), each
    /// used up only once the tab is in a window (SO13): a tab loaded by the tab switch renders
    /// before it is in place, so viewDidAppear asks again.
    private func applyOneShots(_ s: OpaquePointer) {
        guard let w = view.window else { return }
        let seq = core_i64(s, F_PREP_SCROLL_SEQ)
        if seq != scrollSeq {
            scrollSeq = seq
            let r = Int(core_i64(s, F_PREP_SCROLL_ROW))
            if r >= 0 && r < tracksTable.numberOfRows { tracksTable.scrollRowToVisible(r) }
        }
        let scroll = core_i64(s, F_PREP_OUTLINE_SCROLL_SEQ)    // select(id:)'s scrollRowToVisible, once per new value
        if scroll != outlineScrollSeq {
            outlineScrollSeq = scroll
            let sel = Int(core_i64(s, F_PREP_SELECTED_ROW))
            let shown = sel >= 0 && sel < items.count ? outline.row(forItem: items[sel]) : -1
            if shown >= 0 { outline.scrollRowToVisible(shown) }
        }
        let focus = core_i64(s, F_PREP_FOCUS_SEQ)
        if focus != focusSeq { focusSeq = focus; w.makeFirstResponder(outline) }
    }

    override func viewDidAppear() {
        super.viewDidAppear()
        if let s = shell.held { applyOneShots(s) }
    }

    private func renderOutline(_ s: OpaquePointer, _ d: OpaquePointer?) {
        reloadingList = true
        defer { reloadingList = false }
        let n = Int(core_rows(s, T_PREP_OUTLINE))
        let reload = d == nil || core_delta_reload(d, T_PREP_OUTLINE) != 0 || core_delta_old_rows(d, T_PREP_OUTLINE) != Int32(n)
        if reload {
            // the tree from the flattened rows (pre-order: a parent before its children)
            items = (0..<n).map { OutlineItem($0) }
            roots = []
            for r in 0..<n {
                let p = Int(core_cell_i64(s, C_OUT_PARENT, Int32(r)))
                if p >= 0 && p < r { items[p].children.append(items[r]) } else { roots.append(items[r]) }
            }
            outline.reloadData()
            for it in items where !it.children.isEmpty && core_cell_flag(s, C_OUT_OPEN, Int32(it.row)) != 0 { outline.expandItem(it) }
        } else {
            var k: Int32 = 0
            if let ch = core_delta_changed(d, T_PREP_OUTLINE, &k), k > 0 {
                for i in 0..<Int(k) where Int(ch[i]) < items.count {
                    let it = items[Int(ch[i])]
                    outline.reloadItem(it)
                    let open = core_cell_flag(s, C_OUT_OPEN, Int32(it.row)) != 0
                    if !it.children.isEmpty && open != outline.isItemExpanded(it) { if open { outline.expandItem(it) } else { outline.collapseItem(it) } }
                }
            }
        }
        let sel = Int(core_i64(s, F_PREP_SELECTED_ROW))
        shownSelection = sel >= 0 && sel < items.count ? sel : -1
        let shown = sel >= 0 && sel < items.count ? outline.row(forItem: items[sel]) : -1
        if outline.selectedRow != shown {
            if shown >= 0 { outline.selectRowIndexes([shown], byExtendingSelection: false) } else { outline.deselectAll(nil) }
        }
    }

    // MARK: the outline

    func outlineView(_ o: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int { (item as? OutlineItem)?.children.count ?? roots.count }
    func outlineView(_ o: NSOutlineView, child index: Int, ofItem item: Any?) -> Any { (item as? OutlineItem)?.children[index] ?? roots[index] }
    func outlineView(_ o: NSOutlineView, isItemExpandable item: Any) -> Bool { !((item as? OutlineItem)?.children.isEmpty ?? true) }
    func outlineView(_ o: NSOutlineView, viewFor col: NSTableColumn?, item: Any) -> NSView? {
        guard let it = item as? OutlineItem, let h = shell.held else { return nil }
        let kind = core_cell_i64(h, C_OUT_KIND, Int32(it.row))      // 0 folder, 1 playlist, 2 intelligent, 3 another kind (schema.def)
        let symbol = kind == 0 ? "folder" : kind == 2 ? "gearshape" : "music.note.list"
        return cell(o, cellStr(h, C_OUT_TITLE, it.row), secondary: core_cell_flag(h, C_OUT_SECONDARY, Int32(it.row)) != 0, symbol: symbol)
    }
    func outlineViewItemDidCollapse(_ notification: Notification) { folder(notification, open: false) }
    func outlineViewItemDidExpand(_ notification: Notification) { folder(notification, open: true) }
    private func folder(_ n: Notification, open: Bool) {
        guard !reloadingList, let it = n.userInfo?["NSObject"] as? OutlineItem, let h = shell.held else { return }
        shell.send("{\"ev\":\"folder\",\"row\":\(it.row),\"open\":\(open),\"gen\":\(core_list_gen(h, T_PREP_OUTLINE))}")
    }
    func outlineViewSelectionDidChange(_ notification: Notification) {
        guard !reloadingList, let h = shell.held else { return }
        let row = (outline.item(atRow: outline.selectedRow) as? OutlineItem)?.row ?? -1
        guard row != shownSelection else { return }        // AppKit may say so twice (a collapse); today's choose() asks once
        shownSelection = row
        shell.send("{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":\(row),\"gen\":\(core_list_gen(h, T_PREP_OUTLINE))}")
    }

    // MARK: the tracks

    func numberOfRows(in tableView: NSTableView) -> Int { shell.held.map { Int(core_rows($0, T_PREP_TRACKS)) } ?? 0 }
    func tableView(_ tableView: NSTableView, viewFor col: NSTableColumn?, row: Int) -> NSView? {
        let h = shell.held
        switch col?.identifier.rawValue {
        case "track": return cell(tableView, cellStr(h, C_TRK_NAME, row))
        case "result": return cell(tableView, cellStr(h, C_TRK_RESULT, row))
        default: return cell(tableView, cellStr(h, C_TRK_WHY, row), secondary: true)
        }
    }
}
