import Foundation

/// Safety numbers as reported by corvus-cli ("listIdentities"), port of the Qt app's SafetyNumber.
public enum SafetyNumber {
    public static let digitCount = 60

    public struct Identity: Equatable {
        public var number = ""
        public var uuid = ""
        public var fingerprint = ""
        public var safetyNumber = ""   // as delivered (digits, possibly grouped)
        public var scannable = ""      // base64 of the data inside the QR code
        public var trustLevel = ""     // TRUSTED_VERIFIED, TRUSTED_UNVERIFIED, UNTRUSTED, …
        public var addedMs: Int64 = 0

        public init() {}

        public var digits: String { SafetyNumber.digitsOnly(safetyNumber) }
        public var hasSafetyNumber: Bool { digits.count == SafetyNumber.digitCount }
        public var isVerified: Bool { trustLevel == "TRUSTED_VERIFIED" }
        public var isUntrusted: Bool { trustLevel == "UNTRUSTED" }
        /// The chat this identity belongs to: the number, or the UUID without one.
        public var recipient: String { number.isEmpty ? uuid : number }
    }

    public static func digitsOnly(_ text: String) -> String { String(text.filter { ("0"..."9").contains($0) }) }

    /// 60 digits -> 12 groups of five, four groups per line (like the official clients).
    public static func format(_ safetyNumber: String) -> String {
        let d = Array(digitsOnly(safetyNumber))
        guard d.count == digitCount else { return safetyNumber }
        var out = ""
        for i in stride(from: 0, to: digitCount, by: 5) {
            if i > 0 { out += i % 20 == 0 ? "\n" : " " }
            out += String(d[i..<(i + 5)])
        }
        return out
    }

    /// Both texts hold the same 60 digits (separators ignored).
    public static func matches(_ a: String, _ b: String) -> Bool {
        let da = digitsOnly(a)
        return da.count == digitCount && da == digitsOnly(b)
    }

    /// The bytes inside the QR code of the official apps (base64 in corvus-cli's answer).
    public static func scannableBytes(_ base64: String) -> Data {
        Data(base64Encoded: base64.trimmingCharacters(in: .whitespacesAndNewlines)) ?? Data()
    }

    public static func parse(_ result: Any?) -> [Identity] {
        let items: [Any] = (result as? [Any]) ?? (result.map { [$0] } ?? [])
        return items.compactMap { v in
            guard let o = v as? JSON else { return nil }
            var id = Identity()
            id.number = jsonString(o["number"])
            id.uuid = jsonString(o["uuid"])
            id.fingerprint = jsonString(o["fingerprint"])
            id.safetyNumber = jsonString(o["safetyNumber"])
            id.scannable = jsonString(o["scannableSafetyNumber"])
            id.trustLevel = jsonString(o["trustLevel"])
            id.addedMs = jsonInt64(o["addedTimestamp"])
            if id.number.isEmpty && id.uuid.isEmpty && id.safetyNumber.isEmpty { return nil }
            return id
        }
    }

    /// The identity to show for `recipient`: one with a usable safety number, that recipient's own first, newest first.
    public static func pick(_ identities: [Identity], recipient: String) -> Identity? {
        var best: Identity?
        var bestExact = false
        for id in identities where id.hasSafetyNumber {
            let exact = !recipient.isEmpty && (id.number == recipient || id.uuid == recipient)
            if best == nil || (exact && !bestExact) || (exact == bestExact && id.addedMs > best!.addedMs) {
                best = id
                bestExact = exact
            }
        }
        return best
    }

    /// Has the safety number changed since we last saw (or the user last accepted) it? corvus-cli marks a key that
    /// replaced a trusted one as UNTRUSTED; that counts as a change even when nothing is remembered yet.
    public static func changed(stored: String, now: Identity) -> Bool {
        guard now.hasSafetyNumber else { return false }
        if stored.isEmpty { return now.isUntrusted }
        return digitsOnly(stored) != now.digits
    }

    /// corvus-cli refuses to send to a contact whose new key was not accepted yet ("Untrusted Identity for …").
    public static func isUntrustedIdentityError(_ error: String) -> Bool {
        error.range(of: "untrusted identity", options: .caseInsensitive) != nil
    }

    /// Recipients of a multi-recipient send whose key changed (results of type IDENTITY_FAILURE).
    public static func identityFailures(in sendResult: Any?) -> [String] {
        jsonArray(jsonObject(sendResult)["results"]).compactMap { v in
            let r = jsonObject(v)
            guard jsonString(r["type"]) == "IDENTITY_FAILURE" else { return nil }
            let address = jsonObject(r["recipientAddress"])
            let who = jsonString(address["number"])
            return who.isEmpty ? (jsonString(address["uuid"]).isEmpty ? nil : jsonString(address["uuid"])) : who
        }
    }

    public static func trustLabel(_ level: String) -> String {
        switch level {
        case "TRUSTED_VERIFIED": return "Verified"
        case "TRUSTED_UNVERIFIED": return "Not verified"
        case "UNTRUSTED": return "Not trusted (the key has changed)"
        default: return level.isEmpty ? "Unknown" : level
        }
    }
}
