import AppKit
import ImageIO
import UniformTypeIdentifiers

enum Media {
    static let maxThumbSource: Int64 = 30 * 1024 * 1024
    static let maxAttachment: Int64 = 100 * 1024 * 1024 // Signal's upload limit

    static func mimeType(forFile path: String) -> String {
        let ext = (path as NSString).pathExtension
        return UTType(filenameExtension: ext)?.preferredMIMEType ?? "application/octet-stream"
    }

    static func preferredSuffix(forMime mime: String) -> String {
        UTType(mimeType: mime)?.preferredFilenameExtension ?? ""
    }

    static func humanSize(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }

    static func pixelSize(_ data: Data) -> (Int, Int)? {
        guard let src = CGImageSourceCreateWithData(data as CFData, nil),
              let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any],
              let w = props[kCGImagePropertyPixelWidth] as? Int, let h = props[kCGImagePropertyPixelHeight] as? Int
        else { return nil }
        let orientation = props[kCGImagePropertyOrientation] as? Int ?? 1
        return orientation >= 5 ? (h, w) : (w, h)
    }

    /// JPEG/PNG preview with the longest side <= maxDim (orientation applied), as stored in the history.
    static func makeThumbnail(_ data: Data, maxDim: Int = 480) -> Data {
        guard let src = CGImageSourceCreateWithData(data as CFData, nil) else { return Data() }
        let opts: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maxDim,
        ]
        guard let img = CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary) else { return Data() }
        let hasAlpha: Bool = {
            switch img.alphaInfo {
            case .none, .noneSkipFirst, .noneSkipLast: return false
            default: return true
            }
        }()
        let out = NSMutableData()
        let type = (hasAlpha ? UTType.png : UTType.jpeg).identifier as CFString
        guard let dest = CGImageDestinationCreateWithData(out, type, 1, nil) else { return Data() }
        CGImageDestinationAddImage(dest, img, hasAlpha ? nil : [kCGImageDestinationLossyCompressionQuality: 0.82] as CFDictionary)
        return CGImageDestinationFinalize(dest) ? out as Data : Data()
    }

    /// Writes data to a fresh file in the temporary directory.
    static func writeTemp(_ data: Data, name: String) throws -> URL {
        let dir = AppPaths.tempDir.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent(name.isEmpty ? "attachment" : name)
        try data.write(to: url)
        return url
    }

    private static let riskyExtensions: Set<String> = [
        "exe", "bat", "cmd", "com", "scr", "msi", "ps1", "vbs", "js", "jar", "sh", "desktop", "appimage", "run",
        "lnk", "reg", "dll", "app", "deb", "rpm", "apk", "command", "pkg", "dmg", "workflow", "scpt", "terminal",
    ]

    static func isRisky(_ name: String) -> Bool {
        riskyExtensions.contains((name as NSString).pathExtension.lowercased())
    }

    // MARK: Avatars

    /// Coloured circle with the first letter, like the official client shows without a picture.
    static func placeholderAvatar(key: String, title: String, size: CGFloat = 64) -> NSImage {
        let letter = title.first(where: { $0.isLetter || $0.isNumber }).map { String($0).uppercased() } ?? "?"
        var hash: UInt32 = 2166136261
        for b in key.utf8 { hash = (hash ^ UInt32(b)) &* 16777619 }
        let hue = CGFloat(hash % 360) / 360
        return NSImage(size: NSSize(width: size, height: size), flipped: false) { rect in
            NSColor(hue: hue, saturation: 0.45, brightness: 0.62, alpha: 1).setFill()
            NSBezierPath(ovalIn: rect).fill()
            let font = NSFont.boldSystemFont(ofSize: size * 0.5)
            let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.white]
            let s = NSAttributedString(string: letter, attributes: attrs)
            let sz = s.size()
            s.draw(at: NSPoint(x: rect.midX - sz.width / 2, y: rect.midY - sz.height / 2))
            return true
        }
    }

    /// Round avatar, cut from the centre of the picture.
    static func roundAvatar(_ image: NSImage, size: CGFloat = 64) -> NSImage {
        NSImage(size: NSSize(width: size, height: size), flipped: false) { rect in
            NSBezierPath(ovalIn: rect).addClip()
            let s = image.size
            let scale = max(size / max(s.width, 1), size / max(s.height, 1))
            let w = s.width * scale, h = s.height * scale
            image.draw(in: NSRect(x: (size - w) / 2, y: (size - h) / 2, width: w, height: h))
            return true
        }
    }

    static func pngData(_ image: NSImage) -> Data? {
        guard let tiff = image.tiffRepresentation, let rep = NSBitmapImageRep(data: tiff) else { return nil }
        return rep.representation(using: .png, properties: [:])
    }
}
