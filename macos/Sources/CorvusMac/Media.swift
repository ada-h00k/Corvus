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

    /// signal-cli runs as another app (src/SignalCliHelper.cpp) and may not read protected folders (Desktop,
    /// Documents, …): copies the files into a new folder in the temporary directory and returns the copies.
    static func stageForSignalCli(_ paths: [String]) -> (paths: [String], dir: URL) {
        let dir = AppPaths.tempDir.appendingPathComponent("send-" + UUID().uuidString, isDirectory: true)
        let staged = paths.enumerated().map { i, path -> String in
            // One folder per file: signal-cli sends the file name along.
            let target = dir.appendingPathComponent("\(i)", isDirectory: true)
                .appendingPathComponent((path as NSString).lastPathComponent)
            do {
                try FileManager.default.createDirectory(at: target.deletingLastPathComponent(),
                                                        withIntermediateDirectories: true)
                try FileManager.default.copyItem(atPath: path, toPath: target.path)
                return target.path
            } catch {
                return path
            }
        }
        return (staged, dir)
    }

    /// Writes data to a fresh file in the temporary directory.
    static func writeTemp(_ data: Data, name: String) throws -> URL {
        let dir = AppPaths.tempDir.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent(name.isEmpty ? "attachment" : name)
        try data.write(to: url)
        return url
    }

    /// Like `writeTemp`, for data received from others: the file is marked as downloaded (quarantine).
    static func writeAttachmentTemp(_ data: Data, name: String) throws -> URL {
        let url = try writeTemp(data, name: name)
        quarantine(url)
        return url
    }

    /// Sets the quarantine flag browsers set on downloads, so Gatekeeper checks the file (and anything
    /// unpacked from it) before it can run.
    static func quarantine(_ url: URL) {
        var u = url
        var values = URLResourceValues()
        values.quarantineProperties = [kLSQuarantineAgentNameKey as String: "Corvus",
                                       kLSQuarantineTypeKey as String: kLSQuarantineTypeOtherDownload as String]
        try? u.setResourceValues(values)
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

    /// The Note to Self glyph: Signal-blue circle with a white notepad (spiral binding, ruled lines),
    /// drawn like the Qt app's noteToSelfAvatar.
    static let noteToSelfAvatar: NSImage = {
        let size: CGFloat = 64
        return NSImage(size: NSSize(width: size, height: size), flipped: true) { rect in
            let blue = NSColor(srgbRed: 0x3a / 255, green: 0x76 / 255, blue: 0xf0 / 255, alpha: 1)
            blue.setFill()
            NSBezierPath(ovalIn: rect).fill()
            let paper = NSRect(x: size * 0.29, y: size * 0.26, width: size * 0.42, height: size * 0.54)
            NSColor.white.setFill()
            NSBezierPath(roundedRect: paper, xRadius: size * 0.05, yRadius: size * 0.05).fill()
            for i in 0..<3 {
                let x = paper.minX + paper.width * (0.22 + 0.28 * CGFloat(i))
                NSColor.white.setFill()
                NSBezierPath(ovalIn: NSRect(x: x - size * 0.05, y: paper.minY - size * 0.05, width: size * 0.1, height: size * 0.1)).fill()
                blue.setFill()
                NSBezierPath(ovalIn: NSRect(x: x - size * 0.018, y: paper.minY - size * 0.018, width: size * 0.036, height: size * 0.036)).fill()
            }
            blue.setStroke()
            for i in 0..<3 {
                let y = paper.minY + paper.height * (0.38 + 0.22 * CGFloat(i))
                let line = NSBezierPath()
                line.lineWidth = size * 0.035
                line.lineCapStyle = .round
                line.move(to: NSPoint(x: paper.minX + paper.width * 0.16, y: y))
                line.line(to: NSPoint(x: paper.maxX - paper.width * 0.16, y: y))
                line.stroke()
            }
            return true
        }
    }()

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
