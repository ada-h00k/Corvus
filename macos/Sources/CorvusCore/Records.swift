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
        a.filename = jsonString(o["fn"])
        a.path = jsonString(o["p"])
        a.size = jsonInt64(o["sz"])
        a.width = jsonInt(o["w"])
        a.height = jsonInt(o["h"])
        a.thumb = Data(base64Encoded: jsonString(o["th"])) ?? Data()
        return a
    }

    // Edit state: x = current text, et = timestamp of the latest revision, ec = number of edits.
    private static func editToJson(_ m: Message) -> JSON {
        ["x": m.text, "et": m.editTs, "ec": m.editCount]
    }

    public static func message(_ conv: String, _ m: Message) -> JSON {
        var d: JSON = [:]
        if m.deleted { d["dl"] = true } // placeholder only: no quote, attachments or edit state
        if m.expiresInSeconds > 0 {
            d["ex"] = m.expiresInSeconds
            d["es"] = m.expirationStartMs
        }
        if m.quote.valid && !m.deleted {
            d["q"] = ["id": m.quote.id, "a": m.quote.authorKey, "t": m.quote.text, "n": m.quote.attachmentNote] as JSON
        }
        if !m.attachments.isEmpty && !m.deleted { d["at"] = m.attachments.map(attachmentToJson) }
        var rec: JSON = ["t": "m", "c": conv, "ts": m.ts, "s": m.sender, "x": m.deleted ? "" : m.text,
                         "o": m.outgoing, "au": m.authorKey]
        if m.editCount > 0 && !m.deleted { d["ed"] = editToJson(m) }
        if !d.isEmpty { rec["d"] = d }
        return rec
    }

    /// Text edit of a message: an "update" record with the extra "ed"; ts = original timestamp.
    public static func edit(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey, "d": ["ed": editToJson(m)] as JSON]
    }

    /// Delete for everyone: an "update" record with the extra "dl"; ts = original timestamp.
    public static func deletion(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey, "d": ["dl": true] as JSON]
    }

    public static func expirationUpdate(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey,
         "d": ["ex": m.expiresInSeconds, "es": m.expirationStartMs] as JSON]
    }

    public static func attachmentsUpdate(_ conv: String, _ m: Message) -> JSON {
        ["t": "u", "c": conv, "ts": m.ts, "au": m.authorKey, "d": ["at": m.attachments.map(attachmentToJson)] as JSON]
    }

    /// Extras ("d": quote, attachments, edit state, expiry, deletion) as stored in message / update records.
    public static func applyExtras(_ m: inout Message, _ d: JSON) {
        if let q = d["q"] as? JSON {
            m.quote.valid = true
            m.quote.id = jsonInt64(q["id"])
            m.quote.authorKey = jsonString(q["a"])
            m.quote.text = jsonString(q["t"])
            m.quote.attachmentNote = jsonString(q["n"])
        }
        if let e = d["ed"] as? JSON {
            if e["x"] != nil { m.text = jsonString(e["x"]) }
            m.editTs = jsonInt64(e["et"])
            m.editCount = jsonInt(e["ec"])
        }
        if d["ex"] != nil {
            m.expiresInSeconds = jsonInt(d["ex"])
            m.expirationStartMs = jsonInt64(d["es"])
        }
        if d["at"] != nil {
            m.attachments = jsonArray(d["at"]).map { attachmentFromJson(jsonObject($0)) }
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
