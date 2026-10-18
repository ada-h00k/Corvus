import Foundation

/// One-time migration of data written under the former name "signal-cli-qt"
/// (before the project was renamed to Wren).
enum LegacyMigration {
    static let oldBundleID = "io.github.signal-cli-qt.mac"
    private static let oldHistory = "history.scq"
    private static let newHistory = "history.wren"

    /// Set when the history of a still running older version blocks the migration.
    nonisolated(unsafe) static var olderInstanceRunning = false

    /// Copies the preferences of the old bundle identifier into the current one (only if the
    /// current one has none yet) and removes the old domain.
    static func migrateDefaults() {
        guard let current = Bundle.main.bundleIdentifier, current != oldBundleID else { return }
        let std = UserDefaults.standard
        guard let old = std.persistentDomain(forName: oldBundleID), !old.isEmpty else { return }
        if (std.persistentDomain(forName: current) ?? [:]).isEmpty {
            for (key, value) in old { std.set(value, forKey: key) }
        }
        std.removePersistentDomain(forName: oldBundleID)
    }

    /// Moves the encrypted history (history.scq -> history.wren) from the old data folder to `newDir`.
    /// Never overwrites an existing history and refuses while an older version still holds the lock.
    static func migrateData(to newDir: URL) {
        let fm = FileManager.default
        let base = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let oldDir = base.appendingPathComponent("signal-cli-qt/signal-cli-qt", isDirectory: true)
        guard fm.fileExists(atPath: oldDir.appendingPathComponent(oldHistory).path),
              !fm.fileExists(atPath: newDir.appendingPathComponent(newHistory).path) else { return }

        let lockPath = oldDir.appendingPathComponent("history.lock").path
        let fd = open(lockPath, O_RDWR)
        if fd >= 0 {
            let busy = flock(fd, LOCK_EX | LOCK_NB) != 0
            close(fd) // also releases our own lock
            if busy { olderInstanceRunning = true; return }
        }

        try? fm.createDirectory(at: newDir, withIntermediateDirectories: true)
        guard let names = try? fm.contentsOfDirectory(atPath: oldDir.path) else { return }
        for name in names where name != "history.lock" {
            var target = name
            if name.hasPrefix(oldHistory) { target = newHistory + String(name.dropFirst(oldHistory.count)) }
            let dst = newDir.appendingPathComponent(target)
            if fm.fileExists(atPath: dst.path) { continue }
            try? fm.moveItem(at: oldDir.appendingPathComponent(name), to: dst)
        }
        try? fm.removeItem(at: oldDir.appendingPathComponent("history.lock"))
        // rmdir only removes empty folders, so nothing that could not be moved is lost.
        rmdir(oldDir.path)
        rmdir(oldDir.deletingLastPathComponent().path)
    }
}
