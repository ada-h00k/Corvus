import Foundation

/// Conversion between the in-memory model and the JSON records of the encrypted history store
/// (see history-store/src/store.rs `Record`). The keys must stay identical to the Qt app
/// (src/Records.cpp) so both read the same history file.
public enum Records {
    private static func attachmentToJson(_ a: Attachment) -> JSON {
        var o: JSON = [:]
        if !a.id.isEmpty { o["id"] = a.id }
        if !a.contentType.isEmpty { o["ct"] = a.contentType }
        if !a.filename.isEmpty { o["fn"] = a.filename }
        if !a.path.isEmpty { o["p"] = a.path }
        if a.size > 0 { o["sz"] = a.size }
        if a.width > 0 { o["w"] = a.width }
        if a.height > 0 { o["h"] = a.height }
        if !a.thumb.isEmpty { o["th"] = a.thumb.base64EncodedString() }
        return o
    }

    private static func attachmentFromJson(_ o: JSON) -> Attachment {
        var a = Attachment()
        a.id = jsonString(o["id"])
        a.contentType = jsonString(o["ct"])
        a.filename = AttachmentPolicy.stripBidiControls(jsonString(o["fn"])) // also for names stored by older versions
        a.path = jsonString(o["p"])
        a.size = jsonInt64(o["sz"])
        a.width = jsonInt(o["w"])
        a.height = jsonInt(o["h"])
        a.thumb = Data(base64Encoded: jsonString(o["th"])) ?? Data()
        return a
    }

    // Link preview ("lp"): same keys as the Qt app (src/Records.cpp).
    private static func linkPreviewToJson(_ lp: LinkPreview) -> JSON {
        var o: JSON = [:]
        if !lp.url.isEmpty { o["u"] = lp.url }
        if !lp.title.isEmpty { o["t"] = lp.title }
        if !lp.description.isEmpty { o["d"] = lp.description }
        if !lp.imageId.isEmpty { o["id"] = lp.imageId }
        if !lp.imageContentType.isEmpty { o["ct"] = lp.imageContentType }
        if !lp.imageFilename.isEmpty { o["fn"] = lp.imageFilename }
        if lp.imageSize > 0 { o["sz"] = lp.imageSize }
        if !lp.imageThumb.isEmpty { o["th"] = lp.imageThumb.base64EncodedString() }
        return o
    }

    private static func linkPreviewFromJson(_ o: JSON) -> LinkPreview {
        var lp = LinkPreview(url: jsonString(o["u"]), title: jsonString(o["t"]), description: jsonString(o["d"]))
        lp.imageId = jsonString(o["id"])
        lp.imageContentType = jsonString(o["ct"])
        lp.imageFilename = jsonString(o["fn"])
        lp.imageSize = jsonInt64(o["sz"])
        lp.imageThumb = Data(base64Encoded: jsonString(o["th"])) ?? Data()
        return lp.isValid ? lp : LinkPreview()
    }

    /// One record carries the thumbnails of all of a message's attachments. Like the Qt app, the
    /// thumbnails are capped per record well below the store's 16 MiB limit: later attachments lose theirs.
    static let maxThumbBase64PerRecord = 8 * 1024 * 1024

    private static func attachmentsToJson(_ list: [Attachment]) -> [JSON] {
        var thumbBytes = 0
        return list.map { a in
            var o = attachmentToJson(a)
            if let th = o["th"] as? String {
                thumbBytes += th.utf8.count
                if thumbBytes > maxThumbBase64PerRecord { o.removeValue(forKey: "th") }
            }
            return o
        }
    }

    // Text styles ("st"): "start:length:STYLE" strings.
    public static func textStylesToJson(_ styles: [TextStyleRange]) -> [String] {
        styles.filter { $0.start >= 0 && $0.length > 0 && !$0.style.isEmpty }.map { "\($0.start):\($0.length):\($0.style)" }
    }

    public static func textStylesFromJson(_ v: Any?) -> [TextStyleRange] {
        jsonArray(v).compactMap { item in
            let p = jsonString(item).split(separator: ":", omittingEmptySubsequences: false)
            guard p.count == 3, let start = Int(p[0]), let length = Int(p[1]), start >= 0, length > 0, !p[2].isEmpty
            else { return nil }
            return TextStyleRange(start: start, length: length, style: String(p[2]))
        }
    }

