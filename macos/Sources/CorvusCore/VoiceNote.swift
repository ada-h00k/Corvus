import Foundation

/// Rules for recorded voice messages (same as the Qt app, src/VoiceRecorder.cpp).
public enum VoiceNote {
    /// Longest recording; it stops by itself then (the result is kept).
    public static let maxDurationMs: Int64 = 15 * 60 * 1000
    /// Shorter recordings are taken for an accidental click and dropped.
    public static let minDurationMs: Int64 = 500

    /// "voice-message-2026-10-27-14-15-23" (without extension)
    public static func baseName(_ date: Date = Date()) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd-HH-mm-ss"
        return "voice-message-" + f.string(from: date)
    }

    /// "0:07", "12:03", "1:02:03"
    public static func durationText(_ ms: Int64) -> String {
        let total = max(0, ms) / 1000
        let h = total / 3600, m = (total / 60) % 60, s = total % 60
        if h > 0 { return String(format: "%lld:%02lld:%02lld", h, m, s) }
        return String(format: "%lld:%02lld", m, s)
    }

    /// signal-cli can mark audio attachments as voice notes ("voiceNote") since 0.14.2.
    /// Accepts "0.14.2" as well as "signal-cli 0.14.2".
    public static func supportsVoiceNote(signalCliVersion: String) -> Bool {
        let last = signalCliVersion.split(separator: " ").last.map(String.init) ?? ""
        let parts = last.split(separator: ".").map { Int($0.prefix { $0.isNumber }) }
        guard parts.count >= 2, !parts.contains(where: { $0 == nil }) else { return false }
        let v = parts.map { $0! } + [0, 0]
        return (v[0], v[1], v[2]) >= (0, 14, 2)
    }
}
