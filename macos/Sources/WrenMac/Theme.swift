import AppKit
import SwiftUI

/// Colours and metrics shared by the views (close to the official Signal desktop client).
enum Theme {
    static let signalBlue = Color(red: 0x2c / 255, green: 0x6b / 255, blue: 0xed / 255)

    /// Background of the chat list and composer (slightly different from the chat area).
    static let chrome = Color(nsColor: NSColor(name: nil) { a in
        a.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            ? NSColor(srgbRed: 0.13, green: 0.13, blue: 0.14, alpha: 1)
            : NSColor(srgbRed: 0.97, green: 0.97, blue: 0.98, alpha: 1)
    })

    /// Filled background of the message field.
    static let field = Color(nsColor: NSColor(name: nil) { a in
        a.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            ? NSColor(srgbRed: 0.20, green: 0.20, blue: 0.21, alpha: 1)
            : NSColor(srgbRed: 0.92, green: 0.92, blue: 0.93, alpha: 1)
    })

    /// Stable colour per contact for sender names in groups (readable on light and dark bubbles).
    static func senderColor(_ key: String) -> Color {
        let palette: [Color] = [
            Color(red: 0.82, green: 0.25, blue: 0.25), Color(red: 0.85, green: 0.45, blue: 0.10),
            Color(red: 0.62, green: 0.50, blue: 0.05), Color(red: 0.20, green: 0.60, blue: 0.30),
            Color(red: 0.10, green: 0.55, blue: 0.60), Color(red: 0.25, green: 0.45, blue: 0.85),
            Color(red: 0.50, green: 0.35, blue: 0.85), Color(red: 0.80, green: 0.30, blue: 0.60),
        ]
        var hash: UInt32 = 2166136261
        for b in key.utf8 { hash = (hash ^ UInt32(b)) &* 16777619 }
        return palette[Int(hash % UInt32(palette.count))]
    }

    /// Messages of the same author within this time form one group (joined bubbles, one avatar).
    static let groupGapMs: Int64 = 3 * 60 * 1000
}

/// Small round avatar from the model's cache (or the letter placeholder).
struct AvatarView: View {
    let image: NSImage
    var size: CGFloat = 32

    var body: some View {
        Image(nsImage: image)
            .resizable()
            .interpolation(.high)
            .frame(width: size, height: size)
            .clipShape(Circle())
    }
}