    // Mentions ("mn"): s = start, l = length, u = uuid.
    static func mentionsToJson(_ mentions: [MentionRange]) -> [JSON] {
        mentions.filter { $0.start >= 0 && $0.length > 0 }.map { r in
            var o: JSON = ["s": r.start, "l": r.length]
            if !r.uuid.isEmpty { o["u"] = r.uuid }
            return o
        }
    }

    static func mentionsFromJson(_ v: Any?) -> [MentionRange] {
        jsonArray(v).compactMap { item in
            let o = jsonObject(item)
            let start = o["s"] == nil ? -1 : jsonInt(o["s"])
            let length = jsonInt(o["l"])
            guard start >= 0, length > 0 else { return nil }
            return MentionRange(start: start, length: length, uuid: jsonString(o["u"]))
        }
    }

    // Per-recipient receipts ("rr"): key -> {s: status, d: delivered at, r: read at}.
    static func recipientReceiptsToJson(_ rr: [String: RecipientReceipt]) -> JSON {
        var out: JSON = [:]
        for (k, r) in rr { out[k] = ["s": min(max(r.status, 0), 2), "d": r.deliveredAtMs, "r": r.readAtMs] as JSON }
        return out
    }

    static func recipientReceiptsFromJson(_ v: Any?) -> [String: RecipientReceipt] {
        var out: [String: RecipientReceipt] = [:]
        for (k, value) in jsonObject(v) where !k.isEmpty {
            guard let o = value as? JSON else { continue }
            out[k] = RecipientReceipt(status: min(max(jsonInt(o["s"]), 0), 2),
                                      deliveredAtMs: jsonInt64(o["d"]), readAtMs: jsonInt64(o["r"]))
        }
        return out
    }

    // Poll definition ("pl"): q = question, o = options, m = multiple choice allowed.
    static func pollToJson(_ poll: PollData) -> JSON {
        ["q": poll.question, "m": poll.allowMultiple, "o": poll.options]
    }

    static func pollFromJson(_ v: Any?) -> PollData {
        let o = jsonObject(v)
        var poll = PollData()
        poll.question = jsonString(o["q"])
        poll.allowMultiple = jsonBool(o["m"])
        poll.options = jsonArray(o["o"]).map { jsonString($0) }
        return poll
    }

    // Contact card ("cc"): n = name, num = number, u = uuid, av = avatar attachment id, avb = avatar bytes.
    static func contactToJson(_ c: ContactCard) -> JSON {
        var o: JSON = ["n": c.name, "num": c.number]
        if !c.uuid.isEmpty { o["u"] = c.uuid }
        if !c.avatarId.isEmpty { o["av"] = c.avatarId }
        if !c.avatar.isEmpty { o["avb"] = c.avatar }
        return o
    }

    static func contactFromJson(_ v: Any?) -> ContactCard {
        let o = jsonObject(v)
        var c = ContactCard(name: jsonString(o["n"]), number: jsonString(o["num"]), uuid: jsonString(o["u"]))
        c.avatarId = jsonString(o["av"])
        c.avatar = jsonString(o["avb"])
        return c
    }

    // Edit state: x = current text, et = timestamp of the latest revision, ec = number of edits,
    // st / mn = styles and mentions of the current text.
    private static func editToJson(_ m: Message) -> JSON {
        var e: JSON = ["x": m.text, "et": m.editTs, "ec": m.editCount]
        if !m.textStyles.isEmpty { e["st"] = textStylesToJson(m.textStyles) }
        if !m.mentions.isEmpty { e["mn"] = mentionsToJson(m.mentions) }
        return e
    }

