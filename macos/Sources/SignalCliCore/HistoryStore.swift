import CHistoryStore
import Foundation

/// Error codes of the Rust store (history-store/include/history_store.h).
public struct HistoryStoreError: Error, Equatable, LocalizedError {
    public let code: Int32
    public init(code: Int32) { self.code = code }

    public static let wrongPassphrase = HistoryStoreError(code: Int32(SC_ERR_WRONG_PASSPHRASE))

    public var errorDescription: String? { HistoryStore.errorText(code) }
}

/// Thin Swift wrapper around the Rust encrypted history store (history-store/).
/// The handle must be used from one thread at a time; callers serialise access
/// (the app model runs slow calls such as `open` off the main thread while nothing else uses it).
public final class HistoryStore: @unchecked Sendable {
    private var handle: OpaquePointer?

    public init() {}
    deinit { close() }

    public var isOpen: Bool { handle != nil }

    public static func exists(_ path: String) -> Bool { sc_store_exists(path) == 1 }

    public static func errorText(_ code: Int32) -> String {
        switch Int(code) {
        case Int(SC_OK): return ""
        case Int(SC_ERR_WRONG_PASSPHRASE): return "Wrong passphrase."
        case Int(SC_ERR_CORRUPT): return "The history file is damaged or has been modified."
        case Int(SC_ERR_EXISTS): return "A history file already exists."
        case Int(SC_ERR_IO): return "The history file could not be read or written."
        default: return "Encryption error (code \(code))."
        }
    }

    /// Passphrase bytes are wiped after use (best effort).
    private static func withSecret<T>(_ s: String, _ body: (UnsafePointer<CChar>) -> T) -> T {
        var bytes = Array(s.utf8CString)
        defer {
            for i in bytes.indices { bytes[i] = 0 }
        }
        return bytes.withUnsafeBufferPointer { body($0.baseAddress!) }
    }

    private static func encode(_ obj: Any) throws -> String {
        let data = try JSONSerialization.data(withJSONObject: obj, options: [])
        return String(decoding: data, as: UTF8.self)
    }

    public func create(path: String, passphrase: String) throws {
        close()
        var err: Int32 = 0
        handle = HistoryStore.withSecret(passphrase) { sc_store_create(path, $0, &err) }
        if handle == nil { throw HistoryStoreError(code: err) }
    }

    /// Opens the file and returns all records in order.
    public func open(path: String, passphrase: String) throws -> [JSON] {
        close()
        var err: Int32 = 0
        var json: UnsafeMutablePointer<CChar>?
        handle = HistoryStore.withSecret(passphrase) { sc_store_open(path, $0, &json, &err) }
        guard handle != nil else { throw HistoryStoreError(code: err) }
        defer { sc_string_free(json) }
        guard let json else { return [] }
        let data = Data(bytes: json, count: strlen(json))
        let parsed = (try? JSONSerialization.jsonObject(with: data)) as? [Any] ?? []
        return parsed.compactMap { $0 as? JSON }
    }

    public func append(_ record: JSON) throws {
        guard let handle else { throw HistoryStoreError(code: Int32(SC_ERR_ARG)) }
        let rc = sc_store_append(handle, try HistoryStore.encode(record))
        if rc != SC_OK { throw HistoryStoreError(code: rc) }
    }

    /// Replaces the whole file (compaction): removed records' ciphertext is gone afterwards.
    public func rewrite(_ records: [JSON]) throws {
        guard let handle else { throw HistoryStoreError(code: Int32(SC_ERR_ARG)) }
        let rc = sc_store_rewrite(handle, try HistoryStore.encode(records))
        if rc != SC_OK { throw HistoryStoreError(code: rc) }
    }

    public func changePassphrase(old: String, new: String) throws {
        guard let handle else { throw HistoryStoreError(code: Int32(SC_ERR_ARG)) }
        let rc = HistoryStore.withSecret(old) { o in
            HistoryStore.withSecret(new) { n in sc_store_change_passphrase(handle, o, n) }
        }
        if rc != SC_OK { throw HistoryStoreError(code: rc) }
    }

    public func close() {
        sc_store_close(handle)
        handle = nil
    }
}
