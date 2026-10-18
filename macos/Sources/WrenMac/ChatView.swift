import AppKit
import WrenCore
import SwiftUI
import UniformTypeIdentifiers

/// Search bar, message list and composer of the selected conversation.
struct ChatPane: View {
    @Environment(AppModel.self) private var model
    let conversation: Conversation
    @State private var dropTargeted = false

    var body: some View {
        VStack(spacing: 0) {
            if model.searchVisible { SearchBar() }
            MessageList(conversation: conversation)
                .overlay {
                    if dropTargeted {
                        ZStack {
                            RoundedRectangle(cornerRadius: 16).fill(Theme.signalBlue.opacity(0.08))
                            RoundedRectangle(cornerRadius: 16)
                                .strokeBorder(Theme.signalBlue, style: StrokeStyle(lineWidth: 2, dash: [8, 6]))
                            Label("Drop to attach", systemImage: "paperclip")
                                .font(.title3.weight(.medium))
                                .foregroundStyle(Theme.signalBlue)
                        }
                        .padding(12)
                    }
                }
            ComposerArea()
        }
        .onDrop(of: [.fileURL, .image], isTargeted: $dropTargeted) { providers in
            handleDrop(providers)
        }
    }

    private func handleDrop(_ providers: [NSItemProvider]) -> Bool {
        var handled = false
        for p in providers {
            if p.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
                handled = true
                _ = p.loadObject(ofClass: URL.self) { url, _ in
                    guard let url else { return }
                    Task { @MainActor in model.addPendingFiles([url.path]) }
                }
            } else if p.canLoadObject(ofClass: NSImage.self) {
                handled = true
                _ = p.loadObject(ofClass: NSImage.self) { obj, _ in
                    guard let image = obj as? NSImage, let png = Media.pngData(image) else { return }
                    Task { @MainActor in
                        if let url = try? Media.writeTemp(png, name: "dropped.png") { model.addPendingFiles([url.path]) }
                    }
                }
            }
        }
        return handled
    }
}

private struct SearchBar: View {
    @Environment(AppModel.self) private var model
    @FocusState private var focused: Bool

    var body: some View {
        @Bindable var model = model
        HStack(spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                TextField("Search this conversation", text: $model.searchText)
                    .textFieldStyle(.plain)
                    .focused($focused)
                    .onSubmit { model.navigateSearch(backwards: false) }
                    .onKeyPress(.escape) {
                        model.closeSearch()
                        return .handled
                    }
                if !model.searchText.isEmpty {
                    Text(model.searchResults.isEmpty ? "No results" : "\(model.searchPosition + 1) of \(model.searchResults.count)")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(Capsule().fill(Theme.field))
            Button { model.navigateSearch(backwards: true) } label: { Image(systemName: "chevron.up") }
                .help("Previous match")
                .disabled(model.searchResults.isEmpty)
            Button { model.navigateSearch(backwards: false) } label: { Image(systemName: "chevron.down") }
                .help("Next match")
                .disabled(model.searchResults.isEmpty)
            Button("Done") { model.closeSearch() }
        }
        .buttonStyle(.borderless)
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(Theme.chrome)
        .overlay(alignment: .bottom) { Divider() }
        .onAppear { focused = true }
    }
}

// MARK: - Messages

/// Position of a message inside a run of messages from the same author.
struct MessageGrouping {
    var joinedAbove = false
    var joinedBelow = false
}

private struct MessageList: View {
    @Environment(AppModel.self) private var model
    let conversation: Conversation

    private func sameRun(_ a: Message, _ b: Message) -> Bool {
        a.outgoing == b.outgoing && a.authorKey == b.authorKey && abs(a.ts - b.ts) <= Theme.groupGapMs
            && Calendar.current.isDate(a.time, inSameDayAs: b.time)
    }