    public static func message(_ conv: String, _ m: Message) -> JSON {
        var d: JSON = [:]
        if m.orderTs > 0 { d["ot"] = m.orderTs }
        if m.deleted { d["dl"] = true } // placeholder only: no quote, attachments or edit state
        if m.expiresInSeconds > 0 {
            d["ex"] = m.expiresInSeconds
            d["es"] = m.expirationStartMs
        }
        if m.quote.valid && !m.deleted {
            d["q"] = ["id": m.quote.id, "a": m.quote.authorKey, "t": m.quote.text, "n": m.quote.attachmentNote] as JSON
        }
        if m.hasPoll && !m.deleted {
            d["pl"] = pollToJson(m.poll)
            if m.poll.terminated { d["pt"] = true }
        }
        if m.hasContact && !m.deleted { d["cc"] = contactToJson(m.contact) }
        if m.viewOnce {
            d["vo"] = true
            if m.viewOnceViewed { d["vv"] = true }
            if m.viewOnceExpired { d["ve"] = true }
        }
        // Never keep sent or consumed View Once media in the history. For an incoming unopened
        // item only the opaque attachment reference is stored (no thumbnail, no local path).
        let keepViewOnceRef = !m.viewOnce || (!m.outgoing && !m.viewOnceViewed && !m.viewOnceExpired)
        if !m.attachments.isEmpty && !m.deleted && keepViewOnceRef {
            var safe = m.attachments
            if m.viewOnce { for i in safe.indices { safe[i].thumb = Data(); safe[i].path = "" } }
            d["at"] = attachmentsToJson(safe)
        }
        if !m.textStyles.isEmpty && !m.deleted { d["st"] = textStylesToJson(m.textStyles) }
        if !m.mentions.isEmpty && !m.deleted { d["mn"] = mentionsToJson(m.mentions) }
        var rec: JSON = ["t": "m", "c": conv, "ts": m.ts, "s": m.sender, "x": m.deleted ? "" : m.text,
                         "o": m.outgoing, "au": m.authorKey]
        if m.editCount > 0 && !m.deleted { d["ed"] = editToJson(m) }
        if m.linkPreview.isValid && !m.deleted { d["lp"] = linkPreviewToJson(m.linkPreview) }
        if m.outgoing && m.receipt > .sent { d["rs"] = m.receipt.rawValue }
        if m.outgoing && !m.recipientReceipts.isEmpty { d["rr"] = recipientReceiptsToJson(m.recipientReceipts) }
        if !d.isEmpty { rec["d"] = d }
        return rec
    }

