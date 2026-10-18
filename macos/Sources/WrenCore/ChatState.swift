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

    public init() {}

    public func resolveKey(_ id: String) -> String { alias[id] ?? id }

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
    }

    /// Everything that is needed to recreate the current state (for compaction).
    public func allRecords() -> [JSON] {
        var out: [JSON] = []
        for k in convs.keys.sorted() {
            let c = convs[k]!
            if !c.storedName.isEmpty { out.append(Records.name(c.key, c.storedName)) }
            if !c.localNickname.isEmpty { out.append(Records.nickname(c.key, c.localNickname)) }
            if c.archived || c.sigArchived { out.append(Records.archive(c.key, archived: c.archived, sigArchived: c.sigArchived)) }
            if c.muted { out.append(Records.mute(c.key, true)) }
            out.append(Records.expiration(c.key, c.expirationSeconds))
            for m in c.msgs where !isPendingPreview(c.key, ts: m.ts) {
                out.append(Records.message(c.key, m))
                for (by, e) in m.reactions.sorted(by: { $0.key < $1.key }) {
                    out.append(Records.reaction(c.key, ts: m.ts, author: effectiveAuthor(c, m), by: by, emoji: e))
                }
            }
        }
        return out
    }

    // MARK: - Changes (each returns the records to persist)

    /// Adds a message unless it is a duplicate. Returns nil for a duplicate.
    public mutating func addMessage(_ key: String, _ m: Message, title: String = "", group: Bool = false) -> (index: Int, records: [JSON])? {
        ensure(key, title: title, group: group)
        let c = convs[key]!
        if findMessage(c, ts: m.ts, authors: [effectiveAuthor(c, m)]) != nil { return nil }
        let at = convs[key]!.insertSorted(m)
        var recs = [Records.message(key, m)]
        // Like the official client: activity brings an archived chat back.
        if convs[key]!.archived {
            convs[key]!.archived = false
            recs.append(Records.archive(key, archived: false, sigArchived: convs[key]!.sigArchived))
        }
        return (at, recs)
    }

    public mutating func applyReaction(_ key: String, ts: Int64, authors: [String], reactor: String, emoji: String) -> JSON? {
        guard let c = convs[key], let i = findMessage(c, ts: ts, authors: authors) else { return nil }
        if emoji.isEmpty { convs[key]!.msgs[i].reactions.removeValue(forKey: reactor) }
        else { convs[key]!.msgs[i].reactions[reactor] = emoji }
        let m = convs[key]!.msgs[i]
        return Records.reaction(key, ts: m.ts, author: effectiveAuthor(c, m), by: reactor, emoji: emoji) // original ts = stable identity
    }

    public mutating func applyEdit(_ key: String, targetTs: Int64, authors: [String], text: String, newTs: Int64) -> JSON? {
        guard let c = convs[key], let i = findMessage(c, ts: targetTs, authors: authors) else { return nil }
        if newTs <= c.msgs[i].latestTs { return nil } // already applied / outdated revision
        convs[key]!.msgs[i].text = text
        convs[key]!.msgs[i].editTs = newTs
        convs[key]!.msgs[i].editCount += 1
        return Records.edit(key, convs[key]!.msgs[i])
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

    /// Removes expired messages; true if anything was removed (the history must be compacted then).
    public mutating func expireMessages(now: Int64) -> Bool {
        var changed = false
        for key in convs.keys {
            let before = convs[key]!.msgs.count
            convs[key]!.msgs.removeAll { ChatState.isExpired($0, now: now) }
            if convs[key]!.msgs.count != before { changed = true }
        }
        return changed
    }

    // MARK: - Pending sends

    /// Confirms a pending send with the final timestamp (from the RPC reply or our own echo envelope).
    /// Returns the records to persist.
    public mutating func finalizePendingSend(_ id: String, timestamp ts0: Int64, echo: Message? = nil) -> [JSON] {
        guard let pending = pendingSends[id] else { return [] }
        let ts = ts0 > 0 ? ts0 : nowMs()
        var confirmed = pending.message
        confirmed.ts = ts
        if let echo, echo.expiresInSeconds > 0 { confirmed.expiresInSeconds = echo.expiresInSeconds }
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
                .sorted { $0.element.ts != $1.element.ts ? $0.element.ts < $1.element.ts : $0.offset < $1.offset }
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

    /// Conversations of one folder, newest activity first.
    public func sortedConversations(archived: Bool) -> [Conversation] {
        convs.values.filter { $0.archived == archived }.sorted { a, b in
            if a.lastTs != b.lastTs { return a.lastTs > b.lastTs }
            return a.title.localizedStandardCompare(b.title) == .orderedAscending
        }
    }
}
