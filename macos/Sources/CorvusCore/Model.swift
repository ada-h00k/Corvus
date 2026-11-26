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

    /// Signal stickers are kept as an attachment named "signal-sticker:<packId>:<stickerId>.webp"
    /// (same convention as the Qt app). corvus-cli 1.6 stores the image of a received sticker
    /// under the attachment id it reports in the envelope (attachmentId) and serves it through
    /// "getAttachment"; "getSticker" serves installed packs only.
    public static let stickerPrefix = "signal-sticker:"

    public static func sticker(packId: String, stickerId: Int, attachmentId: String = "") -> Attachment {
        var a = Attachment()
        a.contentType = "image/webp"
        a.filename = "\(stickerPrefix)\(packId):\(stickerId).webp"
        a.id = attachmentId
        return a
    }

    public var isSticker: Bool { filename.hasPrefix(Attachment.stickerPrefix) }

    public var stickerPackId: String {
        guard isSticker else { return "" }
        return String(filename.dropFirst(Attachment.stickerPrefix.count).split(separator: ":", omittingEmptySubsequences: false).first ?? "")
    }

    /// -1 if not a (valid) sticker.
    public var stickerId: Int {
        guard isSticker else { return -1 }
        let parts = filename.dropFirst(Attachment.stickerPrefix.count).split(separator: ":", omittingEmptySubsequences: false)
        guard parts.count >= 2, let id = Int(parts[1].split(separator: ".").first ?? ""), id >= 0 else { return -1 }
        return id
    }

    /// A sticker that can be loaded or sent by reference.
    public var isValidSticker: Bool { isSticker && !stickerPackId.isEmpty && stickerId >= 0 }

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

/// A text style range from Signal's bodyRanges: BOLD, ITALIC, STRIKETHROUGH, MONOSPACE or SPOILER.
/// Offsets are UTF-16 units, as signal-cli reports them.
public struct TextStyleRange: Equatable, Hashable {
    public var start = 0
    public var length = 0
    public var style = ""

    public init(start: Int = 0, length: Int = 0, style: String = "") {
        self.start = start
        self.length = length
        self.style = style
    }

    public static let styles = ["BOLD", "ITALIC", "STRIKETHROUGH", "MONOSPACE", "SPOILER"]
}

/// A stored @mention (Qt record "mn"): the U+FFFC placeholder at `start` in the raw text names `uuid`.
public struct MentionRange: Equatable, Hashable {
    public var start = 0
    public var length = 0
    public var uuid = ""

    public init(start: Int = 0, length: Int = 0, uuid: String = "") {
        self.start = start
        self.length = length
        self.uuid = uuid
    }
}

/// Delivery state of an outgoing message for one recipient (status 0 sent, 1 delivered, 2 read).
public struct RecipientReceipt: Equatable, Hashable {
    public var status = 0
    public var deliveredAtMs: Int64 = 0
    public var readAtMs: Int64 = 0

    public init(status: Int = 0, deliveredAtMs: Int64 = 0, readAtMs: Int64 = 0) {
        self.status = status
        self.deliveredAtMs = deliveredAtMs
        self.readAtMs = readAtMs
    }
}

/// One vote on a poll: the vote with the highest voteCount wins (on a tie the later voteTs);
/// an empty optionIndexes list takes the vote back.
public struct PollVote: Equatable, Hashable {
    public var optionIndexes: [Int] = []
    public var voteCount: Int64 = 0
    public var voteTs: Int64 = 0

    public init(optionIndexes: [Int] = [], voteCount: Int64 = 0, voteTs: Int64 = 0) {
        self.optionIndexes = optionIndexes
        self.voteCount = voteCount
        self.voteTs = voteTs
    }

    /// Whether `self` replaces `old` (same rule as the Qt app and the official clients).
    public func wins(over old: PollVote?) -> Bool {
        guard let old else { return true }
        return voteCount > old.voteCount || (voteCount == old.voteCount && voteTs >= old.voteTs)
    }
}

/// A Signal poll: question with 2...10 options, single or multiple choice.
public struct PollData: Equatable {
    public var question = ""
    public var options: [String] = []
    public var allowMultiple = false
    public var terminated = false
    public var votes: [String: PollVote] = [:] // voter key -> current vote