    var body: some View {
        let msgs = conversation.msgs
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(spacing: 0) {
                    if msgs.isEmpty {
                        EmptyChat(conversation: conversation).padding(.top, 60)
                    }
                    ForEach(Array(msgs.enumerated()), id: \.element.id) { i, m in
                        let newDay = i == 0 || !Calendar.current.isDate(msgs[i - 1].time, inSameDayAs: m.time)
                        let g = MessageGrouping(joinedAbove: i > 0 && sameRun(msgs[i - 1], m),
                                                joinedBelow: i + 1 < msgs.count && sameRun(m, msgs[i + 1]))
                        if newDay { DateSeparator(date: m.time) }
                        MessageRow(message: m, isGroup: conversation.isGroup, grouping: g)
                            .padding(.top, g.joinedAbove ? 2 : (newDay ? 2 : 10))
                            .id(m.id)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
            }
            .defaultScrollAnchor(.bottom)
            .background(model.appearance.backgroundColor)
            .onChange(of: msgs.last?.id) { _, id in
                guard let id else { return }
                withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo(id, anchor: .bottom) }
            }
            .onChange(of: model.scrollRequest?.serial) { _, _ in
                guard let id = model.scrollRequest?.id else { return }
                withAnimation(.easeInOut(duration: 0.25)) { proxy.scrollTo(id, anchor: .center) }
            }
            .onAppear {
                if let id = msgs.last?.id { proxy.scrollTo(id, anchor: .bottom) }
            }
        }
    }
}

private struct EmptyChat: View {
    @Environment(AppModel.self) private var model
    let conversation: Conversation

    var body: some View {
        VStack(spacing: 10) {
            AvatarView(image: model.avatarImage(conversation), size: 72)
            Text(conversation.title).font(.title2.weight(.semibold))
            Text(conversation.isGroup ? "Group" : conversation.key)
                .foregroundStyle(.secondary)
            Text("Messages are end-to-end encrypted by Signal.\nThis Mac keeps its own encrypted copy of the history.")
                .font(.caption)
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .padding(.top, 6)
        }
        .frame(maxWidth: .infinity)
    }
}

private struct DateSeparator: View {
    let date: Date

    var body: some View {
        Text(label)
            .font(.caption.weight(.semibold))
            .foregroundStyle(.secondary)
            .padding(.horizontal, 12)
            .padding(.vertical, 4)
            .background(Capsule().fill(.ultraThinMaterial))
            .padding(.top, 14)
            .padding(.bottom, 8)
            .frame(maxWidth: .infinity)
    }

    private var label: String {
        if Calendar.current.isDateInToday(date) { return "Today" }
        if Calendar.current.isDateInYesterday(date) { return "Yesterday" }
        if let days = Calendar.current.dateComponents([.day], from: date, to: Date()).day, days < 7 {
            return date.formatted(.dateTime.weekday(.wide))
        }
        return date.formatted(date: .abbreviated, time: .omitted)
    }
}

private struct MessageRow: View {
    @Environment(AppModel.self) private var model
    let message: Message
    let isGroup: Bool
    let grouping: MessageGrouping
    @State private var hovering = false

    private var showAvatarColumn: Bool { isGroup && !message.outgoing }

    var body: some View {
        let m = message
        HStack(alignment: .bottom, spacing: 8) {
            if m.outgoing { Spacer(minLength: 80) }
            if showAvatarColumn {
                // One avatar per run of messages, next to its last bubble.
                Group {
                    if !grouping.joinedBelow, let c = model.chats.convs[m.authorKey] {
                        AvatarView(image: model.avatarImage(c), size: 28)
                    } else if !grouping.joinedBelow {
                        AvatarView(image: Media.placeholderAvatar(key: m.authorKey, title: m.sender), size: 28)
                    } else {
                        Color.clear
                    }
                }
                .frame(width: 28, height: 28)
                .padding(.bottom, m.reactions.isEmpty ? 0 : 14)
            }
            if m.outgoing { hoverActions }
            Bubble(message: m, isGroup: isGroup, grouping: grouping)
                .padding(.bottom, m.reactions.isEmpty ? 0 : 14)
                .overlay(alignment: m.outgoing ? .bottomTrailing : .bottomLeading) {
                    if !m.reactions.isEmpty {
                        ReactionsView(message: m).padding(.horizontal, 10)
                    }
                }
            if !m.outgoing { hoverActions }
            if !m.outgoing { Spacer(minLength: 80) }
        }
        .onHover { hovering = $0 }
    }

