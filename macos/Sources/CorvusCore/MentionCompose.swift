import Foundation

/// A mention typed in the message field: "@" + `name` in the text, where `name` starts at `start`
/// (UTF-16 offsets, like signal-cli and the Qt app) and stands for the group member `id` (number or UUID).
public struct ComposerMention: Equatable, Hashable, Sendable {
    public var start: Int
    public var length: Int
    public var id: String
    public var name: String

    public init(start: Int, length: Int, id: String, name: String) {
        self.start = start
        self.length = length
        self.id = id
        self.name = name
    }

    public var end: Int { start + length }
}

/// Writing @mentions (port of the Qt app's mention suggestions and prepareComposerMessage).
public enum MentionCompose {
    public static let maxQueryLength = 48

    /// The "@query" the caret is in: the last "@" before `cursor` that does not follow a letter, digit or "_",
    /// with no whitespace or second "@" up to the caret. Offsets in UTF-16 units.
    public static func query(in text: String, cursor: Int) -> (start: Int, query: String)? {
        let s = text as NSString
        guard cursor > 0, cursor <= s.length else { return nil }
        let at = s.range(of: "@", options: .backwards, range: NSRange(location: 0, length: cursor)).location
        guard at != NSNotFound else { return nil }
        if at > 0 {
            let before = s.substring(with: s.rangeOfComposedCharacterSequence(at: at - 1))
            if before == "_" || before.unicodeScalars.contains(where: { CharacterSet.alphanumerics.contains($0) }) { return nil }
        }
        let q = s.substring(with: NSRange(location: at + 1, length: cursor - at - 1))
        if q.contains("@") || q.rangeOfCharacter(from: .whitespacesAndNewlines) != nil || (q as NSString).length > maxQueryLength {
            return nil
        }
        return (at, q)
    }

    /// Mentions after the text in `range` was replaced by `replacementLength` UTF-16 units: later ones move,
    /// a mention that the change touches is dropped (it becomes plain text).
    public static func adjust(_ mentions: [ComposerMention], editing range: NSRange, replacementLength: Int) -> [ComposerMention] {
        let delta = replacementLength - range.length
        return mentions.compactMap { m in
            // the "@" in front belongs to the mention too
            let mStart = m.start - 1
            if range.location + range.length <= mStart { // entirely before (also typing right in front of the "@")
                var moved = m
                moved.start += delta
                return moved
            }
            if range.location >= m.end { return m }
            return nil
        }
    }

    /// The mentions that still stand in `text`: "@" + name at their place.
    public static func valid(_ mentions: [ComposerMention], in text: String) -> [ComposerMention] {
        let s = text as NSString
        return mentions.filter { m in
            m.start >= 1 && m.length > 0 && m.end <= s.length && !m.id.isEmpty
                && s.substring(with: NSRange(location: m.start - 1, length: 1)) == "@"
                && s.substring(with: NSRange(location: m.start, length: m.length)) == m.name
        }
    }

    /// What is sent: the text trimmed, with the "@" in front of each mention removed (signal-cli puts the
    /// mention placeholder in place of the name and the apps show "@Name"), and the mentions moved accordingly.
    public static func prepare(_ raw: String, mentions: [ComposerMention]) -> (text: String, mentions: [ComposerMention]) {
        let s = raw as NSString
        var lead = 0, end = s.length
        let ws = CharacterSet.whitespacesAndNewlines
        while lead < end, let u = UnicodeScalar(s.character(at: lead)), ws.contains(u) { lead += 1 }
        while end > lead, let u = UnicodeScalar(s.character(at: end - 1)), ws.contains(u) { end -= 1 }
        if end <= lead { return ("", []) }

        let included = valid(mentions, in: raw).filter { $0.start > lead && $0.end <= end }
        let removeAt = Set(included.map { $0.start - 1 }).sorted()
        let out = NSMutableString(string: s.substring(with: NSRange(location: lead, length: end - lead)))
        for at in removeAt.reversed() { out.deleteCharacters(in: NSRange(location: at - lead, length: 1)) }
        let mapped: [ComposerMention] = included.map { m in
            var n = m
            n.start = m.start - lead - removeAt.filter { $0 < m.start }.count
            return n
        }.filter { $0.start >= 0 && $0.end <= out.length }
        return (out as String, mapped.sorted { $0.start < $1.start })
    }

    /// signal-cli's "mention" parameter: ["start:length:recipient", …]
    public static func cliArguments(_ mentions: [ComposerMention]) -> [String] {
        mentions.filter { $0.start >= 0 && $0.length > 0 && !$0.id.isEmpty }.map { "\($0.start):\($0.length):\($0.id)" }
    }
}
