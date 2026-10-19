import Foundation

/// One-time migration of data written under the former names of the app:
/// "Wren" (the name immediately before Corvus) and "signal-cli-qt" (the name before that).
enum LegacyMigration {
    /// A former name of the app under which preferences or a history may still exist.
    private struct Source {
        let bundleID: String
        /// Folder below Application Support; used for organisation and application like Qt's AppDataLocation.
        let folder: String
        let historyName: String
    }

    /// Newest name first, so that its data wins if several generations are present.
    private static let sources = [
        Source(bundleID: "io.github.wren.mac", folder: "wren", historyName: "history.wren"),
        Source(bundleID: "io.github.signal-cli-qt.mac", folder: "signal-cli-qt", historyName: "history.scq"),
    ]
    private static let newHistory = "history.corvus"

    /// Set when the history of a still running older version blocks the migration.
    nonisolated(unsafe) static var olderInstanceRunning = false

    /// Copies the preferences of the old bundle identifiers into the current one (only if the
    /// current one has none yet) and removes the old domains.
    static func migrateDefaults() {
        for source in sources { migrateDefaults(from: source.bundleID) }
    }

    private static func migrateDefaults(from oldBundleID: String) {
        guard let current = Bundle.main.bundleIdentifier, current != oldBundleID else { return }
        let std = UserDefaults.standard
        guard let old = std.persistentDomain(forName: oldBundleID), !old.isEmpty else { return }
        if (std.persistentDomain(forName: current) ?? [:]).isEmpty {
            for (key, value) in old { std.set(value, forKey: key) }
        }
        std.removePersistentDomain(forName: oldBundleID)
    }

    /// Moves the encrypted history (history.wren / history.scq -> history.corvus) from the old data folder
    /// to `newDir`. Never overwrites an existing history and refuses while an older version still holds the lock.
    static func migrateData(to newDir: URL) {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        for source in sources {
            let oldDir = base.appendingPathComponent("\(source.folder)/\(source.folder)", isDirectory: true)
            if !migrateData(from: oldDir, historyName: source.historyName, to: newDir) { return }
        }
    }

    /// Returns false if the migration must stop because an older version still runs.
    private static func migrateData(from oldDir: URL, historyName: String, to newDir: URL) -> Bool {
        let fm = FileManager.default
        guard fm.fileExists(atPath: oldDir.appendingPathComponent(historyName).path),
              !fm.fileExists(atPath: newDir.appendingPathComponent(newHistory).path) else { return true }

        let lockPath = oldDir.appendingPathComponent("history.lock").path
        let fd = open(lockPath, O_RDWR)
        if fd >= 0 {
            let busy = flock(fd, LOCK_EX | LOCK_NB) != 0
            close(fd) // also releases our own lock
            if busy { olderInstanceRunning = true; return false }
        }

        try? fm.createDirectory(at: newDir, withIntermediateDirectories: true)
        guard let names = try? fm.contentsOfDirectory(atPath: oldDir.path) else { return true }
        for name in names where name != "history.lock" {
            var target = name
            if name.hasPrefix(historyName) { target = newHistory + String(name.dropFirst(historyName.count)) }
            let dst = newDir.appendingPathComponent(target)
            if fm.fileExists(atPath: dst.path) { continue }
            try? fm.moveItem(at: oldDir.appendingPathComponent(name), to: dst)
        }
        try? fm.removeItem(at: oldDir.appendingPathComponent("history.lock"))
        // rmdir only removes empty folders, so nothing that could not be moved is lost.
        rmdir(oldDir.path)
        rmdir(oldDir.deletingLastPathComponent().path)
        return true
    }
}
