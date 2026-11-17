// The window's three tabs, as in macOS's settings windows: Status (the two features and the
// prepared playlists), Prepare (a playlist from rekordbox's library, what preparing it takes,
// the run) and Manage (each feature's actions, then the log: Controller.swift). All three are
// 984 × 560, so the window never jumps between tabs; its title follows the tab. The Status and
// Prepare tabs are the shell's (app/Shell/ShellTabs.swift, drawn from the core's snapshots, M7's
// switch-over); this file keeps the views they share: the tab controller, labels, tables, cells,
// and the notification the core's NOTIFY step posts.
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
