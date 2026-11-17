// The AppKit shell over the shared C core (the private Windows plan's mac-core §3.5, §3.7, M7): the
// core's dialogs drawn as today's Sheet, the folder picker, and the password sheet, all shown
// through the presenter (Presenter.swift). The core decides every dialog's texts, buttons and
// order; this file only draws them and hands the answer back. Every build (the app is this shell).
import AppKit

/// One core dialog, read on the flow thread during host->dialog (its strings live until the
/// callback returns, so they are copied here).
struct DialogForm: Equatable {
    var id: Int32 = -1
    var title = "", message = ""
    var buttons: [String] = [], enabled: [Bool] = [], left: [Bool] = []
    var fields: [(label: String, value: String)] = []
    var rows: [(label: String, buttons: [String], enabled: [Bool])] = []
    var defaultButton: Int32 = 0, cancelButton: Int32 = -1
    var inline = false

    static func == (a: DialogForm, b: DialogForm) -> Bool {
        a.id == b.id && a.title == b.title && a.message == b.message && a.buttons == b.buttons && a.enabled == b.enabled && a.left == b.left
            && a.fields.map { [$0.label, $0.value] } == b.fields.map { [$0.label, $0.value] }
            && a.rows.map { [$0.label] + $0.buttons + $0.enabled.map { $0 ? "1" : "0" } } == b.rows.map { [$0.label] + $0.buttons + $0.enabled.map { $0 ? "1" : "0" } }
            && a.defaultButton == b.defaultButton && a.cancelButton == b.cancelButton && a.inline == b.inline
    }

    init() {}

    init(_ d: OpaquePointer) {
        func text(_ w: core_dlg_what, _ i: Int32 = 0, _ j: Int32 = 0) -> String {
            var n = 0
            guard let p = core_dlg_text(d, w, i, j, &n) else { return "" }
            return String(decoding: UnsafeRawBufferPointer(start: p, count: n), as: UTF8.self)
        }
        id = core_dlg_int(d, CORE_DLG_ID, 0, 0)
        title = text(CORE_DLG_TITLE)
        message = text(CORE_DLG_MESSAGE)
        for i in 0..<max(0, core_dlg_int(d, CORE_DLG_N_BUTTONS, 0, 0)) {
            buttons.append(text(CORE_DLG_BUTTON, i))
            enabled.append(core_dlg_int(d, CORE_DLG_BUTTON_ENABLED, i, 0) != 0)
            left.append(core_dlg_int(d, CORE_DLG_BUTTON_LEFT, i, 0) != 0)
        }
        for i in 0..<max(0, core_dlg_int(d, CORE_DLG_N_FIELDS, 0, 0)) {
            fields.append((text(CORE_DLG_FIELD_LABEL, i), text(CORE_DLG_FIELD_VALUE, i)))
        }
        for i in 0..<max(0, core_dlg_int(d, CORE_DLG_N_ROWS, 0, 0)) {
            let n = max(0, core_dlg_int(d, CORE_DLG_N_ROW_BUTTONS, i, 0))
            rows.append((text(CORE_DLG_ROW_LABEL, i), (0..<n).map { text(CORE_DLG_ROW_BUTTON, i, $0) },
                         (0..<n).map { core_dlg_int(d, CORE_DLG_ROW_BUTTON_ENABLED, i, $0) != 0 }))
        }
        defaultButton = core_dlg_int(d, CORE_DLG_DEFAULT, 0, 0)
        cancelButton = core_dlg_int(d, CORE_DLG_CANCEL, 0, 0)
        inline = core_dlg_int(d, CORE_DLG_INLINE, 0, 0) != 0
    }
}

/// What the user did with a dialog: a button, a row's button, or nothing (Cancel), with the
/// fields' texts as they were left.
struct DialogAnswer: Equatable {
    enum Choice: Equatable { case button(Int), row(Int, Int), none }
    var choice = Choice.none
    var fields: [String] = []
}

/// Today's Sheet, with the core's form: the same views (Sheet.swift draws them), the core's
/// default and Cancel keys and enabled states, and the answer by index (never by title: a row's
/// button may share a title with a bottom button).
final class CoreSheet: NSObject {
    let sheet: Sheet
    private(set) var answer = DialogAnswer.Choice.none
    private var indexOf: [ObjectIdentifier: DialogAnswer.Choice] = [:]

