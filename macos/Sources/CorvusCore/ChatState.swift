import Foundation

/// A message shown before signal-cli confirmed the send. It is kept out of the encrypted
/// history until the send succeeds, so a failed send leaves no stale record.
public struct PendingSend {
    public var conversationKey = ""
    public var temporaryTimestamp: Int64 = 0
    public var message = Message()
    public var echoReceived = false
    public init() {}
}

/// The UI-free chat model: conversations, aliases and the rules for merging messages,
/// reactions and edits (ported from MainWindow.cpp). Mutating methods return the history
/// records that have to be persisted.
public struct ChatState {
    public var convs: [String: Conversation] = [:]
    public var alias: [String: String] = [:]          // uuid -> number
    public var contactNames: [String: String] = [:]   // key -> contact name from signal-cli
    public var pendingSends: [String: PendingSend] = [:]
    public var account = ""
    /// Declined message requests (Qt record "j"): they outlive a deleted conversation.
    public var declined: Set<String> = []
    /// Receipts for timestamps not (yet) in any chat: the send RPC may still be running. Applied on confirmation.
    public var earlyReceipts: [Int64: ReceiptStatus] = [:]
    static let earlyReceiptLimit = 200

    public init() {}

    public func resolveKey(_ id: String) -> String { alias[id] ?? id }

    public static let noteToSelfTitle = "Note to Self"

    /// The chat with our own account (key may be our number or our UUID).
    public func isNoteToSelf(_ key: String) -> Bool {
        !account.isEmpty && (key == account || resolveKey(key) == account)
    }

    /// Like the official clients, "Note to Self" exists as soon as the account is known, with a fixed title.
    public mutating func ensureNoteToSelf() {
        guard !account.isEmpty else { return }
        ensure(account, title: ChatState.noteToSelfTitle)
        convs[account]!.title = ChatState.noteToSelfTitle
    }

    /// Makes sure the conversation exists.
    public mutating func ensure(_ key: String, title: String = "", group: Bool = false) {
        if convs[key] != nil { return }
        var c = Conversation(key: key, title: title.isEmpty ? key : title, isGroup: group)
        if c.title == key { c.title = contactNames[key] ?? key } // saved contact name, if known
        convs[key] = c
    }

    public func displayName(_ key: String) -> String {
        let r = resolveKey(key)
        if !account.isEmpty && (key == account || r == account) { return "You" }
        return convs[r]?.title ?? key
    }

    public func effectiveAuthor(_ c: Conversation, _ m: Message) -> String {
        if !m.authorKey.isEmpty { return m.authorKey }
        if m.outgoing { return account }
        return c.isGroup ? "" : c.key
    }

    func authorMatches(_ c: Conversation, _ m: Message, _ authors: [String]) -> Bool {
        let mk = effectiveAuthor(c, m)
        return authors.contains { !$0.isEmpty && (mk == $0 || mk == resolveKey($0)) }
    }

    /// Authors empty => match by timestamp only. Matches the original or the latest edit timestamp.
    public func findMessage(_ c: Conversation, ts: Int64, authors: [String]) -> Int? {
        let nonEmpty = authors.filter { !$0.isEmpty }
        for i in c.msgs.indices.reversed() {
            let m = c.msgs[i]
            if m.ts != ts && !(m.editTs > 0 && m.editTs == ts) { continue }
            if nonEmpty.isEmpty || authorMatches(c, m, nonEmpty) { return i }
        }
        return nil
    }

    public func isPendingPreview(_ key: String, ts: Int64) -> Bool {
        pendingSends.values.contains { $0.conversationKey == key && $0.temporaryTimestamp == ts }
    }

    // MARK: - History

