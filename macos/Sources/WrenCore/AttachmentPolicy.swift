import Foundation
import UniformTypeIdentifiers

/// What a click on a received attachment may do. File name and content type both come from the
/// sender, so both are checked and the stricter answer wins.
public enum AttachmentPolicy: Equatable {
    case inApp   // shown or played by the app itself (decoded image, audio, video)
    case risky   // can run code: never opened, only saved
    case ask     // anything else: opened in the default app only after confirmation

    /// `isMedia`: the app would only decode the data itself (audio, video, an image that decoded).
    public static func classify(filename: String, contentType: String, isMedia: Bool) -> AttachmentPolicy {
        if isRiskyName(filename) || isRiskyMime(contentType) { return .risky }
        if isMedia { return .inApp }
        // A file without an extension could be a Unix executable or a script with a shebang.
        if normalizedExtension(filename).isEmpty { return .risky }
        return .ask
    }

    static let riskyExtensions: Set<String> = [
        // Windows / Linux / Android
        "exe", "bat", "cmd", "com", "scr", "msi", "ps1", "vbs", "js", "jar", "dll", "lnk", "reg",
        "desktop", "appimage", "run", "deb", "rpm", "apk",
        // shell and script languages
        "sh", "bash", "zsh", "csh", "ksh", "fish", "tcsh", "py", "pl", "rb", "php",
        // macOS launchers, installers and bundles
        "app", "command", "terminal", "tool", "pkg", "mpkg", "dmg", "xip", "workflow", "action", "scpt", "scptd",
        "applescript", "fileloc", "inetloc", "webloc", "prefpane", "saver", "kext", "osax", "shortcut", "bundle", "plugin",
    ]

    static let riskyMimeTypes: Set<String> = [
        "application/x-sh", "application/x-shellscript", "text/x-shellscript", "text/x-sh",
        "application/x-executable", "application/x-mach-binary", "application/x-msdownload", "application/x-msdos-program",
        "application/java-archive", "application/x-apple-diskimage", "application/vnd.apple.installer+xml",
        "application/x-python", "text/x-python", "application/x-perl", "application/x-ruby",
        "application/javascript", "text/javascript",
    ]

    /// Types the system itself treats as runnable (covers extensions missing from the list above).
    private static let riskyTypes: [UTType] = [
        .executable, .script, .shellScript, .application, .applicationBundle, .unixExecutable, .diskImage,
    ]

    /// Extension without surrounding spaces or dots, lowercased ("x.terminal " -> "terminal").
    static func normalizedExtension(_ filename: String) -> String {
        let name = filename.trimmingCharacters(in: CharacterSet.whitespacesAndNewlines.union(CharacterSet(charactersIn: ".")))
        return (name as NSString).pathExtension.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }

    static func isRiskyName(_ filename: String) -> Bool {
        let ext = normalizedExtension(filename)
        if ext.isEmpty { return false }
        if riskyExtensions.contains(ext) { return true }
        guard let type = UTType(filenameExtension: ext) else { return false }
        return riskyTypes.contains { type.conforms(to: $0) }
    }

    static func isRiskyMime(_ contentType: String) -> Bool {
        let mime = contentType.split(separator: ";").first.map { $0.trimmingCharacters(in: .whitespaces).lowercased() } ?? ""
        return riskyMimeTypes.contains(mime)
    }

    /// Extension for a temporary audio/video file: the sender's extension only if it is a known media type,
    /// otherwise the one of the content type, otherwise `fallback`.
    public static func mediaSuffix(filename: String, contentType: String, fallback: String) -> String {
        let ext = normalizedExtension(filename)
        if let type = UTType(filenameExtension: ext), type.conforms(to: .audiovisualContent) { return ext }
        if let type = UTType(mimeType: contentType), type.conforms(to: .audiovisualContent),
           let suffix = type.preferredFilenameExtension {
            return suffix
        }
        return fallback
    }
}