    public init() {}

    public func voteOf(_ voter: String) -> [Int] { votes[voter]?.optionIndexes ?? [] }

    public var optionCounts: [Int] {
        var counts = Array(repeating: 0, count: options.count)
        for v in votes.values { for i in v.optionIndexes where i >= 0 && i < counts.count { counts[i] += 1 } }
        return counts
    }

    public var topCount: Int { optionCounts.max() ?? 0 }

    /// Applies a vote; returns false if an older one was ignored.
    @discardableResult
    public mutating func apply(_ vote: PollVote, by voter: String) -> Bool {
        guard vote.wins(over: votes[voter]) else { return false }
        let valid = vote.optionIndexes.filter { $0 >= 0 && $0 < options.count }
        if valid.isEmpty { votes.removeValue(forKey: voter) }
        else { votes[voter] = PollVote(optionIndexes: valid, voteCount: vote.voteCount, voteTs: vote.voteTs) }
        return true
    }
}

/// A shared contact card (Qt record "cc").
public struct ContactCard: Equatable {
    public var name = ""
    public var number = ""
    public var uuid = ""
    public var avatarId = ""   // attachment id of the avatar image (persisted)
    public var avatar = ""     // avatar bytes, base64 (runtime cache or own-card preview)

    public init(name: String = "", number: String = "", uuid: String = "") {
        self.name = name
        self.number = number
        self.uuid = uuid
    }
}

/// A pinned message: at most three per chat, oldest first; a pin with a duration expires locally.
public struct PinInfo: Equatable {
    public var ts: Int64 = 0             // target timestamp of the pinned message
    public var authorKey = ""            // author of the pinned message
    public var by = ""                   // who pinned it
    public var pinnedAtMs: Int64 = 0     // local clock, for expiry
    public var durationSeconds: Int64 = -1 // -1 = forever

    public init(ts: Int64 = 0, authorKey: String = "", by: String = "", pinnedAtMs: Int64 = 0, durationSeconds: Int64 = -1) {
        self.ts = ts
        self.authorKey = authorKey
        self.by = by
        self.pinnedAtMs = pinnedAtMs
        self.durationSeconds = durationSeconds
    }

    public static let maxPerChat = 3

    public func isExpired(now: Int64) -> Bool {
        durationSeconds >= 0 && pinnedAtMs > 0 && now - pinnedAtMs >= durationSeconds * 1000
    }
}

/// Delivery state of an outgoing message (the best state any recipient has reached).
public enum ReceiptStatus: Int, Comparable {
    case sent = 0, delivered = 1, read = 2
    public static func < (a: ReceiptStatus, b: ReceiptStatus) -> Bool { a.rawValue < b.rawValue }
}

/// A link preview (Signal "previews"): only HTTPS links, and only with a title.
public struct LinkPreview: Equatable, Hashable, Sendable {
    public var url = ""              // HTTPS URL represented by the preview (exact spelling from the message body)
    public var title = ""
    public var description = ""
    public var imageId = ""          // signal-cli attachment id of an incoming preview image
    public var imageContentType = ""
    public var imageFilename = ""
    public var imageSize: Int64 = 0
    public var imagePath = ""        // transient local file used when sending (not stored)
    public var imageThumb = Data()   // small image kept in the encrypted history

    public init(url: String = "", title: String = "", description: String = "") {
        self.url = url
        self.title = title
        self.description = description
    }

    public var isValid: Bool {
        guard let u = URLComponents(string: url), u.scheme?.lowercased() == "https", !(u.host ?? "").isEmpty else { return false }
        return !title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    public var host: String { URLComponents(string: url)?.host ?? url }
}

public struct Message: Equatable, Identifiable {
    public var ts: Int64 = 0          // ms since epoch: Signal message identity and displayed time
    // Local timeline order is separate from Signal's sender timestamp (`ts`). A sender's
    // device clock can be wrong; the message must still appear where it was received.
    // Zero means a legacy history record without an explicit order value.
    public var orderTs: Int64 = 0
    public var sender = ""            // display name at the time of receiving
    public var text = ""
    public var outgoing = false
    public var authorKey = ""         // number/UUID of the author (own number for outgoing)
    public var quote = Quote()
    public var attachments: [Attachment] = []
    public var reactions: [String: String] = [:] // reactor key -> emoji

