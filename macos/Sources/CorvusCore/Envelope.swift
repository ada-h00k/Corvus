import Foundation

/// A signal-cli "receive" envelope reduced to what the GUI needs.
public struct ParsedEnvelope {
    public enum Kind { case none, message, reaction, edit, expirationUpdate }

    public var kind: Kind = .none
    public var outgoing = false        // sent by one of our own other devices (sync message)
    public var src = ""                // sender number / UUID (incoming)
    public var srcName = ""            // sender's profile name
    public var destination = ""        // recipient number / UUID (outgoing 1:1)
    public var groupId = ""            // set for group messages
    public var expirationSeconds = 0
    public var msg = Message()         // message: ts, text, attachments, quote (author keys unresolved)

    // Reaction
    public var emoji = ""
    public var remove = false
    public var targetNumber = ""
    public var targetUuid = ""
    public var targetTs: Int64 = 0     // reaction: reacted message; edit: timestamp of the message being edited

    // @mentions in msg.text (message, edit)
    public var mentions: [Mention] = []

    public init() {}
}

/// An @mention: Signal puts U+FFFC into the text and names the person separately (offsets in UTF-16 units).
public struct Mention: Equatable {
    public var start = 0
    public var length = 0
    public var number = ""
    public var uuid = ""
    public var name = ""

    public init(start: Int = 0, length: Int = 0, number: String = "", uuid: String = "", name: String = "") {
        self.start = start
        self.length = length
        self.number = number
        self.uuid = uuid
        self.name = name
    }
}

/// Replaces the mention placeholders in `text` with "@" + name.
public func applyMentions(_ text: String, _ mentions: [Mention], name: (Mention) -> String) -> String {
    let s = NSMutableString(string: text)
    for m in mentions.sorted(by: { $0.start > $1.start }) {
        guard m.start >= 0, m.length > 0, m.start + m.length <= s.length else { continue }
        s.replaceCharacters(in: NSRange(location: m.start, length: m.length), with: "@" + name(m))
    }
    return s as String
}

private func parseMentions(_ dm: JSON) -> [Mention] {
    jsonArray(dm["mentions"]).map { v in
        let o = jsonObject(v)
        return Mention(start: Int(jsonInt64(o["start"])), length: Int(jsonInt64(o["length"])),
                       number: jsonString(o["number"]), uuid: jsonString(o["uuid"]), name: jsonString(o["name"]))
    }
}

private func parseAttachments(_ dm: JSON) -> [Attachment] {
    jsonArray(dm["attachments"]).map { v in
        let o = jsonObject(v)
        var a = Attachment()
        a.id = jsonString(o["id"])
        a.contentType = jsonString(o["contentType"])
        a.filename = jsonString(o["filename"])
        a.size = jsonInt64(o["size"])
        a.width = jsonInt(o["width"])
        a.height = jsonInt(o["height"])
        return a
    }
}

private func parseQuote(_ dm: JSON) -> Quote {
    var q = Quote()
    let o = jsonObject(dm["quote"])
    if o.isEmpty { return q }
    q.valid = true
    q.id = jsonInt64(o["id"])
    q.authorKey = jsonString(o["authorNumber"])
    if q.authorKey.isEmpty { q.authorKey = jsonString(o["authorUuid"]) }
    if q.authorKey.isEmpty { q.authorKey = jsonString(o["author"]) }
    q.text = jsonString(o["text"])
    if let first = jsonArray(o["attachments"]).first {
        q.attachmentNote = jsonString(jsonObject(first)["contentType"])
    }
    return q
}

private func groupIdOf(_ dm: JSON) -> String {
    jsonString(jsonObject(dm["groupInfo"])["groupId"])
}

// Fills reaction or message data from a dataMessage-like object (also used for sync sentMessage).
private func parseBody(_ dm: JSON, _ out: inout ParsedEnvelope, envelopeTs: Int64) {
    out.groupId = groupIdOf(dm)
    out.expirationSeconds = jsonInt(dm["expiresInSeconds"])
    if jsonBool(dm["isExpirationUpdate"]) {
        out.kind = .expirationUpdate
        return
    }
    let r = jsonObject(dm["reaction"])
    if !r.isEmpty {
        out.kind = .reaction
        out.emoji = jsonString(r["emoji"])
        out.remove = jsonBool(r["isRemove"])
        out.targetNumber = jsonString(r["targetAuthorNumber"])
        if out.targetNumber.isEmpty { out.targetNumber = jsonString(r["targetAuthor"]) }
        out.targetUuid = jsonString(r["targetAuthorUuid"])
        out.targetTs = jsonInt64(r["targetSentTimestamp"])
        return
    }

    out.msg.text = jsonString(dm["message"])
    out.mentions = parseMentions(dm)
    out.msg.attachments = parseAttachments(dm)
    out.msg.quote = parseQuote(dm)
    out.msg.expiresInSeconds = out.expirationSeconds
    if out.msg.text.isEmpty && out.msg.attachments.isEmpty { return } // receipts, typing, sessions, ...

    var ts = jsonInt64(dm["timestamp"])
    if ts <= 0 { ts = envelopeTs }
    out.msg.ts = ts > 0 ? ts : nowMs()
    out.kind = .message
}

// signal-cli: {"editMessage": {"targetSentTimestamp": ..., "dataMessage": {...}}}
// (top level for incoming edits, inside syncMessage.sentMessage for edits from our other devices).
// Only the text is taken over; attachments of an edit are not changed by the official clients either.
@discardableResult
private func parseEdit(_ edit: JSON, _ out: inout ParsedEnvelope, envelopeTs: Int64) -> Bool {
    let dm = jsonObject(edit["dataMessage"])
    let target = jsonInt64(edit["targetSentTimestamp"])
    let text = jsonString(dm["message"])
    if dm.isEmpty || target <= 0 || text.isEmpty { return false }
    var ts = jsonInt64(dm["timestamp"])
    if ts <= 0 { ts = envelopeTs }
    out.kind = .edit
    out.targetTs = target
    out.msg.text = text
    out.mentions = parseMentions(dm)
    out.msg.ts = ts > 0 ? ts : nowMs()
    out.groupId = groupIdOf(dm)
    return true
}

public func parseEnvelope(_ env: JSON) -> ParsedEnvelope {
    var out = ParsedEnvelope()
    out.src = jsonString(env["sourceNumber"])
    if out.src.isEmpty { out.src = jsonString(env["sourceUuid"]) }
    if out.src.isEmpty { out.src = jsonString(env["source"]) }
    out.srcName = jsonString(env["sourceName"])
    let envTs = jsonInt64(env["timestamp"])

    let dm = jsonObject(env["dataMessage"])
    if !dm.isEmpty {
        parseBody(dm, &out, envelopeTs: envTs)
        return out
    }

    let edit = jsonObject(env["editMessage"])
    if !edit.isEmpty {
        parseEdit(edit, &out, envelopeTs: envTs)
        return out
    }

    let sent = jsonObject(jsonObject(env["syncMessage"])["sentMessage"])
    if !sent.isEmpty {
        out.outgoing = true
        out.destination = jsonString(sent["destinationNumber"])
        if out.destination.isEmpty { out.destination = jsonString(sent["destination"]) }
        if out.destination.isEmpty { out.destination = jsonString(sent["destinationUuid"]) }
        let sentEdit = jsonObject(sent["editMessage"])
        if !sentEdit.isEmpty {
            parseEdit(sentEdit, &out, envelopeTs: envTs)
        } else {
            parseBody(sent, &out, envelopeTs: envTs)
        }
        out.msg.outgoing = true
    }
    return out
}
