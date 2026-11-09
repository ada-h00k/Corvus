import AppKit
import CorvusCore
import SwiftUI

/// Edit / reply / attachment previews and the message input of the current conversation.
struct ComposerArea: View {
    @Environment(AppModel.self) private var model
    @State private var emojiShown = false
    @State private var stickersShown = false

    var body: some View {
        @Bindable var model = model
        let canSend = !(model.composerText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && model.pendingFiles.isEmpty)
        VStack(spacing: 8) {
            if !model.editKey.isEmpty {
                ComposerCard(icon: "pencil", title: "Edit message", text: "Return saves · Esc cancels") { model.cancelEdit() }
            }
            if model.replyQuote.valid {
                ComposerCard(icon: "arrowshape.turn.up.left",
                             title: "Replying to \(model.displayName(model.replyQuote.authorKey))",
                             text: replyText, accent: Theme.senderColor(model.replyQuote.authorKey)) { model.replyQuote = Quote() }
            }
            if !model.mentionSuggestions.isEmpty {
                MentionSuggestions()
            }
            if model.draftLinkPreview.isValid && model.editKey.isEmpty {
                LinkPreviewDraftCard(preview: model.draftLinkPreview) { model.dismissLinkPreview() }
            }
            if !model.pendingFiles.isEmpty {
                AttachmentTray(files: model.pendingFiles, voiceFiles: model.voiceFiles) { f in
                    model.removePendingFile(f)
                }
            }
            if model.recording {
                RecordBar()
            } else {
            HStack(alignment: .bottom, spacing: 8) {
                // The field with the emoji button inside, like the Signal clients.
                HStack(alignment: .bottom, spacing: 2) {
                    Button { emojiShown.toggle() } label: {
                        Image(systemName: "face.smiling").font(.system(size: 17)).frame(width: 28, height: 30)
                    }
                    .help("Emoji")
                    .popover(isPresented: $emojiShown, arrowEdge: .top) {
                        EmojiPicker { e in
                            NotificationCenter.default.post(name: MessageInput.insertText, object: e)
                        }
                    }
                    MessageInput(text: $model.composerText, onUserEdit: { model.composerEdited() })
                        .frame(minHeight: 30)
                }
                .padding(.leading, 6)
                .padding(.trailing, 4)
                .background(RoundedRectangle(cornerRadius: 17, style: .continuous).fill(Theme.field))

                Button { Task { await model.searchGifs() } } label: {
                    Text("GIF")
                        .font(.system(size: 10, weight: .heavy))
                        .padding(.horizontal, 4)
                        .padding(.vertical, 2)
                        .overlay(RoundedRectangle(cornerRadius: 4).strokeBorder(lineWidth: 1.5))
                        .frame(width: 30, height: 34)
                }
                .help("Search GIPHY GIFs")
                Button { stickersShown.toggle() } label: {
                    Image(systemName: "seal").font(.system(size: 17)).frame(width: 30, height: 34)
                }
                .help("Stickers")
                .disabled(!model.editKey.isEmpty)
                .popover(isPresented: $stickersShown, arrowEdge: .top) {
                    StickerPicker { stickersShown = false }
                }
                // The voice recording button sits to the left of the attach-files button (as in the Qt app).
                Button { model.startVoiceRecording() } label: {
                    Image(systemName: "mic").font(.system(size: 17)).frame(width: 30, height: 34)
                }
                .help("Record a voice message")
                .disabled(!model.editKey.isEmpty)
                Button { model.chooseAttachments() } label: {
                    Image(systemName: "paperclip").font(.system(size: 17)).frame(width: 30, height: 34)
                }
                .help("Attach files")
                Button { model.sendMessage() } label: {
                    Image(systemName: model.editKey.isEmpty ? "arrow.up" : "checkmark")
                        .font(.system(size: 15, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 32, height: 32)
                        .background(Circle().fill(canSend ? Theme.signalBlue : Color.secondary.opacity(0.35)))
                }
                .help(model.editKey.isEmpty ? "Send (Return)" : "Save edit (Return)")
                .disabled(!canSend)
                .animation(.easeOut(duration: 0.15), value: canSend)
            }
            .buttonStyle(.borderless)
            .foregroundStyle(.secondary)
            .disabled(!model.connected)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Theme.chrome)
        .overlay(alignment: .top) { Divider() }
    }

    private var replyText: String {
        var t = model.replyQuote.text.isEmpty ? "📎 Attachment" : model.replyQuote.text
        t = t.replacingOccurrences(of: "\n", with: " ")
        if t.count > 120 { t = String(t.prefix(120)) + "…" }
        return t
    }
}

/// Reply / edit preview above the field.
private struct ComposerCard: View {
    let icon: String
    let title: String
    let text: String
    var accent: Color = Theme.signalBlue
    let close: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            RoundedRectangle(cornerRadius: 2).fill(accent).frame(width: 3)
            Image(systemName: icon).foregroundStyle(accent)
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.caption.weight(.semibold)).foregroundStyle(accent)
                Text(text).font(.callout).lineLimit(1).truncationMode(.tail).foregroundStyle(.secondary)
            }
            Spacer()
            Button(action: close) { Image(systemName: "xmark.circle.fill").font(.system(size: 15)) }
                .buttonStyle(.borderless)
                .foregroundStyle(.secondary)
        }
        .padding(8)
        .frame(height: 48)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Theme.field))
    }
}

