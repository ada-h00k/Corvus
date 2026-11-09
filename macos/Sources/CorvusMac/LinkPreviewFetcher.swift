import AppKit
import CorvusCore
import Foundation

/// Fetches the page behind a link and builds a link preview (title, description, image), like the Qt app:
/// HTTPS only, no IP literals or local names, at most 5 redirects (each checked again), pages up to 1 MB,
/// images up to 5 MB, 10 s timeouts, no cookies or cache.
enum LinkPreviewFetcher {
    private static let userAgent = "Corvus (link preview)"

    /// Rejects every redirect that leaves the rules, and more than `maxRedirects` of them.
    private final class RedirectGuard: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
        private let lock = NSLock()
        private var count = 0

        func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                        newRequest request: URLRequest) async -> URLRequest? {
            let n: Int = lock.withLock { count += 1; return count }
            guard n <= LinkPreviewRules.maxRedirects, LinkPreviewRules.isSafeToFetch(request.url) else { return nil }
            return request
        }
    }

    private static func session(_ delegate: RedirectGuard) -> URLSession {
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 10
        c.timeoutIntervalForResource = 20
        c.httpCookieStorage = nil
        c.httpShouldSetCookies = false
        c.urlCache = nil
        return URLSession(configuration: c, delegate: delegate, delegateQueue: nil)
    }

    /// GETs `url`; nil unless the answer is 2xx, of an accepted type and within `limit` bytes.
    private static func get(_ url: URL, accept: String, limit: Int) async -> (data: Data, type: String, finalURL: URL)? {
        guard LinkPreviewRules.isSafeToFetch(url) else { return nil }
        let guardian = RedirectGuard()
        let s = session(guardian)
        defer { s.finishTasksAndInvalidate() }
        var req = URLRequest(url: url)
        req.setValue(userAgent, forHTTPHeaderField: "User-Agent")
        req.setValue(accept, forHTTPHeaderField: "Accept")
        do {
            let (bytes, resp) = try await s.bytes(for: req)
            guard let http = resp as? HTTPURLResponse, (200..<300).contains(http.statusCode) else { return nil }
            if http.expectedContentLength > Int64(limit) { return nil }
            var data = Data()
            for try await b in bytes {
                data.append(b)
                if data.count > limit { return nil }
                if data.count % 65_536 == 0 { try Task.checkCancellation() }
            }
            let type = (http.mimeType ?? "").lowercased()
            return (data, type, http.url ?? url)
        } catch {
            return nil
        }
    }

    /// The preview for `link` (exact spelling from the message), or nil if the page gives no title.
    static func fetch(_ link: String) async -> LinkPreview? {
        guard let url = URL(string: link),
              let page = await get(url, accept: "text/html,application/xhtml+xml", limit: LinkPreviewRules.maxPageBytes),
              page.type == "text/html" || page.type == "application/xhtml+xml", !page.data.isEmpty
        else { return nil }
        let html = String(decoding: page.data, as: UTF8.self)
        let parsed = LinkPreviewRules.parse(html: html, url: link)
        var preview = parsed.preview
        let imageValue = parsed.imageUrl
        guard preview.isValid, !Task.isCancelled else { return nil }

        if !imageValue.isEmpty, let imageURL = URL(string: imageValue, relativeTo: page.finalURL)?.absoluteURL,
           let img = await get(imageURL, accept: "image/png,image/jpeg,image/webp,image/gif", limit: LinkPreviewRules.maxImageBytes) {
            let suffix = LinkPreviewRules.imageSuffix(contentType: img.type)
            if !suffix.isEmpty, NSImage(data: img.data) != nil,
               let file = try? Media.writeTemp(img.data, name: "link-preview-\(UUID().uuidString).\(suffix)") {
                preview.imagePath = file.path
                preview.imageContentType = img.type
                let thumb = Media.makeThumbnail(img.data)
                preview.imageThumb = thumb.isEmpty ? img.data : thumb
            }
        }
        return Task.isCancelled ? nil : preview
    }
}
