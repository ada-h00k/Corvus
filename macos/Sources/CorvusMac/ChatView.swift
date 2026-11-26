import AppKit
import CorvusCore
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
            if let change = model.safetyChanged[conversation.key], !conversation.isGroup {
                SafetyBar(conversation: conversation, change: change)
            }
            if !model.activePins.isEmpty { PinBanner(conversation: conversation, pins: model.activePins) }
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
        .overlay(alignment: .bottom) {
            ZStack {
            if let url = model.savedFile {
                HStack(spacing: 10) {
                    Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                    Text("Saved “\(url.lastPathComponent)”").lineLimit(1)
                    Button("Show in Finder") {
                        NSWorkspace.shared.activateFileViewerSelecting([url])
                        model.savedFile = nil
                    }
                    .buttonStyle(.link)
                    Button { model.savedFile = nil } label: { Image(systemName: "xmark") }
                        .buttonStyle(.borderless)
                        .help("Close")
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
                .background(RoundedRectangle(cornerRadius: 10).fill(.regularMaterial))
                .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(.separator))
                .shadow(color: .black.opacity(0.12), radius: 4, y: 2)
                .padding(.bottom, 80)
                .transition(.opacity.combined(with: .move(edge: .bottom)))
            }
            }
            .animation(.easeOut(duration: 0.2), value: model.savedFile) // the bar only, not the chat
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

/// The pinned messages of the chat (at most three): one at a time, a click shows it and moves to the next,
/// the menu shows or unpins each of them (as in the Qt app's pin banner).
private struct PinBanner: View {
    @Environment(AppModel.self) private var model
    let conversation: Conversation
    let pins: [PinInfo]
    @State private var shown = 0

    private func snippet(_ pin: PinInfo) -> String {
        guard let i = model.chats.pinnedMessage(conversation, pin) else { return "Message not available" }
        let m = conversation.msgs[i]
        var t = model.displayText(m).text.replacingOccurrences(of: "\n", with: " ")
        if t.isEmpty, let a = m.attachments.first {
            t = a.isSticker ? "Sticker" : a.isImage ? "📷 Photo" : a.isVideo ? "🎬 Video" : a.isAudio ? "🎤 Audio" : "📎 " + a.filename
        }
        if m.hasPoll { t = "📊 " + m.poll.question }
        if m.hasContact { t = "👤 " + m.contact.name }
        let who = m.outgoing ? "You" : model.displayName(m.authorKey)
        return who.isEmpty ? t : who + ": " + t
    }

    var body: some View {
        let i = min(shown, pins.count - 1)
        let pin = pins[i]
        HStack(spacing: 10) {
            Image(systemName: "pin.fill").foregroundStyle(Theme.signalBlue)
            Button {
                if let mi = model.chats.pinnedMessage(conversation, pin) { model.scrollToMessage(index: mi) }
                shown = (i + 1) % pins.count
            } label: {
                VStack(alignment: .leading, spacing: 1) {
                    Text(pins.count > 1 ? "Pinned message \(i + 1)/\(pins.count)" : "Pinned message")
                        .font(.caption).foregroundStyle(.secondary)
                    Text(verbatim: snippet(pin)).lineLimit(1).font(.callout)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("Show the pinned message")
            Menu {
                ForEach(Array(pins.enumerated()), id: \.offset) { _, p in
                    let by = p.by.isEmpty ? "" : model.displayName(p.by) + " · "
                    Menu(by + snippet(p)) {
                        if let mi = model.chats.pinnedMessage(conversation, p) {
                            Button("Show message") { model.scrollToMessage(index: mi) }
                        }
                        Button("Unpin") { model.unpin(p) }.disabled(!model.connected)
                    }
                }
            } label: { Image(systemName: "ellipsis.circle") }
            .menuStyle(.borderlessButton)
            .fixedSize()
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 6)
        .background(Theme.chrome)
        .overlay(alignment: .bottom) { Divider() }
    }
}

/// A shared contact: avatar, name and number; "Message" opens the chat with them.
private struct ContactCardView: View {
    @Environment(AppModel.self) private var model
    let message: Message
    let foreground: Color

    var body: some View {
        let card = message.contact
        HStack(spacing: 10) {
            if let data = Data(base64Encoded: card.avatar), let img = NSImage(data: data) {
                AvatarView(image: Media.roundAvatar(img), size: 40)
            } else {
                AvatarView(image: Media.placeholderAvatar(key: card.number + card.uuid, title: card.name), size: 40)
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(verbatim: card.name.isEmpty ? "Contact" : card.name).fontWeight(.semibold)
                if !card.number.isEmpty { Text(verbatim: card.number).font(.caption).opacity(0.8) }
            }
            Spacer(minLength: 12)
            if !card.number.isEmpty || !card.uuid.isEmpty {
                Button("Message") { model.openContactChat(message) }
                    .buttonStyle(.bordered)
                    .help("Open a chat with \(card.name)")
            }
        }
        .foregroundStyle(foreground)
        .frame(minWidth: 220)
    }
}

/// A poll in its bubble: question, options with their share of the votes; a click votes (or takes it back).
private struct PollBubble: View {
    @Environment(AppModel.self) private var model
    let message: Message
    let foreground: Color

    var body: some View {
        let poll = message.poll
        let counts = poll.optionCounts
        let total = poll.votes.count
        let mine = Set(poll.voteOf(model.account))
        VStack(alignment: .leading, spacing: 8) {
            Text(poll.terminated ? "Poll · Final results" : poll.allowMultiple ? "Poll · Select one or more" : "Poll · Select one")
                .font(.caption.weight(.semibold)).foregroundStyle(foreground.opacity(0.75))
            Text(verbatim: poll.question).font(.system(size: NSFont.systemFontSize + 1, weight: .semibold))
                .fixedSize(horizontal: false, vertical: true)
            ForEach(poll.options.indices, id: \.self) { i in
                Button { model.votePoll(message.id, option: i) } label: {
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 6) {
                            Image(systemName: mine.contains(i) ? (poll.allowMultiple ? "checkmark.square.fill" : "checkmark.circle.fill")
                                                               : (poll.allowMultiple ? "square" : "circle"))
                            Text(verbatim: poll.options[i]).fixedSize(horizontal: false, vertical: true)
                            Spacer(minLength: 8)
                            Text("\(counts[i])").monospacedDigit()
                                .fontWeight(counts[i] > 0 && counts[i] == poll.topCount ? .bold : .regular)
                        }
                        GeometryReader { g in
                            ZStack(alignment: .leading) {
                                Capsule().fill(foreground.opacity(0.18))
                                Capsule().fill(foreground.opacity(0.75))
                                    .frame(width: total == 0 ? 0 : g.size.width * CGFloat(counts[i]) / CGFloat(total))
                            }
                        }
                        .frame(height: 4)
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(poll.terminated || !model.connected)
            }
            HStack {
                Text(total == 0 ? "No votes" : "\(total) vote\(total == 1 ? "" : "s")")
                Spacer()
                Button("View votes") { model.chatSheet = .pollVotes(message.id) }.buttonStyle(.plain).underline()
            }
            .font(.caption).foregroundStyle(foreground.opacity(0.75))
        }
        .foregroundStyle(foreground)
        .frame(minWidth: 240)
    }
}

/// A View Once photo: "1×" and its state; an unopened received one opens on click (once).
private struct ViewOnceBox: View {
    @Environment(AppModel.self) private var model
    let message: Message
    let foreground: Color

    var body: some View {
        let m = message
        let openable = !m.outgoing && !m.viewOnceViewed && !m.viewOnceExpired && !m.attachments.isEmpty
        let label = m.viewOnceExpired ? "View Once media expired"
            : m.viewOnceViewed ? "Viewed"
            : m.outgoing ? "View Once photo sent" : "View Once photo · Click to view"
        Button { if openable { model.openViewOnce(m) } } label: {
            HStack(spacing: 10) {
                Text("1×").font(.system(size: 15, weight: .bold))
                    .frame(width: 30, height: 30)
                    .overlay(Circle().strokeBorder(foreground.opacity(0.7), style: StrokeStyle(lineWidth: 1.5, dash: openable ? [] : [3, 3])))
                Text(label).font(.callout.weight(.semibold))
            }
            .foregroundStyle(foreground)
            .padding(.horizontal, 10)
            .padding(.vertical, 8)
            .background(RoundedRectangle(cornerRadius: 10).fill(foreground.opacity(0.12)))
        }
        .buttonStyle(.plain)
        .disabled(!openable)
        .help(openable ? "The photo can be viewed once; afterwards it is gone from this device" : "")
    }
}

/// Warning shown while a contact's safety number changed and the change is not accepted yet.
private struct SafetyBar: View {
    @Environment(AppModel.self) private var model
    let conversation: Conversation
    let change: SafetyChange

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "exclamationmark.shield.fill").foregroundStyle(.orange).font(.title3)
            Text("Your safety number with \(conversation.title) has changed. They may have reinstalled Signal or switched phones, but it can also mean that someone is intercepting your chat. Compare the number with them before you rely on this chat."
                 + (change.blocked ? " Messages cannot be sent until you verify it." : ""))
                .font(.callout)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 8)
            VStack(alignment: .trailing, spacing: 6) {
                Button("Verify …") { model.chatSheet = .safetyNumber(conversation.key) }
                    .disabled(!model.connected)
                if !change.blocked { Button("Dismiss") { model.dismissSafetyChange(conversation.key) } }
            }
        }
        .padding(12)
        .background(Color.orange.opacity(0.14))
        .overlay(alignment: .bottom) { Divider() }
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
    @State private var atBottom = true // the end of the chat is in view
    @State private var missed = 0      // messages that arrived while scrolled up

    static let typingRowId = "typing-indicator"
    static let bottomId = "chat-bottom"

    private func sameRun(_ a: Message, _ b: Message) -> Bool {
        a.outgoing == b.outgoing && a.authorKey == b.authorKey && abs(a.ts - b.ts) <= Theme.groupGapMs
            && Calendar.current.isDate(a.time, inSameDayAs: b.time)
    }

    var body: some View {
        let msgs = conversation.msgs
        let typers = (model.typing[conversation.key] ?? [:]).keys.sorted()
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
                    if !typers.isEmpty {
                        TypingRow(conversation: conversation, typers: typers)
                            .padding(.top, 10)
                            .id(MessageList.typingRowId)
                            .transition(.opacity)
                    }
                    // Seen while the newest message is in view; the "scroll to latest" button shows otherwise.
                    Color.clear.frame(height: 1)
                        .id(MessageList.bottomId)
                        .onAppear { atBottom = true; missed = 0 }
                        .onDisappear { atBottom = false }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
                .background(ChatScroller.Finder())
            }
            .defaultScrollAnchor(.bottom)
            .background(model.appearance.backgroundColor)
            .onChange(of: msgs.last?.id) { old, id in
                guard let id, let last = msgs.last else { return }
                // Follow new messages only while at the end (or for your own); otherwise count them on the button.
                if atBottom || last.outgoing {
                    withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo(id, anchor: .bottom) }
                } else if let old, msgs.contains(where: { $0.id == old }) { // appended, not the old last one removed
                    missed += 1
                }
            }
            .onChange(of: conversation.key) { _, _ in
                missed = 0
                atBottom = true
            }
            .overlay(alignment: .bottomTrailing) {
                // The animation belongs to the button only: on the whole list it would animate the
                // complete rebuild of the chat on every switch (atBottom flips then).
                ZStack {
                    if !atBottom && !msgs.isEmpty {
                        ScrollToLatestButton(missed: missed) {
                            missed = 0
                            withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(MessageList.bottomId, anchor: .bottom) }
                        }
                        .padding(16)
                        .transition(.opacity.combined(with: .scale(scale: 0.8)))
                    }
                }
                .animation(.easeOut(duration: 0.15), value: atBottom)
            }
            .onChange(of: typers.isEmpty) { _, none in
                guard !none else { return }
                withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo(MessageList.typingRowId, anchor: .bottom) }
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