/// Group members matching the "@query" being typed; ↑ ↓ Return / Tab, or a click, take one.
private struct MentionSuggestions: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let list = model.mentionSuggestions
        ScrollViewReader { proxy in
            ScrollView {
                VStack(spacing: 0) {
                    ForEach(Array(list.enumerated()), id: \.element.id) { i, who in
                        HStack(spacing: 10) {
                            if let c = model.chats.convs[who.id] {
                                AvatarView(image: model.avatarImage(c), size: 26)
                            } else {
                                AvatarView(image: Media.placeholderAvatar(key: who.id, title: who.name), size: 26)
                            }
                            Text(who.name).lineLimit(1)
                            if who.name != who.id {
                                Text(who.id).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            }
                            Spacer()
                        }
                        .padding(.horizontal, 10)
                        .frame(height: 36)
                        .background(RoundedRectangle(cornerRadius: 6).fill(i == model.mentionSelection ? Theme.signalBlue.opacity(0.18) : .clear))
                        .contentShape(Rectangle())
                        .onTapGesture { NotificationCenter.default.post(name: MessageInput.acceptMention, object: i) }
                        .id(who.id)
                        .accessibilityElement(children: .combine)
                        .accessibilityAddTraits(i == model.mentionSelection ? [.isButton, .isSelected] : .isButton)
                    }
                }
                .padding(4)
            }
            .frame(height: CGFloat(min(list.count, 5)) * 36 + 8)
            .onChange(of: model.mentionSelection) { _, i in
                if i < list.count { proxy.scrollTo(list[i].id) }
            }
        }
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Theme.field))
        .accessibilityLabel("Mention suggestions")
    }
}

/// The preview that will be sent with the message's link; ✕ sends the link without it.
private struct LinkPreviewDraftCard: View {
    let preview: LinkPreview
    let close: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            Group {
                if !preview.imageThumb.isEmpty, let img = NSImage(data: preview.imageThumb) {
                    Image(nsImage: img).resizable().aspectRatio(contentMode: .fill)
                } else {
                    Image(systemName: "link").font(.title3).foregroundStyle(.secondary)
                }
            }
            .frame(width: 48, height: 48)
            .background(Theme.field.opacity(0.6))
            .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
            VStack(alignment: .leading, spacing: 1) {
                Text(preview.title).font(.callout.weight(.semibold)).lineLimit(1)
                if !preview.description.isEmpty {
                    Text(preview.description).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
                Text(preview.host).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer()
            Button(action: close) { Image(systemName: "xmark.circle.fill").font(.system(size: 15)) }
                .buttonStyle(.borderless)
                .foregroundStyle(.secondary)
                .help("Remove link preview")
                .accessibilityLabel("Remove link preview")
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Theme.field))
    }
}

