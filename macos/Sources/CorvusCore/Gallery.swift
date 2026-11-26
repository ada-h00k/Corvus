import Foundation

/// The media gallery of a chat (port of the Qt app's MediaGallery): its photos and videos, audio files,
/// links and other files, newest first.
public enum Gallery {
    public enum Tab: String, CaseIterable, Identifiable {
        case media = "Media", audio = "Audio", links = "Links", files = "Files"
        public var id: String { rawValue }
    }

    public struct Item: Identifiable, Equatable {
        public var tab: Tab
        public var messageId: String
        public var messageTs: Int64
        public var attachmentIndex = -1   // -1 for links
        public var attachment = Attachment()
        public var url = ""               // links
        public var title = ""             // link preview title, if any
        public var id: String { "\(messageId)#\(attachmentIndex)#\(url)" }
    }

    private static let detector = try? NSDataDetector(types: NSTextCheckingResult.CheckingType.link.rawValue)

    /// Web links in `text` (http, https), in order, without duplicates.
    public static func links(in text: String) -> [String] {
        guard let detector, !text.isEmpty else { return [] }
        var seen = Set<String>()
        return detector.matches(in: text, range: NSRange(location: 0, length: (text as NSString).length)).compactMap { m in
            guard let u = m.url, ["http", "https"].contains(u.scheme?.lowercased() ?? "") else { return nil }
            let s = u.absoluteString
            return seen.insert(s).inserted ? s : nil
        }
    }

    public static func items(_ msgs: [Message]) -> [Item] {
        var out: [Item] = []
        for m in msgs.reversed() where !m.deleted && !m.viewOnce {
            for (i, a) in m.attachments.enumerated() where !a.isSticker {
                let tab: Tab = a.isImage || a.isVideo ? .media : (a.isAudio ? .audio : .files)
                out.append(Item(tab: tab, messageId: m.id, messageTs: m.ts, attachmentIndex: i, attachment: a))
            }
            var urls = links(in: m.text)
            if m.linkPreview.isValid && !urls.contains(m.linkPreview.url) { urls.insert(m.linkPreview.url, at: 0) }
            for u in urls {
                out.append(Item(tab: .links, messageId: m.id, messageTs: m.ts, url: u,
                                title: m.linkPreview.url == u ? m.linkPreview.title : ""))
            }
        }
        return out
    }
}