/// Round "scroll to the newest message" button, with a badge for the messages that arrived meanwhile.
private struct ScrollToLatestButton: View {
    let missed: Int
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: "chevron.down")
                .font(.system(size: 15, weight: .semibold))
                .frame(width: 36, height: 36)
                .background(Circle().fill(.regularMaterial))
                .overlay(Circle().strokeBorder(.separator))
                .shadow(color: .black.opacity(0.15), radius: 3, y: 1)
                .overlay(alignment: .topTrailing) {
                    if missed > 0 {
                        Text(missed > 99 ? "99+" : "\(missed)")
                            .font(.caption2.weight(.bold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 5)
                            .frame(minWidth: 18, minHeight: 18)
                            .background(Capsule().fill(Theme.signalBlue))
                            .offset(x: 6, y: -6)
                    }
                }
        }
        .buttonStyle(.plain)
        .help(missed > 0 ? "\(missed) new message\(missed == 1 ? "" : "s") – scroll to the newest" : "Scroll to the newest message")
        .accessibilityLabel("Scroll to the newest message")
    }
}

/// Keyboard scrolling of the chat (Page Up / Page Down / Home / End) while the focus is in the message field.
/// SwiftUI's ScrollView offers no paging, so this works on the NSScrollView underneath it.
@MainActor
final class ChatScroller {
    static let shared = ChatScroller()
    weak var scrollView: NSScrollView?