    public mutating func loadHistory(_ records: [JSON]) {
        for o in records {
            let t = jsonString(o["t"])
            let key = jsonString(o["c"])
            if key.isEmpty { continue }
            let group = key.hasPrefix("g:")
            switch t {
            case "m":
                ensure(key, title: group ? "Group" : key, group: group)
                convs[key]!.insertSorted(Records.messageFromRecord(o))
            case "n":
                ensure(key, title: key, group: group)
                convs[key]!.storedName = jsonString(o["n"])
                if !convs[key]!.storedName.isEmpty && convs[key]!.localNickname.isEmpty {
                    convs[key]!.title = convs[key]!.storedName
                }
            case "p":
                ensure(key, title: key, group: group)
                let nick = jsonString(o["n"])
                convs[key]!.localNickname = nick
                let stored = convs[key]!.storedName
                convs[key]!.title = !nick.isEmpty ? nick : (contactNames[key] ?? (stored.isEmpty ? key : stored))
            case "d":
                convs.removeValue(forKey: key)
            case "u", "r":
                guard let c = convs[key] else { continue }
                let au = jsonString(o["au"])
                guard let i = findMessage(c, ts: jsonInt64(o["ts"]), authors: au.isEmpty ? [] : [au]) else { continue }
                if t == "u" {
                    Records.applyExtras(&convs[key]!.msgs[i], jsonObject(o["d"]))
                } else {
                    let by = jsonString(o["by"]), e = jsonString(o["e"])
                    if e.isEmpty { convs[key]!.msgs[i].reactions.removeValue(forKey: by) }
                    else { convs[key]!.msgs[i].reactions[by] = e }
                }
            case "pv":
                // A poll vote; the poll message is written before its votes, like reactions.
                guard let c = convs[key] else { continue }
                let au = jsonString(o["au"]), by = jsonString(o["by"])
                guard !by.isEmpty, let i = findMessage(c, ts: jsonInt64(o["ts"]), authors: au.isEmpty ? [] : [au]),
                      c.msgs[i].hasPoll else { continue }
                let vote = PollVote(optionIndexes: jsonArray(o["oi"]).map { jsonInt($0) },
                                    voteCount: jsonInt64(o["vc"]), voteTs: jsonInt64(o["vt"]))
                convs[key]!.msgs[i].poll.apply(vote, by: by)
            case "pi":
                guard let c = convs[key] else { continue }
                let ts = jsonInt64(o["ts"]), au = jsonString(o["au"])
                if jsonBool(o["rm"]) {
                    convs[key]!.pins.removeAll { $0.ts == ts && (au.isEmpty || $0.authorKey == au) }
                    continue
                }
                guard findMessage(c, ts: ts, authors: au.isEmpty ? [] : [au]) != nil else { continue }
                addPin(key, PinInfo(ts: ts, authorKey: au, by: jsonString(o["by"]),
                                    pinnedAtMs: jsonInt64(o["at"]), durationSeconds: jsonInt64(o["d"])))
            case "b":
                ensure(key, title: key, group: group)
                convs[key]!.blocked = jsonBool(o["v"])
            case "j":
                declined.insert(key) // declined message request: no conversation is created
            case "k":
                ensure(key, title: key, group: group)
                convs[key]!.muted = jsonBool(o["v"])
            case "e":
                ensure(key, title: key, group: group)
                convs[key]!.expirationSeconds = jsonInt(o["v"])
            case "a":
                ensure(key, title: key, group: group)
                convs[key]!.archived = jsonBool(o["v"])
                convs[key]!.sigArchived = jsonBool(o["s"])
            default:
                break
            }
        }
        pruneExpiredPins(now: nowMs()) // a pin that ran out while Corvus was closed is not restored
    }

    // MARK: - Pins

    /// Adds a pin; a re-applied pin replaces itself, and like the official clients a fourth pin
    /// replaces the oldest one.
    public mutating func addPin(_ key: String, _ pin: PinInfo) {
        guard convs[key] != nil else { return }
        convs[key]!.pins.removeAll { $0.ts == pin.ts && ($0.authorKey == pin.authorKey || pin.authorKey.isEmpty) }
        while convs[key]!.pins.count >= PinInfo.maxPerChat { convs[key]!.pins.removeFirst() }
        convs[key]!.pins.append(pin)
    }

    /// Drops pins whose duration ran out. Returns the keys of the chats that changed.
    @discardableResult
    public mutating func pruneExpiredPins(now: Int64) -> [String] {
        var changed: [String] = []
        for key in convs.keys where convs[key]!.pins.contains(where: { $0.isExpired(now: now) }) {
            convs[key]!.pins.removeAll { $0.isExpired(now: now) }
            changed.append(key)
        }
        return changed
    }