    init(_ f: DialogForm) {
        sheet = Sheet(title: f.title, message: f.message, buttons: f.buttons,
                      fields: f.fields, rows: f.rows.map { ($0.label, $0.buttons) }, inline: f.inline)
        super.init()
        for (i, b) in sheet.buttonViews.enumerated() {
            indexOf[ObjectIdentifier(b)] = .button(i)
            b.target = self
            b.action = #selector(pressed(_:))
            // the core's keys: the same as Sheet's own rule for every 1.3.0 dialog (flows.c dlg_button)
            b.keyEquivalent = Int32(i) == f.defaultButton ? "\r" : Int32(i) == f.cancelButton ? "\u{1b}" : ""
            if i < f.enabled.count { b.isEnabled = f.enabled[i] }
        }
        var k = 0
        for (r, row) in f.rows.enumerated() {
            for j in row.buttons.indices where k < sheet.rowButtons.count {
                let b = sheet.rowButtons[k]
                indexOf[ObjectIdentifier(b)] = .row(r, j)
                b.target = self
                b.action = #selector(pressed(_:))
                b.isEnabled = row.enabled[j]
                k += 1
            }
        }
    }

    @objc private func pressed(_ b: NSButton) {
        answer = indexOf[ObjectIdentifier(b)] ?? .none
        sheet.choice = b.title
        NSApp.stopModal()
    }

    /// Main: as a sheet on `window` (Sheet.run(on:)), or alone when there's no window yet.
    func run(on window: NSWindow?) -> DialogAnswer {
        if let w = window { _ = sheet.run(on: w) } else { _ = sheet.runAlone() }
        return DialogAnswer(choice: answer, fields: sheet.inputs.map { $0.stringValue })
    }
}

/// A text from the core's table, with no slots (the shell's own dialogs: the password sheet).
func coreText(_ id: core_text_id) -> String {
    var buf = [CChar](repeating: 0, count: 1024)
    var n = 0
    guard core_text(CORE_OS_MAC, id, nil, 0, &buf, buf.count, &n) == Int32(CORE_OK) else { return "" }
    return String(decoding: buf.prefix(n).map { UInt8(bitPattern: $0) }, as: UTF8.self)
}

enum ShellDialogs {
    /// host->dialog: flow thread. Shows the core's dialog and fills its reply. 0: answered (an
    /// unanswered dialog leaves the reply unset: Cancel); nonzero: not shown (shutting down).
    /// `present` is the presentation (the window's sheet in the app; a test's stand-in).
    static func dialog(_ d: OpaquePointer, _ r: OpaquePointer, present: (DialogForm) -> DialogAnswer?) -> Int32 {
        let form = DialogForm(d)
        guard let a = present(form) else { return 1 }
        for (i, v) in a.fields.enumerated() { v.withCString { core_reply_field(r, Int32(i), $0, strlen($0)) } }
        switch a.choice {
        case .button(let i): core_reply_button(r, Int32(i))
        case .row(let i, let j): core_reply_row_button(r, Int32(i), Int32(j))
        case .none: break
        }
        return 0
    }

    /// The app's presentation of a core dialog: the presenter, today's Sheet on the window.
    static func presentSheet(_ f: DialogForm, window: @escaping () -> NSWindow?) -> DialogAnswer? {
        DialogPresenter.shared.present { CoreSheet(f).run(on: window()) }
    }

    /// host->pick_folder: flow thread. Today's NSOpenPanel (LocationActions.swift changeLocation),
    /// its texts from the core, shown through the presenter. 0 chosen, 1 cancelled, -1 not shown.
    static func pickFolder(start: String, _ r: OpaquePointer) -> Int32 {
        let message = coreText(T_PICK_FOLDER_MESSAGE), prompt = coreText(T_PICK_FOLDER_CHOOSE)
        let picked: String?? = DialogPresenter.shared.present {
            let panel = NSOpenPanel()
            panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
            panel.allowsMultipleSelection = false
            panel.prompt = prompt
            panel.message = message
            panel.directoryURL = URL(fileURLWithPath: start.isEmpty ? "/Volumes" : start)
            guard panel.runModal() == .OK, let url = panel.url else { return nil }
            return url.path
        }
        guard let shown = picked else { return -1 }
        guard let path = shown else { return 1 }
        path.withCString { core_reply_field(r, 0, $0, strlen($0)) }
        return 0
    }

    /// The password sheet (Admin.swift askPassword), its texts from the core, shown through the
    /// presenter instead of onMainSync { Sheet.run }: only the asking thread blocks. nil: cancelled
    /// (or not shown).
    static func askPassword(_ again: Bool, window: @escaping () -> NSWindow?) -> String? {
        let title = coreText(again ? T_PW_WRONG_TITLE : T_PW_TITLE), message = coreText(again ? T_PW_WRONG_MSG : T_PW_MSG)
        let buttons = [coreText(T_CONTINUE), coreText(T_CANCEL)], placeholder = coreText(T_PW_FIELD)
        let r: (choice: String, text: String)?? = DialogPresenter.shared.present {
            guard let w = window() else { return nil }
            return Sheet(title: title, message: message, buttons: buttons, password: placeholder).run(on: w)
        }
        guard let shown = r, let answer = shown, answer.choice == buttons[0] else { return nil }
        return answer.text
    }
}