/// Shown instead of the message field while a voice message is being recorded.
private struct RecordBar: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        HStack(spacing: 10) {
            TimelineView(.periodic(from: .now, by: 0.6)) { ctx in
                let on = Int(ctx.date.timeIntervalSinceReferenceDate / 0.6) % 2 == 0
                Circle().fill(Color.red.opacity(on ? 0.9 : 0.15)).frame(width: 10, height: 10)
            }
            Text(VoiceNote.durationText(model.recordingMs))
                .font(.system(size: 13, weight: .semibold).monospacedDigit())
            Text("Recording a voice message …")
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .truncationMode(.tail)
            Spacer(minLength: 8)
            Button("Discard", role: .destructive) { model.cancelVoiceRecording() }
            Button("Stop") { model.stopVoiceRecording(send: false) }
                .help("Stop and attach the recording to the message. You can add text before sending.")
            Button("Send") { model.stopVoiceRecording(send: true) }
                .keyboardShortcut(.defaultAction)
                .disabled(!model.connected)
        }
        .padding(.horizontal, 12)
        .frame(minHeight: 34)
        .background(RoundedRectangle(cornerRadius: 17, style: .continuous).fill(Theme.field))
    }
}

/// Files queued for the next message: image thumbnails or file chips, each removable.
private struct AttachmentTray: View {
    let files: [String]
    var voiceFiles: [String: Int64] = [:]
    let remove: (String) -> Void

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(files, id: \.self) { f in
                    ZStack(alignment: .topTrailing) {
                        Group {
                            if let ms = voiceFiles[f] {
                                VStack(spacing: 4) {
                                    Image(systemName: "waveform").font(.title3)
                                    Text("Voice message (\(VoiceNote.durationText(ms)))").font(.caption2).lineLimit(2)
                                        .multilineTextAlignment(.center)
                                }
                                .padding(6)
                                .frame(width: 110, height: 64)
                                .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Theme.field))
                            } else if Media.mimeType(forFile: f).hasPrefix("image/"), let img = NSImage(contentsOfFile: f) {
                                Image(nsImage: img).resizable().aspectRatio(contentMode: .fill)
                                    .frame(width: 64, height: 64)
                                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                            } else {
                                VStack(spacing: 4) {
                                    Image(systemName: "doc.fill").font(.title3)
                                    Text((f as NSString).lastPathComponent).font(.caption2).lineLimit(2)
                                        .multilineTextAlignment(.center)
                                }
                                .padding(6)
                                .frame(width: 96, height: 64)
                                .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(Theme.field))
                            }
                        }
                        .help((f as NSString).lastPathComponent)
                        Button { remove(f) } label: {
                            Image(systemName: "xmark.circle.fill")
                                .symbolRenderingMode(.palette)
                                .foregroundStyle(.white, .black.opacity(0.6))
                                .font(.system(size: 16))
                        }
                        .buttonStyle(.plain)
                        .offset(x: 5, y: -5)
                    }
                }
            }
            .padding(.top, 6)
            .padding(.horizontal, 2)
        }
    }
}

/// Multi-line message field: Return sends, Shift/Option+Return inserts a line break, ↑ in an empty field
/// edits the last message, Esc cancels an edit, and pasting a screenshot or files attaches them.
struct MessageInput: NSViewRepresentable {
    static let insertText = Notification.Name("MessageInput.insertText")
    static let acceptMention = Notification.Name("MessageInput.acceptMention") // object: index into the suggestions

    static let spellCheckKey = "composer.spellCheck"