    /// Everything that is needed to recreate the current state (for compaction).
    public func allRecords() -> [JSON] {
        var out: [JSON] = []
        for k in convs.keys.sorted() {
            let c = convs[k]!
            if !c.storedName.isEmpty { out.append(Records.name(c.key, c.storedName)) }
            if !c.localNickname.isEmpty { out.append(Records.nickname(c.key, c.localNickname)) }
            if c.archived || c.sigArchived { out.append(Records.archive(c.key, archived: c.archived, sigArchived: c.sigArchived)) }
            if c.blocked { out.append(Records.blocked(c.key, true)) }
            if c.muted { out.append(Records.mute(c.key, true)) }
            out.append(Records.expiration(c.key, c.expirationSeconds))
            for m in c.msgs where !isPendingPreview(c.key, ts: m.ts) {
                out.append(Records.message(c.key, m))
                for (by, e) in m.reactions.sorted(by: { $0.key < $1.key }) {
                    out.append(Records.reaction(c.key, ts: m.ts, author: effectiveAuthor(c, m), by: by, emoji: e))
                }
                for (by, v) in m.poll.votes.sorted(by: { $0.key < $1.key }) {
                    out.append(Records.pollVote(c.key, ts: m.ts, author: effectiveAuthor(c, m), by: by, vote: v))
                }
            }
            // Pins come after the messages they reference; their order is kept.
            for p in c.pins { out.append(Records.pin(c.key, p)) }
        }
        for k in declined.sorted() { out.append(Records.requestDeclined(k)) }
        return out
    }

    // MARK: - Changes (each returns the records to persist)

    /// Adds a message unless it is a duplicate. Returns nil for a duplicate.
    public mutating func addMessage(_ key: String, _ m: Message, title: String = "", group: Bool = false) -> (index: Int, records: [JSON])? {
        ensure(key, title: title, group: group)
        let c = convs[key]!
        if findMessage(c, ts: m.ts, authors: [effectiveAuthor(c, m)]) != nil { return nil }
        var message = m
        if message.outgoing, let r = earlyReceipts.removeValue(forKey: message.ts), r > message.receipt {
            message.receipt = r
        }
        // Always append new arrivals in local receive order. The original Signal timestamp
        // remains untouched because quotes, edits and reactions use it as the message ID.
        let previousOrder = c.msgs.last?.sortTs ?? 0
        message.orderTs = max(message.orderTs, max(nowMs(), previousOrder + 1))
        let at = convs[key]!.insertSorted(message)
        var recs = [Records.message(key, message)]
        // Like the official client: activity brings an archived chat back.
        if convs[key]!.archived {
            convs[key]!.archived = false
            recs.append(Records.archive(key, archived: false, sigArchived: convs[key]!.sigArchived))
        }
        return (at, recs)
    }

    public mutating func applyReaction(_ key: String, ts: Int64, authors: [String], reactor: String, emoji: String) -> JSON? {
        guard let c = convs[key], let i = findMessage(c, ts: ts, authors: authors) else { return nil }
        if c.msgs[i].deleted { return nil } // nothing to react to any more
        if emoji.isEmpty { convs[key]!.msgs[i].reactions.removeValue(forKey: reactor) }
        else { convs[key]!.msgs[i].reactions[reactor] = emoji }
        let m = convs[key]!.msgs[i]
        return Records.reaction(key, ts: m.ts, author: effectiveAuthor(c, m), by: reactor, emoji: emoji) // original ts = stable identity
    }

    public mutating func applyEdit(_ key: String, targetTs: Int64, authors: [String], text: String, newTs: Int64,
                                   styles: [TextStyleRange] = []) -> JSON? {
        guard let c = convs[key], let i = findMessage(c, ts: targetTs, authors: authors) else { return nil }
        if c.msgs[i].deleted || newTs <= c.msgs[i].latestTs { return nil } // deleted / already applied / outdated revision
        convs[key]!.msgs[i].text = text
        convs[key]!.msgs[i].textStyles = styles
        convs[key]!.msgs[i].mentions = [] // the new text has its mentions written out
        convs[key]!.msgs[i].linkPreview = LinkPreview() // it belonged to the old text; edits do not regenerate it
        convs[key]!.msgs[i].editTs = newTs
        convs[key]!.msgs[i].editCount += 1
        return Records.edit(key, convs[key]!.msgs[i])
    }

