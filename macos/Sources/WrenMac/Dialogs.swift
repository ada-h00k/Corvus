import AppKit

/// Modal dialogs built on NSAlert. Shown as a sheet on the main window when it is visible,
/// otherwise as an app-modal panel, so sequential flows (unlock, connect, …) read like the Qt code.
@MainActor
enum Dialogs {
    private static func present(_ alert: NSAlert) async -> NSApplication.ModalResponse {
        if let window = MainWindowTracker.shared.window, window.isVisible, window.attachedSheet == nil {
            return await alert.beginSheetModal(for: window)
        }
        NSApp.activate(ignoringOtherApps: true)
        return alert.runModal()
    }

    /// Returns the index of the clicked button, or nil for Cancel / Escape.
    @discardableResult
    static func ask(_ title: String, _ message: String = "", buttons: [String] = ["OK"],
                    style: NSAlert.Style = .informational, cancel: Bool = false,
                    destructive: Set<Int> = []) async -> Int? {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = style
        for (i, b) in buttons.enumerated() {
            let button = alert.addButton(withTitle: b)
            if destructive.contains(i) { button.hasDestructiveAction = true }
        }
        if cancel { alert.addButton(withTitle: "Cancel").keyEquivalent = "\u{1b}" }
        let r = await present(alert).rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
        return r >= 0 && r < buttons.count ? r : nil
    }

    static func info(_ title: String, _ message: String = "") async { await ask(title, message) }
    static func warn(_ title: String, _ message: String = "") async { await ask(title, message, style: .warning) }
    static func error(_ title: String, _ message: String = "") async { await ask(title, message, style: .critical) }

    static func confirm(_ title: String, _ message: String = "", ok: String = "OK", destructive: Bool = false) async -> Bool {
        await ask(title, message, buttons: [ok], cancel: true, destructive: destructive ? [0] : []) == 0
    }

    static func text(_ title: String, _ message: String = "", initial: String = "", placeholder: String = "",
                     secure: Bool = false) async -> String? {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        let field: NSTextField = secure ? NSSecureTextField() : NSTextField()
        field.stringValue = initial
        field.placeholderString = placeholder
        field.frame = NSRect(x: 0, y: 0, width: 320, height: 24)
        alert.accessoryView = field
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Cancel")
        alert.window.initialFirstResponder = field
        let r = await present(alert)
        return r == .alertFirstButtonReturn ? field.stringValue : nil
    }

    static func choose(_ title: String, _ message: String = "", options: [String], selected: Int = 0) async -> Int? {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        let popup = NSPopUpButton(frame: NSRect(x: 0, y: 0, width: 280, height: 26), pullsDown: false)
        popup.addItems(withTitles: options)
        popup.selectItem(at: max(0, min(selected, options.count - 1)))
        alert.accessoryView = popup
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Cancel")
        let r = await present(alert)
        return r == .alertFirstButtonReturn ? popup.indexOfSelectedItem : nil
    }

    enum PassphraseMode { case create, unlock, change }
    static let minPassphraseLength = 8

    /// Returns (old, new); old is only set for .change.
    static func passphrase(_ mode: PassphraseMode, error: String = "") async -> (old: String, new: String)? {
        let alert = NSAlert()
        switch mode {
        case .create:
            alert.messageText = "Protect chat history"
            alert.informativeText = "Your chat history is stored encrypted on this computer.\nChoose a passphrase (at least "
                + "\(minPassphraseLength) characters). It cannot be recovered – if you forget it, the stored history is lost."
        case .unlock:
            alert.messageText = "Unlock chat history"
            alert.informativeText = "Enter your passphrase to unlock the encrypted chat history."
        case .change:
            alert.messageText = "Change passphrase"
            alert.informativeText = "Choose a new passphrase for the chat history (at least \(minPassphraseLength) characters)."
        }
        let ok = alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Cancel")

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        func field(_ label: String) -> NSSecureTextField {
            stack.addArrangedSubview(NSTextField(labelWithString: label))
            let f = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 300, height: 24))
            f.widthAnchor.constraint(equalToConstant: 300).isActive = true
            stack.addArrangedSubview(f)
            return f
        }
        let old = mode == .change ? field("Current passphrase:") : nil
        let pass = field(mode == .change ? "New passphrase:" : "Passphrase:")
        let confirm = mode == .unlock ? nil : field("Repeat passphrase:")
        let errorLabel = NSTextField(wrappingLabelWithString: error)
        errorLabel.textColor = .systemRed
        errorLabel.preferredMaxLayoutWidth = 300
        stack.addArrangedSubview(errorLabel)
        stack.layoutSubtreeIfNeeded()
        stack.frame = NSRect(origin: .zero, size: stack.fittingSize)
        alert.accessoryView = stack
        alert.window.initialFirstResponder = old ?? pass

        let validator = PassphraseValidator(mode: mode, old: old, pass: pass, confirm: confirm, error: errorLabel, ok: ok)
        validator.validate()
        let r = await present(alert)
        withExtendedLifetime(validator) {}
        guard r == .alertFirstButtonReturn else { return nil }
        return (old?.stringValue ?? "", pass.stringValue)
    }
}

/// Enables OK only for a valid input, like PassphraseDialog::validate().
@MainActor
private final class PassphraseValidator: NSObject, NSTextFieldDelegate {
    let mode: Dialogs.PassphraseMode
    let old: NSTextField?, pass: NSTextField, confirm: NSTextField?, error: NSTextField, ok: NSButton
    let mismatch = "The passphrases do not match."

    init(mode: Dialogs.PassphraseMode, old: NSTextField?, pass: NSTextField, confirm: NSTextField?, error: NSTextField, ok: NSButton) {
        self.mode = mode
        self.old = old
        self.pass = pass
        self.confirm = confirm
        self.error = error
        self.ok = ok
        super.init()
        for f in [old, pass, confirm].compactMap({ $0 }) { f.delegate = self }
    }

    func controlTextDidChange(_ obj: Notification) { validate() }

    func validate() {
        var valid = !pass.stringValue.isEmpty
        if let old, old.stringValue.isEmpty { valid = false }
        if mode != .unlock {
            if pass.stringValue.count < Dialogs.minPassphraseLength { valid = false }
            if let confirm, confirm.stringValue != pass.stringValue {
                valid = false
                error.stringValue = confirm.stringValue.isEmpty ? "" : mismatch
            } else if error.stringValue == mismatch {
                error.stringValue = ""
            }
        }
        ok.isEnabled = valid
    }
}

/// Remembers the main chat window (for sheets, "user is looking" and bringing it to the front).
@MainActor
final class MainWindowTracker {
    static let shared = MainWindowTracker()
    weak var window: NSWindow?
}
