import Foundation

/// The pure parts of generating link previews (port of the Qt app's helpers in src/MainWindow.cpp):
/// which link a message previews, which URLs may be fetched, and reading title / description / image
/// from a page's HTML.
public enum LinkPreviewRules {
    public static let maxPageBytes = 1024 * 1024
    public static let maxImageBytes = 5 * 1024 * 1024
    public static let maxRedirects = 5

    /// The first https:// link in `text`, in its exact spelling (signal-cli requires previewUrl to appear in
    /// the body). Trailing punctuation and unmatched closing brackets are not part of it.
    public static func firstHttpsLink(_ text: String) -> String {
        let re = try! NSRegularExpression(pattern: #"https://[^\s<>"']+"#, options: [.caseInsensitive])
        guard let m = re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
              let r = Range(m.range, in: text) else { return "" }
        var c = String(text[r])
        while let last = c.last, ".,!?;:".contains(last) { c.removeLast() }
        while c.hasSuffix(")") && c.filter({ $0 == ")" }).count > c.filter({ $0 == "(" }).count { c.removeLast() }
        while c.hasSuffix("]") && c.filter({ $0 == "]" }).count > c.filter({ $0 == "[" }).count { c.removeLast() }
        guard let u = URLComponents(string: c), u.scheme?.lowercased() == "https", !(u.host ?? "").isEmpty else { return "" }
        return c
    }

    /// Only public HTTPS hosts by name: no credentials, no IP literals, no local / internal names.
    public static func isSafeToFetch(_ url: URL?) -> Bool {
        guard let url, let u = URLComponents(url: url, resolvingAgainstBaseURL: true),
              u.scheme?.lowercased() == "https", let host = u.host?.lowercased(), !host.isEmpty,
              u.user == nil, u.password == nil else { return false }
        if host == "localhost" || host.hasSuffix(".localhost") || host.hasSuffix(".local") || host.hasSuffix(".internal")
            || host.hasSuffix(".test") || !host.contains(".") { return false }
        if isIPLiteral(host) { return false } // never fetch an IP literal (local/private or otherwise)
        return true
    }

    static func isIPLiteral(_ host: String) -> Bool {
        let h = host.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        var v4 = in_addr(), v6 = in6_addr()
        return inet_pton(AF_INET, h, &v4) == 1 || inet_pton(AF_INET6, h, &v6) == 1
    }

    /// File suffix for a preview image's content type; "" if the type is not accepted.
    public static func imageSuffix(contentType: String) -> String {
        switch contentType.split(separator: ";").first.map({ $0.trimmingCharacters(in: .whitespaces).lowercased() }) ?? "" {
        case "image/jpeg", "image/jpg": return "jpg"
        case "image/png": return "png"
        case "image/webp": return "webp"
        case "image/gif": return "gif"
        default: return ""
        }
    }

    /// Title, description and image URL of a page: Open Graph / Twitter tags, else <title>.
    public static func parse(html: String, url: String) -> (preview: LinkPreview, imageUrl: String) {
        var lp = LinkPreview(url: url)
        lp.title = meta(html, ["og:title", "twitter:title"])
        if lp.title.isEmpty { lp.title = title(html) }
        lp.title = String(lp.title.prefix(240)).trimmingCharacters(in: .whitespacesAndNewlines)
        lp.description = String(meta(html, ["og:description", "description", "twitter:description"]).prefix(280))
        return (lp, meta(html, ["og:image", "og:image:url", "twitter:image"]))
    }

    static func meta(_ html: String, _ names: [String]) -> String {
        let tagRe = try! NSRegularExpression(pattern: #"<meta\b[^>]*>"#, options: [.caseInsensitive])
        for m in tagRe.matches(in: html, range: NSRange(html.startIndex..., in: html)) {
            guard let r = Range(m.range, in: html) else { continue }
            let tag = String(html[r])
            if names.contains(attribute(tag, "property").lowercased()) || names.contains(attribute(tag, "name").lowercased()) {
                let v = attribute(tag, "content").trimmingCharacters(in: .whitespacesAndNewlines)
                if !v.isEmpty { return String(v.prefix(1000)) }
            }
        }
        return ""
    }

    static func title(_ html: String) -> String {
        let re = try! NSRegularExpression(pattern: #"<title\b[^>]*>(.*?)</title\s*>"#,
                                          options: [.caseInsensitive, .dotMatchesLineSeparators])
        guard let m = re.firstMatch(in: html, range: NSRange(html.startIndex..., in: html)),
              let r = Range(m.range(at: 1), in: html) else { return "" }
        return String(unescape(String(html[r])).prefix(240))
    }

    static func attribute(_ tag: String, _ name: String) -> String {
        let pattern = #"(?:^|\s)"# + NSRegularExpression.escapedPattern(for: name) + #"\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))"#
        let re = try! NSRegularExpression(pattern: pattern, options: [.caseInsensitive])
        guard let m = re.firstMatch(in: tag, range: NSRange(tag.startIndex..., in: tag)) else { return "" }
        for i in 1...3 { if let r = Range(m.range(at: i), in: tag) { return unescape(String(tag[r])) } }
        return ""
    }

    /// HTML entities (named basics and numeric) decoded, tags removed, whitespace collapsed.
    static func unescape(_ s: String) -> String {
        var t = s.replacingOccurrences(of: #"<[^>]*>"#, with: "", options: .regularExpression)
        let named = ["&quot;": "\"", "&apos;": "'", "&#39;": "'", "&lt;": "<", "&gt;": ">", "&nbsp;": " ",
                     "&ndash;": "–", "&mdash;": "—", "&hellip;": "…", "&rsquo;": "’", "&lsquo;": "‘",
                     "&rdquo;": "”", "&ldquo;": "“", "&middot;": "·"]
        for (k, v) in named { t = t.replacingOccurrences(of: k, with: v, options: .caseInsensitive) }
        let num = try! NSRegularExpression(pattern: #"&#(x[0-9a-fA-F]+|[0-9]+);"#)
        for m in num.matches(in: t, range: NSRange(t.startIndex..., in: t)).reversed() {
            guard let r = Range(m.range, in: t), let gr = Range(m.range(at: 1), in: t) else { continue }
            let g = t[gr]
            let code = g.hasPrefix("x") || g.hasPrefix("X") ? UInt32(g.dropFirst(), radix: 16) : UInt32(g)
            if let code, let scalar = Unicode.Scalar(code) { t.replaceSubrange(r, with: String(Character(scalar))) }
        }
        t = t.replacingOccurrences(of: "&amp;", with: "&", options: .caseInsensitive) // last: "&amp;lt;" stays "&lt;"
        return t.split(whereSeparator: \.isWhitespace).joined(separator: " ")
    }
}