    /// A delivery/read receipt for our message sent at `ts`. Receipts carry no conversation, so
    /// `preferredKey` (the sender's 1:1 chat) is searched first, then every chat (group messages).
    /// The state only ever goes up. Returns the record to persist, nil if nothing changed.
    /// `recipient` (who sent the receipt) and `timeMs` fill the per-recipient state shown in Message details.
    public mutating func applyReceipt(ts: Int64, level: ReceiptStatus, preferredKey: String = "",
                                      recipient: String = "", timeMs: Int64 = 0) -> JSON? {
        guard ts > 0, level > .sent else { return nil }
        var keys = convs.keys.sorted()
        if let i = keys.firstIndex(of: preferredKey) { keys.swapAt(0, i) }
        let who = recipient.isEmpty ? "" : resolveKey(recipient)
        for key in keys {
            guard let i = findMessage(convs[key]!, ts: ts, authors: [account]) else { continue }
            let m = convs[key]!.msgs[i]
            guard m.outgoing, !isPendingPreview(key, ts: m.ts) else { continue }
            if m.deleted { return nil }
            var changed = false
            if !who.isEmpty && who != account {
                var r = m.recipientReceipts[who] ?? RecipientReceipt()
                let old = r
                r.status = max(r.status, level.rawValue)
                let at = timeMs > 0 ? timeMs : nowMs()
                if level == .delivered && r.deliveredAtMs <= 0 { r.deliveredAtMs = at }
                if level == .read && r.readAtMs <= 0 { r.readAtMs = at }
                if r != old {
                    convs[key]!.msgs[i].recipientReceipts[who] = r
                    changed = true
                }
            }
            if m.receipt < level {
                convs[key]!.msgs[i].receipt = level
                changed = true
            }
            return changed ? Records.receiptUpdate(key, convs[key]!.msgs[i]) : nil
        }
        if earlyReceipts[ts].map({ $0 < level }) ?? true {
            if earlyReceipts.count >= ChatState.earlyReceiptLimit, let oldest = earlyReceipts.keys.min() {
                earlyReceipts.removeValue(forKey: oldest)
            }
            earlyReceipts[ts] = level
        }
        return nil
    }

    /// Delete for everyone (ours or the author's): the message turns into a placeholder.
    /// Authors empty => match by timestamp only. Returns the record for the history, nil if nothing changed.
    public mutating func applyDelete(_ key: String, targetTs: Int64, authors: [String]) -> JSON? {
        guard let c = convs[key], let i = findMessage(c, ts: targetTs, authors: authors) else { return nil }
        if c.msgs[i].deleted { return nil } // redelivery
        convs[key]!.msgs[i].markDeleted()
        return Records.deletion(key, convs[key]!.msgs[i])
    }

    /// A View Once item was opened (or ran out): its content is dropped for good. Returns the record and the
    /// attachments whose stored copies are to be deleted; nil if there was nothing to consume.
    public mutating func consumeViewOnce(_ key: String, ts: Int64, author: String, expired: Bool) -> (record: JSON, attachments: [Attachment])? {
        guard let c = convs[key], let i = findMessage(c, ts: ts, authors: [author]) else { return nil }
        let m = c.msgs[i]
        guard m.viewOnce, !m.viewOnceViewed, !m.viewOnceExpired else { return nil }
        convs[key]!.msgs[i].viewOnceViewed = !expired
        convs[key]!.msgs[i].viewOnceExpired = expired
        convs[key]!.msgs[i].attachments = []
        convs[key]!.msgs[i].text = ""
        convs[key]!.msgs[i].textStyles = []
        convs[key]!.msgs[i].mentions = []
        convs[key]!.msgs[i].quote = Quote()
        return (Records.viewOnceUpdate(key, convs[key]!.msgs[i]), m.attachments)
    }

    /// Unopened View Once items older than 45 days expire.
    public mutating func expireViewOnce(now: Int64) -> [(record: JSON, attachments: [Attachment])] {
        var out: [(record: JSON, attachments: [Attachment])] = []
        for key in convs.keys {
            for m in convs[key]!.msgs where m.viewOnce && !m.viewOnceViewed && !m.viewOnceExpired
                && now - m.ts >= Message.viewOnceLifetimeMs {
                if let r = consumeViewOnce(key, ts: m.ts, author: effectiveAuthor(convs[key]!, m), expired: true) { out.append(r) }
            }
        }
        return out
    }

    public func hasExpiredViewOnce(now: Int64) -> Bool {
        convs.values.contains { c in
            c.msgs.contains { $0.viewOnce && !$0.viewOnceViewed && !$0.viewOnceExpired && now - $0.ts >= Message.viewOnceLifetimeMs }
        }
    }