    @Environment(AppModel.self) private var model
    @AppStorage(MessageInput.spellCheckKey) private var spellCheck = false
    @Binding var text: String
    var onUserEdit: (() -> Void)? = nil   // the user changed the text (not a programmatic update)

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.drawsBackground = false
        scroll.borderType = .noBorder

        let tv = InputTextView()
        tv.delegate = context.coordinator
        tv.isRichText = false
        tv.importsGraphics = false
        tv.allowsUndo = true
        tv.font = .systemFont(ofSize: NSFont.systemFontSize + 1)
        tv.textContainerInset = NSSize(width: 2, height: 7)
        tv.isAutomaticQuoteSubstitutionEnabled = false
        tv.isAutomaticDashSubstitutionEnabled = false
        tv.drawsBackground = false
        tv.isVerticallyResizable = true
        tv.isHorizontallyResizable = false
        tv.autoresizingMask = [.width]
        tv.textContainer?.widthTracksTextView = true
        tv.placeholder = "Write a message …"
        tv.coordinator = context.coordinator
        scroll.documentView = tv

        context.coordinator.textView = tv
        context.coordinator.scroll = scroll
        context.coordinator.observeInsertions()
        DispatchQueue.main.async { tv.window?.makeFirstResponder(tv) }
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        let c = context.coordinator
        c.parent = self
        guard let tv = c.textView else { return }
        if tv.string != text {
            tv.string = text
            // Replaced from outside (sent, chat switched, draft restored): keep only mentions that still stand.
            model.composerMentions = MentionCompose.valid(model.composerMentions, in: text)
            c.highlightMentions()
            tv.needsDisplay = true
        }
        tv.isEditable = model.connected
        // Settings → General: spell checking, grammar and autocorrect follow one switch.
        if tv.isContinuousSpellCheckingEnabled != spellCheck {
            tv.isContinuousSpellCheckingEnabled = spellCheck
            tv.isGrammarCheckingEnabled = spellCheck
            tv.isAutomaticSpellingCorrectionEnabled = spellCheck
        }
        if c.focusRequest != model.composerFocusRequest {
            c.focusRequest = model.composerFocusRequest
            DispatchQueue.main.async { tv.window?.makeFirstResponder(tv) }
        }
        if c.cursorRequest != model.composerCursorToEnd {
            c.cursorRequest = model.composerCursorToEnd
            tv.setSelectedRange(NSRange(location: (tv.string as NSString).length, length: 0))
        }
        c.updateHeight()
    }

    @MainActor
    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: MessageInput
        weak var textView: InputTextView?
        weak var scroll: NSScrollView?
        var focusRequest = -1
        var cursorRequest = 0
        private var heightConstraint: NSLayoutConstraint?
        private var observer: NSObjectProtocol?
        private var mentionObserver: NSObjectProtocol?

        init(_ parent: MessageInput) {
            self.parent = parent
            self.cursorRequest = 0
        }

        func observeInsertions() {
            observer = NotificationCenter.default.addObserver(forName: MessageInput.insertText, object: nil, queue: .main) { [weak self] n in
                MainActor.assumeIsolated {
                    guard let self, let tv = self.textView, let s = n.object as? String else { return }
                    tv.insertText(s, replacementRange: tv.selectedRange())
                    tv.window?.makeFirstResponder(tv)
                }
            }
            mentionObserver = NotificationCenter.default.addObserver(forName: MessageInput.acceptMention, object: nil, queue: .main) { [weak self] n in
                MainActor.assumeIsolated {
                    guard let self, let i = n.object as? Int else { return }
                    self.acceptMention(i)
                    self.textView?.window?.makeFirstResponder(self.textView)
                }
            }
        }

        // MARK: @mentions

        /// Mentions move with the text in front of them; one that an edit touches becomes plain text.
        func textView(_ textView: NSTextView, shouldChangeTextIn range: NSRange, replacementString: String?) -> Bool {
            let model = parent.model
            if !model.composerMentions.isEmpty {
                model.composerMentions = MentionCompose.adjust(model.composerMentions, editing: range,
                                                               replacementLength: ((replacementString ?? "") as NSString).length)
            }
            return true
        }

        func textViewDidChangeSelection(_ notification: Notification) {
            guard let tv = textView, !tv.hasMarkedText() else { return }
            let sel = tv.selectedRange()
            if sel.length == 0 { parent.model.updateMentionSuggestions(text: tv.string, cursor: sel.location) }
            else { parent.model.hideMentionSuggestions() }
        }

        /// Replaces the "@query" with "@Name " and remembers the mention.
        func acceptMention(_ index: Int) {
            let model = parent.model
            guard let tv = textView, let trig = model.mentionTrigger, index >= 0, index < model.mentionSuggestions.count else {
                model.hideMentionSuggestions()
                return
            }
            let who = model.mentionSuggestions[index]
            let s = tv.string as NSString
            guard trig.start >= 0, trig.end <= s.length, trig.end >= trig.start else { return model.hideMentionSuggestions() }
            let range = NSRange(location: trig.start, length: trig.end - trig.start)
            let next = trig.end < s.length ? s.substring(with: NSRange(location: trig.end, length: 1)) : ""
            let insert = "@" + who.name + (next.isEmpty || next.rangeOfCharacter(from: .whitespacesAndNewlines) == nil ? " " : "")
            model.hideMentionSuggestions()
            guard tv.shouldChangeText(in: range, replacementString: insert) else { return } // also moves later mentions
            model.composerMentions.append(ComposerMention(start: trig.start + 1, length: (who.name as NSString).length,
                                                          id: who.id, name: who.name))
            tv.textStorage?.replaceCharacters(in: range, with: NSAttributedString(string: insert, attributes: tv.typingAttributes))
            tv.setSelectedRange(NSRange(location: trig.start + (insert as NSString).length, length: 0))
            tv.didChangeText()
        }

        /// Mentions in link colour (temporary attributes: the field stays plain text).
        func highlightMentions() {
            guard let tv = textView, let lm = tv.layoutManager else { return }
            let full = NSRange(location: 0, length: (tv.string as NSString).length)
            lm.removeTemporaryAttribute(.foregroundColor, forCharacterRange: full)
            for m in MentionCompose.valid(parent.model.composerMentions, in: tv.string) {
                lm.addTemporaryAttribute(.foregroundColor, value: NSColor.linkColor,
                                         forCharacterRange: NSRange(location: m.start - 1, length: m.length + 1))
            }
        }

        func textDidChange(_ notification: Notification) {
            guard let tv = textView else { return }
            if parent.text != tv.string {
                parent.text = tv.string
                parent.onUserEdit?()
            }
            highlightMentions()
            let sel = tv.selectedRange()
            if sel.length == 0 && !tv.hasMarkedText() { parent.model.updateMentionSuggestions(text: tv.string, cursor: sel.location) }
            updateHeight()
        }

        /// 1 … 6 lines high.
        func updateHeight() {
            guard let tv = textView, let scroll, let lm = tv.layoutManager, let tc = tv.textContainer else { return }
            lm.ensureLayout(for: tc)
            let lineHeight = (tv.font?.boundingRectForFont.height ?? 17)
            let content = lm.usedRect(for: tc).height + tv.textContainerInset.height * 2
            let h = min(max(content, lineHeight + tv.textContainerInset.height * 2), lineHeight * 6 + tv.textContainerInset.height * 2)
            if heightConstraint == nil {
                heightConstraint = scroll.heightAnchor.constraint(equalToConstant: h)
                heightConstraint?.isActive = true
            } else if abs((heightConstraint?.constant ?? 0) - h) > 0.5 {
                heightConstraint?.constant = h
            }
        }

        func textView(_ textView: NSTextView, doCommandBy selector: Selector) -> Bool {
            let model = parent.model
            let flags = NSApp.currentEvent?.modifierFlags ?? []
            // While the @mention suggestions are open: ↑ ↓ choose, Return / Tab take, Esc closes (as in the Qt app).
            if !model.mentionSuggestions.isEmpty {
                switch selector {
                case #selector(NSResponder.moveUp(_:)): model.moveMentionSelection(-1); return true
                case #selector(NSResponder.moveDown(_:)): model.moveMentionSelection(1); return true
                case #selector(NSResponder.insertNewline(_:)), #selector(NSResponder.insertTab(_:)):
                    acceptMention(model.mentionSelection)
                    return true
                case #selector(NSResponder.cancelOperation(_:)): model.hideMentionSuggestions(); return true
                default: break
                }
            }
            switch selector {
            case #selector(NSResponder.insertNewline(_:)):
                if flags.contains(.shift) || flags.contains(.option) {
                    textView.insertNewlineIgnoringFieldEditor(nil)
                } else {
                    model.sendMessage()
                }
                return true
            case #selector(NSResponder.cancelOperation(_:)):
                if !model.editKey.isEmpty {
                    model.cancelEdit()
                    return true
                }
                return false
            case #selector(NSResponder.moveUp(_:)):
                if textView.string.isEmpty { return model.editLastMessage() }
                return false
            // Page Up / Page Down / Home / End scroll the chat while typing (the field itself is at most 6 lines).
            case #selector(NSResponder.scrollPageUp(_:)), #selector(NSResponder.pageUp(_:)):
                ChatScroller.shared.page(-1)
                return true
            case #selector(NSResponder.scrollPageDown(_:)), #selector(NSResponder.pageDown(_:)):
                ChatScroller.shared.page(1)
                return true
            case #selector(NSResponder.scrollToBeginningOfDocument(_:)):
                ChatScroller.shared.toEnd(top: true)
                return true
            case #selector(NSResponder.scrollToEndOfDocument(_:)):
                ChatScroller.shared.toEnd(top: false)
                return true
            case #selector(NSResponder.moveUpAndModifySelection(_:)):
                // Shift+↑ selects the whole current draft.
                if !textView.string.isEmpty {
                    textView.selectAll(nil)
                    return true
                }
                return false
            default:
                return false
            }
        }

        func paste(from pb: NSPasteboard) -> Bool { parent.model.attach(from: pb) }
    }
}