    /// Text edit of a message: an "update" record with the extra "ed"; ts = original timestamp.
    /// The empty "lp" drops the link preview: it belonged to the old text (as in the Qt app).
    public static func edit(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey, "d": ["ed": editToJson(m), "lp": JSON()] as JSON]
    }

    /// A late link-preview image (thumbnail fetched after the message arrived).
    public static func linkPreviewUpdate(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey, "d": ["lp": linkPreviewToJson(m.linkPreview)] as JSON]
    }

    /// Delete for everyone: an "update" record with the extra "dl"; ts = original timestamp.
    public static func deletion(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey, "d": ["dl": true] as JSON]
    }

    public static func expirationUpdate(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey,
         "d": ["ex": m.expiresInSeconds, "es": m.expirationStartMs] as JSON]
    }

    /// Delivery state of an outgoing message ("rs": 1 = delivered, 2 = read; "rr": per recipient).
    public static func receiptUpdate(_ conv: String, _ m: Message) -> JSON {
        var d: JSON = ["rs": m.receipt.rawValue]
        if !m.recipientReceipts.isEmpty { d["rr"] = recipientReceiptsToJson(m.recipientReceipts) }
        return ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey, "d": d]
    }

    public static func attachmentsUpdate(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey, "d": ["at": attachmentsToJson(m.attachments)] as JSON]
    }

    /// A View Once item was opened or expired: the attachment reference is dropped for good.
    public static func viewOnceUpdate(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey,
         "d": ["vv": m.viewOnceViewed, "ve": m.viewOnceExpired, "at": [Any]()] as JSON]
    }

    public static func pollTerminate(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey, "d": ["pt": true] as JSON]
    }

    /// A poll vote ("oi" empty = the vote was taken back).
    public static func pollVote(_ conv: String, ts: Int64, author: String, by: String, vote: PollVote) -> JSON {
        ["t": "pv", "c": conv, "ts": ts, "au": author, "by": by, "oi": vote.optionIndexes,
         "vc": vote.voteCount, "vt": vote.voteTs]
    }

    /// A pin ("rm" = the pin was taken back).
    public static func pin(_ conv: String, _ p: PinInfo, removed: Bool = false) -> JSON {
        var rec: JSON = ["t": "pi", "c": conv, "ts": p.ts, "au": p.authorKey, "by": p.by,
                         "at": p.pinnedAtMs, "d": p.durationSeconds]
        if removed { rec["rm"] = true }
        return rec
    }

    public static func blocked(_ conv: String, _ blocked: Bool) -> JSON { ["t": "b", "c": conv, "v": blocked] }

    /// A declined message request (no conversation is created for it any more).
    public static func requestDeclined(_ conv: String) -> JSON { ["t": "j", "c": conv] }

    /// Extras ("d": quote, attachments, edit state, expiry, deletion) as stored in message / update records.
    public static func applyExtras(_ m: inout Message, _ d: JSON) {
        if d["ot"] != nil { m.orderTs = jsonInt64(d["ot"]) }
        if let q = d["q"] as? JSON {
            m.quote.valid = true
            m.quote.id = jsonInt64(q["id"])
            m.quote.authorKey = jsonString(q["a"])
            m.quote.text = jsonString(q["t"])
            m.quote.attachmentNote = jsonString(q["n"])
        }
        if d["rr"] != nil && m.outgoing { m.recipientReceipts = recipientReceiptsFromJson(d["rr"]) }
        if let e = d["ed"] as? JSON {
            if e["x"] != nil { m.text = jsonString(e["x"]) }
            if e["st"] != nil { m.textStyles = textStylesFromJson(e["st"]) }
            if e["mn"] != nil { m.mentions = mentionsFromJson(e["mn"]) }
            m.editTs = jsonInt64(e["et"])
            m.editCount = jsonInt(e["ec"])
        }
        if d["ex"] != nil {
            m.expiresInSeconds = jsonInt(d["ex"])
            m.expirationStartMs = jsonInt64(d["es"])
        }
        if let lp = d["lp"] as? JSON { m.linkPreview = linkPreviewFromJson(lp) }
        if d["pl"] != nil {
            let votes = m.poll.votes
            m.hasPoll = true
            m.poll = pollFromJson(d["pl"])
            m.poll.votes = votes
        }
        if d["pt"] != nil { m.poll.terminated = jsonBool(d["pt"]) }
        if d["cc"] != nil {
            m.hasContact = true
            m.contact = contactFromJson(d["cc"])
        }
        if d["vo"] != nil { m.viewOnce = jsonBool(d["vo"]) }
        if d["vv"] != nil { m.viewOnceViewed = jsonBool(d["vv"]) }
        if d["ve"] != nil { m.viewOnceExpired = jsonBool(d["ve"]) }
        if d["st"] != nil { m.textStyles = textStylesFromJson(d["st"]) }
        if d["mn"] != nil { m.mentions = mentionsFromJson(d["mn"]) }
        if d["at"] != nil {
            m.attachments = jsonArray(d["at"]).map { attachmentFromJson(jsonObject($0)) }
        }
        if m.viewOnce && (m.viewOnceViewed || m.viewOnceExpired) { m.attachments = [] }
        if d["rs"] != nil, m.outgoing, let r = ReceiptStatus(rawValue: jsonInt(d["rs"])), r > m.receipt {
            m.receipt = r
        }
        if jsonBool(d["dl"]) { // last: wins over anything else in the same record
            m.markDeleted()
        }
    }

    public static func messageFromRecord(_ rec: JSON) -> Message {
        var m = Message()
        m.ts = jsonInt64(rec["ts"])
        m.sender = jsonString(rec["s"])
        m.text = jsonString(rec["x"])
        m.outgoing = jsonBool(rec["o"])
        m.authorKey = jsonString(rec["au"])
        applyExtras(&m, jsonObject(rec["d"]))
        return m
    }

    public static func reaction(_ conv: String, ts: Int64, author: String, by: String, emoji: String) -> JSON {
        ["t": "r", "c": conv, "ts": ts, "au": author, "by": by, "e": emoji]
    }

    public static func archive(_ conv: String, archived: Bool, sigArchived: Bool) -> JSON {
        ["t": "a", "c": conv, "v": archived, "s": sigArchived]
    }

    public static func mute(_ conv: String, _ muted: Bool) -> JSON { ["t": "k", "c": conv, "v": muted] }
    public static func expiration(_ conv: String, _ seconds: Int) -> JSON { ["t": "e", "c": conv, "v": seconds] }
    public static func name(_ conv: String, _ n: String) -> JSON { ["t": "n", "c": conv, "n": n] }
    public static func nickname(_ conv: String, _ n: String) -> JSON { ["t": "p", "c": conv, "n": n] }
}
