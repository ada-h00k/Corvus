import Foundation
import Security

/// Generic-password items in the login keychain. Service and account names match the
/// Qt app (QtKeychain uses the same layout on macOS), so both find the same entries.
public enum Keychain {
    public struct Failure: Error, LocalizedError {
        public let status: OSStatus
        public var errorDescription: String? {
            (SecCopyErrorMessageString(status, nil) as String?) ?? "Keychain error \(status)"
        }
    }

    private static func query(_ service: String, _ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    /// nil when there is no such entry.
    public static func load(service: String, account: String) throws -> String? {
        var q = query(service, account)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status = SecItemCopyMatching(q as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data else { throw Failure(status: status) }
        return String(decoding: data, as: UTF8.self)
    }

    public static func save(service: String, account: String, secret: String) throws {
        let data = Data(secret.utf8)
        let status = SecItemUpdate(query(service, account) as CFDictionary,
                                   [kSecValueData as String: data] as CFDictionary)
        if status == errSecSuccess { return }
        guard status == errSecItemNotFound else { throw Failure(status: status) }
        var add = query(service, account)
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        let addStatus = SecItemAdd(add as CFDictionary, nil)
        if addStatus != errSecSuccess { throw Failure(status: addStatus) }
    }

    public static func clear(service: String, account: String) throws {
        let status = SecItemDelete(query(service, account) as CFDictionary)
        if status != errSecSuccess && status != errSecItemNotFound { throw Failure(status: status) }
    }
}

/// The random secret that protects the history in "keychain" mode.
public enum HistoryKeychain {
    static let service = "corvus-history"
    /// Service names used under former names of the app (newest first); entries are moved over on first read.
    static let legacyServices = ["wren-history", "signal-cli-qt-history"]
    static let account = "history-encryption-key"

    private static func clearLegacy() {
        for legacy in legacyServices { try? Keychain.clear(service: legacy, account: account) }
    }

    public static func load() throws -> String? {
        if let secret = try Keychain.load(service: service, account: account) { return secret }
        for legacy in legacyServices {
            guard let old = try Keychain.load(service: legacy, account: account) else { continue }
            try Keychain.save(service: service, account: account, secret: old)
            clearLegacy()
            return old
        }
        return nil
    }
    public static func save(_ secret: String) throws { try Keychain.save(service: service, account: account, secret: secret) }
    public static func clear() throws {
        try Keychain.clear(service: service, account: account)
        clearLegacy() // must not resurrect the old secret
    }

    /// 256 random bits as 64 hex characters.
    public static func generateSecret() -> String {
        var bytes = [UInt8](repeating: 0, count: 32)
        _ = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        return bytes.map { String(format: "%02x", $0) }.joined()
    }
}

public enum GiphyApiKeyStore {
    static let service = "corvus"
    /// Service names used under former names of the app (newest first); entries are moved over on first read.
    static let legacyServices = ["wren", "signal-cli-qt"]
    static let account = "giphy-api-key"

    private static func clearLegacy() {
        for legacy in legacyServices { try? Keychain.clear(service: legacy, account: account) }
    }

    public static func load() throws -> String {
        if let key = try Keychain.load(service: service, account: account) {
            return key.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        for legacy in legacyServices {
            guard let old = try Keychain.load(service: legacy, account: account) else { continue }
            try Keychain.save(service: service, account: account, secret: old)
            clearLegacy()
            return old.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return ""
    }

    /// An empty key removes the entry.
    public static func save(_ key: String) throws {
        let k = key.trimmingCharacters(in: .whitespacesAndNewlines)
        if k.isEmpty {
            try Keychain.clear(service: service, account: account)
            clearLegacy()
        }
        else { try Keychain.save(service: service, account: account, secret: k) }
    }
}