final class InputTextView: NSTextView {
    weak var coordinator: MessageInput.Coordinator?
    var placeholder = ""

    // Screenshots and copied files become attachments; real text is pasted as usual.
    override func paste(_ sender: Any?) {
        if let coordinator, MainActor.assumeIsolated({ coordinator.paste(from: .general) }) { return }
        pasteAsPlainText(sender)
    }

    // A plain-text view only accepts text types, so Paste would be disabled for a screenshot on the clipboard.
    override var readablePasteboardTypes: [NSPasteboard.PasteboardType] {
        super.readablePasteboardTypes + [.png, .tiff, .fileURL]
    }

    override func readSelection(from pboard: NSPasteboard, type: NSPasteboard.PasteboardType) -> Bool {
        if let coordinator, MainActor.assumeIsolated({ coordinator.paste(from: pboard) }) { return true }
        return super.readSelection(from: pboard, type: type)
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        guard string.isEmpty, !placeholder.isEmpty else { return }
        let attrs: [NSAttributedString.Key: Any] = [.foregroundColor: NSColor.placeholderTextColor, .font: font ?? .systemFont(ofSize: 13)]
        let origin = NSPoint(x: textContainerInset.width + (textContainer?.lineFragmentPadding ?? 5), y: textContainerInset.height)
        (placeholder as NSString).draw(at: origin, withAttributes: attrs)
    }

    override func becomeFirstResponder() -> Bool {
        needsDisplay = true
        return super.becomeFirstResponder()
    }

    override func didChangeText() {
        super.didChangeText()
        needsDisplay = true
    }
}