    // Editing: `ts` stays the timestamp of the original message and remains its protocol
    // identity. An edit arrives as a new timestamp, remembered in editTs; peers refer to
    // the message by that latest timestamp.
    public var editTs: Int64 = 0
    public var editCount = 0
    public var expiresInSeconds = 0
    public var expirationStartMs: Int64 = 0
    public var deleted = false        // deleted for everyone: only a placeholder is left
    public var receipt = ReceiptStatus.sent // outgoing only: delivered / read by a recipient
    public var linkPreview = LinkPreview()
    public var textStyles: [TextStyleRange] = []
    public var mentions: [MentionRange] = []   // stored mention placeholders (messages written by the Qt app)
    public var recipientReceipts: [String: RecipientReceipt] = [:] // outgoing only: per recipient
    // Signal View Once media: the attachment reference is kept only until it is opened.
    public var viewOnce = false
    public var viewOnceViewed = false
    public var viewOnceExpired = false
    public var hasPoll = false
    public var poll = PollData()
    public var hasContact = false
    public var contact = ContactCard()

    public init() {}

    public var id: String { "\(ts)|\(authorKey)|\(outgoing)" }
    public var time: Date { Date(timeIntervalSince1970: TimeInterval(ts) / 1000) }
    public var sortTs: Int64 { orderTs > 0 ? orderTs : ts }
    public var latestTs: Int64 { editTs > 0 ? editTs : ts }
    public var isEdited: Bool { editCount > 0 }

    // Same limits as the official clients: own text messages, 3 hours after sending, 10 edits.
    public static let editWindowMs: Int64 = 3 * 60 * 60 * 1000
    public static let maxEdits = 10

    /// Turns the message into the "This message was deleted." placeholder, dropping all content.
    public mutating func markDeleted() {
        deleted = true
        text = ""
        attachments = []
        quote = Quote()
        linkPreview = LinkPreview()
        reactions = [:]
        editTs = 0
        editCount = 0
        textStyles = []
        mentions = []
        hasPoll = false
        poll = PollData()
        hasContact = false
        contact = ContactCard()
        viewOnce = false
        viewOnceViewed = false
        viewOnceExpired = false
    }

    /// Like the official clients a View Once message is only the photo: no caption, quote or preview.
    /// Our own sent item cannot be reopened here, so its attachment reference is dropped too.
    public mutating func sanitizeViewOnce() {
        guard viewOnce else { return }
        text = ""
        textStyles = []
        mentions = []
        quote = Quote()
        linkPreview = LinkPreview()
        if outgoing { attachments = [] }
    }

    /// An unopened View Once item older than this is gone (as in the Qt app and the official clients).
    public static let viewOnceLifetimeMs: Int64 = 45 * 24 * 60 * 60 * 1000

    public func canEdit(nowMs: Int64) -> Bool {
        outgoing && !deleted && !text.isEmpty && editCount < Message.maxEdits && nowMs - ts <= Message.editWindowMs
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
    public var blocked = false        // corvus-cli "block" state (synced to the phone)
    public var pins: [PinInfo] = []   // oldest first, at most PinInfo.maxPerChat
    public var expirationSeconds = 0  // Signal disappearing-message timer; 0 = disabled

    public init(key: String = "", title: String = "", isGroup: Bool = false) {
        self.key = key
        self.title = title
        self.isGroup = isGroup
    }

    public var id: String { key }
    public var lastTs: Int64 { msgs.last?.sortTs ?? 0 }

    /// Inserts `m` keeping the list sorted by local timeline order. Returns the index.
    @discardableResult
    public mutating func insertSorted(_ m: Message) -> Int {
        var at = msgs.count
        while at > 0 && msgs[at - 1].sortTs > m.sortTs { at -= 1 }
        msgs.insert(m, at: at)
        return at
    }
}