    /// Starts the local timer of incoming disappearing messages (when the chat is looked at).
    public mutating func startIncomingExpirationTimers(_ key: String, now: Int64) -> [JSON] {
        guard convs[key] != nil else { return [] }
        var recs: [JSON] = []
        for i in convs[key]!.msgs.indices {
            let m = convs[key]!.msgs[i]
            if m.outgoing || m.expiresInSeconds <= 0 || m.expirationStartMs > 0 { continue }
            convs[key]!.msgs[i].expirationStartMs = now
            recs.append(Records.expirationUpdate(key, convs[key]!.msgs[i]))
        }
        return recs
    }

    private static func isExpired(_ m: Message, now: Int64) -> Bool {
        m.expiresInSeconds > 0 && m.expirationStartMs > 0 && now - m.expirationStartMs >= Int64(m.expiresInSeconds) * 1000
    }

    /// Read-only check for the 1 s timer: calling the mutating `expireMessages` on an observed model
    /// counts as a change even when nothing is removed, and every chat view is rebuilt.
    public func hasExpiredMessages(now: Int64) -> Bool {
        convs.values.contains { c in c.msgs.contains { ChatState.isExpired($0, now: now) } }
    }

    /// Removes expired messages and returns them (the client deletes their stored attachment
    /// copies on the backend, F1); empty when nothing expired.
    @discardableResult
    public mutating func expireMessages(now: Int64) -> [Message] {
        var removed: [Message] = []
        for key in convs.keys {
            let expired = convs[key]!.msgs.filter { ChatState.isExpired($0, now: now) }
            guard !expired.isEmpty else { continue }
            removed.append(contentsOf: expired)
            convs[key]!.msgs.removeAll { ChatState.isExpired($0, now: now) }
        }
        return removed
    }

    // MARK: - Pending sends

    /// Confirms a pending send with the final timestamp (from the RPC reply or our own echo envelope).
    /// Returns the records to persist.
    public mutating func finalizePendingSend(_ id: String, timestamp ts0: Int64, echo: Message? = nil) -> [JSON] {
        guard let pending = pendingSends[id] else { return [] }
        let ts = ts0 > 0 ? ts0 : nowMs()
        var confirmed = pending.message
        confirmed.ts = ts
        if let r = earlyReceipts.removeValue(forKey: ts), r > confirmed.receipt { confirmed.receipt = r }
        if let echo, echo.expiresInSeconds > 0 { confirmed.expiresInSeconds = echo.expiresInSeconds }
        if let echo, echo.viewOnce { confirmed.viewOnce = true }
        confirmed.sanitizeViewOnce() // a sent View Once photo is not kept on this device
        if confirmed.expiresInSeconds > 0 { confirmed.expirationStartMs = ts }
        if let echo {
            // Keep the local file paths and thumbnails, but copy the remote attachment IDs
            // when signal-cli's own-message envelope reaches us before the RPC reply.
            for i in confirmed.attachments.indices where i < echo.attachments.count && !echo.attachments[i].id.isEmpty {
                confirmed.attachments[i].id = echo.attachments[i].id
            }
        }
        let markDone = { (state: inout ChatState) in
            if echo != nil { state.pendingSends[id]?.echoReceived = true } // keep until the RPC callback
            else { state.pendingSends.removeValue(forKey: id) }
        }

        let key = pending.conversationKey
        guard let conversation = convs[key],
              let previewIndex = conversation.msgs.firstIndex(where: {
                  $0.ts == pending.temporaryTimestamp && $0.outgoing && $0.authorKey == account
              })
        else {
            markDone(&self)
            return addMessage(key, confirmed)?.records ?? []
        }

        // If an envelope already inserted the confirmed timestamp by another path, remove
        // only our temporary preview rather than creating a duplicate chat bubble.
        let duplicate = conversation.msgs.indices.contains { i in
            i != previewIndex && conversation.msgs[i].ts == ts && conversation.msgs[i].outgoing
                && conversation.msgs[i].authorKey == account
        }
        var recs: [JSON] = []
        if duplicate {
            convs[key]!.msgs.remove(at: previewIndex)
        } else {
            convs[key]!.msgs[previewIndex] = confirmed
            // stable sort by time
            convs[key]!.msgs = convs[key]!.msgs.enumerated()
                .sorted { $0.element.sortTs != $1.element.sortTs ? $0.element.sortTs < $1.element.sortTs : $0.offset < $1.offset }
                .map(\.element)
            recs.append(Records.message(key, confirmed))
        }
        // Sending a message brings an archived conversation back, just like receiving one.
        if convs[key]!.archived {
            convs[key]!.archived = false
            recs.append(Records.archive(key, archived: false, sigArchived: convs[key]!.sigArchived))
        }
        markDone(&self)
        return recs
    }

