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
    static let account = "history-encryption-key"

    public static func load() throws -> String? {
        if let secret = try Keychain.load(service: service, account: account) { return secret }
        return nil
    }
    public static func save(_ secret: String) throws { try Keychain.save(service: service, account: account, secret: secret) }
    public static func clear() throws {
        try Keychain.clear(service: service, account: account)
    }

    /// 256 random bits as 64 hex characters. Fails closed: an RNG failure must never
    /// yield an all-zero (or otherwise predictable) history encryption key.
    public static func generateSecret() throws -> String {
        var bytes = [UInt8](repeating: 0, count: 32)
        let status = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        guard status == errSecSuccess else {
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey:
                                     "SecRandomCopyBytes failed (OSStatus \(status))"])
        }
        return bytes.map { String(format: "%02x", $0) }.joined()
    }
}

public enum GiphyApiKeyStore {
    static let service = "corvus"
    static let account = "giphy-api-key"

    public static func load() throws -> String {
        if let key = try Keychain.load(service: service, account: account) {
            return key.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return ""
    }

    /// An empty key removes the entry.
    public static func save(_ key: String) throws {
        let k = key.trimmingCharacters(in: .whitespacesAndNewlines)
        if k.isEmpty { try Keychain.clear(service: service, account: account) }
        else { try Keychain.save(service: service, account: account, secret: k) }
    }
}