    /// Placed inside the chat's ScrollView; remembers the NSScrollView around it.
    struct Finder: NSViewRepresentable {
        func makeNSView(context: Context) -> NSView { NSView() }
        func updateNSView(_ v: NSView, context: Context) {
            DispatchQueue.main.async { if let sv = v.enclosingScrollView { ChatScroller.shared.scrollView = sv } }
        }
    }

    /// One screen up (-1) or down (+1), keeping a little of the previous screen in view.
    func page(_ direction: Int) {
        guard let g = geometry() else { return }
        let step = (g.clip.bounds.height - g.insets) * 0.9 * CGFloat(direction)
        scroll(g.clip, to: g.y + (g.flipped ? step : -step), min: g.minY, max: g.maxY)
    }

    func toEnd(top: Bool) {
        guard let g = geometry() else { return }
        scroll(g.clip, to: top == g.flipped ? g.minY : g.maxY, min: g.minY, max: g.maxY)
    }

    /// Scroll range of the clip view, including the toolbar's content insets (the origin may go negative).
    private func geometry() -> (clip: NSClipView, y: CGFloat, minY: CGFloat, maxY: CGFloat, insets: CGFloat, flipped: Bool)? {
        guard let sv = scrollView, sv.window != nil, let doc = sv.documentView else { return nil }
        let clip = sv.contentView
        let lead = doc.isFlipped ? sv.contentInsets.top : sv.contentInsets.bottom
        let trail = doc.isFlipped ? sv.contentInsets.bottom : sv.contentInsets.top
        let minY = -lead
        let maxY = max(minY, doc.frame.height - clip.bounds.height + trail)
        return (clip, clip.bounds.origin.y, minY, maxY, sv.contentInsets.top + sv.contentInsets.bottom, doc.isFlipped)
    }

