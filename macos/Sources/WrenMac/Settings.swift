import AppKit
import SwiftUI

/// Settings → Notifications & sound. Every switch is independent of the others.
struct NotificationSettings: Equatable {
    enum Content: Int, CaseIterable { case nameAndText = 0, nameOnly = 1, hidden = 2 }

    var notifications = true          // desktop notifications
    var content = Content.nameAndText // what a notification reveals
    var sound = true                  // play a sound for new messages
    var soundId = "pop-ogg"           // built-in id or "custom"
    var soundFile = ""                // used when soundId == "custom"
    var trayIcon = true               // show the menu bar icon
    var closeToTray = true            // closing the window keeps the app running in the menu bar
    var startMinimizedInTray = false  // start with the window hidden

    private static let prefix = "notifications."

    static func load(_ d: UserDefaults = .standard) -> NotificationSettings {
        var n = NotificationSettings()
        func bool(_ k: String, _ def: Bool) -> Bool { d.object(forKey: prefix + k) == nil ? def : d.bool(forKey: prefix + k) }
        n.notifications = bool("enabled", n.notifications)
        n.content = Content(rawValue: min(2, max(0, d.integer(forKey: prefix + "content")))) ?? .nameAndText
        n.sound = bool("sound", n.sound)
        n.soundId = d.string(forKey: prefix + "soundId") ?? n.soundId
        n.soundFile = d.string(forKey: prefix + "soundFile") ?? ""
        n.trayIcon = bool("trayIcon", n.trayIcon)
        n.closeToTray = bool("closeToTray", n.closeToTray)
        n.startMinimizedInTray = bool("startMinimizedInTray", n.startMinimizedInTray)
        return n
    }

    func save(_ d: UserDefaults = .standard) {
        let p = NotificationSettings.prefix
        d.set(notifications, forKey: p + "enabled")
        d.set(content.rawValue, forKey: p + "content")
        d.set(sound, forKey: p + "sound")
        d.set(soundId, forKey: p + "soundId")
        d.set(soundFile, forKey: p + "soundFile")
        d.set(trayIcon, forKey: p + "trayIcon")
        d.set(closeToTray, forKey: p + "closeToTray")
        d.set(startMinimizedInTray, forKey: p + "startMinimizedInTray")
    }
}

/// Colours and shape of the chat bubbles (Settings → Appearance). A colour that the user did not
/// change is nil and follows the light or dark system appearance.
struct Appearance: Equatable {
    var background: String?   // chat area, "#rrggbb"
    var outBg: String?        // your messages
    var outText: String?
    var inBg: String?         // received messages
    var inText: String?
    var radius = 16           // bubble corner radius in points
    var showAvatars = true    // contact avatars in the chat list

    static let colorKeys: [(String, WritableKeyPath<Appearance, String?>)] = [
        ("background", \.background), ("outBg", \.outBg), ("outText", \.outText), ("inBg", \.inBg), ("inText", \.inText),
    ]

    // Defaults (adapt to the system appearance where it makes sense).
    static let defaultBackground = NSColor.textBackgroundColor
    static let defaultOutBg = NSColor(srgbRed: 0x2c / 255, green: 0x6b / 255, blue: 0xed / 255, alpha: 1) // Signal blue
    static let defaultOutText = NSColor.white
    static let defaultInBg = NSColor(name: nil) { a in
        a.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            ? NSColor(srgbRed: 0x3a / 255, green: 0x3a / 255, blue: 0x3c / 255, alpha: 1)
            : NSColor(srgbRed: 0xe9 / 255, green: 0xe9 / 255, blue: 0xeb / 255, alpha: 1)
    }
    static let defaultInText = NSColor(name: nil) { a in
        a.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            ? NSColor(srgbRed: 0xee / 255, green: 0xee / 255, blue: 0xee / 255, alpha: 1)
            : NSColor(srgbRed: 0x1b / 255, green: 0x1b / 255, blue: 0x1b / 255, alpha: 1)
    }

    var backgroundColor: Color { Color(nsColor: Appearance.color(background) ?? Appearance.defaultBackground) }
    var outBgColor: Color { Color(nsColor: Appearance.color(outBg) ?? Appearance.defaultOutBg) }
    var outTextColor: Color { Color(nsColor: Appearance.color(outText) ?? Appearance.defaultOutText) }
    var inBgColor: Color { Color(nsColor: Appearance.color(inBg) ?? Appearance.defaultInBg) }
    var inTextColor: Color { Color(nsColor: Appearance.color(inText) ?? Appearance.defaultInText) }

    static func color(_ hex: String?) -> NSColor? {
        guard var h = hex?.trimmingCharacters(in: .whitespaces), h.hasPrefix("#"), h.count == 7 else { return nil }
        h.removeFirst()
        guard let v = UInt32(h, radix: 16) else { return nil }
        return NSColor(srgbRed: CGFloat((v >> 16) & 0xff) / 255, green: CGFloat((v >> 8) & 0xff) / 255,
                       blue: CGFloat(v & 0xff) / 255, alpha: 1)
    }

    static func hex(_ c: Color) -> String {
        let n = NSColor(c).usingColorSpace(.sRGB) ?? .black
        return String(format: "#%02x%02x%02x", Int((n.redComponent * 255).rounded()),
                      Int((n.greenComponent * 255).rounded()), Int((n.blueComponent * 255).rounded()))
    }

    private static let prefix = "appearance."

    static func load(_ d: UserDefaults = .standard) -> Appearance {
        var a = Appearance()
        for (name, kp) in colorKeys {
            let v = d.string(forKey: prefix + name)
            if color(v) != nil { a[keyPath: kp] = v }
        }
        if d.object(forKey: prefix + "radius") != nil { a.radius = min(28, max(0, d.integer(forKey: prefix + "radius"))) }
        if d.object(forKey: prefix + "showAvatars") != nil { a.showAvatars = d.bool(forKey: prefix + "showAvatars") }
        return a
    }

    /// Only values that differ from the defaults are stored, so untouched colours keep following the system.
    func save(_ d: UserDefaults = .standard) {
        let p = Appearance.prefix
        for (name, kp) in Appearance.colorKeys {
            if let v = self[keyPath: kp] { d.set(v, forKey: p + name) } else { d.removeObject(forKey: p + name) }
        }
        if radius == 16 { d.removeObject(forKey: p + "radius") } else { d.set(radius, forKey: p + "radius") }
        if showAvatars { d.removeObject(forKey: p + "showAvatars") } else { d.set(false, forKey: p + "showAvatars") }
    }
}

enum AppPaths {
    /// Same location as Qt's AppDataLocation for organisation/application "wren" on macOS,
    /// so a history created by the Qt build is found as well.
    private static var dataDirURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("wren/wren", isDirectory: true)
    }

    /// Moves a history from the former "signal-cli-qt" location once per run.
    private static let legacyMigration: Void = { LegacyMigration.migrateData(to: dataDirURL) }()

    static var dataDir: URL {
        _ = legacyMigration
        let dir = dataDirURL
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    static var historyPath: String { dataDir.appendingPathComponent("history.wren").path }
    static var lockPath: String { dataDir.appendingPathComponent("history.lock").path }

    /// Attachments that are opened, pasted screenshots, downloaded GIFs, … removed when the app quits.
    static let tempDir: URL = {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("wren-\(ProcessInfo.processInfo.processIdentifier)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true,
                                                 attributes: [.posixPermissions: 0o700])
        return dir
    }()
}
