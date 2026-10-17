import Foundation

public typealias JSON = [String: Any]

/// Lenient number conversions for JSON values (signal-cli sends numbers, older records may hold strings).
public func jsonInt64(_ v: Any?) -> Int64 {
    switch v {
    case let n as NSNumber: return n.int64Value
    case let s as String: return Int64(s) ?? 0
    default: return 0
    }
}

public func jsonInt(_ v: Any?) -> Int { Int(clamping: jsonInt64(v)) }
public func jsonBool(_ v: Any?) -> Bool { (v as? NSNumber)?.boolValue ?? false }
public func jsonString(_ v: Any?) -> String { v as? String ?? "" }
public func jsonObject(_ v: Any?) -> JSON { v as? JSON ?? [:] }
public func jsonArray(_ v: Any?) -> [Any] { v as? [Any] ?? [] }

public func nowMs() -> Int64 { Int64((Date().timeIntervalSince1970 * 1000).rounded()) }

public struct Attachment: Equatable, Hashable {
    public var id = ""          // signal-cli attachment id (for getAttachment)
    public var contentType = ""
    public var filename = ""
    public var path = ""        // local source file (outgoing attachments only)
    public var size: Int64 = 0
    public var width = 0
    public var height = 0
    public var thumb = Data()   // JPEG/PNG preview (<= 480 px), kept in the encrypted history

    public init() {}

    private var suffix: String {
        guard let dot = filename.lastIndex(of: ".") else { return filename.lowercased() }
        return String(filename[filename.index(after: dot)...]).lowercased()
    }

    public var isImage: Bool { contentType.hasPrefix("image/") }

    public var isAudio: Bool {
        if contentType.hasPrefix("audio/") { return true }
        return ["aac", "flac", "m4a", "mp3", "oga", "ogg", "opus", "wav", "weba", "webm"].contains(suffix)
    }

    public var isVideo: Bool {
        if contentType.lowercased().hasPrefix("video/") { return true }
        return ["3gp", "avi", "m4v", "mkv", "mov", "mp4", "mpeg", "mpg", "ogv", "webm"].contains(suffix)
    }

    public var isGif: Bool {
        contentType.caseInsensitiveCompare("image/gif") == .orderedSame || filename.lowercased().hasSuffix(".gif")
    }
}

public struct Quote: Equatable, Hashable {
    public var valid = false
    public var id: Int64 = 0         // timestamp of the quoted message
    public var authorKey = ""        // number or UUID of the quoted author
    public var text = ""
    public var attachmentNote = ""   // e.g. "image/png"

    public init() {}
}

public struct Message: Equatable, Identifiable {
    public var ts: Int64 = 0          // ms since epoch: identity in the history and place in the list
    public var sender = ""            // display name at the time of receiving
    public var text = ""
    public var outgoing = false
    public var authorKey = ""         // number/UUID of the author (own number for outgoing)
    public var quote = Quote()
    public var attachments: [Attachment] = []
    public var reactions: [String: String] = [:] // reactor key -> emoji

    // Editing: `ts` stays the timestamp of the original message. An edit arrives as a new
    // message timestamp, remembered in editTs; peers refer to the message by that latest timestamp.
    public var editTs: Int64 = 0
    public var editCount = 0
    public var expiresInSeconds = 0
    public var expirationStartMs: Int64 = 0

    public init() {}

    public var id: String { "\(ts)|\(authorKey)|\(outgoing)" }
    public var time: Date { Date(timeIntervalSince1970: TimeInterval(ts) / 1000) }
    public var latestTs: Int64 { editTs > 0 ? editTs : ts }
    public var isEdited: Bool { editCount > 0 }

    // Same limits as the official clients: own text messages, 3 hours after sending, 10 edits.
    public static let editWindowMs: Int64 = 3 * 60 * 60 * 1000
    public static let maxEdits = 10

    public func canEdit(nowMs: Int64) -> Bool {
        outgoing && !text.isEmpty && editCount < Message.maxEdits && nowMs - ts <= Message.editWindowMs
    }
}

public struct Conversation: Identifiable {
    public var key = ""               // number/UUID or "g:<groupId>"
    public var title = ""
    public var isGroup = false
    public var msgs: [Message] = []
    public var unread = 0
    public var storedName = ""        // display name learnt from an incoming message (persisted)
    public var localNickname = ""     // local-only contact nickname (persisted)
    public var archived = false       // shown in the "Archived chats" folder
    public var sigArchived = false    // archive state last reported by signal-cli
    public var muted = false          // local "mute notifications"
    public var expirationSeconds = 0  // Signal disappearing-message timer; 0 = disabled

    public init(key: String = "", title: String = "", isGroup: Bool = false) {
        self.key = key
        self.title = title
        self.isGroup = isGroup
    }

    public var id: String { key }
    public var lastTs: Int64 { msgs.last?.ts ?? 0 }

    /// Inserts `m` keeping the list sorted by time (after messages with the same time). Returns the index.
    @discardableResult
    public mutating func insertSorted(_ m: Message) -> Int {
        var at = msgs.count
        while at > 0 && msgs[at - 1].ts > m.ts { at -= 1 }
        msgs.insert(m, at: at)
        return at
    }
}