    public mutating func removePendingPreview(_ pending: PendingSend) {
        guard let c = convs[pending.conversationKey],
              let i = c.msgs.firstIndex(where: { $0.ts == pending.temporaryTimestamp && $0.outgoing && $0.authorKey == account })
        else { return }
        convs[pending.conversationKey]!.msgs.remove(at: i)
    }

    /// For an echo of our own message (sync sentMessage): the id of the oldest pending send it confirms,
    /// or nil if the echo belongs to an already confirmed message or no matching preview exists.
    public func pendingSendMatching(echo m: Message, key: String) -> String? {
        // An envelope for an earlier send can arrive while another identical message is
        // pending. Don't let that older echo consume the newer message's preview.
        if let c = convs[key], c.msgs.contains(where: {
            $0.ts == m.ts && $0.outgoing && $0.authorKey == account && !isPendingPreview(key, ts: $0.ts)
        }) {
            return nil
        }
        var best: (id: String, ts: Int64)?
        for (id, p) in pendingSends {
            if p.echoReceived || p.conversationKey != key || p.message.text != m.text { continue }
            if !p.message.attachments.isEmpty && !m.attachments.isEmpty && p.message.attachments.count != m.attachments.count {
                continue
            }
            if best == nil || p.temporaryTimestamp < best!.ts { best = (id, p.temporaryTimestamp) }
        }
        guard let best, let c = convs[key],
              c.msgs.contains(where: { $0.ts == best.ts && $0.outgoing && $0.authorKey == account })
        else { return nil }
        return best.id
    }

    // MARK: - Sorting for the chat list

    /// Conversations of one folder: newest activity first, or with `alphabetical` by name only, so the
    /// order does not change while messages arrive (same rules as the Qt app's ConversationOrder).
    /// "Note to Self" is pinned at the top of the chat list (an archived one lives in the folder).
    public func sortedConversations(archived: Bool, alphabetical: Bool = false) -> [Conversation] {
        convs.values.filter { $0.archived == archived }.sorted { a, b in
            if !archived && isNoteToSelf(a.key) != isNoteToSelf(b.key) { return isNoteToSelf(a.key) }
            return ChatState.before(a, b, alphabetical: alphabetical)
        }
    }

    /// The chats shown in the list: sorted, and narrowed to `query` (title or key contains it).
    public func visibleConversations(archived: Bool, alphabetical: Bool, query: String) -> [Conversation] {
        let q = query.trimmingCharacters(in: .whitespaces)
        return sortedConversations(archived: archived, alphabetical: alphabetical).filter {
            q.isEmpty || $0.title.localizedCaseInsensitiveContains(q) || $0.key.localizedCaseInsensitiveContains(q)
        }
    }

    /// Row to switch to from `current` (nil: none) when moving `delta` (-1 up, +1 down) through `count` chats.
    /// Does not wrap around; nil if there is nowhere to go. Without a current row the first (down) or last (up)
    /// one is used. Same rules as the Qt app's ConversationOrder::step.
    public static func step(current: Int?, count: Int, delta: Int) -> Int? {
        if count <= 0 || delta == 0 { return nil }
        guard let c = current, c >= 0, c < count else { return delta > 0 ? 0 : count - 1 }
        let t = c + (delta > 0 ? 1 : -1)
        return t < 0 || t >= count ? nil : t
    }

    /// Names compared the way people expect: ignoring case, with the language's rules (accents, ä next to a).
    /// Identical names fall back to the key, so the order is stable.
    static func before(_ a: Conversation, _ b: Conversation, alphabetical: Bool) -> Bool {
        if !alphabetical && a.lastTs != b.lastTs { return a.lastTs > b.lastTs }
        switch a.title.compare(b.title, options: [.caseInsensitive, .numeric], locale: .current) {
        case .orderedAscending: return true
        case .orderedDescending: return false
        case .orderedSame: return a.key < b.key
        }
    }
}