    private func scroll(_ clip: NSClipView, to y: CGFloat, min lo: CGFloat, max hi: CGFloat) {
        let target = NSPoint(x: clip.bounds.origin.x, y: Swift.min(Swift.max(lo, y), hi))
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.18
            clip.animator().setBoundsOrigin(target)
        }
        clip.enclosingScrollView?.reflectScrolledClipView(clip)
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
        .opacity(hovering && model.connected && !message.deleted ? 1 : 0)
        .animation(.easeOut(duration: 0.12), value: hovering)
    }
}

/// The bubble itself: sender, quote, attachments, text and the time row.
private struct Bubble: View {
    @Environment(AppModel.self) private var model
    let message: Message
    let isGroup: Bool
    let grouping: MessageGrouping
    @State private var revealed = false // spoilers of this message shown

    var body: some View {
        let m = message
        let a = model.appearance
        let fg = m.outgoing ? a.outTextColor : a.inTextColor
        let bg = m.outgoing ? a.outBgColor : a.inBgColor
        let shape = bubbleShape(radius: CGFloat(a.radius))
        let pending = model.chats.isPendingPreview(model.current ?? "", ts: m.ts)
        let mediaOnly = !m.viewOnce && m.text.isEmpty && !m.quote.valid && !m.attachments.isEmpty
            && m.attachments.allSatisfy { $0.isImage && !$0.thumb.isEmpty }
        let showSender = isGroup && !m.outgoing && !grouping.joinedAbove
        // A sticker on its own is shown without a bubble, like in the Signal apps.
        let stickerOnly = !m.viewOnce && m.text.isEmpty && !m.quote.valid && m.attachments.count == 1 && m.attachments[0].isSticker

        Group {
            if m.deleted {
                // "This message was deleted." / "You deleted this message." in a muted italic style
                BubbleContentLayout(spacing: 3) {
                    VStack(alignment: .leading, spacing: 6) {
                        if showSender { senderLabel }
                        Text(m.deletedText)
                            .font(.system(size: NSFont.systemFontSize + 1).italic())
                            .foregroundStyle(fg.opacity(0.7))
                    }
                    MetaRow(message: m, pending: pending, color: fg.opacity(0.7))
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(shape.fill(bg))
            } else if stickerOnly {
                VStack(alignment: m.outgoing ? .trailing : .leading, spacing: 2) {
                    if showSender { senderLabel }
                    StickerAttachmentView(message: m, index: 0, attachment: m.attachments[0])
                    MetaRow(message: m, pending: pending, color: .secondary)
                }
            } else if mediaOnly {
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
                        if m.hasPoll { PollBubble(message: m, foreground: fg) }
                        if m.hasContact { ContactCardView(message: m, foreground: fg) }
                        if m.viewOnce {
                            ViewOnceBox(message: m, foreground: fg)
                        } else {
                            ForEach(Array(m.attachments.enumerated()), id: \.offset) { i, att in
                                AttachmentView(message: m, index: i, attachment: att, foreground: fg, edgeToEdge: false)
                            }
                        }
                        if !m.text.isEmpty {
                            let shown = model.displayText(m)
                            let spoilers = shown.styles.contains { $0.style == "SPOILER" }
                            Text(Bubble.formatted(shown.text, styles: shown.styles, emojiOnly: Bubble.isEmojiOnly(shown.text),
                                                  revealSpoilers: revealed, spoilerColor: fg))
                                .onTapGesture { if spoilers { revealed.toggle() } }
                                .help(spoilers && !revealed ? "Click to show the spoiler" : "")
                                .textSelection(.enabled)
                                .foregroundStyle(fg)
                                .tint(m.outgoing ? fg : Theme.signalBlue)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        if m.linkPreview.isValid { LinkPreviewCard(preview: m.linkPreview, foreground: fg) }
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

    /// Created once: building a detector is expensive and `linkified` runs on every redraw of a bubble.
    private static let linkDetector = try? NSDataDetector(types: NSTextCheckingResult.CheckingType.link.rawValue)

    /// Message text with Signal formatting (SwiftUI attributes, so `Text` draws them); spoilers are
    /// covered with `spoilerColor` until `revealSpoilers`.
    static func formatted(_ text: String, styles: [TextStyleRange], emojiOnly: Bool = false,
                          revealSpoilers: Bool = false, spoilerColor: Color = .primary) -> AttributedString {
        let valid = styles.filter { $0.length > 0 && $0.start >= 0 && $0.start + $0.length <= (text as NSString).length }
        guard !valid.isEmpty, !emojiOnly else { return linkified(text, emojiOnly: emojiOnly) }
        let ns = text as NSString
        var cuts: Set<Int> = [0, ns.length]
        for st in valid { cuts.insert(st.start); cuts.insert(st.start + st.length) }
        let points = cuts.sorted()
        let size = NSFont.systemFontSize + 1
        var out = AttributedString()
        for (a, b) in zip(points, points.dropFirst()) where b > a {
            let active = Set(valid.filter { $0.start <= a && $0.start + $0.length >= b }.map(\.style))
            var seg = AttributedString(ns.substring(with: NSRange(location: a, length: b - a)))
            var font = Font.system(size: size, design: active.contains("MONOSPACE") ? .monospaced : .default)
            if active.contains("BOLD") { font = font.bold() }
            if active.contains("ITALIC") { font = font.italic() }
            seg.font = font
            if active.contains("STRIKETHROUGH") { seg.strikethroughStyle = .single }
            if active.contains("SPOILER") && !revealSpoilers {
                seg.foregroundColor = spoilerColor
                seg.backgroundColor = spoilerColor
            }
            out += seg
        }
        guard let detector = linkDetector else { return out }
        let plain = String(out.characters)
        for match in detector.matches(in: plain, range: NSRange(location: 0, length: (plain as NSString).length)) {
            guard let url = match.url, ["http", "https", "mailto"].contains(url.scheme?.lowercased() ?? ""),
                  let r = Range(match.range, in: plain), let ar = Range(r, in: out) else { continue }
            out[ar].link = url
            out[ar].underlineStyle = .single
        }
        return out
    }

    static func linkified(_ text: String, emojiOnly: Bool = false) -> AttributedString {
        var s = AttributedString(text)
        s.font = emojiOnly ? .system(size: 40) : .system(size: NSFont.systemFontSize + 1)
        guard !emojiOnly, let detector = linkDetector else { return s }
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

extension Message {
    /// Placeholder shown instead of a message that was deleted for everyone (wording as in Signal Desktop).
    var deletedText: String { outgoing ? "You deleted this message." : "This message was deleted." }
}

/// Timer, "edited", time and delivery state.
private struct MetaRow: View {
    @Environment(AppModel.self) private var model
    let message: Message
    let pending: Bool
    let color: Color

    var body: some View {
        HStack(spacing: 3) {
            if message.expiresInSeconds > 0 { Image(systemName: "timer") }
            if message.isEdited { Text("Edited") }
            Text(message.time.formatted(date: .omitted, time: .shortened))
            if message.outgoing {
                if pending || message.deleted {
                    if pending { Image(systemName: "circle.dotted").help("Sending") }
                } else if model.privacy.showDeliveryStatus {
                    ReceiptIcon(status: message.receipt)
                }
            }
        }
        .font(.system(size: 10, weight: .medium))
        .foregroundStyle(color)
    }
}

/// Signal's delivery ticks: one outlined check = sent, two outlined = delivered, two filled = read.
private struct ReceiptIcon: View {
    let status: ReceiptStatus
    private static let size: CGFloat = 11   // the glyph's box at the meta row's 10 pt font
    private static let shift: CGFloat = 5   // how far the front circle sits right of the back one

    var body: some View {
        let symbol = status == .read ? "checkmark.circle.fill" : "checkmark.circle"
        Group {
            if status == .sent {
                Image(systemName: symbol)
            } else {
                // Two circles stacked like Signal's: the front one half covers the back one, and the back
                // one is cut out around it (with a small gap) so the outlines never cross.
                // The back circle has no check of its own (as in Signal); only its uncovered crescent shows.
                ZStack(alignment: .leading) {
                    Image(systemName: status == .read ? "circle.fill" : "circle")
                        .frame(width: Self.size, height: Self.size)
                        .mask {
                            Rectangle()
                                .overlay {
                                    Circle()
                                        .frame(width: Self.size + 2.5, height: Self.size + 2.5)
                                        .offset(x: Self.shift)
                                        .blendMode(.destinationOut)
                                }
                                .compositingGroup()
                        }
                    Image(systemName: symbol)
                        .frame(width: Self.size, height: Self.size)
                        .offset(x: Self.shift)
                }
                .frame(width: Self.size + Self.shift, height: Self.size, alignment: .leading)
            }
        }
        .help(status == .read ? "Read" : status == .delivered ? "Delivered" : "Sent")
        .accessibilityLabel(status == .read ? "Read" : status == .delivered ? "Delivered" : "Sent")
    }
}

/// "… is typing": three pulsing dots in an incoming bubble at the end of the chat.
private struct TypingRow: View {
    @Environment(AppModel.self) private var model
    let conversation: Conversation
    let typers: [String]   // sender keys

    var body: some View {
        let a = model.appearance
        HStack(alignment: .bottom, spacing: 8) {
            if conversation.isGroup, let first = typers.first {
                Group {
                    if let c = model.chats.convs[first] { AvatarView(image: model.avatarImage(c), size: 28) }
                    else { AvatarView(image: Media.placeholderAvatar(key: first, title: model.displayName(first)), size: 28) }
                }
                .frame(width: 28, height: 28)
            }
            VStack(alignment: .leading, spacing: 4) {
                if conversation.isGroup {
                    Text(label)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(Theme.senderColor(typers.first ?? ""))
                }
                TimelineView(.animation(minimumInterval: 1.0 / 20)) { ctx in
                    let t = ctx.date.timeIntervalSinceReferenceDate
                    HStack(spacing: 4) {
                        ForEach(0..<3, id: \.self) { i in
                            Circle()
                                .fill(a.inTextColor)
                                .frame(width: 7, height: 7)
                                .opacity(0.3 + 0.6 * max(0, sin((t * 2 * .pi / 1.2) - Double(i) * 0.9)))
                        }
                    }
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 11)
            .background(RoundedRectangle(cornerRadius: CGFloat(a.radius), style: .continuous).fill(a.inBgColor))
            Spacer(minLength: 80)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(label)
    }

    private var label: String {
        let names = typers.map { model.displayName($0) }
        switch names.count {
        case 0: return ""
        case 1: return conversation.isGroup ? "\(names[0]) is typing" : "Typing"
        case 2: return "\(names[0]) and \(names[1]) are typing"
        default: return "\(names[0]) and \(names.count - 1) others are typing"
        }
    }
}

/// A link preview inside a bubble: image, title, description and host; a click opens the link.
private struct LinkPreviewCard: View {
    let preview: LinkPreview
    let foreground: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if !preview.imageThumb.isEmpty, let img = NSImage(data: preview.imageThumb) {
                Image(nsImage: img).resizable().aspectRatio(contentMode: .fill)
                    .frame(maxWidth: .infinity)
                    .frame(height: min(180, max(90, 300 * img.size.height / max(img.size.width, 1))))
                    .clipped()
            }
            VStack(alignment: .leading, spacing: 3) {
                Text(preview.title).font(.callout.weight(.semibold)).lineLimit(2)
                if !preview.description.isEmpty {
                    Text(preview.description).font(.caption).lineLimit(3).opacity(0.85)
                }
                Text(preview.host).font(.caption2).opacity(0.7).lineLimit(1)
            }
            .foregroundStyle(foreground)
            .fixedSize(horizontal: false, vertical: true)
            .padding(8)
        }
        .frame(maxWidth: 300, alignment: .leading)
        .background(foreground.opacity(0.06))
        .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        .contentShape(Rectangle())
        .onTapGesture { if let u = URL(string: preview.url), u.scheme?.lowercased() == "https" { NSWorkspace.shared.open(u) } }
        .help(preview.url)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isLink)
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
        if !m.deleted {
            Button { model.startReply(m.id) } label: { Label("Reply", systemImage: "arrowshape.turn.up.left") }
            if m.outgoing && m.authorKey == model.account && m.canEdit(nowMs: nowMs()) {
                Button { model.startEdit(m.id) } label: { Label("Edit", systemImage: "pencil") }
            }
            Menu {
                ForEach(EmojiCatalog.quickReactions, id: \.self) { e in
                    Button(m.reactions[model.account] == e ? "\(e)  (remove)" : e) { model.sendReaction(m.id, e) }
                }
                if let mine = m.reactions[model.account], !EmojiCatalog.quickReactions.contains(mine) {
                    Button("\(mine)  (remove)") { model.sendReaction(m.id, mine) }
                }
                Divider()
                Button("More …") { model.chatSheet = .react(m.id) }
            } label: { Label("React", systemImage: "face.smiling") }
            if !m.viewOnce {
                if model.isPinned(m) {
                    Button { model.unpin(message: m.id) } label: { Label("Unpin", systemImage: "pin.slash") }
                        .disabled(!model.connected)
                } else {
                    Menu {
                        ForEach(AppModel.pinDurations, id: \.1) { label, seconds in
                            Button(label) { model.pin(m.id, durationSeconds: seconds) }
                        }
                    } label: { Label("Pin …", systemImage: "pin") }
                    .disabled(!model.connected)
                }
            }
            if m.hasPoll {
                Button { model.chatSheet = .pollVotes(m.id) } label: { Label("View votes …", systemImage: "chart.bar") }
                if m.outgoing && !m.poll.terminated {
                    Button { model.endPoll(m.id) } label: { Label("End poll", systemImage: "stop.circle") }
                        .disabled(!model.connected)
                }
            }
            if model.canForward(m) {
                Button { model.chatSheet = .forward(m.id) } label: { Label("Forward …", systemImage: "arrowshape.turn.up.right") }
                    .disabled(!model.connected)
            }
            Divider()
        }
        Button { model.chatSheet = .messageDetails(m.id) } label: { Label("Message details …", systemImage: "info.circle") }
        if !m.text.isEmpty {
            Button {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(m.text, forType: .string)
            } label: { Label("Copy text", systemImage: "doc.on.doc") }
        }
        if !m.deleted && !m.viewOnce {
            ForEach(Array(m.attachments.enumerated()).filter { $0.element.isImage && !$0.element.isSticker }, id: \.offset) { i, _ in
                Button { model.copyImage(m, i) } label: { Label("Copy image", systemImage: "photo.on.rectangle") }
            }
            let urls = model.links(of: m)
            if urls.count == 1, let u = urls.first {
                Button { if let url = URL(string: u) { NSWorkspace.shared.open(url) } } label: { Label("Open link", systemImage: "safari") }
                Button {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(u, forType: .string)
                } label: { Label("Copy link", systemImage: "link") }
            } else if urls.count > 1 {
                Menu {
                    ForEach(urls, id: \.self) { u in
                        Button(u) { if let url = URL(string: u) { NSWorkspace.shared.open(url) } }
                    }
                } label: { Label("Open link", systemImage: "safari") }
                Menu {
                    ForEach(urls, id: \.self) { u in
                        Button(u) {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(u, forType: .string)
                        }
                    }
                } label: { Label("Copy link", systemImage: "link") }
            }
        }
        ForEach(Array((m.viewOnce ? [] : m.attachments).enumerated()), id: \.offset) { i, a in
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
