// The dialogs, in the app's own style: used by the app (as sheets on its window) and by the
// watcher (as a small window of its own).
import AppKit

/// A sheet in the app's own style (replaces NSAlert): a title, a message, an optional password
/// field or labelled text fields (Settings), then the buttons. Stacked (default): full width, one
/// under the other, like the action column, the first (blue, Return) on top. Inline: in a row,
/// the first on the right.
final class Sheet: NSObject {
    let panel: NSPanel
    let field: NSSecureTextField?
    var inputs: [NSTextField] = []       // the labelled fields, in order
    var buttonViews: [NSButton] = []
    var choice = ""

    init(title: String, message: String, buttons: [String], password placeholder: String? = nil,
         fields: [(label: String, value: String)] = [], inline: Bool = false) {
        let width: CGFloat = inline ? 520 : 320
        panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: width, height: 10), styleMask: [.titled], backing: .buffered, defer: true)
        field = placeholder == nil ? nil : NSSecureTextField()
        super.init()
        let titleLabel = NSTextField(wrappingLabelWithString: title)
        titleLabel.font = .boldSystemFont(ofSize: 14)
        var views: [NSView] = [titleLabel]
        if !message.isEmpty { views.append(NSTextField(wrappingLabelWithString: message)) }
        if let f = field { f.placeholderString = placeholder; views.append(f) }
        for (label, value) in fields {
            // the label on the left, a short field on the right
            let input = NSTextField(string: value)
            input.alignment = .right
            input.translatesAutoresizingMaskIntoConstraints = false
            input.widthAnchor.constraint(equalToConstant: 60).isActive = true      // "100000" fits
            let rowGap = NSView(); rowGap.setContentHuggingPriority(.init(1), for: .horizontal)
            // the label is never cut short: the gap gives way first
            let name = NSTextField(labelWithString: label)
            name.setContentCompressionResistancePriority(.required, for: .horizontal)
            let row = NSStackView(views: [name, rowGap, input])
            inputs.append(input)
            views.append(row)
        }
        let gap = NSView()
        gap.translatesAutoresizingMaskIntoConstraints = false
        gap.heightAnchor.constraint(equalToConstant: 2).isActive = true
        views.append(gap)
        let made: [NSButton] = buttons.enumerated().map { i, t in
            let b = NSButton(title: t, target: self, action: #selector(pressed(_:)))   // same style as the window's
            if i == 0 { b.keyEquivalent = "\r" } else if t == "Cancel" || t == "Remind Me Later" { b.keyEquivalent = "\u{1b}" }
            return b
        }
        buttonViews = made
        if inline {
            // "Don't Ask Again…" on its own on the left; the others on the right, the first rightmost
            let isOptOut: (NSButton) -> Bool = { $0.title.hasPrefix("Don't Ask Again") }
            let rowGap = NSView(); rowGap.setContentHuggingPriority(.init(1), for: .horizontal)
            let row = NSStackView(views: made.filter(isOptOut) + [rowGap] + made.filter { !isOptOut($0) }.reversed())
            row.spacing = 10
            views.append(row)
        } else {
            views.append(contentsOf: made)
        }
        let stack = NSStackView(views: views)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.edgeInsets = NSEdgeInsets(top: 18, left: 18, bottom: 18, right: 18)
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.widthAnchor.constraint(equalToConstant: width).isActive = true
        for v in views {
            v.translatesAutoresizingMaskIntoConstraints = false
            v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -36).isActive = true
        }
        panel.contentView = stack
        panel.setContentSize(stack.fittingSize)
    }

    @objc func pressed(_ b: NSButton) { choice = b.title; NSApp.stopModal() }

    /// Shows the panel as its own small window (the watcher has no app window to attach it to).
    func runAlone() -> String {
        panel.title = "RB Stems Plus"
        panel.level = .floating
        panel.center()
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
        NSApp.runModal(for: panel)
        panel.orderOut(nil)
        return choice
    }

    func run(on window: NSWindow) -> (choice: String, text: String) {
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
        window.beginSheet(panel)
        if let f = field ?? inputs.first { panel.makeFirstResponder(f) }
        NSApp.runModal(for: panel)
        window.endSheet(panel)
        panel.orderOut(nil)
        return (choice, field?.stringValue ?? "")
    }
}