    /// Quick actions that appear next to the bubble under the pointer (like the Signal desktop client).
    private var hoverActions: some View {
        HStack(spacing: 2) {
            Menu {
                ForEach(EmojiCatalog.quickReactions, id: \.self) { e in
                    Button(e) { model.sendReaction(message.id, e) }
                }
            } label: {
                Image(systemName: "face.smiling").foregroundStyle(.secondary)
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .tint(.secondary)
            .fixedSize()
            .help("React")
            Button { model.startReply(message.id) } label: { Image(systemName: "arrowshape.turn.up.left") }
                .buttonStyle(.borderless)
                .help("Reply")
        }
        .foregroundStyle(.secondary)
        .padding(.bottom, 6)
        .opacity(hovering && model.connected ? 1 : 0)
        .animation(.easeOut(duration: 0.12), value: hovering)
    }
}

/// The bubble itself: sender, quote, attachments, text and the time row.
private struct Bubble: View {
    @Environment(AppModel.self) private var model
    let message: Message
    let isGroup: Bool
    let grouping: MessageGrouping

    var body: some View {
        let m = message
        let a = model.appearance
        let fg = m.outgoing ? a.outTextColor : a.inTextColor
        let bg = m.outgoing ? a.outBgColor : a.inBgColor
        let shape = bubbleShape(radius: CGFloat(a.radius))
        let pending = model.chats.isPendingPreview(model.current ?? "", ts: m.ts)
        let mediaOnly = m.text.isEmpty && !m.quote.valid && !m.attachments.isEmpty
            && m.attachments.allSatisfy { $0.isImage && !$0.thumb.isEmpty }
        let showSender = isGroup && !m.outgoing && !grouping.joinedAbove

        Group {
            if mediaOnly {
                VStack(spacing: 2) {
                    ForEach(Array(m.attachments.enumerated()), id: \.offset) { i, att in
                        AttachmentView(message: m, index: i, attachment: att, foreground: fg, edgeToEdge: true)
                    }
                }
                .overlay(alignment: .bottomTrailing) {
                    MetaRow(message: m, pending: pending, color: .white)
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(Capsule().fill(.black.opacity(0.45)))
                        .padding(6)
                }
                .overlay(alignment: .topLeading) {
                    if showSender { senderLabel.padding(.horizontal, 8).padding(.vertical, 3)
                        .background(Capsule().fill(.black.opacity(0.45))).padding(6) }
                }
                .clipShape(shape)
            } else if Bubble.isEmojiOnly(m.text) && !m.quote.valid && m.attachments.isEmpty {
                VStack(alignment: m.outgoing ? .trailing : .leading, spacing: 2) {
                    if showSender { senderLabel }
                    Text(m.text).font(.system(size: 44))
                    MetaRow(message: m, pending: pending, color: .secondary)
                }
            } else {
                BubbleContentLayout(spacing: 3) {
                    VStack(alignment: .leading, spacing: 6) {
                        if showSender { senderLabel }
                        if m.quote.valid { QuoteView(quote: m.quote, foreground: fg, outgoing: m.outgoing) }
                        ForEach(Array(m.attachments.enumerated()), id: \.offset) { i, att in
                            AttachmentView(message: m, index: i, attachment: att, foreground: fg, edgeToEdge: false)
                        }
                        if !m.text.isEmpty {
                            Text(Bubble.linkified(m.text, emojiOnly: Bubble.isEmojiOnly(m.text)))
                                .textSelection(.enabled)
                                .foregroundStyle(fg)
                                .tint(m.outgoing ? fg : Theme.signalBlue)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    MetaRow(message: m, pending: pending, color: fg.opacity(0.7))
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(shape.fill(bg))
            }
        }
        .overlay {
            if model.searchMatchId == m.id {
                shape.strokeBorder(Color.yellow, lineWidth: 2.5)
            }
        }
        .modifier(MaxWidth(480))
        .contentShape(shape)
        .contextMenu { MessageMenu(message: m) }
        .opacity(pending ? 0.7 : 1)
    }

    private var senderLabel: some View {
        Text(message.sender.isEmpty ? model.displayName(message.authorKey) : message.sender)
            .font(.caption.weight(.semibold))
            .foregroundStyle(Theme.senderColor(message.authorKey))
    }

    /// Joined sides of a run get small corners, like the official clients.
    private func bubbleShape(radius r: CGFloat) -> UnevenRoundedRectangle {
        let small = min(r, 4)
        let top = grouping.joinedAbove ? small : r
        let bottom = grouping.joinedBelow ? small : r
        if message.outgoing {
            return UnevenRoundedRectangle(topLeadingRadius: r, bottomLeadingRadius: r, bottomTrailingRadius: bottom,
                                          topTrailingRadius: top, style: .continuous)
        }
        return UnevenRoundedRectangle(topLeadingRadius: top, bottomLeadingRadius: bottom, bottomTrailingRadius: r,
                                      topTrailingRadius: r, style: .continuous)
    }

    /// Up to three emoji and nothing else are shown large.
    static func isEmojiOnly(_ text: String) -> Bool {
        let t = text.trimmingCharacters(in: .whitespaces)
        guard !t.isEmpty, t.count <= 3 else { return false }
        return t.allSatisfy { $0.unicodeScalars.first?.properties.isEmojiPresentation == true || ($0.unicodeScalars.count > 1 && $0.unicodeScalars.first?.properties.isEmoji == true) }
    }

    static func linkified(_ text: String, emojiOnly: Bool = false) -> AttributedString {
        var s = AttributedString(text)
        s.font = emojiOnly ? .system(size: 40) : .system(size: NSFont.systemFontSize + 1)
        guard !emojiOnly, let detector = try? NSDataDetector(types: NSTextCheckingResult.CheckingType.link.rawValue) else { return s }
        let ns = text as NSString
        for match in detector.matches(in: text, range: NSRange(location: 0, length: ns.length)) {
            guard let url = match.url, ["http", "https", "mailto"].contains(url.scheme?.lowercased() ?? ""),
                  let r = Range(match.range, in: text), let ar = Range(r, in: s) else { continue }
            s[ar].link = url
            s[ar].underlineStyle = .single
        }
        return s
    }
}

/// Timer, "edited", time and delivery state.
private struct MetaRow: View {
    let message: Message
    let pending: Bool
    let color: Color

    var body: some View {
        HStack(spacing: 3) {
            if message.expiresInSeconds > 0 { Image(systemName: "timer") }
            if message.isEdited { Text("Edited") }
            Text(message.time.formatted(date: .omitted, time: .shortened))
            if message.outgoing {
                Image(systemName: pending ? "circle.dotted" : "checkmark.circle")
            }
        }
        .font(.system(size: 10, weight: .medium))
        .foregroundStyle(color)
    }
}

private struct QuoteView: View {
    @Environment(AppModel.self) private var model
    let quote: Quote
    let foreground: Color
    let outgoing: Bool

    var body: some View {
        let author = model.displayName(quote.authorKey)
        let accent = outgoing ? foreground : Theme.senderColor(quote.authorKey)
        Button { model.jumpToQuote(quote) } label: {
            HStack(spacing: 8) {
                RoundedRectangle(cornerRadius: 2).fill(accent).frame(width: 3)
                VStack(alignment: .leading, spacing: 2) {
                    Text(author).font(.caption.weight(.semibold)).foregroundStyle(accent)
                    Text(quote.text.isEmpty ? (quote.attachmentNote.hasPrefix("image/") ? "📷 Photo" : "📎 Attachment") : quote.text)
                        .font(.callout)
                        .lineLimit(3)
                        .foregroundStyle(foreground.opacity(0.9))
                }
            }
            .padding(.vertical, 6)
            .padding(.leading, 6)
            .padding(.trailing, 10)
            .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(foreground.opacity(0.12)))
        }
        .buttonStyle(.plain)
        .help("Jump to the quoted message")
    }
}

private struct ReactionsView: View {
    @Environment(AppModel.self) private var model
    let message: Message

    var body: some View {
        let counts = Dictionary(grouping: message.reactions, by: \.value).map { ($0.key, $0.value.map(\.key)) }
            .sorted { $0.1.count != $1.1.count ? $0.1.count > $1.1.count : $0.0 < $1.0 }
        HStack(spacing: 4) {
            ForEach(counts, id: \.0) { emoji, who in
                let mine = who.contains(model.account)
                Button { model.sendReaction(message.id, emoji) } label: {
                    HStack(spacing: 3) {
                        Text(emoji).font(.system(size: 14))
                        if who.count > 1 { Text("\(who.count)").font(.caption.weight(.semibold)).foregroundStyle(.secondary) }
                    }
                    .padding(.horizontal, 7)
                    .padding(.vertical, 2)
                    .background(Capsule().fill(mine ? Color.secondary.opacity(0.35) : Theme.field))
                    .overlay(Capsule().strokeBorder(model.appearance.backgroundColor, lineWidth: 2))
                }
                .buttonStyle(.plain)
                .help(who.map(model.displayName).joined(separator: ", "))
            }
        }
    }
}

private struct MessageMenu: View {
    @Environment(AppModel.self) private var model
    let message: Message

    var body: some View {
        let m = message
        Button { model.startReply(m.id) } label: { Label("Reply", systemImage: "arrowshape.turn.up.left") }
        if m.outgoing && m.authorKey == model.account && m.canEdit(nowMs: nowMs()) {
            Button { model.startEdit(m.id) } label: { Label("Edit", systemImage: "pencil") }
        }
        Menu {
            ForEach(EmojiCatalog.quickReactions, id: \.self) { e in
                Button(m.reactions[model.account] == e ? "\(e)  (remove)" : e) { model.sendReaction(m.id, e) }
            }
        } label: { Label("React", systemImage: "face.smiling") }
        Divider()
        if !m.text.isEmpty {
            Button {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(m.text, forType: .string)
            } label: { Label("Copy text", systemImage: "doc.on.doc") }
        }
        ForEach(Array(m.attachments.enumerated()), id: \.offset) { i, a in
            Button { model.saveAttachment(m, i) } label: {
                Label("Save “\(a.filename.isEmpty ? "attachment" : a.filename)” …", systemImage: "square.and.arrow.down")
            }
        }
        Divider()
        Button(role: .destructive) { Task { await model.deleteMessage(m.id) } } label: { Label("Delete …", systemImage: "trash") }
    }
}

/// Limits the width offered to a view without making it wider than its content
/// (`.frame(maxWidth:)` would stretch wrapping text to the maximum).
private struct MaxWidth: ViewModifier {
    let width: CGFloat
    init(_ width: CGFloat) { self.width = width }
    func body(content: Content) -> some View { MaxWidthLayout(maxWidth: width) { content } }
}

/// Stacks the bubble content vertically: everything leading, only the last view (the time row) trailing.
/// A VStack can only align all children the same way, which pushed short texts to the right.
private struct BubbleContentLayout: Layout {
    let spacing: CGFloat

    // Children are always measured by width only: a proposed height would stretch flexible parts
    // (e.g. the quote's accent bar) when sizing, but not when placing, leaving the bubble too tall.
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let sizes = subviews.map { $0.sizeThatFits(ProposedViewSize(width: proposal.width, height: nil)) }
        let height = sizes.map(\.height).reduce(0, +) + spacing * CGFloat(max(sizes.count - 1, 0))
        return CGSize(width: sizes.map(\.width).max() ?? 0, height: height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var y = bounds.minY
        for (i, v) in subviews.enumerated() {
            let size = v.sizeThatFits(ProposedViewSize(width: bounds.width, height: nil))
            let x = i == subviews.count - 1 ? bounds.maxX - size.width : bounds.minX
            v.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(width: size.width, height: size.height))
            y += size.height + spacing
        }
    }
}

private struct MaxWidthLayout: Layout {
    let maxWidth: CGFloat

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        guard let v = subviews.first else { return .zero }
        let w = min(proposal.width ?? maxWidth, maxWidth)
        return v.sizeThatFits(ProposedViewSize(width: w, height: proposal.height))
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        subviews.first?.place(at: bounds.origin, proposal: ProposedViewSize(width: bounds.width, height: bounds.height))
    }
}
